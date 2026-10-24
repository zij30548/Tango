"""有界意图路由服务：只保存受控计划，派发与偏好确认仍由既有客户端负责。"""

import asyncio
import json
import logging
import time
from uuid import uuid4
from .errors import ApiError
from .compat import digest, now_ms, utf16_len, well_formed, maybe_await
from .conversation_route_store import ConversationRouteStore
from .intent_routing import deterministicRoute, validateRouteProposal, isHistoryQuestion, _lookup_request, _release_artist, ROUTE_DIAGNOSTIC_REASONS
from .capability_registry import CAPABILITY_REGISTRY_VERSION
from .agent_tracing import AgentTracing
from .music_request import artistCatalogRequest
from .music_skills import routedMusicSkills
from .conversation_store import _decoded
from .lookup_context import LOOKUP_CONTEXT_MAX_DEPTH, LOOKUP_CONTEXT_TTL_MS, lookupReference, lookupSelection, validateLookupContext
import re

logger = logging.getLogger(__name__)


def _fail(status, code, message):
    raise ApiError(status, code, message)


def _request_body(body):
    if (
        type(body) is not dict
        or not {"deviceId", "preference"} <= set(body) <= {"deviceId", "preference", "lookupContext"}
        or not isinstance(body["deviceId"], str)
        or not isinstance(body["preference"], str)
        or utf16_len(body["preference"]) > 500
        or not well_formed(body["preference"])
        or re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", body["preference"])
    ):
        _fail(
            400,
            "INVALID_ROUTE_REQUEST",
            "路由请求只接受本机设备标识、已确认音乐偏好和可选的只读候选上下文。",
        )
    return {**body, **({"lookupContext": validateLookupContext(body["lookupContext"])} if "lookupContext" in body else {})}


def _public_route(route):
    return (
        {
            key: route.get(key)
            for key in ["id", "messageId", "status", "result", "errorCode", "paid"]
        }
        if route
        else None
    )


def _cancelled(signal):
    return bool(signal and signal.is_set())


