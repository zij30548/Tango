"""原生端到端评测的隔离 HTTP 后台；默认零付费，不读取真实用户存储。"""

import argparse
from collections import Counter
from datetime import datetime
import hmac
import json
import os
from pathlib import Path
import re

from tango_backend.agent_tracing import AgentTracing, disable_automatic_tracing
from tango_backend.app import createApp
from tango_backend.compat import clone, now_ms
from tango_backend.store import Store

from . import calibration
from .scripted import ScriptedModel


CONTROL_HEADER = b"x-tango-evaluation-control"
ID = re.compile(r"[A-Za-z0-9_-]{8,100}")
BODY_LIMIT = 256 * 1024
RESPONSE_LIMIT = 512 * 1024
TASK_FIELDS = {
    "id", "deviceId", "revision", "stateVersion", "runId", "status", "brief",
    "briefSummary", "preview", "lockedIds", "messages", "sources", "steps",
    "pendingSteps", "error", "budget", "progress", "recovery", "adoption",
    "canUndo", "toolHistory", "createdAt", "updatedAt",
}
ROUTE_FIELDS = {
    "id", "conversationId", "messageId", "deviceId", "inputHash",
    "capabilityVersion", "status", "paid", "attemptId", "result", "errorCode",
    "createdAt", "finishedAt",
}
CONVERSATION_FIELDS = {
    "id", "deviceId", "version", "messages", "latestTaskId", "latestTaskLink",
    "hasEarlier", "earliestSequence",
}
PRIVATE_FIELDS = {
    "token", "authorization", "cookie", "password", "apikey", "api_key",
    "session", "sessions", "owner_token", "ownertoken", "leaseid",
    "playurl", "playbackurl", "url", "_observation",
}


class IndependentServerError(ValueError):
    """只使用固定错误码，启动失败不显示配置或上游正文。"""


class WiringModel(ScriptedModel):
    """固定假模型只验接线；不读取新题金标，也不产生真实语义成绩。"""

    model = "independent-wiring-fixture"

    def __init__(self, now):
        super().__init__("independent_wiring_only", now)

    async def call(self, kind, input, options=None):
        if kind == "plan" and not input.get("currentBrief"):
            input = {**input, "currentBrief": {
                "kind": "research", "title": "离线接线歌单", "scene": "读书",
                "styles": ["纯音乐"], "queries": ["读书 纯音乐"], "artist": "",
                "requiredArtist": "", "artistOnly": False, "since": None,
                "until": None, "targetMinutes": 20, "excludeLive": True,
                "avoidPrevious": False, "excludedArtists": [],
                "requireReleases": False, "retainFirst": 0, "removeIds": [],
                "question": "", "adjustment": "",
            }}
        return await super().call(kind, input, options)


def _safe(value, keys=()):
    """从自有合成运行导出证据，递归移除凭据与操作租约。"""
    if isinstance(value, dict):
        value = {key: _safe(item, keys) for key, item in value.items()
                 if str(key).lower() not in PRIVATE_FIELDS}
    elif isinstance(value, list):
        value = [_safe(item, keys) for item in value]
    return calibration.redact(value, keys)


def _selected(value, fields, keys=()):
    return _safe({key: item for key, item in value.items() if key in fields}, keys) if isinstance(value, dict) else None


