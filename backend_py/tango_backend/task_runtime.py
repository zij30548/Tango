"""原生 Python LangGraph 固定音乐工作流；持久账本、CAS 与租约负责执行边界。"""

import asyncio
import logging
import re
import uuid
import weakref
from typing import TypedDict, Any
from langgraph.graph import StateGraph, START, END
from .compat import clone, digest, iso, now_ms, maybe_await, utf16_slice, safe_integer
from .errors import ApiError
from .task_store import TaskStore
from .task_schema import (
    createInput,
    actionInput,
    toolResult,
    assemblePreview,
    refreshPreviewLocksCheck,
    artistCreditMatches,
    clean,
    identifier,
    TASK_SKILLS,
    date_ms,
    fixed1,
)
from .task_style_evidence import (
    adjustmentStyles,
    adjustmentAssessment,
    playlistStyleEvidence,
    styleLabelMatches,
)
from .task_language import catalogLanguageForTurn, catalogLanguageConflict
from .task_query_coverage import sceneQueryPlan
from .task_tool_evidence import mergeToolEvidence, observerAssessment
from .task_pending_request import validatePendingRequest
from .music_request import compileMusicBrief, musicRequirement
from .task_tool_cache import DisabledTaskToolCache
from .task_progress import taskProgress
from .agent_tracing import AgentTracing, disable_automatic_tracing

ACTIVE = {"planning", "researching", "composing", "waiting_tools"}
STATES = {
    "planning": "正在理解完整要求",
    "researching": "正在分别规划场景和新作研究",
    "waiting_tools": "等待本机查询真实曲库",
    "composing": "正在编排并检查歌单",
    "preview": "歌单已生成，等待采用",
    "awaiting_apply": "等待本机采用歌单",
    "adopted": "歌单已采用",
    "paused": "任务已暂停，可继续",
    "needs_clarification": "需要补充一项信息",
    "failed": "任务未完成，可修改要求",
    "cancelled": "任务已取消",
    "budget_exhausted": "达到调用预算，已保留进度",
}


def _id():
    return str(uuid.uuid4())


def _refresh_requirement_plan(task, previous_brief):
    """当前需求与已校验brief同步；旧版本由previousRequirementPlan单独保存。"""
    plan = task.get("requirementPlan")
    if not plan:
        return
    brief, old = task["brief"], previous_brief or {}
    partial = musicRequirement(task["instruction"], allow_partial=True) or {}
    sources = clone(plan.get("sources", []))
    for field, brief_field in (("scene", "scene"), ("artist", "requiredArtist"),
                               ("targetMinutes", "targetMinutes"), ("retainFirst", "retainFirst")):
        if field not in plan and field not in partial:
            continue
        # 用户的原称与曲库规范名分开；选择身份不会把工具文字改写成用户原文。
        if field == "artist" and brief.get(brief_field) == old.get(brief_field):
            continue
        value = brief.get(brief_field)
        if plan.get(field) == value:
            continue
        evidence = next((source for source in partial.get("sources", [])
                         if source["field"] == field and source["value"] == value), None)
        # 未能受限编译的合法修改仍经过正式补丁校验；只引用本轮原话，不借用工具文本。
        evidence = clone(evidence) if evidence else {"field": field, "value": value,
                                                    "source": task["instruction"], "scope": "task"}
        plan[field] = value
        sources = [source for source in sources if source["field"] != field] + [evidence]
        plan["goal"] = "revise_playlist"
    plan["sources"] = sources[-32:]


def _scene_artist_identity(task, branch, parsed):
    """场景里的指定歌手同样需要身份回执；名称别名不能替代真实曲目署名ID。"""
    if branch["skill"] != "scene.research" or not task["brief"].get("requiredArtist"):
        return
    resolved = parsed.get("resolvedArtist")
    if not resolved:
        if task.get("requirementPlan") and parsed.get("tracks"):
            raise ApiError(422, "CATALOG_ARTIST_UNVERIFIED", "指定歌手候选缺少曲库身份回执，请重新查询。")
        return
    expected_id = branch["plan"].get("requiredArtistId") or task["brief"].get("requiredArtistId")
    requested = task["brief"]["requiredArtist"]
    canonical = lambda name: re.sub(r"[\s.·]", "", name).casefold()
    if expected_id and expected_id != resolved["id"]:
        raise ApiError(409, "ARTIST_ID_MISMATCH", "曲库返回了不同歌手，请重新选择。")
    if not expected_id and canonical(requested) not in {canonical(name) for name in [resolved["name"], *resolved.get("aliases", [])]}:
        raise ApiError(422, "CATALOG_ARTIST_UNVERIFIED", "曲库身份与指定歌手不一致，请先确认歌手身份。")
    task["brief"].update(requiredArtist=resolved["name"], requiredArtistId=resolved["id"])
    task["verifiedArtist"] = {**resolved, "requestedName": requested, "checkedAt": parsed["checkedAt"]}


def _message(role, content, revision):
    return {"messageId": _id(), "role": role, "content": content, "revision": revision}


def _code(error):
    return getattr(error, "code", None)


def _scene_research_required(task):
    """发行检索词不等于场景需求，优先沿用已核验的会话意图。"""
    brief = task["brief"]
    if not brief["requireReleases"]:
        return True
    routing = task.get("routingContext") or {}
    intents = routing.get("intents", [])
    if "release_research" in intents:
        return "scene_research" in intents
    if (task.get("previousBrief") or {}).get("requireReleases") and any(
        b["skill"] == "scene.research"
        for b in task.get("previousBranches", {}).values()
    ):
        return True
    # 兼容直接任务入口：只有用户原话中出现实际场景才增加场景分支，
    # 不用模型生成的“某歌手发行查询”等检索词扩大到通用歌单。
    scene = brief.get("scene", "").strip()
    return bool(
        scene
        and scene in task.get("instruction", "")
        and not re.search(r"新歌|新作|新专辑|发行|发布|releases?", scene, re.I)
    )


def _catalog_clarification(task):
    """从当前分支恢复待澄清状态，避免并行结果或恢复漏掉歌手确认。"""
    for branch in task["branches"].values():
        result = branch.get("result") or {}
        if result.get("question"):
            task["question"] = result["question"]
            task["providedArtists"] = result.get("artists", [])
            return True
    return bool(task.get("question"))


def _ensure_messages(task):
    seen = set()
    messages = []
    for entry in task.get("messages", []):
        entry = dict(entry)
        if not identifier(entry.get("messageId")) or entry["messageId"] in seen:
            entry["messageId"] = _id()
        seen.add(entry["messageId"])
        if (
            not safe_integer(entry.get("revision"))
            or not 1 <= entry["revision"] <= task["revision"]
        ):
            entry["revision"] = 1
        messages.append(entry)
    task["messages"] = messages


def _confirmed(task):
    if type(task.get("hasConfirmedBrief")) is bool:
        return task["hasConfirmedBrief"]
    return bool(
        task.get("preview")
        or task.get("versions")
        or task.get("brief")
        and task["brief"].get("kind") != "clarify"
        or any(e.get("node") == "brief_ready" for e in task.get("trace", []))
        or task.get("branches")
        or task.get("previousBranches")
    )


def _pending_context(task):
    return {
        "taskId": task["id"],
        "revision": task["revision"],
        "messages": task["messages"],
        "currentBrief": task.get("brief"),
        "preview": (task.get("preview") or {}).get("tracks", []),
        "lockedIds": task["lockedIds"],
        "hasConfirmedBrief": _confirmed(task),
        "instruction": task["instruction"],
    }


def _pending_from_messages(task, question):
    sources = [
        {k: e[k] for k in ("messageId", "role", "content", "revision")}
        for e in task["messages"]
        if e["role"] == "user"
    ]
    return validatePendingRequest(
        {
            "schemaVersion": 1,
            "id": _id(),
            "taskId": task["id"],
            "rootRevision": sources[0]["revision"] if sources else None,
            "revision": task["revision"],
            "status": "awaiting_answer",
            "question": question,
            "sources": sources,
        },
        _pending_context(task),
    )


class _GraphState(TypedDict):
    task: dict[str, Any]
    revision: int
    route: str