class ConversationRouter:
    def __init__(self, options=None, **kwargs):
        o = {**(options or {}), **kwargs}
        self.store, self.conversations, self.tasks, self.model = (
            o["store"],
            o["conversations"],
            o["tasks"],
            o.get("model"),
        )
        self.now, self.allowCall = (
            o.get("now", now_ms),
            o.get("allowCall", lambda _user: True),
        )
        self.tracing = o.get("tracing") or AgentTracing()
        self.closed, self.controllers, self.running, self.localSlots = (
            False,
            set(),
            set(),
            {},
        )
        self._repository_options = {
            k: o[k]
            for k in [
                "workerId",
                "leaseMs",
                "dailyLimit",
                "globalDailyLimit",
                "globalConcurrency",
            ]
            if k in o
        }
        self._repository_options["workerId"] = self._repository_options.get(
            "workerId"
        ) or str(uuid4())
        self._repository_options.setdefault("dailyLimit", 100)
        self._repository_options.setdefault("globalDailyLimit", 2000)
        self._repository_options.setdefault("globalConcurrency", 8)
        self._initialized, self._init_lock = False, asyncio.Lock()
        self.repository = None

    async def initialize(self):
        async with self._init_lock:
            if not self._initialized:
                await self.conversations.initialize()
                await self.tasks.initialize()
                limit = self._repository_options["globalConcurrency"]

                def reserve(claim):
                    tasks = self.tasks
                    if (
                        claim["userId"] in tasks.accountCalls
                        or any(
                            e["claim"]["userId"] == claim["userId"]
                            for e in tasks.legacyClaims.values()
                        )
                        or tasks.inflight
                        + tasks.legacyCount()
                        + len(tasks.legacyClaims)
                        >= limit
                    ):
                        _fail(
                            429,
                            "AI_BUSY",
                            "当前模型请求仍在执行，请稍后继续；本次未调用模型。",
                        )
                    tail = asyncio.get_running_loop().create_future()
                    entry = {"taskId": "route:" + claim["routeId"], "tail": tail}
                    tasks.accountCalls[claim["userId"]] = entry
                    tasks.inflight += 1
                    self.localSlots[claim["attemptId"]] = {"entry": entry, "tail": tail}

                def release(claim):
                    slot = self.localSlots.pop(claim["attemptId"], None)
                    if not slot:
                        return
                    self.tasks.inflight -= 1
                    if self.tasks.accountCalls.get(claim["userId"]) is slot["entry"]:
                        self.tasks.accountCalls.pop(claim["userId"], None)
                    if not slot["tail"].done():
                        slot["tail"].set_result(None)

                self.repository = ConversationRouteStore(
                    store=self.store,
                    taskStore=self.tasks.store,
                    conversations=self.conversations,
                    now=self.now,
                    localHooks={"reserve": reserve, "release": release},
                    **self._repository_options,
                )
                await self.repository.initialize()
                self._initialized = True
        return self

    async def get(self, conversationId, messageId, userId):
        await self.initialize()
        conversation = await self.conversations.get(conversationId, userId)
        route = await self.repository.get(
            {
                "conversationId": conversationId,
                "messageId": messageId,
                "userId": userId,
                "deviceId": conversation["deviceId"],
            }
        )
        if not route:
            _fail(
                404, "ROUTE_NOT_FOUND", "这条消息尚未开始意图路由；读取不会触发执行。"
            )
        return {"conversation": conversation, "route": _public_route(route)}

    def observationContext(self, userId, deviceId, messageId, route):
        """用持久路由时间重建关联，既不改变业务请求，也不发送账号与设备原值。"""
        if not self.tracing.enabled:
            return None
        try:
            return self.tracing.new_context(
                "conversation:" + userId + ":" + deviceId + ":" + route["conversationId"] + ":" + messageId,
                metadata={
                    "conversation_id": route["conversationId"],
                    "message_id": messageId,
                    "route_id": route["id"],
                    "attempt_id": route["attemptId"],
                    "thread_id": route["conversationId"],
                },
                start_ms=route["createdAt"],
            )
        except Exception:
            # 观测失败不得改变持久路由、额度或现有恢复语义。
            logger.warning("agent_trace_context_unavailable")
            return None

    def finishObservation(self, context, span, outcome, paid):
        """只报告已持久化的路由状态；音乐计划就绪后等待任务继续，不能先宣称完成。"""
        if not context:
            return
        try:
            status = (outcome or {}).get("status", "uncertain")
            if status not in {"completed", "failed", "uncertain"}:
                status = "uncertain"
            result = (outcome or {}).get("result") or {}
            actions = result.get("actions", []) if status == "completed" else []
            capabilities = list(dict.fromkeys(a["capability"] for a in actions))
            outputs = {
                "status": status,
                "paid": paid,
                "intents": result.get("intents", []) if status == "completed" else [],
                "capabilities": capabilities,
                "action_count": len(actions),
                "skills": list(dict.fromkeys(skill for action in actions for skill in routedMusicSkills(action["capability"], result.get("intents", [])))),
            }
            if span is not None:
                span.outputs = outputs
                span.metadata.update(status=status)
                if status != "completed":
                    span.error_code = (outcome or {}).get("errorCode") or "ROUTE_PERSISTENCE_UNKNOWN"
            if status == "completed" and "music.task" in capabilities:
                self.tracing.update_root(
                    context, {**outputs, "status": "awaiting_task", "root_complete": False}
                )
            else:
                self.tracing.finish_root(
                    context,
                    outputs,
                    error_code=(outcome or {}).get("errorCode")
                    or ("ROUTE_PERSISTENCE_UNKNOWN" if outcome is None else None),
                )
        except Exception:
            # 观测导出失败不覆盖原业务异常，也不重试模型或数据库写入。
            logger.warning("agent_trace_finish_unavailable")

    async def route(self, conversationId, messageId, userId, raw, options=None):
        body, options = _request_body(raw), options or {}
        await self.initialize()
        if self.closed:
            _fail(503, "ROUTER_CLOSED", "助手正在关闭，请稍后重试。")
        key = {
            "conversationId": conversationId,
            "messageId": messageId,
            "userId": userId,
            "deviceId": body["deviceId"],
        }
        existing = await self.repository.get(key)
        if existing:
            if (existing["input"]["preference"] != body["preference"]
                    or existing["input"].get("lookupContext") != body.get("lookupContext")):
                _fail(
                    409,
                    "IDEMPOTENCY_CONFLICT",
                    "这条消息的路由输入已保存，不能替换偏好或候选上下文后重复执行。",
                )
            return {
                "conversation": await self.conversations.get(conversationId, userId),
                "route": _public_route(existing),
            }
        context = await self.conversations.routeContext(
            conversationId, userId, body["deviceId"], messageId
        )
        if not context["isLatest"]:
            _fail(409, "CONVERSATION_STALE", "已有更新的用户消息，请处理最新一轮。")
        if "lookupContext" in body:
            await self.validateLookupSource(key, body["lookupContext"])
        input = {
            **{k: key[k] for k in ["conversationId", "messageId", "deviceId"]},
            "instruction": context["instruction"],
            "history": context["history"],
            "task": context["task"],
            "preference": body["preference"],
            **({"lookupContext": body["lookupContext"]} if "lookupContext" in body else {}),
        }
        if isHistoryQuestion(input["instruction"]):
            input["historyArtists"] = await self.historyArtists(conversationId, userId, body["deviceId"])
        direct = deterministicRoute(input)
        paid = direct is None
        if paid and not getattr(self.model, "configured", False):
            _fail(
                503,
                "MODEL_NOT_CONFIGURED",
                "管理员尚未配置意图模型；帮助与明确任务控制仍可使用。",
            )
        if paid and not await maybe_await(self.allowCall(userId)):
            _fail(429, "AI_RATE_LIMITED", "请求过于频繁，请稍后再试。")
        signal, authorize = options.get("signal"), options.get("authorize")
        if _cancelled(signal):
            _fail(499, "REQUEST_CANCELLED", "本次请求已取消。")
        if authorize and not await maybe_await(authorize()):
            _fail(401, "AUTH_REQUIRED", "登录已过期，请重新连接。")
        # 设备候选仅供确定性只读筛选和持久核验，不作为模型上下文或模型选择策略输入。
        modelInput = {key: value for key, value in input.items() if key != "lookupContext"}
        selector = getattr(self.model, "selectCall", None)
        selection = (
            await maybe_await(selector("route", modelInput))
            if paid and selector
            else None
        )
        reserved = await self.repository.begin(
            {**key, "input": input, "inputHash": digest(input), "capabilityVersion": CAPABILITY_REGISTRY_VERSION},
            paid,
            **({"modelSelection": selection} if selection else {}),
        )
        if not reserved["acquired"]:
            return {
                "conversation": await self.conversations.get(conversationId, userId),
                "route": _public_route(reserved["route"]),
            }
        claim, controller = reserved["claim"], asyncio.Event()
        self.controllers.add(controller)
        settled = asyncio.get_running_loop().create_future()
        self.running.add(settled)
        if _cancelled(signal):
            controller.set()

        async def watch_signal():
            await signal.wait()
            controller.set()

        watcher = asyncio.create_task(watch_signal()) if signal else None

        async def heartbeat():
            while True:
                await asyncio.sleep(max(0.1, self.repository.leaseMs / 3000))
                try:
                    if not await self.repository.heartbeat(claim):
                        controller.set()
                        return
                except Exception:
                    controller.set()
                    return

        beat = asyncio.create_task(heartbeat())
        usage, metadata, outcome = None, None, None
        observationContext = self.observationContext(
            userId, body["deviceId"], messageId, reserved["route"]
        )
        try:
            self.tracing.begin_root(observationContext)
        except Exception:
            logger.warning("agent_trace_begin_unavailable")
        observation, observationFinished = None, False
        try:
            with self.tracing.span(
                observationContext,
                "conversation.route",
                inputs={},
                metadata={
                    "paid": paid,
                    "planner": "deterministic" if direct is not None else "model",
                },
            ) as observation:
                try:
                    if controller.is_set():
                        _fail(499, "REQUEST_CANCELLED", "请求已取消。")
                    response = (
                        {"value": direct}
                        if direct is not None
                        else await self.model.route(
                            modelInput,
                            {"signal": controller, **({"modelSelection": selection} if selection else {})},
                        )
                    )
                    usage = response.get("usage")
                    metadata = response.get("modelMetadata")
                    result = validateRouteProposal(
                        response["value"],
                        input,
                        messageId=messageId,
                        planner="deterministic" if direct is not None else "model",
                    )
                    if controller.is_set():
                        _fail(499, "REQUEST_CANCELLED", "请求已取消。")
                    if authorize and not await maybe_await(authorize()):
                        _fail(401, "AUTH_REQUIRED", "登录已过期，未派发动作。")
                    latest = await self.conversations.routeContext(
                        conversationId, userId, body["deviceId"], messageId
                    )
                    if not latest["isLatest"]:
                        _fail(409, "CONVERSATION_STALE", "已有新消息，本轮旧计划未派发。")
                    if "lookupContext" in input:
                        # 长时间运行后再次核验根查找有效期和原动作，不因模型结束而续期。
                        await self.validateLookupSource(key, input["lookupContext"])
                    bound = any(
                        a["capability"] == "task.control"
                        or a["capability"] == "music.task"
                        and a["parameters"]["mode"] == "continue"
                        for a in result["actions"]
                    )
                    if bound and (
                        not latest["task"]
                        or latest["task"]["id"] != (input.get("task") or {}).get("id")
                        or latest["task"]["revision"]
                        != (input.get("task") or {}).get("revision")
                    ):
                        _fail(409, "TASK_REVISION_CONFLICT", "任务版本已更新，本轮计划未派发。")
                    outcome = await self.repository.finish(
                        claim, status="completed", result=result, usage=usage,
                        **({"modelMetadata": metadata} if metadata else {}),
                    )
                except (Exception, asyncio.CancelledError) as error:
                    usage = usage or getattr(error, "taskUsage", None)
                    code = (
                        error.code
                        if isinstance(error, ApiError)
                        else "REQUEST_CANCELLED"
                        if isinstance(error, asyncio.CancelledError)
                        else "ROUTE_FAILED"
                    )
                    uncertain = code in [
                        "REQUEST_CANCELLED",
                        "MODEL_TIMEOUT",
                        "ROUTE_CLAIM_LOST",
                    ]
                    if code == "INVALID_ROUTE_PLAN":
                        # 只写固定原因枚举及随机关联 ID；提案、用户正文和上游响应均不进入诊断日志。
                        reason = getattr(error, "routeReason", "proposal_shape")
                        diagnostic = getattr(error, "routeDiagnostic", {})
                        # 固定计数帮助区分缺字段和多字段，不记录任何提案内容或未知字段名。
                        logger.warning("route_plan_rejected routeId=%s messageId=%s reason=%s structure=%s", claim["routeId"], messageId, reason if isinstance(reason, str) and reason in ROUTE_DIAGNOSTIC_REASONS else "proposal_shape", json.dumps(diagnostic, sort_keys=True))
                    outcome = await self.repository.finish(
                        claim,
                        status="uncertain" if uncertain else "failed",
                        errorCode=code,
                        usage=usage,
                        **({"modelMetadata": metadata or getattr(error, "taskModelMetadata", None)} if metadata or getattr(error, "taskModelMetadata", None) else {}),
                    )
                finally:
                    beat.cancel()
                    if watcher:
                        watcher.cancel()
                    await asyncio.gather(
                        beat, *([watcher] if watcher else []), return_exceptions=True
                    )
                    # 持久写入失败也要释放本机占位；数据库未知尝试和租约仍按原恢复规则处理。
                    self.repository.releaseLocal(claim)
                    self.controllers.discard(controller)
                    self.running.discard(settled)
                    if not settled.done():
                        settled.set_result(None)
                    self.finishObservation(observationContext, observation, outcome, paid)
                    observationFinished = True
        finally:
            if not observationFinished:
                self.finishObservation(observationContext, observation, outcome, paid)
        return {
            "conversation": await self.conversations.get(conversationId, userId),
            "route": _public_route(outcome),
        }

    async def validateLookupSource(self, key, candidateContext):
        """核对同账号设备会话的相邻来源及派生链；不认证设备曲目声明的上游真实性。"""
        def stale():
            _fail(409, "LOOKUP_CONTEXT_STALE", "歌曲候选已失效或不属于这一轮，请重新搜索后选择。")

        journal = await self.conversations.row(key["conversationId"], key["userId"])
        if not journal or journal["device_id"] != key["deviceId"]:
            stale()
        state = _decoded(journal["journal_json"])
        users = [message for message in state["messages"] if message["role"] == "user"]
        anchors = [index for index, message in enumerate(users) if message["id"] == key["messageId"]]
        if len(anchors) != 1 or anchors[0] == 0:
            stale()
        previous = users[anchors[0] - 1]

        async def source(messageId):
            try:
                return await self.repository.get({**key, "messageId": messageId})
            except ApiError:
                # 错误只暴露固定失效原因，不泄露其他会话或账号的消息是否存在。
                stale()

        if previous["id"] != candidateContext["originMessageId"]:
            adjacent = await source(previous["id"])
            actions = ((adjacent or {}).get("result") or {}).get("actions", [])
            reference = lookupReference((adjacent or {}).get("input", {}).get("instruction", ""), candidateContext)
            if (not adjacent or adjacent["status"] != "completed"
                    or adjacent["input"].get("lookupContext") != candidateContext
                    or (adjacent["result"] or {}).get("intents") != ["song_lookup"]
                    or not reference or "question" not in reference
                    or len(actions) != 1 or actions[0].get("capability") != "assistant.reply"
                    or actions[0].get("parameters", {}).get("kind") != "clarify"):
                stale()
        current, seen = candidateContext, set()
        for _depth in range(LOOKUP_CONTEXT_MAX_DEPTH):
            messageId = current["originMessageId"]
            if messageId in seen:
                stale()
            seen.add(messageId)
            origin = await source(messageId)
            actions = ((origin or {}).get("result") or {}).get("actions", [])
            if not origin or origin["status"] != "completed" or len(actions) != 1:
                stale()
            action = actions[0]
            if action.get("id") != current["originActionId"]:
                stale()
            if action.get("capability") == "music.lookup":
                age = self.now() - origin["createdAt"]
                if not 0 <= age <= LOOKUP_CONTEXT_TTL_MS:
                    stale()
                return
            if action.get("capability") != "music.refine":
                stale()
            before = origin["input"].get("lookupContext")
            try:
                before = validateLookupContext(before)
                visible = lookupSelection(before, action["parameters"])
            except (ApiError, KeyError, TypeError):
                stale()
            if (before["rows"] != current["rows"] or visible != current["visibleIds"]
                    or current["candidateSetId"] == before["candidateSetId"]
                    or any(action["parameters"].get(field) != before[field]
                           for field in ["candidateSetId", "originMessageId", "originActionId"])):
                stale()
            current = before
        stale()

    async def historyArtists(self, conversationId, userId, deviceId):
        """只读当前归属下的有限路由历史；失败提案不能伪造已核实歌手。"""
        rows = await self.store.db.fetchall(
            "SELECT input_json,result_json FROM music_conversation_routes WHERE conversation_id=? AND user_id=? AND device_id=? ORDER BY created_at DESC,id DESC LIMIT 200",
            (conversationId, userId, deviceId),
        )
        artists = []
        for row in reversed(rows):
            saved = json.loads(row["input_json"])
            result = json.loads(row["result_json"]) if row["result_json"] else {}
            candidates = []
            if isinstance(result, dict):
                candidates.append((result.get("slots") or {}).get("artist", ""))
                candidates.extend(a.get("parameters", {}).get("artist", "") for a in result.get("actions", []) if a.get("capability") in ("music.lookup", "music.artist_catalog"))
            instruction = saved.get("instruction", "") if isinstance(saved, dict) else ""
            lookup = _lookup_request(instruction)
            candidates.extend([(lookup or {}).get("artist", ""), (artistCatalogRequest(instruction) or {}).get("artist", ""), _release_artist(instruction)])
            for artist in candidates:
                if isinstance(artist, str) and 0 < utf16_len(artist) <= 100 and artist not in artists:
                    artists.append(artist)
        return artists[-30:]

    async def taskContext(self, userId, deviceId, messageId, instruction):
        await self.initialize()
        saved = await self.repository.findForTask(
            {
                "userId": userId,
                "deviceId": deviceId,
                "messageId": messageId,
                "instruction": instruction,
            }
        )
        if not saved:
            return None
        result = saved["result"]
        context = {
            "history": saved["input"]["history"],
            "slots": result["slots"],
            "intents": result["intents"],
            "dependencyCount": sum(bool(a.get("dependsOn")) for a in result["actions"]),
            "handledIntents": [
                v
                for v in result["intents"]
                if v in ["preference_proposal", "preference_deletion"]
            ],
            "musicMode": next(
                a for a in result["actions"] if a["capability"] == "music.task"
            )["parameters"]["mode"],
        }
        if self.tracing.enabled:
            # 私有观测字段由运行时立即分离，不能成为模型上下文或客户端参数。
            try:
                route = await self.repository.get(
                    {
                        "conversationId": saved["input"]["conversationId"],
                        "messageId": messageId,
                        "userId": userId,
                        "deviceId": deviceId,
                    }
                )
                observation = self.observationContext(userId, deviceId, messageId, route)
                if observation:
                    context["_observation"] = observation
            except Exception:
                logger.warning("agent_trace_task_link_unavailable")
        return context

    async def waitForModelSlot(self, userId, signal=None):
        await self.initialize()
        if not self.repository.distributed:
            return False
        deadline, waited = (
            time.monotonic() + min(45000, self.repository.leaseMs + 15000) / 1000,
            False,
        )
        while True:
            if self.closed or _cancelled(signal):
                _fail(499, "REQUEST_CANCELLED", "任务已取消，未发起下一次模型请求。")
            active = await self.store.db.fetchone(
                "SELECT 1 FROM music_conversation_routes WHERE user_id=? AND paid=1 AND status='running' AND lease_until>? LIMIT 1",
                (userId, self.now()),
            )
            if not active:
                return waited
            if time.monotonic() >= deadline:
                _fail(429, "AI_BUSY", "同账号的意图请求尚未结束，请稍后恢复音乐任务。")
            waited = True
            if signal:
                try:
                    await asyncio.wait_for(signal.wait(), 0.08)
                except TimeoutError:
                    pass
            else:
                await asyncio.sleep(0.08)

    async def close(self):
        self.closed = True
        for controller in self.controllers:
            controller.set()
        if self.running:
            await asyncio.wait(list(self.running), timeout=1)