def load_run(run):
    """核验本轮清单与新题；历史授权和旧运行目录都不能直接复用。"""
    run = calibration.private_directory(run)
    manifest = calibration.read_json(run / "manifest.json")
    suite = calibration.read_json(run / "suite.json")
    if not isinstance(manifest, dict) or not isinstance(suite, dict):
        raise IndependentServerError("invalid_run_documents")
    unsigned = {key: value for key, value in manifest.items() if key != "seal"}
    if manifest.get("seal") != calibration.sha(calibration.encode(unsigned).encode()):
        raise IndependentServerError("invalid_manifest_seal")
    if manifest.get("suiteDigest") != calibration.sha(calibration.encode(suite).encode()):
        raise IndependentServerError("frozen_suite_changed")
    mode, maximum = manifest.get("mode"), manifest.get("maximumProviderRequests")
    if mode not in {"offline", "live"} or type(maximum) is not int or not 0 <= maximum <= 10000:
        raise IndependentServerError("invalid_run_mode_or_budget")
    if mode == "offline" and maximum != 0 or mode == "live" and (
        maximum <= 0 or not isinstance(manifest.get("authorization"), str) or not manifest["authorization"].strip()
    ):
        raise IndependentServerError("explicit_new_live_authorization_required")
    cases = suite.get("cases")
    if not isinstance(cases, list) or not 1 <= len(cases) <= 200:
        raise IndependentServerError("invalid_suite_cases")
    turns = {}
    for case in cases:
        case_id = case.get("id") if isinstance(case, dict) else None
        if not isinstance(case_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", case_id) or case_id in turns:
            raise IndependentServerError("invalid_case_id")
        values = case.get("turns")
        if not isinstance(values, list) or not 1 <= len(values) <= 30:
            raise IndependentServerError("invalid_case_turns")
        if any(not isinstance(turn, dict) or not isinstance(turn.get("text"), str) or not 0 < len(turn["text"]) <= 1000 for turn in values):
            raise IndependentServerError("invalid_synthetic_instruction")
        setup = case.get("setupInstructions", [])
        if not isinstance(setup, list) or len(setup) > 20 or any(not isinstance(text, str) or not 0 < len(text) <= 1000 for text in setup):
            raise IndependentServerError("invalid_setup_instructions")
        turns[case_id] = [turn["text"] for turn in values]
    try:
        now = int(datetime.fromisoformat(suite["clock"].replace("Z", "+00:00")).timestamp() * 1000)
    except (KeyError, TypeError, ValueError, OverflowError):
        raise IndependentServerError("invalid_suite_clock") from None
    path = run / "control.token"
    if path.is_symlink() or not path.is_file() or path.stat().st_mode & 0o077 or path.stat().st_size > 300:
        raise IndependentServerError("private_control_token_required")
    token = path.read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,256}", token):
        raise IndependentServerError("invalid_control_token")
    return run, manifest, suite, turns, now, token