class MusicTaskRuntime:
    def __init__(
        self,
        options=None,
        *,
        store=None,
        model=None,
        now=now_ms,
        dailyLimit=100,
        globalDailyLimit=2000,
        globalConcurrency=8,
        maxRunCalls=6,
        maxTaskCalls=24,
        workerId=None,
        leaseMs=30000,
        legacyBusy=None,
        legacyCount=None,
        allowCall=None,
        strategy="single_agent",
        progressHub=None,
        toolCache=None,
        tracing=None,
    ):
        if options:
            store = options.get("store", store)
            model = options.get("model", model)
            now = options.get("now", now)
            dailyLimit = options.get("dailyLimit", dailyLimit)
            globalDailyLimit = options.get("globalDailyLimit", globalDailyLimit)
            globalConcurrency = options.get("globalConcurrency", globalConcurrency)
            maxRunCalls = options.get("maxRunCalls", maxRunCalls)
            maxTaskCalls = options.get("maxTaskCalls", maxTaskCalls)
            workerId = options.get("workerId", workerId)
            leaseMs = options.get("leaseMs", leaseMs)
            legacyBusy = options.get("legacyBusy", legacyBusy)
            legacyCount = options.get("legacyCount", legacyCount)
            allowCall = options.get("allowCall", allowCall)
            strategy = options.get("strategy", strategy)
            progressHub = options.get("progressHub", progressHub)
            toolCache = options.get("toolCache", toolCache)
            tracing = options.get("tracing", tracing)
        self.accountStore = store
        self.store = TaskStore(store)
        self.model = model
        self.now = now
        self.progressHub = progressHub
        self.toolCache = toolCache or DisabledTaskToolCache()
        self.tracing = tracing or AgentTracing()
        self.dailyLimit = dailyLimit
        self.globalDailyLimit = globalDailyLimit
        self.globalConcurrency = globalConcurrency
        self.maxRunCalls = maxRunCalls
        self.maxTaskCalls = maxTaskCalls
        self.legacyBusy = legacyBusy or (lambda user: False)
        self.legacyCount = legacyCount or (lambda: 0)
        self.allowCall = allowCall or (lambda user: True)
        if strategy not in ("fixed", "single_agent", "dual_agent"):
            raise ValueError("Invalid evaluation strategy")
        self.strategy = strategy
        self.jobs = {}
        self.accountCalls = {}
        self.inflight = 0
        self.closed = False
        self.workerId = workerId or _id()
        self.leaseMs = leaseMs
        self.taskClaims = {}
        self.checkpointTails = weakref.WeakValueDictionary()
        self.legacyClaims = {}
        self.runningJobs = set()
        self.conversationContextProvider = None
        self.beforeModelCall = None
        self._initialized = False
        self._initializing = asyncio.Lock()
        self.recoveryTimer = None
        graph = StateGraph(_GraphState)

        def enter_graph(state):
            with self.tracing.span(
                state["task"].get("_observation"), "task.entry",
                metadata=self.observationState(state["task"]),
            ) as span:
                span.outputs.update(self.observationState(state["task"]))
                return {"route": state["task"]["phase"]}

        graph.add_node("entry", enter_graph)
        for node, method in [
            ("understand", self.understand),
            ("research", self.research),
            ("dispatch", self.dispatch),
            ("observe", self.observe),
            ("compose", self.compose),
        ]:

            def wrapped(fn, stage):
                async def run(state):
                    task = state["task"]
                    with self.tracing.span(
                        task.get("_observation"), "task." + stage,
                        metadata=self.observationState(task),
                    ) as span:
                        await fn(task, state["revision"])
                        span.outputs.update(self.observationState(task))
                    return {"task": state["task"]}

                return run

            graph.add_node(node, wrapped(method, node))
        graph.add_edge(START, "entry")
        graph.add_conditional_edges(
            "entry",
            lambda s: {
                "plan": "understand",
                "research": "research",
                "dispatch": "dispatch",
                "observe": "observe",
                "compose": "compose",
            }.get(s["route"], END),
        )
        graph.add_conditional_edges(
            "understand",
            lambda s: (
                END
                if s["task"]["status"] == "needs_clarification"
                else "compose"
                if s["task"]["phase"] == "compose"
                else "research"
            ),
        )
        graph.add_edge("research", "dispatch")
        graph.add_conditional_edges(
            "dispatch", lambda s: "observe" if s["task"]["phase"] == "observe" else END
        )
        graph.add_conditional_edges(
            "observe",
            lambda s: (
                END
                if s["task"]["status"] == "needs_clarification"
                else "dispatch"
                if s["task"]["phase"] == "dispatch"
                else "compose"
            ),
        )
        graph.add_edge("compose", END)
        self.graph = graph.compile()

    def observationState(self, task):
        """只投影执行事实；不把任务正文、偏好、候选或播放凭据送入追踪。"""
        preview = task.get("preview") or {}
        return {
            "task_id": task.get("id"), "run_id": task.get("runId"),
            "revision": task.get("revision"), "state_version": task.get("stateVersion", 0),
            "status": task.get("status"), "stage": task.get("phase"),
            "track_count": len(preview.get("tracks", [])),
            "checks": [
                {"key": check.get("key"), "status": check.get("status")}
                for check in preview.get("checks", [])
            ],
        }

    def observeCheckpoint(self, task, node):
        """仅在业务持久写入成功后更新云端事实，通知和追踪不决定任务状态。"""
        try:
            self._observeCheckpoint(task, node)
        except Exception:
            # 观测适配自身的配置或编码错误也不能推翻已经提交的业务结果。
            logging.getLogger(__name__).warning("agent_trace_observation_failed")

    def _observeCheckpoint(self, task, node):
        context = task.get("_observation")
        summary = {**self.observationState(task), "checkpoint": node}
        with self.tracing.span(context, "task.checkpoint", inputs={"checkpoint": node}) as span:
            span.outputs.update(summary)
        if node in {"turn", "pause", "cancel", "resume", "select_artist", "lock", "undo", "adopt", "applied"}:
            name = "task." + node if node in {"adopt", "applied"} else "task.action"
            with self.tracing.span(context, name, inputs={"checkpoint": node}) as span:
                span.outputs.update(summary)
        if node == "preview":
            with self.tracing.span(context, "task.checks") as span:
                span.outputs.update(summary)
        terminal = task["status"] in {
            "preview", "needs_clarification", "failed", "budget_exhausted", "paused", "cancelled", "adopted",
        }
        summary["root_complete"] = terminal
        completed_version = (context or {}).get("completed_state_version")
        if completed_version is not None and completed_version != task.get("stateVersion"):
            # 根保留本次生成的完成事实；后续采用等动作追加节点，避免云端乱序覆盖终态。
            return
        if terminal:
            self.tracing.finish_root(
                context, summary,
                error_code=(task.get("error") or {}).get("code") if task["status"] in {"failed", "budget_exhausted"} else None,
            )
        else:
            self.tracing.update_root(context, summary)

    async def initialize(self):
        if self._initialized:
            return self
        async with self._initializing:
            if self._initialized:
                return self
            await self.store.initialize()
            await self.recoverExpired()
            self._initialized = True
            if self.accountStore.dialect in ("postgres", "mysql"):

                async def recover():
                    while not self.closed:
                        await asyncio.sleep(max(0.25, self.leaseMs / 2000))
                        try:
                            await self.recoverExpired()
                        except Exception:
                            pass

                self.recoveryTimer = asyncio.create_task(recover())
        return self

    async def recoverExpired(self):
        if self.closed:
            return
        options = {
            "workerId": self.workerId,
            "now": self.now(),
            "graceMs": self.leaseMs,
        }
        for task in await self.store.interrupted(options):
            await self.store.recoverInterrupted(task, options)

    async def checkpoint(
        self, task, node, note="", requestId=None, raw=None, options=None
    ):
        lock = self.checkpointTails.setdefault(task["id"], asyncio.Lock())
        async with lock:
            if task.get("status") == "needs_clarification" and clean(task.get("question"), 300):
                # 聊天同步只读取持久assistant消息；同revision重复保存不重复插入问题。
                _ensure_messages(task)
                question = clean(task["question"], 300)
                if not any(e.get("role") == "assistant" and e.get("revision") == task["revision"] and e.get("content") == question for e in task["messages"]):
                    task["messages"] = [*task["messages"], _message("assistant", question, task["revision"])][-24:]
            expected = task.get("stateVersion", 0)
            task["updatedAt"] = max(self.now(), task.get("updatedAt", 0) + 1)
            task["stateVersion"] = expected + 1
            observation = task.get("_observation")
            if observation and task["status"] in {
                "preview", "needs_clarification", "failed", "budget_exhausted", "paused", "cancelled", "adopted",
            }:
                observation.setdefault("completed_state_version", task["stateVersion"])
            task["statusText"] = STATES.get(task["status"], task["status"])
            task["trace"] = [
                *task.get("trace", []),
                {
                    "node": node,
                    "status": task["status"],
                    "revision": task["revision"],
                    "at": task["updatedAt"],
                    "note": clean(note, 200),
                },
            ][-64:]
            write = {
                "expectedStateVersion": expected,
                "claim": self.taskClaims.get(id(task)),
                "now": self.now(),
                **(options or {}),
            }
            if requestId:
                # 动作类型来自已验证的调用路径，不从允许附加字段的原始请求推断。
                write["requestAction"] = "create" if node == "created" else node
                await self.store.commit(task, requestId, raw, write)
            else:
                await self.store.save(task, write)
            self.observeCheckpoint(task, node)
            if self.progressHub:
                try:
                    await maybe_await(self.progressHub.publish(task))
                except Exception:
                    pass

    async def snapshot(self, task):
        if not task:
            return None
        visible = clone(task)
        for key in (
            "userId",
            "branches",
            "phase",
            "versions",
            "instruction",
            "preference",
            "excludedIds",
            "toolHistory",
            "pendingRequest",
            "hasConfirmedBrief",
            "catalogContext",
            "routingContext",
            "_observation",
            "previousRequirementPlan",
        ):
            visible.pop(key, None)
        visible["toolHistory"] = []
        for entry in task.get("toolHistory", []):
            out = {
                k: clone(v) for k, v in entry.items() if k not in ("result", "input")
            }
            result = entry.get("result")
            inputs = entry.get("input") or {}
            out.update(
                queries=inputs.get("queries", []),
                queryCoverage=inputs.get("queryCoverage", []),
                resultSummary={
                    "tracks": len(result.get("tracks", [])),
                    "sources": [
                        {k: s.get(k) for k in ("id", "name", "checkedAt")}
                        for s in result.get("sources", [])
                    ],
                    "checkedAt": result.get("checkedAt"),
                    "error": result.get("error"),
                }
                if result
                else None,
            )
            visible["toolHistory"].append(out)
        visible["canUndo"] = bool(task["versions"])
        visible["progress"] = taskProgress(task)
        visible["recovery"] = await self.recovery(task)
        brief = task.get("brief")
        parts = []
        if brief:
            parts = [
                brief["scene"],
                *brief["styles"],
                f"约 {brief['targetMinutes']} 分钟" if brief["targetMinutes"] else "",
                f"目标 {brief['targetTrackCount']} 首" if brief.get("targetTrackCount") else "",
                f"{brief['artist']} 的近期作品" if brief["requireReleases"] else "",
                "排除现场版" if brief["excludeLive"] else "",
                ("仅限" if brief["artistOnly"] else "包含")
                + " "
                + brief["requiredArtist"]
                if brief["requiredArtist"]
                else "",
                "保留华语来源"
                if brief.get("catalogLanguage") == "zh"
                else "保留英文来源"
                if brief.get("catalogLanguage") == "en"
                else "",
                f"保留 {len(task['lockedIds'])} 首锁定歌曲"
                if task["lockedIds"]
                else "",
            ]
        visible["briefSummary"] = (
            " · ".join(p for p in parts if p) if brief else "正在理解要求"
        )
        return visible

    async def recovery(self, task):
        resumable = task["status"] in (
            "paused",
            "failed",
            "budget_exhausted",
            "waiting_tools",
        )
        result = {
            "canResume": resumable,
            "action": "resume" if resumable else "none",
            "reason": "",
            "message": "",
        }
        if resumable and task["phase"] == "done" and task.get("preview"):
            # 已完成的预览恢复不需要模型额度，也不重新查询或生成歌曲。
            return {**result, "message": "歌单已生成，继续会返回已有预览。"}
        if task["budget"]["totalCalls"] >= self.maxTaskCalls:
            return {
                "canResume": False,
                "action": "new_task",
                "reason": "task_limit",
                "message": "此任务的总调用次数已用完，继续不会重置上限。请新建任务；每日额度仍然有效。",
            }
        if task["budget"]["runCalls"] >= self.maxRunCalls:
            return {
                "canResume": False,
                "action": "new_turn",
                "reason": "run_limit",
                "message": "本轮调用次数已用完，直接继续不能执行。请修改要求后重新发送，新一轮仍受任务总额度和每日额度限制。",
            }
        if (
            task["phase"] in ("dispatch", "waiting")
            and task["budget"]["toolCalls"] >= task["budget"]["maxToolCalls"]
            and any(not b.get("result") for b in task["branches"].values())
        ):
            return {
                "canResume": False,
                "action": "new_turn",
                "reason": "tool_limit",
                "message": "本轮曲库查询次数已用完，请检查音乐服务并修改要求后重新发送。",
            }
        day = iso(self.now())[:10]
        used = await self.accountStore.getDailyUsage(task["userId"], day)
        global_used = await self.store.getGlobalDailyUsage(day)
        if used >= self.dailyLimit or global_used >= self.globalDailyLimit:
            return {
                "canResume": False,
                "action": "retry_later",
                "reason": "daily_limit"
                if used >= self.dailyLimit
                else "global_daily_limit",
                "message": "今日 AI 额度已用完，额度恢复后可继续，不会重置任务进度。"
                if used >= self.dailyLimit
                else "全站今日 AI 额度已用完，额度恢复后可继续。",
                "retryAt": int(date_ms(day + "T00:00:00.000Z")) + 86400000,
            }
        return result

    async def assertUserAvailable(self, userId, exceptTaskId=None):
        if await self.isUserRunning(userId, exceptTaskId):
            raise ApiError(
                429,
                "AI_BUSY",
                "请先完成或暂停当前音乐任务；同一账号同时只能执行一个任务。",
            )

    async def assertRecoverableBudget(self, task):
        self.modelPolicy(task)
        recovery = await self.recovery(task)
        if recovery["reason"]:
            raise ApiError(
                429,
                {
                    "daily_limit": "AI_DAILY_LIMIT",
                    "global_daily_limit": "AI_GLOBAL_DAILY_LIMIT",
                }.get(recovery["reason"], "TASK_BUDGET"),
                recovery["message"],
            )

    def modelPolicy(self, task):
        """旧夹具保留原契约；真实策略在新任务固定，恢复时不得随环境漂移。"""
        snapshotter = getattr(self.model, "policySnapshot", None)
        if not snapshotter:
            return None
        current = snapshotter()
        if current is None:
            return None
        if task.get("modelPolicy") is None:
            if task["budget"]["totalCalls"] and current["strategy"] != "flash_only":
                raise ApiError(
                    409, "MODEL_POLICY_MISSING",
                    "旧任务没有保存模型策略，不能自动迁移到多模型；请新建任务或恢复 Flash 单模型配置。",
                )
            task["modelPolicy"] = clone(current)
            if task["budget"]["totalCalls"]:
                task["modelPolicy"]["legacyFlash"] = True
        validator = getattr(self.model, "validatePolicy", None)
        if validator:
            validator(task["modelPolicy"])
        return task["modelPolicy"]

    async def owned(self, taskId, userId, deviceId=None):
        await self.initialize()
        task = await self.store.load(taskId, userId)
        if not task:
            raise ApiError(404, "TASK_NOT_FOUND", "音乐任务不存在。")
        if deviceId and task["deviceId"] != deviceId:
            raise ApiError(
                409,
                "TASK_DEVICE_MISMATCH",
                "请在发起任务的设备继续，网易云授权不会跨设备共享。",
            )
        return task

    async def get(self, taskId, userId):
        task = await self.owned(taskId, userId)
        if self.accountStore.dialect in ("postgres", "mysql") and task["status"] in (
            "planning",
            "researching",
            "composing",
        ):
            task = (
                await self.store.recoverInterrupted(
                    task,
                    {
                        "workerId": self.workerId,
                        "now": self.now(),
                        "graceMs": self.leaseMs,
                    },
                )
                or task
            )
        if task["status"] == "waiting_tools" and (
            not task["pendingSteps"]
            or any(s["expiresAt"] < self.now() for s in task["pendingSteps"])
        ):
            task.update(
                status="paused",
                pendingSteps=[],
                recoveryReason="本机查询已超时或执行被中断，已保留已完成结果，请主动恢复。",
            )
            await self.checkpoint(
                task, "tool_timeout", options={"invalidateLease": True}
            )
            self.abort(task["id"])
        return await self.snapshot(task)

    async def progress(self, taskId, userId):
        return taskProgress(await self.owned(taskId, userId))

    async def latest(self, userId, deviceId):
        await self.initialize()
        if not identifier(deviceId):
            raise ApiError(400, "INVALID_DEVICE", "设备标识无效。")
        task = await self.store.latest(userId, deviceId)
        return await self.get(task["id"], userId) if task else None

    async def create(self, userId, raw):
        await self.initialize()
        body = createInput(raw)
        replay = await self.store.replay(
            userId, body["deviceId"], body["requestId"], raw
        )
        if replay:
            return await self.snapshot(replay)
        if not self.model or not getattr(self.model, "configured", False):
            raise ApiError(503, "MODEL_NOT_CONFIGURED", "管理员尚未配置任务 AI 服务。")
        try:
            await self.assertUserAvailable(userId)
        except ApiError as error:
            if error.code == "AI_BUSY":
                replay = await self.store.replay(
                    userId, body["deviceId"], body["requestId"], raw
                )
                if replay:
                    return await self.snapshot(replay)
            raise
        if not await maybe_await(self.allowCall(userId)):
            raise ApiError(429, "AI_RATE_LIMITED", "任务请求过于频繁，请稍后再试。")
        routing = (
            await maybe_await(
                self.conversationContextProvider(
                    userId, body["deviceId"], body["requestId"], body["instruction"]
                )
            )
            if self.conversationContextProvider
            else None
        )
        observation = routing.pop("_observation", None) if routing else None
        time = self.now()
        task = {
            "id": _id(),
            "runId": _id(),
            "userId": userId,
            "deviceId": body["deviceId"],
            "revision": 1,
            "stateVersion": 0,
            "status": "planning",
            "phase": "plan",
            "instruction": body["instruction"],
            "preference": body["preference"],
            "excludedIds": body["excludedIds"],
            "catalogContext": body["catalogContext"],
            "brief": None,
            "lockedIds": [],
            "branches": {},
            "pendingSteps": [],
            "preview": None,
            "versions": [],
            "messages": [_message("user", body["instruction"], 1)],
            "trace": [],
            "createdAt": time,
            "updatedAt": time,
            "pendingRequest": None,
            "hasConfirmedBrief": False,
            "routingContext": routing,
            "budget": {
                "runCalls": 0,
                "maxRunCalls": self.maxRunCalls,
                "totalCalls": 0,
                "maxTaskCalls": self.maxTaskCalls,
                "toolCalls": 0,
                "maxToolCalls": 4,
                "inputTokens": 0,
                "outputTokens": 0,
            },
            "adoption": None,
            "question": "",
            "strategy": self.strategy,
            "toolHistory": [],
            "degradations": [],
        }
        if self.tracing.enabled:
            task["_observation"] = observation or self.tracing.new_context(
                "task:" + userId + ":" + body["deviceId"] + ":" + body["requestId"],
                metadata={"task_id": task["id"], "run_id": task["runId"]}, start_ms=time,
            )
            self.tracing.begin_root(task["_observation"])
        self.modelPolicy(task)
        try:
            await self.checkpoint(task, "created", requestId=body["requestId"], raw=raw)
        except ApiError as error:
            if error.code == "IDEMPOTENT_REPLAY":
                return await self.snapshot(
                    await self.store.replay(
                        userId, body["deviceId"], body["requestId"], raw
                    )
                )
            raise
        self.schedule(task)
        return await self.snapshot(task)

    async def action(self, taskId, userId, raw):
        body = actionInput(raw)
        for attempt in range(5):
            try:
                return await self.applyAction(taskId, userId, raw)
            except ApiError as error:
                if (
                    error.code != "TASK_WRITE_CONFLICT"
                    or body["action"] not in ("pause", "cancel")
                    or attempt == 4
                ):
                    raise

    async def applyAction(self, taskId, userId, raw):
        await self.initialize()
        body = actionInput(raw)
        action = body["action"]
        replay = await self.store.replay(
            userId, body["deviceId"], body["requestId"], raw
        )
        if replay:
            if replay["id"] != taskId:
                raise ApiError(409, "IDEMPOTENCY_CONFLICT", "操作标识已用于另一任务。")
            return await self.snapshot(replay)
        task = await self.owned(taskId, userId, body["deviceId"])
        await self.store.assertConversationActive(self.store.db, task, body["requestId"], action)
        if task["revision"] != body["expectedRevision"]:
            replay = await self.store.replay(
                userId, body["deviceId"], body["requestId"], raw
            )
            if replay:
                if replay["id"] != taskId:
                    raise ApiError(
                        409, "IDEMPOTENCY_CONFLICT", "操作标识已用于另一任务。"
                    )
                return await self.snapshot(replay)
            raise ApiError(409, "TASK_REVISION_CONFLICT", "任务已更新，请刷新后重试。")
        previous = task["status"]
        _ensure_messages(task)
        task["hasConfirmedBrief"] = _confirmed(task)
        finished_resume = action == "resume" and task["phase"] == "done" and bool(task.get("preview"))
        if action in ("turn", "resume", "select_artist") and not finished_resume:
            self.modelPolicy(task)
            await self.assertUserAvailable(userId, taskId)
        if action == "resume":
            if task["status"] not in (
                "paused",
                "failed",
                "budget_exhausted",
                "waiting_tools",
            ):
                raise ApiError(409, "TASK_NOT_RESUMABLE", "此任务不需要恢复。")
            if task["phase"] == "done" and not task.get("preview"):
                raise ApiError(409, "TASK_NOT_RESUMABLE", "此任务没有可恢复的歌单预览。")
            if not finished_resume:
                await self.assertRecoverableBudget(task)
            if task["phase"] == "waiting" and any(
                b.get("error")
                and b["attempt"] >= TASK_SKILLS[b["skill"]]["maximumAttempts"] - 1
                for b in task["branches"].values()
            ):
                raise ApiError(
                    409,
                    "TOOL_RETRY_LIMIT",
                    "本轮曲库查询已达到重试上限，请检查服务后重新提交要求。",
                )
        if action == "turn" and task["budget"]["totalCalls"] >= self.maxTaskCalls:
            raise ApiError(
                429,
                "TASK_BUDGET",
                "此任务的总调用次数已用完，请新建任务；每日额度不会重置。",
            )
        if action in ("turn", "resume") and not finished_resume and not await maybe_await(
            self.allowCall(userId)
        ):
            raise ApiError(429, "AI_RATE_LIMITED", "任务请求过于频繁，请稍后再试。")
        routing = (
            await maybe_await(
                self.conversationContextProvider(
                    userId, body["deviceId"], body["requestId"], body["instruction"]
                )
            )
            if action == "turn" and self.conversationContextProvider
            else None
        )
        observation = routing.pop("_observation", None) if routing else None
        if self.tracing.enabled and action == "turn":
            task["_observation"] = observation or self.tracing.new_context(
                "task:" + userId + ":" + body["deviceId"] + ":" + body["requestId"],
                metadata={"task_id": task["id"]}, start_ms=self.now(),
            )
            self.tracing.begin_root(task["_observation"])
        if action == "applied":
            if (task.get("adoption") or {}).get("id") != body["adoptionId"] or task[
                "status"
            ] not in ("awaiting_apply", "adopted"):
                raise ApiError(409, "ADOPTION_CONFLICT", "采用操作已失效，请重新预览。")
            task["status"] = "adopted"
            task["adoption"]["appliedAt"] = self.now()
        elif action == "adopt":
            if not (task.get("preview") or {}).get("tracks") or task["status"] not in (
                "preview",
                "awaiting_apply",
                "adopted",
            ):
                raise ApiError(409, "NO_TASK_PREVIEW", "请先完成歌单预览。")
            if not task.get("adoption"):
                task["adoption"] = {
                    "id": _id(),
                    "previewVersion": task["preview"]["version"],
                }
            if task["status"] != "adopted":
                task["status"] = "awaiting_apply"
        elif action in ("pause", "cancel"):
            self.abort(taskId)
            task["revision"] += 1
            task.update(
                status="paused" if action == "pause" else "cancelled", pendingSteps=[]
            )
            if action == "cancel":
                task["pendingRequest"] = None
        elif action == "lock":
            if not task.get("preview"):
                raise ApiError(409, "NO_TASK_PREVIEW", "请先生成歌单。")
            if not all(
                i in {t["id"] for t in task["preview"]["tracks"]}
                for i in body["lockedIds"]
            ):
                raise ApiError(400, "INVALID_LOCK", "只能锁定当前歌单歌曲。")
            self.abort(taskId)
            task["revision"] += 1
            task["lockedIds"] = list(body["lockedIds"])
            refreshPreviewLocksCheck(task)
            task.update(status="preview", pendingSteps=[], adoption=None)
        elif action == "undo":
            if not task["versions"]:
                raise ApiError(409, "NO_TASK_UNDO", "没有可撤回的歌单修改。")
            self.abort(taskId)
            old = task["versions"].pop()
            task["revision"] += 1
            task.update(
                preview=old["preview"],
                brief=old["brief"],
                lockedIds=old["lockedIds"],
                status="preview",
                pendingSteps=[],
                adoption=None,
                question="",
                pendingRequest=None,
                hasConfirmedBrief=True,
            )
            if "requirementPlan" in old:
                task["requirementPlan"] = clone(old["requirementPlan"])
                task["branches"] = clone(old.get("branches") or {})
            else:
                task.pop("requirementPlan", None)
            refreshPreviewLocksCheck(task)
        elif action == "select_artist":
            selected = next(
                (
                    a
                    for a in task.get("providedArtists", [])
                    if a["id"] == body["artistId"]
                ),
                None,
            )
            branch = next(
                (
                    b
                    for b in task["branches"].values()
                    if b["skill"] in ("releases.research", "scene.research")
                    and selected
                    and any(a["id"] == selected["id"] and a["name"] == selected["name"] for a in (b.get("result") or {}).get("artists", []))
                ),
                None,
            )
            if (
                task["status"] != "needs_clarification"
                or not ((task.get("brief") or {}).get("requireReleases") or (task.get("brief") or {}).get("requiredArtist"))
                or not selected
                or not branch
                or not any(
                    a["id"] == selected["id"] and a["name"] == selected["name"]
                    for a in (branch.get("result") or {}).get("artists", [])
                )
            ):
                raise ApiError(
                    409,
                    "INVALID_ARTIST_SELECTION",
                    "请从当前曲库返回的歌手选项中选择。",
                )
            if task["budget"]["toolCalls"] >= task["budget"]["maxToolCalls"]:
                raise ApiError(
                    429, "TASK_BUDGET", "达到本轮曲库查询预算，请修改任务后继续。"
                )
            self.abort(taskId)
            task["revision"] += 1
            task["verifiedArtist"] = {
                "id": selected["id"],
                "name": selected["name"],
                "requestedName": branch["plan"].get("artist") or task["brief"].get("requiredArtist", ""),
                "checkedAt": (branch.get("result") or {}).get("checkedAt")
                or iso(self.now()),
                "source": "user_catalog_choice",
            }
            if branch["skill"] == "releases.research":
                if task["brief"]["requiredArtist"] == branch["plan"]["artist"]:
                    task["brief"]["requiredArtist"] = selected["name"]
                task["brief"].update(artist=selected["name"], artistId=selected["id"])
                branch["plan"].update(artist=selected["name"], artistId=selected["id"])
            else:
                task["brief"].update(requiredArtist=selected["name"], requiredArtistId=selected["id"])
                branch["plan"].update(requiredArtistId=selected["id"])
            branch.update(
                evidenceGeneration=_id(),
                result=None,
                error=None,
                resultHash=None,
                step=None,
            )
            task.update(
                question="",
                providedArtists=[],
                pendingSteps=[],
                phase="dispatch",
                status="planning",
            )
            task["messages"] = [
                *task["messages"],
                _message(
                    "user", f"选择曲库歌手：{selected['name']}。", task["revision"]
                ),
            ][-24:]
        elif action == "turn":
            next_task = {
                **task,
                "revision": task["revision"] + 1,
                "instruction": body["instruction"],
                "routingContext": routing,
            }
            if previous == "cancelled" and not next_task["hasConfirmedBrief"]:
                next_task.update(messages=[], brief=None, pendingRequest=None)
            reply = _message("user", body["instruction"], next_task["revision"])
            next_task["messages"] = [*next_task["messages"], reply][-24:]
            if (
                not next_task["hasConfirmedBrief"]
                and (next_task.get("brief") or {}).get("kind") == "clarify"
                and not next_task.get("pendingRequest")
            ):
                next_task["pendingRequest"] = _pending_from_messages(
                    next_task, next_task["brief"].get("question") or "请补充必要信息。"
                )
            elif next_task.get("pendingRequest"):
                next_task["pendingRequest"] = {
                    **next_task["pendingRequest"],
                    "revision": next_task["revision"],
                    "sources": [*next_task["pendingRequest"]["sources"], dict(reply)],
                }
            if next_task.get("pendingRequest"):
                next_task["pendingRequest"] = validatePendingRequest(
                    next_task["pendingRequest"], _pending_context(next_task)
                )
            self.abort(taskId)
            task.update(next_task)
            if "preference" in body:
                task["preference"] = clean(body["preference"], 500)
            task.update(
                status="planning",
                phase="plan",
                pendingSteps=[],
                question="",
                adoption=None,
                error=None,
                degradations=[],
            )
            task.pop("observation", None)
            task["runId"] = _id()
            task["budget"].update(runCalls=0, toolCalls=0)
            task["previousBranches"] = clone(task["branches"])
            task["previousBrief"] = clone(task["brief"])
            if task.get("requirementPlan"):
                task["previousRequirementPlan"] = clone(task["requirementPlan"])
            task["branches"] = {}
        elif action == "resume":
            self.abort(taskId)
            task["revision"] += 1
            if task["phase"] == "waiting":
                for branch in task["branches"].values():
                    if branch.get("error"):
                        branch["attempt"] += 1
                        branch.update(error=None, resultHash=None)
                    if (
                        branch.get("result")
                        and self.now() - date_ms(branch["result"]["checkedAt"])
                        > 86400000
                    ):
                        branch.update(result=None, resultHash=None)
                    if not branch.get("result") and not branch.get("error"):
                        branch["step"] = None
                task["phase"] = "dispatch"
            task.update(
                status="preview" if finished_resume else "composing" if task["phase"] == "compose" else "planning",
                recoveryReason="",
                error=None,
            )
        task["catalogContext"] = body.get("catalogContext", "")
        if task.get("pendingRequest"):
            task["pendingRequest"] = {
                **task["pendingRequest"],
                "revision": task["revision"],
            }
        if self.tracing.enabled and action in {"resume", "select_artist"} and not finished_resume:
            previous_context = task.get("_observation") or {}
            previous_metadata = previous_context.get("metadata") or {}
            task["_observation"] = self.tracing.new_context(
                "task-action:" + userId + ":" + taskId + ":" + body["requestId"],
                metadata={
                    "task_id": task["id"], "run_id": task["runId"],
                    "previous_trace_id": previous_context.get("trace_id"),
                    "thread_id": previous_metadata.get("thread_id"),
                    "conversation_id": previous_metadata.get("conversation_id"),
                }, start_ms=self.now(),
            )
            self.tracing.begin_root(task["_observation"])
        try:
            await self.checkpoint(
                task,
                action,
                requestId=body["requestId"],
                raw=raw,
                options={
                    "invalidateLease": task["revision"] != body["expectedRevision"]
                },
            )
        except ApiError as error:
            if error.code == "IDEMPOTENT_REPLAY":
                return await self.snapshot(
                    await self.store.replay(
                        userId, body["deviceId"], body["requestId"], raw
                    )
                )
            raise
        if action in ("turn", "resume", "select_artist") and not finished_resume:
            self.schedule(task)
        return await self.snapshot(task)

    async def result(self, taskId, stepId, userId, body):
        for attempt in range(5):
            try:
                return await self.acceptResult(taskId, stepId, userId, body)
            except ApiError as error:
                if error.code != "TASK_WRITE_CONFLICT" or attempt == 4:
                    raise

    async def acceptResult(self, taskId, stepId, userId, body):
        await self.initialize()
        parsed = toolResult(body, self.now())
        task = await self.owned(taskId, userId, body["deviceId"])
        branch = next(
            (
                b
                for b in task["branches"].values()
                if (b.get("step") or {}).get("id") == stepId
            ),
            None,
        )
        if (
            not branch
            or branch["step"]["leaseId"] != body["leaseId"]
            or body["revision"] != task["revision"]
            or branch["step"]["revision"] != body["revision"]
        ):
            raise ApiError(409, "STALE_TASK_STEP", "此查询已经失效，请获取最新任务。")
        if "tracks" in parsed:
            styles = (
                adjustmentStyles(task["brief"], task.get("previousBrief"))
                or task["brief"]["styles"]
            )
            parsed["tracks"] = [
                {
                    **t,
                    "styleEvidence": playlistStyleEvidence(
                        t, parsed["sources"], styles
                    ),
                }
                for t in parsed["tracks"]
            ]
        hashed = digest(parsed)
        if branch.get("resultHash"):
            if branch["resultHash"] != hashed:
                raise ApiError(409, "STEP_RESULT_CONFLICT", "此步骤已提交不同结果。")
            return await self.snapshot(task)
        if task["status"] != "waiting_tools":
            raise ApiError(409, "TASK_NOT_WAITING", "任务当前不接收查询结果。")
        if branch["step"]["expiresAt"] < self.now():
            raise ApiError(409, "TOOL_LEASE_EXPIRED", "本机查询已超时，请恢复任务。")
        if (
            branch["skill"] == "releases.research"
            and parsed.get("resolvedArtist")
            and branch["plan"].get("artistId")
            and branch["plan"]["artistId"] != parsed["resolvedArtist"]["id"]
        ):
            raise ApiError(
                409, "ARTIST_ID_MISMATCH", "曲库返回了不同歌手，请重新选择。"
            )
        if (
            branch["skill"] == "releases.research"
            and parsed.get("tracks")
            and not parsed.get("resolvedArtist")
        ):
            raise ApiError(
                422, "RELEASE_ARTIST_UNVERIFIED", "新作候选缺少已核验的歌手身份，请重新查询。"
            )
        _scene_artist_identity(task, branch, parsed)
        branch.update(
            resultHash=hashed,
            result=None if parsed.get("error") else parsed,
            error=parsed.get("error"),
        )
        if branch["skill"] == "releases.research" and parsed.get("resolvedArtist"):
            resolved = parsed["resolvedArtist"]
            # 别名只有经曲库身份核验后才能换成规范名，不能对任意歌手模糊放宽约束。
            if task["brief"]["requiredArtist"] == branch["plan"]["artist"]:
                task["brief"]["requiredArtist"] = resolved["name"]
            task["brief"].update(artist=resolved["name"], artistId=resolved["id"])
            task["verifiedArtist"] = {
                **resolved,
                "requestedName": branch["plan"]["artist"],
                "checkedAt": parsed["checkedAt"],
            }
        if branch.get("result") and task["brief"]["artistOnly"]:
            branch["result"]["tracks"] = [
                t
                for t in branch["result"]["tracks"]
                if artistCreditMatches(t, task["brief"]["requiredArtist"], task["brief"].get("requiredArtistId", 0))
            ]
        evidence = {
            "status": "failed" if parsed.get("error") else "completed",
            "completedAt": self.now(),
            "resultHash": hashed,
            "result": clone(branch.get("result") or parsed),
        }
        for history in (task.get("toolHistory", []), branch.get("attemptHistory", [])):
            entry = next(
                (
                    e
                    for e in history
                    if e["stepId"] == stepId and e["revision"] == body["revision"]
                ),
                None,
            )
            if entry is not None:
                entry.update(evidence)
        entries = self.sceneHistory(task, branch)
        branch["result"] = mergeToolEvidence(
            entries, TASK_SKILLS[branch["skill"]]["maximumTracks"]
        ) or branch.get("result")
        if parsed.get("error") and branch.get("result"):
            await self.degrade(
                task,
                "catalog_supplement_failed",
                parsed["error"]["code"],
                "补充曲库查询失败，已保留先前核验的候选；缺失条件仍会显示未满足或待核验。",
            )
        branch["step"]["completedAt"] = self.now()
        task["pendingSteps"] = [s for s in task["pendingSteps"] if s["id"] != stepId]
        if parsed.get("question"):
            task.update(
                question=parsed["question"], providedArtists=parsed.get("artists", [])
            )
        handoff = not task["pendingSteps"]
        if handoff:
            task.update(phase="observe", status="researching")
        await self.checkpoint(
            task,
            "tool_result",
            "查询失败，进度已保存。"
            if parsed.get("error")
            else f"收到 {len(parsed['tracks'])} 首真实候选。",
            options={"invalidateLease": handoff},
        )
        history = next(
            (entry for entry in task.get("toolHistory", []) if entry["stepId"] == stepId), {}
        )
        # 这里观测的是设备派发至验收回执的区间，不声称看到了设备内部各次网易云请求。
        with self.tracing.span(
            task.get("_observation"), "task.tool", run_type="tool",
            start_ms=history.get("dispatchedAt"),
            metadata={"step_id": stepId, "skill": branch["skill"], "revision": task["revision"]},
            inputs={"query_count": len((branch["step"].get("input") or {}).get("queries", []))},
        ) as span:
            span.outputs.update(
                status="failed" if parsed.get("error") else "completed",
                track_count=len(parsed.get("tracks", [])), source_count=len(parsed.get("sources", [])),
            )
            if parsed.get("error"):
                span.error_code = parsed["error"].get("code")
        if body.get("catalogContext") and body["catalogContext"] == task.get(
            "catalogContext"
        ):
            await self.toolCache.put(
                {**self.toolCacheScope(task, branch), "result": parsed}
            )
        if handoff:
            self.abort(task["id"])
            self.schedule(task)
        return await self.snapshot(task)

    def abort(self, taskId):
        job = self.jobs.pop(taskId, None)
        if job:
            job["controller"].set()

    async def current(self, task, revision):
        if self.closed:
            return False
        stored = await self.store.load(task["id"], task["userId"])
        if (
            not stored
            or stored["revision"] != revision
            or stored["status"] in ("paused", "cancelled", "failed", "budget_exhausted")
        ):
            return False
        claim = self.taskClaims.get(id(task))
        return not claim or await self.store.isTaskClaimCurrent(
            task["id"], claim, self.now()
        )

    async def assertCurrent(self, task, revision):
        if not await self.current(task, revision):
            raise ApiError(499, "TASK_OBSOLETE", "任务已取消或更新。")

    async def isUserRunning(self, userId, exceptTaskId=None):
        await self.initialize()
        if await maybe_await(self.legacyBusy(userId)):
            return True
        if any(e["claim"]["userId"] == userId for e in self.legacyClaims.values()):
            return True
        if self.accountStore.dialect not in ("postgres", "mysql") and any(
            i != exceptTaskId and job["userId"] == userId
            for i, job in self.jobs.items()
        ):
            return True
        queued = self.accountCalls.get(userId)
        if queued and queued["taskId"] != exceptTaskId:
            return True
        return await self.store.hasActiveTask(userId, exceptTaskId)

    async def acquireLegacy(self, userId):
        await self.initialize()
        await self.assertUserAvailable(userId)
        if (
            any(e["claim"]["userId"] == userId for e in self.legacyClaims.values())
            or self.inflight + len(self.legacyClaims) >= self.globalConcurrency
        ):
            raise ApiError(429, "AI_BUSY", "AI 繁忙，请稍后重试。")
        token = _id()
        self.legacyClaims[token] = {"claim": {"userId": userId}, "pending": True}
        try:
            claim = await self.store.acquireLegacy(
                userId,
                {
                    "workerId": self.workerId,
                    "now": self.now(),
                    "leaseMs": self.leaseMs,
                    "globalConcurrency": self.globalConcurrency,
                },
            )
        except BaseException:
            self.legacyClaims.pop(token, None)
            raise
        if not claim:
            claim = {"userId": userId, "token": token, "local": True}
        self.legacyClaims.pop(token, None)
        entry = {"claim": claim}
        if not claim.get("local"):
            controller = asyncio.Event()
            claim["signal"] = controller

            async def heartbeat():
                while not controller.is_set():
                    await asyncio.sleep(max(0.1, self.leaseMs / 3000))
                    try:
                        if not await self.store.heartbeatLegacy(
                            claim, {"now": self.now(), "leaseMs": self.leaseMs}
                        ):
                            controller.set()
                    except Exception:
                        controller.set()

            entry.update(controller=controller, timer=asyncio.create_task(heartbeat()))
        self.legacyClaims[claim["token"]] = entry
        return claim

    async def releaseLegacy(self, claim):
        if not claim:
            return
        entry = self.legacyClaims.pop(claim["token"], None)
        if entry and entry.get("timer"):
            entry["timer"].cancel()
        if not claim.get("local"):
            await self.store.releaseLegacy(claim)

    async def assertLegacyCurrent(self, claim):
        if not claim:
            return
        if claim.get("local"):
            if claim["token"] not in self.legacyClaims:
                raise ApiError(409, "TASK_LEASE_LOST", "本次处理已失效，请重新提交。")
            return
        if (
            claim.get("signal")
            and claim["signal"].is_set()
            or not await self.store.heartbeatLegacy(
                claim, {"now": self.now(), "leaseMs": self.leaseMs}
            )
        ):
            raise ApiError(
                409, "TASK_LEASE_LOST", "本次处理已失去执行租约，请重新提交。"
            )

    async def reserveLegacyUsage(self, userId, claim):
        await self.initialize()
        await self.store.reserveLegacyUsage(
            userId,
            self.now(),
            self.accountStore,
            self.dailyLimit,
            self.globalDailyLimit,
            {"claim": None if claim and claim.get("local") else claim},
        )

    def schedule(self, task):
        if self.closed or task["id"] in self.jobs:
            return
        controller = asyncio.Event()
        job = {
            "controller": controller,
            "userId": task["userId"],
            "revision": task["revision"],
            "claim": None,
        }
        self.jobs[task["id"]] = job

        async def run():
            current_task = task
            heartbeat = None
            try:
                await asyncio.sleep(0)
                if self.jobs.get(task["id"]) is not job or self.closed:
                    return
                await self.initialize()
                job["claim"] = await self.store.claimTask(
                    task["id"],
                    task["userId"],
                    {
                        "workerId": self.workerId,
                        "now": self.now(),
                        "leaseMs": self.leaseMs,
                    },
                )
                current_task = await self.store.load(task["id"], task["userId"])
                if not current_task or current_task["revision"] != job["revision"]:
                    return
                if job["claim"]:
                    self.taskClaims[id(current_task)] = job["claim"]

                    async def renew():
                        while not controller.is_set():
                            await asyncio.sleep(max(0.1, self.leaseMs / 3000))
                            try:
                                if not await self.store.heartbeatTask(
                                    task["id"],
                                    job["claim"],
                                    {"now": self.now(), "leaseMs": self.leaseMs},
                                ):
                                    controller.set()
                            except Exception:
                                controller.set()

                    heartbeat = asyncio.create_task(renew())
                # 图的原始 state 含用户正文和曲库数据；只手动发送每个节点的摘要。
                with disable_automatic_tracing(), self.tracing.span(
                    current_task.get("_observation"), "task.run",
                    metadata=self.observationState(current_task),
                ) as span:
                    await self.graph.ainvoke(
                        {"task": current_task, "revision": current_task["revision"]},
                        {"recursion_limit": 12},
                    )
                    span.outputs.update(self.observationState(current_task))
            except Exception as error:
                try:
                    if (
                        current_task
                        and await self.current(current_task, job["revision"])
                        and _code(error)
                        not in (
                            "TASK_CLAIMED",
                            "TASK_WRITE_CONFLICT",
                            "TASK_LEASE_LOST",
                        )
                    ):
                        status = (
                            "budget_exhausted"
                            if isinstance(error, ApiError)
                            and error.code
                            in (
                                "AI_DAILY_LIMIT",
                                "AI_GLOBAL_DAILY_LIMIT",
                                "TASK_BUDGET",
                            )
                            else "failed"
                        )
                        current_task.update(
                            status=status,
                            pendingSteps=[],
                            error={
                                "code": error.code
                                if isinstance(error, ApiError)
                                else "TASK_FAILED",
                                "message": error.message
                                if isinstance(error, ApiError)
                                else "任务执行未完成，已保留进度。",
                            },
                        )
                        await self.checkpoint(
                            current_task, "failed", current_task["error"]["message"]
                        )
                except Exception:
                    pass
            finally:
                if heartbeat:
                    heartbeat.cancel()
                if job["claim"]:
                    try:
                        await self.store.releaseTask(task["id"], job["claim"])
                    except Exception:
                        pass
                if self.jobs.get(task["id"]) is job:
                    self.jobs.pop(task["id"], None)
                if current_task:
                    self.taskClaims.pop(id(current_task), None)

        work = asyncio.create_task(run())
        job["done"] = work
        self.runningJobs.add(work)
        work.add_done_callback(self.runningJobs.discard)

    async def call(self, task, revision, node, method, inputs):
        preceding = (self.accountCalls.get(task["userId"]) or {}).get("tail")
        tail = asyncio.get_running_loop().create_future()
        entry = {"taskId": task["id"], "tail": tail}
        self.accountCalls[task["userId"]] = entry
        try:
            if preceding:
                await asyncio.shield(preceding)
            return await self.performCall(task, revision, node, method, inputs)
        finally:
            if not tail.done():
                tail.set_result(None)
            if self.accountCalls.get(task["userId"]) is entry:
                self.accountCalls.pop(task["userId"], None)

    async def performCall(self, task, revision, node, method, inputs):
        await self.assertCurrent(task, revision)
        await self.assertUserAvailable(task["userId"], task["id"])
        await self.assertRecoverableBudget(task)
        selection = None
        selector = getattr(self.model, "selectCall", None)
        if selector:
            # 选择只读可信状态；不把路由信心或模型自报难度作为选择依据。
            routingInputs = dict(inputs)
            if task.get("routingContext"):
                routingInputs["conversationContext"] = task["routingContext"]
            selection = await maybe_await(
                selector(method, routingInputs, task.get("modelPolicy"))
            )
        if (
            await maybe_await(self.legacyBusy(task["userId"]))
            or self.inflight + self.legacyCount() >= self.globalConcurrency
        ):
            raise ApiError(429, "AI_BUSY", "AI 繁忙，请稍后恢复任务。")
        task["budget"]["runCalls"] += 1
        task["budget"]["totalCalls"] += 1
        try:
            while True:
                try:
                    callId = await self.store.reserveCall(
                        task,
                        node,
                        self.now(),
                        self.accountStore,
                        self.dailyLimit,
                        self.globalDailyLimit,
                        {
                            "claim": self.taskClaims.get(id(task)),
                            "globalConcurrency": self.globalConcurrency,
                            "leaseMs": self.leaseMs,
                            **({"modelSelection": selection} if selection else {}),
                        },
                    )
                    break
                except ApiError as error:
                    signal = (self.jobs.get(task["id"]) or {}).get("controller")
                    if (
                        error.code != "AI_BUSY"
                        or not self.beforeModelCall
                        or not await maybe_await(
                            self.beforeModelCall(task["userId"], signal)
                        )
                    ):
                        raise
                    await self.assertCurrent(task, revision)
        except BaseException:
            task["budget"]["runCalls"] -= 1
            task["budget"]["totalCalls"] -= 1
            raise
        self.inflight += 1
        signal = (self.jobs.get(task["id"]) or {}).get("controller")
        try:
            await self.checkpoint(task, node, "已预留一次模型调用。")
            with self.tracing.span(
                task.get("_observation"), "task.model_call",
                metadata={"call_id": callId, "purpose": method, **self.observationState(task)},
            ) as span:
                response = await self.model.call(
                    method, inputs,
                    {"signal": signal, **({"modelSelection": selection} if selection else {})},
                )
                usage = response.get("usage")
                if usage:
                    span.outputs.update(inputTokens=usage.get("inputTokens"), outputTokens=usage.get("outputTokens"))
            await self.store.finishCall(
                callId,
                "completed",
                self.now(),
                response.get("usage"),
                None,
                response.get("diagnostics"),
                **({"modelMetadata": response["modelMetadata"]} if response.get("modelMetadata") else {}),
            )
            await self.assertCurrent(task, revision)
            if response.get("usage"):
                task["budget"]["inputTokens"] += response["usage"].get("inputTokens", 0)
                task["budget"]["outputTokens"] += response["usage"].get(
                    "outputTokens", 0
                )
            return response.get("value", response)
        except Exception as error:
            usage = getattr(error, "taskUsage", None)
            await self.store.finishCall(
                callId,
                "uncertain" if _code(error) in ("REQUEST_CANCELLED", "MODEL_TIMEOUT") else "failed",
                self.now(),
                usage,
                error.code if isinstance(error, ApiError) else "MODEL_FAILED",
                getattr(error, "taskDiagnostic", None),
                **({"modelMetadata": error.taskModelMetadata} if getattr(error, "taskModelMetadata", None) else {}),
            )
            if usage:
                task["budget"]["inputTokens"] += usage.get("inputTokens", 0)
                task["budget"]["outputTokens"] += usage.get("outputTokens", 0)
            raise
        finally:
            self.inflight -= 1

    async def understand(self, task, revision):
        task["status"] = "planning"
        await self.checkpoint(task, "understand")
        _ensure_messages(task)
        task["hasConfirmedBrief"] = _confirmed(task)
        if (
            not task["hasConfirmedBrief"]
            and (task.get("brief") or {}).get("kind") == "clarify"
            and not task.get("pendingRequest")
        ):
            task["pendingRequest"] = _pending_from_messages(
                task, task["brief"].get("question") or "请补充必要信息。"
            )
        if task.get("pendingRequest"):
            task["pendingRequest"] = validatePendingRequest(
                task["pendingRequest"], _pending_context(task)
            )
            if len(task["pendingRequest"]["sources"]) == 1:
                task.update(
                    status="needs_clarification",
                    question=task["pendingRequest"]["question"],
                )
                await self.checkpoint(
                    task, "clarify_waiting", "等待补充信息，未重复调用模型。"
                )
                return
        old = task.get("brief")
        inputs = {
            "instruction": task["instruction"],
            "currentBrief": task.get("brief"),
            "preference": task["preference"],
            "history": [
                {k: e[k] for k in ("role", "content")} for e in task["messages"][-8:]
            ],
            "preview": [
                {k: t.get(k) for k in ("id", "title", "artist")}
                for t in (task.get("preview") or {}).get("tracks", [])
            ],
            "lockedIds": task["lockedIds"],
            "providedArtists": task.get("providedArtists", []),
            "taskId": task["id"],
            "revision": task["revision"],
            "messages": task["messages"],
            "pendingRequest": task.get("pendingRequest"),
            "hasConfirmedBrief": task["hasConfirmedBrief"],
        }
        if task.get("routingContext"):
            inputs["conversationContext"] = task["routingContext"]
        try:
            compiled = compileMusicBrief(inputs)
            if compiled:
                brief, requirement = compiled
                await self.assertCurrent(task, revision)
                previous = task.get("requirementPlan") or {}
                updated_fields = {source["field"] for source in requirement["sources"]}
                task["requirementPlan"] = {**previous, **requirement,
                    "sources": [*(source for source in previous.get("sources", []) if old and source["field"] not in updated_fields), *requirement["sources"]][-32:]}
                await self.checkpoint(task, "understand_compiled", "当前原文条件完整编译，未发起规划模型请求。")
            else:
                brief = await self.call(task, revision, "understand", "plan", inputs)
        except ApiError as error:
            if error.code != "INVALID_TASK_MODEL":
                raise
            await self.assertCurrent(task, revision)
            await self.checkpoint(
                task,
                "understand_invalid",
                "AI 意图或JSON不符合契约，将在原预算内最多纠正一次。",
            )
            brief = await self.call(
                task,
                revision,
                "understand_correction",
                "plan",
                {
                    **inputs,
                    "validationFeedback": {
                        "invalidReason": clean(error.message, 500),
                        "previousProposal": utf16_slice(error.taskProposal, 8000)
                        if isinstance(getattr(error, "taskProposal", None), str)
                        else None,
                        "instruction": "只纠正上次提案的字段与语义；保持用户约束，不猜测歌手ID。此次纠正计入同一任务预算。",
                    },
                },
            )
        proposed = next(
            (
                a
                for a in task.get("providedArtists", [])
                if a["id"] == brief.get("artistId") and a["name"] == brief["artist"]
            ),
            None,
        )
        if not proposed and not (
            old
            and old.get("artist") == brief.get("artist")
            and old.get("artistId") == brief.get("artistId")
        ):
            brief.pop("artistId", None)
        verified = task.get("verifiedArtist")
        if verified and brief["artist"] in (
            verified["name"],
            verified["requestedName"],
        ):
            brief["artistId"] = verified["id"]
        if task.get("pendingRequest"):
            language = "unknown"
            for source in task["pendingRequest"]["sources"]:
                language = catalogLanguageForTurn(source["content"], language)
            brief["catalogLanguage"] = language
        else:
            brief["catalogLanguage"] = catalogLanguageForTurn(task["instruction"], old)
        task["brief"] = brief
        if brief["kind"] == "clarify":
            task.update(
                status="needs_clarification",
                question=brief["question"] or "请补充要查询的场景或歌手。",
            )
            if not task["hasConfirmedBrief"]:
                task["pendingRequest"] = (
                    {**task["pendingRequest"], "question": task["question"]}
                    if task.get("pendingRequest")
                    else _pending_from_messages(task, task["question"])
                )
            task["messages"].append(
                _message("assistant", task["question"], task["revision"])
            )
            await self.checkpoint(task, "clarify")
            return
        _refresh_requirement_plan(task, old)
        task.update(pendingRequest=None, hasConfirmedBrief=True)
        retained_ids = list(task["lockedIds"])
        if brief["retainFirst"]:
            if not task.get("preview") or brief["retainFirst"] > len(
                task["preview"]["tracks"]
            ):
                task.update(
                    status="needs_clarification",
                    question="当前歌单没有这么多首歌曲，请确认要保留的范围。",
                )
                await self.checkpoint(task, "clarify")
                return
            retained_ids = list(
                dict.fromkeys(
                    [
                        *task["lockedIds"],
                        *[
                            t["id"]
                            for t in task["preview"]["tracks"][: brief["retainFirst"]]
                        ],
                    ]
                )
            )
        target_count = brief.get("targetTrackCount", 0)
        if target_count and len(retained_ids) > target_count:
            task.update(
                status="needs_clarification",
                question=f"目标为 {target_count} 首，但需要保留 {len(retained_ids)} 首锁定歌曲。请先解除部分锁定，或增加目标曲数；未删除任何锁定歌曲。",
            )
            await self.checkpoint(task, "track_count_lock_conflict")
            return
        task["lockedIds"] = retained_ids
        task["currentAvoidIds"] = (
            [
                t["id"]
                for t in (task.get("preview") or {}).get("tracks", [])
                if t["id"] not in task["lockedIds"]
            ]
            if brief["avoidPrevious"]
            else []
        )
        sources = [
            s
            for b in task.get("previousBranches", {}).values()
            for s in (b.get("result") or {}).get("sources", [])
        ]

        def conflict(t):
            return t["id"] in task["lockedIds"] and (
                t["id"] in brief["removeIds"]
                or t["id"] in task["excludedIds"]
                or catalogLanguageConflict(t, sources, brief.get("catalogLanguage"))
                or brief["artistOnly"]
                and not artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0))
                or any(
                    a.lower() in t.get("artist", "").lower()
                    for a in brief["excludedArtists"]
                )
                or brief["excludeLive"]
                and re.search(
                    r"(?:\bLive\b|现场|演唱会|音乐会|巡演|concert)",
                    t["title"] + " " + t.get("album", ""),
                    re.I | re.ASCII,
                )
            )

        if any(conflict(t) for t in (task.get("preview") or {}).get("tracks", [])):
            task.update(
                status="needs_clarification",
                question="锁定歌曲与新的排除条件冲突，请先解除相关锁定，或保留原条件。",
            )
            await self.checkpoint(task, "clarify")
            return
        task["phase"] = (
            "compose"
            if brief["kind"] == "edit"
            and task.get("preview")
            and not adjustmentStyles(brief, old)
            and brief.get("requiredArtist") == (old or {}).get("requiredArtist")
            and brief.get("scene") == (old or {}).get("scene")
            else "research"
        )
        if task["phase"] == "compose":
            task["branches"] = task.get("previousBranches", {})
        task.update(
            status="composing" if task["phase"] == "compose" else "researching",
            question="",
        )
        await self.checkpoint(task, "brief_ready")

    async def research(self, task, revision):
        await self.assertCurrent(task, revision)
        task["status"] = "researching"
        await self.checkpoint(task, "research")
        roles = []
        brief = task["brief"]
        if _scene_research_required(task):
            roles.append("scene")
        if brief["requireReleases"]:
            roles.append("releases")

        def branch(role, plan):
            return {
                "skill": "scene.research" if role == "scene" else "releases.research",
                "attempt": 0,
                "plan": {
                    "queries": plan.get("queries") or brief["queries"],
                    "artist": brief["artist"],
                    "artistId": brief.get("artistId", 0),
                    **({"requiredArtistId": brief["requiredArtistId"]} if brief.get("requiredArtistId") else {}),
                    "since": brief["since"],
                    "until": brief["until"],
                },
                "result": None,
                "error": None,
            }

        if self.strategy == "fixed":
            for role in roles:
                task["branches"][role] = branch(role, brief)
            task["phase"] = "dispatch"
            await self.checkpoint(task, "fixed_dispatch")
            return
        if self.strategy == "single_agent":
            plan = await self.call(
                task,
                revision,
                "shared_research",
                "research",
                {
                    "role": "coordinator",
                    "scope": brief,
                    "instruction": task["instruction"],
                },
            )
            for role in roles:
                task["branches"][role] = branch(role, plan)
            task["phase"] = "dispatch"
            await self.checkpoint(task, "shared_planned")
            return

        async def plan_role(role):
            plan = await self.call(
                task,
                revision,
                f"{role}_agent",
                "research",
                {"role": role, "scope": brief, "instruction": task["instruction"]},
            )
            await self.assertCurrent(task, revision)
            task["branches"][role] = branch(role, plan)
            await self.checkpoint(task, f"{role}_planned")

        await asyncio.gather(*(plan_role(role) for role in roles))
        task["phase"] = "dispatch"

    async def dispatch(self, task, revision):
        await self.assertCurrent(task, revision)
        pending = []
        hits = 0
        for branch in task["branches"].values():
            if branch.get("result") or branch.get("error"):
                continue
            if not branch.get("step"):
                query = (
                    sceneQueryPlan(
                        task,
                        branch["plan"]["queries"],
                        {
                            "attempt": branch["attempt"],
                            "history": self.sceneHistory(task, branch),
                        },
                    )
                    if branch["skill"] == "scene.research"
                    else None
                )
                inputs = {
                    **branch["plan"],
                    "queries": query["queries"] if query else branch["plan"]["queries"],
                    "instruction": task["instruction"],
                    "brief": task["brief"],
                    "adjustmentStyles": adjustmentStyles(
                        task["brief"], task.get("previousBrief")
                    ),
                    "excludedIds": list(
                        dict.fromkeys(
                            [*task["excludedIds"], *task.get("currentAvoidIds", [])]
                        )
                    ),
                    "requiredArtist": task["brief"]["requiredArtist"] or "",
                    "artistOnly": task["brief"]["artistOnly"] is True,
                    "excludedArtists": task["brief"]["excludedArtists"],
                    "attempt": branch["attempt"],
                    "feedback": branch.get("feedback") or "",
                }
                if query:
                    inputs.update(
                        queryCoverage=query["coverage"], queryStrategy=query["strategy"]
                    )
                branch["step"] = {
                    "id": _id(),
                    "skill": branch["skill"],
                    "version": TASK_SKILLS[branch["skill"]]["version"],
                    "leaseId": _id(),
                    "revision": revision,
                    "expiresAt": self.now() + TASK_SKILLS[branch["skill"]]["timeoutMs"],
                    "input": inputs,
                }
                branch.setdefault("evidenceGeneration", _id())
                branch.setdefault("attemptHistory", [])
                task.setdefault("toolHistory", [])
                entry = {
                    "stepId": branch["step"]["id"],
                    "skill": branch["skill"],
                    "version": branch["step"]["version"],
                    "runId": task["runId"],
                    "generation": branch["evidenceGeneration"],
                    "revision": revision,
                    "attempt": branch["attempt"],
                    "input": clone(inputs),
                    "dispatchedAt": self.now(),
                    "expiresAt": branch["step"]["expiresAt"],
                    "status": "dispatched",
                    "result": None,
                }
                branch["attemptHistory"].append(clone(entry))
                task["toolHistory"].append(entry)
                cached = await self.toolCache.get(self.toolCacheScope(task, branch))
                await self.assertCurrent(task, revision)
                if cached:
                    parsed = clone(cached["result"])
                    _scene_artist_identity(task, branch, parsed)
                    styles = (
                        adjustmentStyles(task["brief"], task.get("previousBrief"))
                        or task["brief"]["styles"]
                    )
                    parsed["tracks"] = [
                        {
                            **t,
                            "styleEvidence": playlistStyleEvidence(
                                t, parsed["sources"], styles
                            ),
                        }
                        for t in parsed["tracks"]
                    ]
                    if task["brief"]["artistOnly"]:
                        parsed["tracks"] = [
                            t
                            for t in parsed["tracks"]
                            if artistCreditMatches(t, task["brief"]["requiredArtist"], task["brief"].get("requiredArtistId", 0))
                        ]
                    hashed = digest(parsed)
                    evidence = {
                        "status": "completed",
                        "completedAt": self.now(),
                        "resultHash": hashed,
                        "result": clone(parsed),
                        "cache": cached["cache"],
                    }
                    branch.update(resultHash=hashed, result=parsed, error=None)
                    branch["step"]["completedAt"] = self.now()
                    entry.update(evidence)
                    branch["attemptHistory"][-1].update(clone(evidence))
                    branch["result"] = (
                        mergeToolEvidence(
                            self.sceneHistory(task, branch),
                            TASK_SKILLS[branch["skill"]]["maximumTracks"],
                        )
                        or parsed
                    )
                    task["budget"]["cacheHits"] = task["budget"].get("cacheHits", 0) + 1
                    hits += 1
                    continue
                if task["budget"]["toolCalls"] >= task["budget"]["maxToolCalls"]:
                    raise ApiError(429, "TASK_BUDGET", "达到本机曲库查询预算。")
                task["budget"]["toolCalls"] += 1
            pending.append(branch["step"])
        task.update(
            pendingSteps=pending,
            phase="waiting" if pending else "observe",
            status="waiting_tools" if pending else "researching",
        )
        await self.checkpoint(
            task,
            "catalog_cache_hit" if hits else "dispatch",
            f"复用 {hits} 项近期核验的曲库结果，保留原始查询时间；播放权益仍由本机实时核验。"
            if hits
            else "",
        )

    def toolCacheScope(self, task, branch):
        return {
            "userId": task["userId"],
            "deviceId": task["deviceId"],
            "catalogContext": task.get("catalogContext") or "",
            "skill": branch["skill"],
            "skillVersion": (branch.get("step") or {}).get("version")
            or TASK_SKILLS[branch["skill"]]["version"],
            "input": (branch.get("step") or {}).get("input") or {},
        }

    def sceneQueries(self, task, queries):
        return sceneQueryPlan(task, queries)["queries"]

    def sceneHistory(self, task, branch):
        return [
            e
            for e in branch.get("attemptHistory", [])
            if e.get("runId") == task["runId"]
            and e.get("generation") == branch.get("evidenceGeneration")
        ]

    def canSupplement(self, task, branch):
        return (
            branch["skill"] != "scene.research"
            or not sceneQueryPlan(
                task,
                branch["plan"]["queries"],
                {
                    "attempt": branch["attempt"] + 1,
                    "history": self.sceneHistory(task, branch),
                },
            )["exhausted"]
        )

    async def degrade(self, task, stage, code, message):
        task.setdefault("degradations", []).append(
            {
                "stage": stage,
                "code": code,
                "revision": task["revision"],
                "at": self.now(),
                "message": message,
            }
        )
        await self.checkpoint(task, stage, message)

    def observationSufficiency(self, task):
        branches = list(task["branches"].values())
        if not branches or any(b.get("error") or not b.get("result") for b in branches):
            return {
                "sufficient": False,
                "reason": "missing_catalog_result",
                "missingEvidence": [
                    {"kind": "catalog_result", "skill": b["skill"], "status": "missing"}
                    for b in branches
                    if b.get("error") or not b.get("result")
                ],
            }
        provisional = assemblePreview(task, [], self.now())
        keys = {
            "locks",
            "duration",
            "excluded_ids",
            "excluded_artists",
            "required_artist",
            "releases",
        }
        missing = [
            {"kind": "hard_condition", "key": c["key"], "status": c["status"]}
            for c in provisional["checks"]
            if c["key"] in keys and c["status"] in ("unmet", "unverified")
        ]
        if not provisional["tracks"]:
            missing.append({"kind": "candidate_pool", "status": "empty"})
        sources = [s for b in branches for s in b["result"].get("sources", [])]
        labels = [" ".join([s["name"], *s.get("tags", [])]) for s in sources]
        scene = task["brief"]["scene"]
        if (
            scene
            and not re.fullmatch(r"(?:听歌|音乐|日常|歌曲|随便听听|日常听歌)", scene)
            and not any(scene in label for label in labels)
        ):
            missing.append(
                {"kind": "scene_source_label", "value": scene, "status": "unverified"}
            )
        for entry in sceneQueryPlan(task, [])["coverage"]:
            if (
                entry["field"].startswith("style:")
                and entry.get("reason") != "negative_style_not_positive_query"
                and not any(
                    styleLabelMatches(label, entry["value"]) for label in labels
                )
            ):
                missing.append(
                    {
                        "kind": "style_source_label",
                        "value": entry["value"],
                        "status": "unverified",
                    }
                )
        return {
            "sufficient": not missing,
            "reason": "catalog_evidence_missing"
            if missing
            else "bounded_catalog_metadata_and_source_labels",
            "missingEvidence": missing,
            "listeningQuality": "not_measured",
            "unknownChecks": [
                c["key"] for c in provisional["checks"] if c["status"] == "unverified"
            ],
        }

    async def advise(self, task, revision, node, inputs):
        try:
            proposal = await self.call(task, revision, node, "observe", inputs)
            await self.assertCurrent(task, revision)
            assessment = observerAssessment(proposal)
            if assessment is None:
                raise ApiError(422, "INVALID_TASK_MODEL", "观察建议未通过结构校验。")
            return assessment
        except ApiError as error:
            await self.assertCurrent(task, revision)
            if error.code != "INVALID_TASK_MODEL":
                raise
            await self.degrade(
                task,
                "observer",
                "INVALID_TASK_MODEL",
                "AI 补查建议无效或被截断，已使用经过校验的曲库候选继续编排；风格和版本等未知项仍待核验。",
            )
            return {
                "decision": "done",
                "queries": [],
                "note": "观察建议失败，已明确降级。",
            }

    async def remainingModelCalls(self, task):
        day = iso(self.now())[:10]
        used = await self.accountStore.getDailyUsage(task["userId"], day)
        global_used = await self.store.getGlobalDailyUsage(day)
        return min(
            self.maxRunCalls - task["budget"]["runCalls"],
            self.maxTaskCalls - task["budget"]["totalCalls"],
            self.dailyLimit - used,
            self.globalDailyLimit - global_used,
        )

    async def observe(self, task, revision):
        await self.assertCurrent(task, revision)
        if _catalog_clarification(task):
            task["status"] = "needs_clarification"
            task["messages"].append(
                _message("assistant", task["question"], task["revision"])
            )
            await self.checkpoint(task, "clarify")
            return
        branches = list(task["branches"].items())
        slots = task["budget"]["maxToolCalls"] - task["budget"]["toolCalls"]
        can_observe = [
            (r, b) for r, b in branches if b["attempt"] == 0 and not b.get("error")
        ]
        task["status"] = "researching"
        await self.checkpoint(task, "observe")
        if self.strategy == "fixed":
            task["phase"] = "compose"
            return
        # DP 的 CPU 工作放到工作线程，避免阻塞 HTTP 及租约心跳。
        task["observation"] = await asyncio.to_thread(self.observationSufficiency, task)
        if task["observation"]["sufficient"]:
            task["observation"]["status"] = "program_checked"
            task["phase"] = "compose"
            await self.checkpoint(
                task,
                "observation_program_checked",
                "已按程序检查候选时长、歌手和来源标签；实际听感及未知版本未被认证。",
            )
            return

        def retry(branch, assessment):
            branch.update(
                attempt=1,
                feedback=assessment["note"],
                previousResult=branch.get("result"),
                result=None,
                step=None,
                resultHash=None,
            )
            branch["plan"]["queries"] = (
                assessment["queries"] or branch["plan"]["queries"]
            )

        if self.strategy == "single_agent":
            if can_observe and await self.remainingModelCalls(task) > 1:
                assessment = await self.advise(
                    task,
                    revision,
                    "shared_observe",
                    {
                        "role": "coordinator",
                        "scope": task["brief"],
                        "sufficiency": task["observation"],
                        "result": {
                            r: b.get("result") or b.get("error") for r, b in branches
                        },
                        "remainingCalls": await self.remainingModelCalls(task),
                    },
                )
                for _, branch in branches:
                    if (
                        assessment["decision"] == "retry"
                        and branch["attempt"] == 0
                        and not branch.get("error")
                        and slots > 0
                    ):
                        if not self.canSupplement(task, branch):
                            await self.checkpoint(
                                task,
                                "supplement_skipped",
                                "没有可核验的新检索路线，保留候选和未知项，不重复相同查询。",
                            )
                            continue
                        slots -= 1
                        retry(branch, assessment)
            task["phase"] = (
                "dispatch"
                if any(
                    not b.get("result") and not b.get("error")
                    for b in task["branches"].values()
                )
                else "compose"
            )
            return
        for role, branch in can_observe:
            if await self.remainingModelCalls(task) <= 1:
                break
            assessment = await self.advise(
                task,
                revision,
                f"{role}_observe",
                {
                    "role": role,
                    "scope": task["brief"],
                    "sufficiency": task["observation"],
                    "result": branch.get("result"),
                    "remainingCalls": await self.remainingModelCalls(task),
                },
            )
            await self.assertCurrent(task, revision)
            branch["observed"] = True
            if (
                assessment["decision"] == "retry"
                and branch["attempt"] == 0
                and slots > 0
            ):
                if not self.canSupplement(task, branch):
                    await self.checkpoint(
                        task,
                        "supplement_skipped",
                        "没有可核验的新检索路线，保留候选和未知项，不重复相同查询。",
                    )
                    continue
                slots -= 1
                retry(branch, assessment)
                branch["error"] = None
            await self.checkpoint(task, f"{role}_observed", assessment["note"])
        task["phase"] = (
            "dispatch"
            if any(
                not b.get("result") and not b.get("error")
                for b in task["branches"].values()
            )
            else "compose"
        )

    async def compose(self, task, revision):
        await self.assertCurrent(task, revision)
        if _catalog_clarification(task):
            task["status"] = "needs_clarification"
            await self.checkpoint(task, "clarify", "等待歌手或查询范围确认，未生成新歌单。")
            return
        task["status"] = "composing"
        await self.checkpoint(task, "compose")
        release_only = task["brief"]["requireReleases"] and not _scene_research_required(task)
        if release_only:
            # 恢复旧检查点时也不能把历史场景候选当成新作查询结果。
            task["branches"] = {
                key: branch for key, branch in task["branches"].items()
                if branch["skill"] == "releases.research"
            }
        pool = {
            t["id"]: t
            for b in task["branches"].values()
            for t in (b.get("result") or {}).get("tracks", [])
        }
        candidates = list(pool.values())[:60]
        if not candidates and any(b.get("error") for b in task["branches"].values()):
            task["phase"] = "waiting"
            raise ApiError(
                502,
                "CATALOG_QUERY_FAILED",
                "本机曲库查询失败，进度已保留。请检查网易云服务连接后点击继续；这不是没有匹配歌曲。",
            )
        if not candidates and not task["lockedIds"]:
            task.update(
                status="needs_clarification",
                question="在本次查询范围内没有找到可核验的新作，是否扩大时间范围？"
                if release_only
                else "没有找到足够的可核验候选，请换一个主题，或缩小查询范围。",
            )
            await self.checkpoint(task, "no_candidates")
            return
        ranked = (
            await self.call(
                task,
                revision,
                "compose",
                "compose",
                {
                    "brief": task["brief"],
                    **({"preference": task["preference"]} if task.get("requirementPlan") else {}),
                    "instruction": task["instruction"],
                    "candidates": [
                        {k: v for k, v in t.items() if k != "coverUrl"}
                        for t in candidates
                    ],
                    "lockedIds": task["lockedIds"],
                    "previousOrder": [
                        t["id"] for t in (task.get("preview") or {}).get("tracks", [])
                    ],
                },
            )
            if candidates
            else {"ids": []}
        )
        await self.assertCurrent(task, revision)
        old = (
            {
                "preview": clone(task["preview"]),
                "brief": clone(task.get("previousBrief") or task["brief"]),
                "lockedIds": list(task["preview"].get("lockedIds", [])),
                **({"requirementPlan": clone(task["previousRequirementPlan"]),
                    # 撤回只保存该预览候选与来源，不复制旧租约和重复的工具尝试账本。
                    "branches": {role: {"skill": branch["skill"], "plan": clone(branch["plan"]),
                                        "result": clone(branch.get("result")), "error": None, "attempt": 0}
                                 for role, branch in (task.get("previousBranches") or task["branches"]).items()}}
                   if task.get("previousRequirementPlan") else {}),
            }
            if task.get("preview")
            else None
        )
        preview = await asyncio.to_thread(
            assemblePreview, task, ranked.get("ids", []), self.now()
        )
        await self.assertCurrent(task, revision)
        if release_only and (
            not preview["tracks"]
            or any(t.get("sourceSkill") != "releases.research" for t in preview["tracks"])
        ):
            task.update(
                status="needs_clarification",
                question="没有可采用的已核验新作；请调整时间范围，或先解除与新作查询冲突的锁定歌曲。",
            )
            await self.checkpoint(task, "release_candidates_unavailable")
            return
        preview["checks"] = [{**c, "detail": c["text"]} for c in preview["checks"]]
        preview["degradations"] = clone(
            [e for e in task.get("degradations", []) if e["revision"] == revision]
        )
        for item in preview["degradations"]:
            preview["checks"].append(
                {
                    "key": item["stage"],
                    "status": "unverified",
                    "text": item["message"],
                    "detail": item["message"],
                }
            )
        if preview["degradations"]:
            preview["note"] = (
                " ".join(d["message"] for d in preview["degradations"])
                + " "
                + preview["note"]
            )
        preview["ranking"] = {
            **(
                ranked.get("ranking")
                or {"status": "model", "source": "model_order", "reasonCounts": {}}
            ),
            "orderedIds": ranked["ids"],
        }
        ranking = preview["ranking"]
        ranking_text = (
            "AI 排序未通过校验，已采用真实曲库检索顺序和时长规则；未将无效 ID 加入歌单。"
            if ranking["status"] == "degraded"
            else f"AI 优先编排 {ranking['rankedCount']} 首候选，其余 {ranking['unrankedCount']} 首保留曲库顺序；最终时长与硬约束由程序编排。"
            if ranking.get("scope") == "priority_prefix"
            else "排序通过真实候选 ID 校验，最终时长与硬约束由程序编排。"
        )
        preview["checks"].append(
            {
                "key": "ranking",
                "status": "unverified"
                if ranking["status"] == "degraded"
                else "satisfied",
                "text": ranking_text,
                "detail": ranking_text,
            }
        )
        if ranking["status"] == "degraded":
            preview["note"] = ranking_text + " " + preview["note"]
        previous_tracks = (task.get("preview") or {}).get("tracks", [])
        preview["diff"] = {
            "addedIds": [
                t["id"]
                for t in preview["tracks"]
                if not any(o["id"] == t["id"] for o in previous_tracks)
            ],
            "removedIds": [
                t["id"]
                for t in previous_tracks
                if not any(n["id"] == t["id"] for n in preview["tracks"])
            ],
        }
        preview["adjustment"] = adjustmentAssessment(task, preview)
        adjustment = preview["adjustment"]
        adjustment_text = ""
        if adjustment:
            texts = {
                "source_supported": f"未锁定部分已调整，{len(adjustment['evidenceTrackIds'])} 首来自与「"
                + "、".join(adjustment["styles"])
                + "」相关的歌单名称或标签；尚未验证实际听感。",
                "unchanged": "这次未锁定部分没有改变，尚未完成本轮调整，请修改要求或换一批候选。",
                "degraded": "AI 排序未通过校验，本轮调整未验收，请检查候选或重试。",
                "unverified": "未锁定部分已改变，但没有找到能核验本轮风格要求的歌单来源，调整效果待确认。",
            }
            adjustment_text = texts[adjustment["status"]]
            preview["checks"].append(
                {
                    "key": "adjustment",
                    "status": "satisfied"
                    if adjustment["status"] == "source_supported"
                    else "unmet"
                    if adjustment["status"] == "unchanged"
                    else "unverified",
                    "text": adjustment_text,
                    "detail": adjustment_text,
                }
            )
            preview["note"] = adjustment_text + " " + preview["note"]
        if old:
            task["versions"] = [*task["versions"], old][-8:]
        task.update(
            preview=preview,
            status="preview",
            phase="done",
            pendingSteps=[],
            adoption=None,
        )
        task["messages"] = [
            *task["messages"],
            _message(
                "assistant",
                f"已生成 {len(preview['tracks'])} 首歌曲，约 {fixed1(preview['totalDurationMs'] / 60000)} 分钟。"
                + " ".join("尚未满足：" + check["text"] for check in preview["checks"] if check["key"] in ("track_count", "duration") and check["status"] == "unmet")
                + f"{adjustment_text}请检查约束与预览后采用。",
                task["revision"],
            ),
        ][-24:]
        await self.checkpoint(task, "preview")

    async def pauseUser(self, userId):
        await self.initialize()
        for original in await self.store.activeForUser(userId):
            for attempt in range(5):
                task = (
                    await self.store.load(original["id"], userId)
                    if attempt
                    else original
                )
                if not task or task["status"] not in ACTIVE:
                    break
                self.abort(task["id"])
                task["revision"] += 1
                task.update(status="paused", pendingSteps=[])
                try:
                    await self.checkpoint(
                        task, "logout", options={"invalidateLease": True}
                    )
                    break
                except ApiError as error:
                    if error.code != "TASK_WRITE_CONFLICT" or attempt == 4:
                        raise

    async def close(self):
        self.closed = True
        if self.recoveryTimer:
            self.recoveryTimer.cancel()
        for job in list(self.jobs.values()):
            job["controller"].set()
        self.jobs.clear()
        # 等待有界收尾，不取消未知的已发起付费请求，更不自动重放。
        if self.runningJobs:
            await asyncio.wait(list(self.runningJobs), timeout=1)
        for entry in self.legacyClaims.values():
            if entry.get("timer"):
                entry["timer"].cancel()
            if entry.get("controller"):
                entry["controller"].set()
        self.legacyClaims.clear()