class IndependentEvaluationServer:
    """包装真实产品 ASGI，旁路控制能力仅可读取本次新建的拥有者存储。"""

    def __init__(self, run, *, port=0):
        self.run, self.manifest, self.suite, self.turns, self.semantic_anchor, self.token = load_run(run)
        self.setup_instructions = {case["id"]: case.get("setupInstructions", []) for case in self.suite["cases"]}
        self.port, self.initialized, self.closed = port, False, False
        self.context, self.contexts, self.case_tasks = None, {}, {}
        self.keys, self.ledger = [], None
        path = self.run / "owned.sqlite"
        # 先独占创建再打开；不能从旧库恢复后继续发起付费请求。
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
        os.close(descriptor)
        try:
            self.ledger = calibration.RequestLedger(self.run, self.manifest["maximumProviderRequests"])
            if self.manifest["mode"] == "live":
                model, route, self.keys, self.model_snapshot = calibration.live_models(self.ledger, self.semantic_anchor)
            else:
                if self.suite.get("modelFixture") == "demo-stability-v1":
                    from .demo_stability import DemoWiringModel
                    model = route = DemoWiringModel(self.semantic_anchor)
                elif self.suite.get("modelFixture") == "repair-regression-v1":
                    from .repair_regression import RepairWiringModel
                    model = route = RepairWiringModel(self.semantic_anchor)
                else:
                    model = route = WiringModel(self.semantic_anchor)
                self.model_snapshot = {"strategy": "offline_fixture", "model": model.model,
                                       "semanticEvidence": False}
            self.store = Store(path)
            with disable_automatic_tracing():
                self.product = createApp(
                    store=self.store, model=model, taskModel=model, routingModel=route,
                    # Qt 以系统 UTC 检查工具租约；产品时钟必须持续前进，题集时间只锚定语义。
                    now=now_ms, registrationMode="open", tracing=AgentTracing(enabled=False),
                    aiPerMinute=10000, aiDailyLimit=10000, aiGlobalDailyLimit=10000,
                    authPerTenMinutes=10000, conversationCreatesPerDay=10000,
                    requestPerMinute=100000, userRequestPerMinute=100000,
                    defaultQuotaPolicy={"dailyLimit": 10000, "totalLimit": 100000, "enabled": True},
                )
        except BaseException:
            if self.ledger:
                self.ledger.close()
            raise

    async def initialize(self):
        if not self.initialized:
            await self.product.initialize()
            self.initialized = True

    async def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            if self.initialized:
                await self.product.close()
        finally:
            await self.store.close()
            self.ledger.close()

    def outcomes(self):
        """没有结束回执的 started 保留 uncertain，分母仍含全部尝试。"""
        events = [json.loads(line) for line in self.ledger.path.read_text().splitlines()]
        finished = {event["number"]: event for event in events if event.get("event") == "finished"}
        return dict(Counter(finished.get(number, {}).get("status", "uncertain") for number in range(1, self.ledger.started + 1)))

    def provider_ledger(self, key):
        """结束记录按尝试号关联原上下文，不把后续轮次当作原调用来源。"""
        events = [json.loads(line) for line in self.ledger.path.read_text().splitlines()]
        numbers = {event["number"] for event in events if event.get("event") == "started"
                   and event.get("caseId") == key[0] and event.get("turn") == key[1]
                   and event.get("phase", "evaluation") == key[2]}
        return [event for event in events if event.get("number") in numbers]

    def status(self):
        return {"mode": self.manifest["mode"], "storage": "sqlite", "authority": "owned_sqlite", "tracing": False,
                "providerRequests": self.ledger.started, "providerOutcomes": self.outcomes(),
                "maximumProviderRequests": self.manifest["maximumProviderRequests"],
                "modelSnapshot": _safe(self.model_snapshot, self.keys),
                "semanticEvidence": self.manifest["mode"] == "live"}

    def _key(self, body, *, fallback=True):
        if fallback and "caseId" not in body and "turn" not in body:
            if not self.context:
                raise IndependentServerError("evaluation_context_required")
            if "phase" in body and body["phase"] != self.context[2]:
                raise IndependentServerError("evaluation_phase_mismatch")
            return self.context
        case_id, turn, phase = body.get("caseId"), body.get("turn"), body.get("phase", "evaluation")
        if case_id not in self.turns or type(turn) is not int or not 1 <= turn <= len(self.turns[case_id]) or phase not in {"setup", "evaluation"}:
            raise IndependentServerError("invalid_evaluation_context")
        return case_id, turn, phase

    @staticmethod
    def context_value(key):
        return {"caseId": key[0], "turn": key[1], "phase": key[2]}

    async def _task(self, identifier):
        if not identifier:
            return None, None
        row = await self.store.db.fetchone("SELECT snapshot_json FROM music_tasks WHERE id=?", [identifier])
        raw = json.loads(row["snapshot_json"]) if row else None
        public = await self.product.tasks.snapshot(raw) if raw else None
        return _selected(public, TASK_FIELDS, self.keys), raw

    async def set_context(self, body):
        if not {"caseId", "turn"} <= set(body) or set(body) - {"caseId", "turn", "phase"}:
            raise IndependentServerError("invalid_context_shape")
        key = self._key(body, fallback=False)
        if key not in self.contexts:
            before, _ = await self._task(self.case_tasks.get(key[0]))
            self.contexts[key] = {"before": before, "httpErrors": [], "conversationId": None,
                                  "messageId": None, "taskId": self.case_tasks.get(key[0])}
        self.context = key
        self.ledger.context = self.context_value(key)
        return {"context": self.context_value(key), "accepted": True}

    async def observation(self, body):
        if set(body) - {"caseId", "turn", "phase", "conversationId", "messageId"}:
            raise IndependentServerError("invalid_observation_shape")
        key = self._key(body)
        data = self.contexts.get(key)
        if data is None:
            raise IndependentServerError("evaluation_context_not_started")
        if key[2] == "setup" and not body.get("conversationId") and not body.get("messageId"):
            # 前置指令不冒充正式题：默认只对账成本，不用正式原文校验前置消息。
            return {"context": self.context_value(key), "authority": "owned_sqlite",
                    "providerLedger": _safe(self.provider_ledger(key), self.keys),
                    "providerRequests": self.ledger.started, "providerOutcomes": self.outcomes()}
        cid, mid = body.get("conversationId", data["conversationId"]), body.get("messageId", data["messageId"])
        if any(value is not None and (not isinstance(value, str) or not ID.fullmatch(value)) for value in (cid, mid)):
            raise IndependentServerError("invalid_observation_identifier")
        if cid and data["conversationId"] and cid != data["conversationId"] or mid and data["messageId"] and mid != data["messageId"]:
            raise IndependentServerError("observation_context_mismatch")
        route, conversation, owner, device = None, None, None, None
        if cid:
            row = await self.store.db.fetchone("SELECT journal_json,user_id,device_id FROM music_conversations WHERE id=?", [cid])
            if row:
                owner, device = row["user_id"], row["device_id"]
                journal = json.loads(row["journal_json"])
                anchor = next((message for message in journal["messages"] if message["id"] == mid and message["role"] == "user"), None)
                expected = self.setup_instructions[key[0]] if key[2] == "setup" else [self.turns[key[0]][key[1] - 1]]
                if mid and (not anchor or anchor.get("content") not in expected):
                    raise IndependentServerError("observation_anchor_mismatch")
                conversation = _selected(journal, CONVERSATION_FIELDS, self.keys)
                if mid:
                    row = await self.store.db.fetchone("SELECT * FROM music_conversation_routes WHERE conversation_id=? AND message_id=?", [cid, mid])
                    if row:
                        from tango_backend.conversation_route_store import _view
                        route = _selected(_view(row, now_ms()), ROUTE_FIELDS, self.keys)
        task_id = data["taskId"]
        if mid and owner and device:
            row = await self.store.db.fetchone("SELECT task_id FROM music_task_requests WHERE request_id=? AND user_id=? AND device_id=?", [mid, owner, device])
            if row:
                task_id = row["task_id"]
        task, raw = await self._task(task_id)
        calls = []
        if task_id:
            rows = await self.store.db.fetchall("SELECT id,task_id,revision,run_id,node,status,started_at,finished_at,input_tokens,output_tokens,error_code FROM music_task_calls WHERE task_id=? ORDER BY started_at,id", [task_id])
            calls.extend({"kind": "task", **dict(row)} for row in rows)
        if route:
            rows = await self.store.db.fetchall("SELECT id,route_id,status,paid,started_at,finished_at,input_tokens,output_tokens,error_code FROM music_conversation_route_attempts WHERE route_id=?", [route["id"]])
            calls.extend({"kind": "route", **dict(row)} for row in rows)
        return {"context": self.context_value(key), "authority": "owned_sqlite", "route": route, "task": task,
                "conversation": conversation, "messages": (conversation or {}).get("messages", []),
                "before": clone(data["before"]), "tools": _safe((raw or {}).get("toolHistory", []), self.keys),
                "httpErrors": clone(data["httpErrors"]), "callLedger": _safe(calls, self.keys),
                "providerLedger": _safe(self.provider_ledger(key), self.keys),
                "providerRequests": self.ledger.started, "providerOutcomes": self.outcomes()}

    def capture(self, key, method, path, request_body, status, response_body):
        """只关联本次已冻结输入，不保存账号请求、Bearer 或完整 HTTP 请求。"""
        if not key or key not in self.contexts:
            return
        data = self.contexts[key]
        try:
            request = json.loads(request_body) if request_body else {}
            response = json.loads(response_body) if response_body else {}
        except (ValueError, UnicodeError):
            return
        if not isinstance(request, dict) or not isinstance(response, dict):
            return
        parts = path.split("/")
        if method == "POST" and len(parts) == 5 and parts[1:3] == ["v1", "conversations"] and parts[4] == "messages":
            expected = self.setup_instructions[key[0]] if key[2] == "setup" else [self.turns[key[0]][key[1] - 1]]
            if request.get("content") in expected:
                data["conversationId"], data["messageId"] = parts[3], request.get("messageId")
        task = response.get("task") if isinstance(response, dict) else None
        if isinstance(task, dict) and isinstance(task.get("id"), str):
            # 同一轮读回和控制可更新观测，但不能把另一题的历史恢复绑定进来。
            if data["messageId"] and (method == "POST" or task["id"] == data["taskId"]):
                data["taskId"] = task["id"]
                self.case_tasks[key[0]] = task["id"]
        if status >= 400 and path.startswith(("/v1/conversations/", "/v1/tasks")):
            code = response.get("error", {}).get("code") if isinstance(response, dict) else None
            if not isinstance(code, str) or not re.fullmatch(r"[A-Z_]{1,60}", code):
                code = "HTTP_ERROR"
            public_parts = {"", "v1", "conversations", "messages", "route", "tasks", "requests", "actions", "steps", "result", "events", "latest"}
            endpoint = "/".join(part if part in public_parts else ":id" for part in path.split("/"))
            data["httpErrors"] = (data["httpErrors"] + [{"method": method, "path": endpoint[:200], "status": status, "code": code}])[-40:]

    async def __call__(self, scope, receive, send):
        if scope["type"] == "lifespan":
            await self._lifespan(receive, send)
            return
        if scope["type"] != "http":
            await self.product(scope, receive, send)
            return
        if scope["path"].startswith("/__evaluation__"):
            await self._control(scope, receive, send)
            return
        key, request_body, response_body, status = self.context, bytearray(), bytearray(), 0
        async def observed_receive():
            message = await receive()
            if message["type"] == "http.request" and len(request_body) <= BODY_LIMIT:
                request_body.extend(message.get("body", b""))
            return message
        async def observed_send(message):
            nonlocal status
            if message["type"] == "http.response.start":
                status = message["status"]
            elif message["type"] == "http.response.body" and len(response_body) <= RESPONSE_LIMIT:
                response_body.extend(message.get("body", b""))
            await send(message)
        await self.product(scope, observed_receive, observed_send)
        if len(request_body) <= BODY_LIMIT and len(response_body) <= RESPONSE_LIMIT:
            self.capture(key, scope["method"], scope["path"], request_body, status, response_body)

    async def _control(self, scope, receive, send):
        headers = dict(scope.get("headers", []))
        supplied = headers.get(CONTROL_HEADER, b"")
        if scope.get("client") and scope["client"][0] not in {"127.0.0.1", "::1", "testclient"}:
            await self._respond(send, 403, {"error": {"code": "EVALUATION_CONTROL_FORBIDDEN"}})
            return
        if not hmac.compare_digest(supplied, self.token.encode("ascii")):
            await self._respond(send, 403, {"error": {"code": "EVALUATION_CONTROL_FORBIDDEN"}})
            return
        try:
            method, path = scope["method"], scope["path"]
            if method == "GET" and path == "/__evaluation__/status":
                result = self.status()
            elif method == "POST" and path in {"/__evaluation__/context", "/__evaluation__/observation"}:
                raw = bytearray()
                while True:
                    message = await receive()
                    if message["type"] != "http.request":
                        raise IndependentServerError("invalid_control_request")
                    raw.extend(message.get("body", b""))
                    if len(raw) > 4096:
                        raise IndependentServerError("control_body_too_large")
                    if not message.get("more_body"):
                        break
                body = json.loads(raw)
                if not isinstance(body, dict):
                    raise IndependentServerError("invalid_control_request")
                result = await (self.set_context(body) if path.endswith("/context") else self.observation(body))
            else:
                await self._respond(send, 404, {"error": {"code": "EVALUATION_CONTROL_NOT_FOUND"}})
                return
        except (IndependentServerError, ValueError, TypeError, KeyError):
            await self._respond(send, 400, {"error": {"code": "INVALID_EVALUATION_CONTROL"}})
            return
        await self._respond(send, 200, _safe(result, self.keys))

    @staticmethod
    async def _respond(send, status, value):
        raw = calibration.encode(value).encode()
        await send({"type": "http.response.start", "status": status,
                    "headers": [(b"content-type", b"application/json"), (b"cache-control", b"no-store")]})
        await send({"type": "http.response.body", "body": raw})

    async def _lifespan(self, receive, send):
        while True:
            message = await receive()
            if message["type"] == "lifespan.startup":
                try:
                    await self.initialize()
                    calibration.write_new(self.run / "ready.json", {"pid": os.getpid(), "port": self.port,
                                                                   "mode": self.manifest["mode"], "ready": True})
                    await send({"type": "lifespan.startup.complete"})
                except Exception:
                    await send({"type": "lifespan.startup.failed", "message": "isolated_evaluation_startup_failed"})
                    await self.close()
                    return
            elif message["type"] == "lifespan.shutdown":
                await self.close()
                await send({"type": "lifespan.shutdown.complete"})
                return


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("端口须在1至65535之间。")
    try:
        server = IndependentEvaluationServer(args.run, port=args.port)
    except Exception:
        parser.exit(2, "隔离评测后台未启动；请检查本轮冻结清单、授权和私有目录。\n")
    import uvicorn
    uvicorn.run(server, host="127.0.0.1", port=args.port, access_log=False, log_level="error", lifespan="on")


if __name__ == "__main__":
    main()
