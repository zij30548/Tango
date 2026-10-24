"""显式启用的 Agent 追踪：只发送受控摘要，网络故障不改变业务结果。"""

from collections import Counter, OrderedDict
from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
import json
import logging
import math
import os
from queue import Empty, Full, Queue
import re
from threading import Event, Lock, Thread
import time
from uuid import UUID, uuid4, uuid5


_LOGGER = logging.getLogger(__name__)
_NAMESPACE = UUID("4c6ecf92-724b-4adf-b0d5-eac49df3f563")
_ACTIVE = ContextVar("tango_safe_agent_span", default=None)
_ENDPOINTS = {
    "https://api.smith.langchain.com", "https://eu.api.smith.langchain.com",
    "https://apac.api.smith.langchain.com", "https://aws.api.smith.langchain.com",
}
_NAMES = {
    "agent.conversation", "agent.step", "agent.progress", "conversation.route", "conversation.reply",
    "task.run", "task.entry", "task.understand", "task.research", "task.dispatch",
    "task.observe", "task.compose", "task.tool", "task.checks", "task.adopt",
    "task.resume", "task.cancel", "task.pause", "task.applied", "model.request",
    "task.checkpoint", "task.tool_result", "task.action", "task.model_call",
    "route", "entry", "understand", "research", "dispatch", "observe", "compose",
    "scene.research", "releases.research", "music.lookup", "constraint.check",
}
_IDS = {
    "conversationId", "messageId", "routeId", "taskId", "requestId", "callId",
    "traceId", "rootId", "operationId", "runId",
}
_NUMBERS = {
    "revision", "stateVersion", "attempt", "attemptCount", "stepCount", "actionCount",
    "queryCount", "candidateCount", "sourceCount", "trackCount", "lockedCount",
    "toolCalls", "modelCalls", "maxCalls", "targetMinutes", "totalDurationMs",
    "durationMs", "elapsedMs", "inputTokens", "outputTokens", "totalTokens",
    "input_tokens", "output_tokens", "total_tokens", "prompt_tokens", "completion_tokens",
    "satisfiedCount", "unmetCount", "unverifiedCount", "notRequestedCount",
    "httpStatus", "statusCode", "timeoutMs", "schemaVersion", "version",
}
_BOOLS = {"paid", "configured", "cacheHit", "cancelled", "accepted", "applied", "recovered", "hasPreview", "hasTask", "hasUsage"}
_ENUMS = {
    "status": {
        "pending", "reserved", "routing", "planning", "researching", "running", "completed",
        "failed", "uncertain", "cancelled", "paused", "preview", "awaiting_apply", "adopted",
        "clarifying", "needs_clarification", "awaiting_tool", "awaiting_tools", "blocked",
        "satisfied", "unmet", "unverified", "not_requested", "rejected", "accepted", "unknown",
        "ok", "error", "done", "queued", "awaiting_confirmation", "in_progress",
    },
    "stage": {"route", "entry", "understand", "research", "dispatch", "observe", "compose", "tool", "checks", "preview", "adopt", "applied", "done", "planning"},
    "node": {"route", "entry", "understand", "research", "dispatch", "observe", "compose", "tool", "checks", "preview", "applied"},
    "intent": {"help", "chat", "recommend", "scene", "release", "new_release", "adjust", "explain", "memory", "lookup", "play", "search", "history", "cancel", "pause", "resume", "undo", "apply", "unsupported", "clarify", "compound"},
    "kind": {"new", "revise", "scene", "release", "lookup", "chat", "help", "clarify", "memory", "control", "unsupported"},
    "purpose": {"intent", "route", "routing", "understand", "planning", "research", "observe", "compose", "conversation", "task_planning", "scene_research", "release_research"},
    "role": {"flash", "plus", "planner", "researcher", "scene", "release", "router", "composer"},
    "skill": {"scene.research", "releases.research", "music.lookup"},
    "skillId": {"scene.research", "releases.research", "music.lookup"},
    "source": {"rule", "model", "device", "server", "cache", "program"},
    "selectionReason": {"flash_only", "default_flash", "complex_planning", "research_plus", "plus_disabled", "purpose_policy", "fixed_policy", "complex_request"},
    "check": {"duration", "style", "artist", "language", "locked", "duplicates", "live", "instrumental", "availability", "exclusions"},
}
_MODEL_KEYS = {"model", "modelName", "selectedModel", "actualModel", "providerModel", "ls_model_name"}
_SAFE_ERRORS = {
    "TRACE_OPERATION_FAILED", "TRACE_EXPORT_FAILED", "TRACE_CONFIGURATION_INVALID", "TRACE_QUEUE_FULL",
    "MODEL_TIMEOUT", "MODEL_REQUEST_FAILED", "MODEL_HTTP_ERROR", "MODEL_OUTPUT_TRUNCATED",
    "INVALID_MODEL_STREAM", "INVALID_ROUTE_PLAN", "INVALID_TASK_PLAN", "INVALID_TOOL_RESULT",
    "REQUEST_CANCELLED", "TASK_CANCELLED", "TASK_CONFLICT", "OWNER_LOST", "QUOTA_EXCEEDED",
    "TOOL_TIMEOUT", "TOOL_FAILED", "INTERNAL_ERROR", "CANCELLED", "TIMEOUT",
    "MODEL_UNAVAILABLE", "MODEL_RESPONSE_TOO_LARGE", "INVALID_MODEL_RESPONSE", "MODEL_NOT_CONFIGURED",
    "TASK_NOT_FOUND", "TASK_NOT_READY", "TASK_BUDGET_EXHAUSTED", "INVALID_ARGUMENT", "FORBIDDEN",
    "INVALID_TOOL_RESULT", "INVALID_TASK_TRANSITION", "STALE_TASK_VERSION", "MODEL_POLICY_CHANGED",
    "TRACE_AUTH_FAILED", "TRACE_FORBIDDEN", "TRACE_RATE_LIMITED", "TRACE_NETWORK_FAILED", "TRACE_HTTP_FAILED",
    "TASK_BUDGET", "AI_DAILY_LIMIT", "AI_GLOBAL_DAILY_LIMIT", "AI_BUSY", "TASK_LEASE_LOST",
    "TASK_WRITE_CONFLICT", "TASK_CLAIMED", "STALE_TASK_STEP", "TOOL_LEASE_EXPIRED", "AUTH_REQUIRED", "INVALID_TASK_MODEL",
    "LOOKUP_CONTEXT_STALE", "INVALID_LOOKUP_CONTEXT",
}
_ALIASES = {
    "conversation_id": "conversationId", "message_id": "messageId", "route_id": "routeId",
    "attempt_id": "callId", "thread_id": "conversationId", "task_id": "taskId", "run_id": "runId",
    "previous_trace_id": "traceId",
    "step_id": "operationId", "call_id": "callId", "state_version": "stateVersion",
    "action_count": "actionCount", "track_count": "trackCount", "source_count": "sourceCount",
    "query_count": "queryCount", "candidate_count": "candidateCount", "locked_count": "lockedCount",
    "duration_ms": "durationMs", "elapsed_ms": "elapsedMs", "total_duration_ms": "totalDurationMs",
    "target_minutes": "targetMinutes", "actual_model": "actualModel", "selected_model": "selectedModel",
    "error_code": "errorCode", "cached": "cacheHit", "root_complete": "accepted",
    "route_completed": "accepted", "fixture": "configured", "checkpoint": "node",
}
_INTENTS = {"scene_research", "release_research", "song_lookup", "artist_catalog", "playlist_search", "conversation_history", "explain_result", "preference_proposal", "preference_deletion", "task_control", "clarification", "help", "unsupported"}
_CAPABILITIES = {"music.task", "music.lookup", "music.refine", "music.artist_catalog", "music.playlist_search", "memory.propose", "task.control", "assistant.reply"}
_ENUMS["status"].update({"awaiting_task", "waiting_tools", "composing", "budget_exhausted"})
_ENUMS["stage"].update({"plan", "waiting"})
_ENUMS["purpose"].add("plan")
_ENUMS["check"].update({"real_ids", "locks", "exclude_live", "excluded_ids", "excluded_artists", "required_artist", "releases", "catalog_language", "ranking", "adjustment"})
_ENUMS["check"].update({"catalog_supplement_failed", "observer", "observer_degraded"})
_ENUMS["check"].add("scene")
_ENUMS["node"].update({
    "created", "turn", "applied", "clarify", "brief_ready", "fixed_dispatch", "shared_planned",
    "scene_planned", "release_planned", "shared_observed", "scene_observed", "release_observed",
    "observation_program_checked", "tool_result", "tool_failed", "waiting_tools", "no_candidates",
    "release_candidates_unavailable", "paused", "cancelled", "resumed", "undo", "adopt", "failed",
    "releases_planned", "releases_observed", "released_observed", "catalog_supplement_failed",
    "observer", "observer_degraded", "tool_timeout", "clarify_waiting", "understand_invalid",
    "supplement_skipped", "logout", "shared_plan", "scene_plan", "releases_plan", "shared_observe",
    "scene_observe", "releases_observe", "understand_compiled",
})
_ENUMS["role"].add("releases")
_ENUMS.update({"planner": {"deterministic", "model"}, "upstream_mode": {"scripted"}, "evaluation_scope": {"integration_wiring"}})


def _uuid(value):
    try:
        return str(UUID(value)) if isinstance(value, str) and len(value) == 36 else None
    except ValueError:
        return None


def _safe_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and 0 <= value <= 9007199254740991 and math.isfinite(value)


def _configuration(source):
    """独立解析白名单配置，不继承模型密钥、SDK 副本或任意出站域名。"""
    endpoint = source.get("LANGSMITH_ENDPOINT", "https://api.smith.langchain.com")
    endpoint = endpoint.rstrip("/") if isinstance(endpoint, str) else None
    project = source.get("LANGSMITH_PROJECT", "tango-development")
    key = source.get("LANGSMITH_API_KEY", "")
    workspace = source.get("LANGSMITH_WORKSPACE_ID") or None
    valid = (
        endpoint in _ENDPOINTS and isinstance(project, str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}", project)
        and _valid_key_format(key)
        and (workspace is None or _uuid(workspace))
    )
    return {"endpoint": endpoint, "project": project, "key": key, "workspace": workspace} if valid else None


def _valid_key_format(value):
    """拒绝误填模型密钥、已停用旧格式及模板；格式通过仍不代表服务端鉴权成功。"""
    match = re.fullmatch(r"lsv2_(?:pt|sk|tr)_([A-Za-z0-9_-]{16,480})", value) if isinstance(value, str) else None
    return bool(match and not match.group(1).lower().startswith((
        "your_", "replace_", "changeme", "placeholder", "example", "dummy_",
    )))


def safe_summary(value):
    """按字段契约重建摘要；未知字段及嵌套正文直接丢弃，不靠关键词猜测隐私。"""
    if not isinstance(value, dict):
        return {}
    result = {}
    for original_key, item in list(value.items())[:128]:
        key = _ALIASES.get(original_key, original_key)
        if key in _IDS and (ident := _uuid(item)):
            result[original_key] = ident
        elif key in _NUMBERS and _safe_number(item) and (
            isinstance(item, int) or key in {"durationMs", "elapsedMs", "totalDurationMs", "targetMinutes", "timeoutMs"}
        ):
            result[original_key] = item
        elif key in _BOOLS and isinstance(item, bool):
            result[original_key] = item
        elif key in _ENUMS and isinstance(item, str) and item in _ENUMS[key]:
            result[original_key] = item
        elif key in _MODEL_KEYS and isinstance(item, str) and re.fullmatch(r"qwen(?:[0-9]+(?:\.[0-9]+)?)?-(?:flash|plus|turbo|max|coder|vl|long|omni)(?:-[0-9]{4}-[0-9]{2}-[0-9]{2})?", item, re.ASCII):
            result[original_key] = item
        elif key == "ls_provider" and item == "dashscope":
            result[key] = item
        elif key == "errorCode" and isinstance(item, str) and item in _SAFE_ERRORS:
            result[original_key] = item
        elif key in ("intents", "capabilities", "skills") and isinstance(item, list):
            allowed = _INTENTS if key == "intents" else {"song.lookup", "artist.catalog", "playlist.find", "scene.playlist", "release.query"} if key == "skills" else _CAPABILITIES
            result[key] = [v for v in item[:16] if isinstance(v, str) and v in allowed]
        elif key == "checks" and isinstance(item, list):
            result[key] = [{"key": v["key"], "status": v["status"]} for v in item[:16]
                           if isinstance(v, dict) and isinstance(v.get("key"), str) and v["key"] in _ENUMS["check"]
                           and isinstance(v.get("status"), str) and v["status"] in {"satisfied", "unmet", "unverified", "not_requested"}]
        elif key in ("usage", "usage_metadata") and isinstance(item, dict):
            usage = {k: v for k, v in item.items() if k in {"input_tokens", "output_tokens", "total_tokens", "prompt_tokens", "completion_tokens"} and isinstance(v, int) and _safe_number(v)}
            if usage:
                result[key] = usage
    return result


def _stamp(milliseconds=None):
    if not _safe_number(milliseconds):
        milliseconds = time.time() * 1000
    try:
        return datetime.fromtimestamp(milliseconds / 1000, timezone.utc)
    except (ValueError, OverflowError, OSError):
        return datetime.now(timezone.utc)


def _order(stamp, ident):
    return stamp.strftime("%Y%m%dT%H%M%S%fZ") + ident


def _error_code(value):
    return value if isinstance(value, str) and value in _SAFE_ERRORS else "TRACE_OPERATION_FAILED"


class _ExportFailure(Exception):
    """只携带固定故障码和 HTTP 数字，不持有响应正文或原始请求。"""

    def __init__(self, code, status=None):
        super().__init__(code)
        self.code = code
        self.status = status


def _export_failure(error):
    """有界查看 SDK 的异常链，只读取类型和 HTTP 状态，不格式化原始异常。"""
    seen = set()
    network = False
    typed_code = None
    current = error
    for _ in range(8):
        if current is None or id(current) in seen:
            break
        seen.add(id(current))
        if isinstance(current, _ExportFailure):
            return current.code, current.status
        response = getattr(current, "response", None)
        status = getattr(response, "status_code", None)
        if isinstance(status, int) and not isinstance(status, bool) and 100 <= status <= 599:
            code = {401: "TRACE_AUTH_FAILED", 403: "TRACE_FORBIDDEN", 429: "TRACE_RATE_LIMITED"}.get(status, "TRACE_HTTP_FAILED")
            return code, status
        name = type(current).__name__
        if name in {"LangSmithAuthError", "LangSmithRateLimitError"}:
            typed_code = "TRACE_AUTH_FAILED" if name == "LangSmithAuthError" else "TRACE_RATE_LIMITED"
        if name in {"LangSmithConnectionError", "ConnectionError", "ConnectTimeout", "ReadTimeout", "Timeout", "TimeoutError"}:
            network = True
        current = current.__cause__ if current.__cause__ is not None else current.__context__
    return (typed_code, None) if typed_code else (("TRACE_NETWORK_FAILED", None) if network else ("TRACE_EXPORT_FAILED", None))


class LangSmithExporter:
    """SDK 只在发送线程构造；禁用 SDK 额外队列、重试与自动环境采集。"""

    def __init__(self, config):
        # 底层构造同样验证，组合 exporter 或直接调用不能绕开凭据与端点守卫。
        validated = _configuration({
            "LANGSMITH_ENDPOINT": config.get("endpoint"), "LANGSMITH_PROJECT": config.get("project"),
            "LANGSMITH_API_KEY": config.get("key"), "LANGSMITH_WORKSPACE_ID": config.get("workspace"),
        }) if isinstance(config, dict) else None
        if validated is None:
            raise ValueError("TRACE_CONFIGURATION_INVALID")
        self._config = validated
        self.client = None
        self.project = validated["project"]

    @classmethod
    def from_env(cls, env=None):
        """只解析观测专用配置；构造和关闭均不会主动启用或发送追踪。"""
        source = os.environ if env is None else env
        config = _configuration(source)
        if source.get("QUIET_LANGSMITH_ENABLED") != "1" or config is None:
            raise ValueError("TRACE_CONFIGURATION_INVALID")
        return cls(config)

    def _open(self):
        from langsmith import Client
        import requests
        from urllib3.util.retry import Retry

        config = self._config
        session = requests.Session()
        # 与模型传输保持一致，不隐式携带 netrc 身份或继承进程代理凭据。
        session.trust_env = False
        self.client = Client(
            api_urls={config["endpoint"]: config["key"]}, api_key=config["key"],
            workspace_id=config.get("workspace") or "", auto_batch_tracing=False,
            timeout_ms=2000, retry_config=Retry(total=0, connect=0, read=0, status=0, redirect=0),
            omit_traced_runtime_info=True, hide_inputs=safe_summary, hide_outputs=safe_summary,
            hide_metadata=safe_summary, tracing_sampling_rate=1.0,
            tracing_mode="langsmith", otel_enabled=False, disable_prompt_cache=True,
            session=session,
        )

    def _prepare(self, operation, payload):
        """末端再次重建信封；不会将原始 state、附件或自由文本带入批量请求。"""
        # 即使组合 exporter 在队列之后加字段，最终出站仍重建固定信封，拒绝附件和序列化 state。
        if operation not in ("create", "patch") or not _uuid(payload.get("id")):
            raise ValueError("TRACE_OPERATION_FAILED")
        if operation == "patch" and not isinstance(payload.get("end_time"), str):
            raise ValueError("TRACE_OPERATION_FAILED")
        data = {k: payload[k] for k in (
            "id", "trace_id", "parent_run_id", "dotted_order", "start_time", "end_time",
            "name", "run_type", "inputs", "outputs", "extra", "error",
        ) if k in payload}
        for field in ("trace_id", "parent_run_id"):
            if field in data and not _uuid(data[field]):
                raise ValueError("TRACE_OPERATION_FAILED")
        for field in ("start_time", "end_time"):
            if field in data:
                parsed = datetime.fromisoformat(data[field])
                if parsed.tzinfo is None:
                    raise ValueError("TRACE_OPERATION_FAILED")
                data[field] = parsed.astimezone(timezone.utc).isoformat()
        if "dotted_order" in data and not re.fullmatch(r"[0-9TZ.a-f-]{58,4096}", data["dotted_order"]):
            raise ValueError("TRACE_OPERATION_FAILED")
        if "name" in data and data["name"] not in _NAMES:
            data["name"] = "agent.step"
        if "run_type" in data and data["run_type"] not in ("chain", "llm", "tool"):
            data["run_type"] = "chain"
        if data.get("error"):
            data["error"] = _error_code(data["error"])
        data["inputs"] = safe_summary(data["inputs"]) if "inputs" in data else None
        if data["inputs"] is None:
            data.pop("inputs")
        if "outputs" in data:
            data["outputs"] = safe_summary(data["outputs"])
        if "extra" in data:
            data["extra"] = {"metadata": safe_summary(data["extra"].get("metadata"))}
        data["session_name"] = self.project
        if data.get("error") is None:
            data.pop("error", None)
        return data

    def export(self, operation, payload):
        self.export_many([(operation, payload)])

    def export_many(self, events):
        """一个有界请求合并同批创建与更新；同一 run 的最后输出和结束状态优先。"""
        if not isinstance(events, (tuple, list)) or not 1 <= len(events) <= 32:
            raise ValueError("TRACE_OPERATION_FAILED")
        creates, patches = OrderedDict(), OrderedDict()

        def merge(previous, incoming):
            if "extra" in incoming:
                metadata = {**previous.get("extra", {}).get("metadata", {}), **incoming["extra"].get("metadata", {})}
                incoming = {**incoming, "extra": {"metadata": metadata}}
            previous.update(incoming)

        for operation, payload in events:
            data = self._prepare(operation, payload)
            ident = data["id"]
            if operation == "create":
                if ident in creates:
                    merge(creates[ident], data)
                else:
                    creates[ident] = data
                if ident in patches:
                    merge(creates[ident], patches.pop(ident))
            elif ident in creates:
                merge(creates[ident], data)
            elif ident in patches:
                merge(patches[ident], data)
            else:
                patches[ident] = data
        batch = {key: list(values.values()) for key, values in (("post", creates), ("patch", patches)) if values}
        encoded = json.dumps(batch, allow_nan=False).encode("utf-8")
        if len(encoded) > 256 * 1024:
            raise ValueError("TRACE_OPERATION_FAILED")
        if self.client is None:
            self._open()
        response = self.client.request_with_retries(
            "POST", "/runs/batch", data=encoded,
            headers={"Content-Type": "application/json"}, stop_after_attempt=1,
            allow_redirects=False,
        )
        if not 200 <= response.status_code < 300:
            code = {401: "TRACE_AUTH_FAILED", 403: "TRACE_FORBIDDEN", 429: "TRACE_RATE_LIMITED"}.get(response.status_code, "TRACE_HTTP_FAILED")
            raise _ExportFailure(code, response.status_code)

    def close(self, timeout=0):
        if self.client is not None:
            self.client.close(timeout=timeout)


class _Span:
    """仅保存可供调用方填写的摘要；退出时再次投影，不保存模型或工具原文。"""

    def __init__(self, ident=None, dotted_order=None):
        self.id = ident
        self.dotted_order = dotted_order
        self.outputs = {}
        self.metadata = {}
        self.error_code = None


class AgentTracing:
    """以有限队列隔离观测平台；队列丢弃、失败和未完成通过健康计数显式暴露。"""

    def __init__(self, *, enabled=False, exporter=None, queue_capacity=256, config=None, configuration_error=False):
        self.enabled = bool(enabled)
        self._exporter = exporter
        self._config = config
        self._queue = Queue(maxsize=max(1, min(int(queue_capacity), 2048)))
        self._lock = Lock()
        self._stop = Event()
        self._closed = False
        self._counts = Counter()
        self._last_error = "TRACE_CONFIGURATION_INVALID" if configuration_error else None
        self._last_http_status = None
        self._last_failed_run_id = None
        self._last_failed_operation = None
        self._last_failed_events = []
        self._last_batch_size = 0
        self._begun = OrderedDict()
        self._thread = None
        if self.enabled:
            self._thread = Thread(target=self._work, name="tango-safe-tracing", daemon=True)
            self._thread.start()

    @classmethod
    def from_env(cls, env=None, *, exporter=None, queue_capacity=256):
        """默认关闭；仅项目专用开关的精确值 1 授权向外部发送摘要。"""
        source = os.environ if env is None else env
        if source.get("QUIET_LANGSMITH_ENABLED") != "1":
            return cls()
        config = _configuration(source)
        if config is None and exporter is None:
            return cls(configuration_error=True)
        return cls(enabled=True, exporter=exporter, queue_capacity=queue_capacity, config=config)

    def new_context(self, seed, metadata=None, start_ms=None):
        """根 ID 由后台种子派生；种子不出站，持久字段仍须由服务端验证归属后恢复。"""
        ident = str(uuid5(_NAMESPACE, str(seed)))
        start = _stamp(start_ms)
        return {
            "schemaVersion": 1, "root_id": ident, "trace_id": ident,
            "start_time": start.isoformat(), "dotted_order": _order(start, ident),
            "metadata": safe_summary(metadata),
        }

    def _context(self, context):
        if not isinstance(context, dict) or context.get("schemaVersion") != 1:
            return None
        ident = _uuid(context.get("root_id"))
        if not ident or context.get("trace_id") != ident:
            return None
        try:
            stamp = datetime.fromisoformat(context["start_time"])
            if stamp.tzinfo is None or context.get("dotted_order") != _order(stamp, ident):
                return None
        except (KeyError, TypeError, ValueError, OverflowError):
            return None
        return {
            "id": ident, "trace_id": ident, "start_time": stamp.isoformat(),
            "dotted_order": context["dotted_order"], "extra": {"metadata": safe_summary(context.get("metadata"))},
        }

    def begin_root(self, context):
        root = self._context(context)
        if not self.enabled or root is None:
            return False
        with self._lock:
            if root["id"] in self._begun:
                return True
            self._begun[root["id"]] = True
            if len(self._begun) > 1024:
                self._begun.popitem(last=False)
        return self._enqueue("create", {**root, "name": "agent.conversation", "run_type": "chain", "inputs": {}})

    def finish_root(self, context, outputs=None, error_code=None):
        root = self._context(context)
        if not self.enabled or root is None:
            return False
        return self._enqueue("patch", {
            **root, "end_time": _stamp().isoformat(), "outputs": safe_summary(outputs),
            "error": _error_code(error_code) if error_code else None,
        })

    def update_root(self, context, outputs=None, metadata=None):
        """用已结束的进度子 span 展示等待状态；云端根只在明确终态时结束。"""
        root = self._context(context)
        if not self.enabled or root is None or self._closed:
            return False
        ident, stamp = str(uuid4()), _stamp()
        info = {**root["extra"]["metadata"], **safe_summary(metadata)}
        common = {
            "id": ident, "trace_id": root["trace_id"], "parent_run_id": root["id"],
            "dotted_order": root["dotted_order"] + "." + _order(stamp, ident),
            "extra": {"metadata": info},
        }
        created = self._enqueue("create", {
            **common, "name": "agent.progress", "run_type": "chain", "inputs": {},
            "start_time": stamp.isoformat(),
        })
        finished = self._enqueue("patch", {
            **common, "end_time": stamp.isoformat(),
            "outputs": {**safe_summary(outputs), "root_complete": False},
        })
        return created and finished

    @contextmanager
    def span(self, context, name, run_type="chain", inputs=None, metadata=None, start_ms=None):
        """只关联同一后台根任务；ContextVar 用于进程内嵌套，不充当跨请求授权。"""
        root = self._context(context)
        if not self.enabled or root is None or self._closed:
            yield _Span()
            return
        previous = _ACTIVE.get()
        parent = previous[2] if previous and previous[0] is self and previous[1]["trace_id"] == root["trace_id"] else None
        ident = str(uuid4())
        stamp = _stamp(start_ms)
        dotted = (parent.dotted_order if parent else root["dotted_order"]) + "." + _order(stamp, ident)
        span = _Span(ident, dotted)
        span.metadata = safe_summary(metadata)
        payload = {
            "id": ident, "trace_id": root["trace_id"], "parent_run_id": parent.id if parent else root["id"],
            "dotted_order": dotted, "start_time": stamp.isoformat(), "name": name if isinstance(name, str) and name in _NAMES else "agent.step",
            "run_type": run_type if run_type in ("chain", "tool", "llm") else "chain",
            "inputs": safe_summary(inputs), "extra": {"metadata": safe_summary(span.metadata)},
        }
        self._enqueue("create", payload)
        token = _ACTIVE.set((self, context, span))
        try:
            yield span
        except BaseException as error:
            span.error_code = span.error_code or _error_code(getattr(error, "code", None))
            raise
        finally:
            _ACTIVE.reset(token)
            self._enqueue("patch", {
                "id": ident, "trace_id": root["trace_id"], "parent_run_id": payload["parent_run_id"],
                "dotted_order": dotted, "end_time": _stamp().isoformat(),
                "outputs": safe_summary(span.outputs), "extra": {"metadata": safe_summary(span.metadata)},
                "error": _error_code(span.error_code) if span.error_code else None,
            })

    def current_span(self, name, run_type="chain", inputs=None, metadata=None):
        active = _ACTIVE.get()
        return self.span(active[1] if active and active[0] is self else None, name, run_type, inputs, metadata)

    def _enqueue(self, operation, payload):
        if not self.enabled or self._closed:
            return False
        try:
            # 队列只拿投影后的独立 JSON 副本，调用方后改 dict 不能扩大出站范围。
            self._queue.put_nowait((operation, json.loads(json.dumps(payload, allow_nan=False))))
        except Full:
            with self._lock:
                self._counts["dropped"] += 1
                self._last_error = "TRACE_QUEUE_FULL"
            return False
        with self._lock:
            self._counts["queued"] += 1
        return True

    def _work(self):
        exporter = self._exporter
        initialization_failed = False
        try:
            while not self._stop.is_set():
                try:
                    first = self._queue.get(timeout=0.05)
                except Empty:
                    continue
                events = [first]
                try:
                    if initialization_failed:
                        raise RuntimeError()
                    if exporter is None:
                        try:
                            exporter = LangSmithExporter(self._config)
                            self._exporter = exporter
                        except Exception:
                            initialization_failed = True
                            raise
                    if callable(getattr(exporter, "export_many", None)):
                        while len(events) < 32:
                            try:
                                events.append(self._queue.get_nowait())
                            except Empty:
                                break
                        exporter.export_many(events)
                    else:
                        exporter.export(*first)
                    with self._lock:
                        self._counts["exported"] += len(events)
                        self._counts["exportBatches"] += 1
                        self._last_batch_size = len(events)
                except Exception as error:
                    code, status = _export_failure(error)
                    with self._lock:
                        self._counts["failed"] += len(events)
                        self._counts["failedBatches"] += 1
                        self._counts["exportBatches"] += 1
                        self._counts["error:" + code] += len(events)
                        self._last_error = code
                        self._last_http_status = status
                        self._last_batch_size = len(events)
                        self._last_failed_events = [
                            {"runId": _uuid(payload.get("id")), "operation": operation}
                            for operation, payload in events[:8]
                        ]
                        self._last_failed_run_id = _uuid(events[-1][1].get("id"))
                        self._last_failed_operation = events[-1][0]
                        count = self._counts["failedBatches"]
                    if count <= 3:
                        _LOGGER.warning("agent_trace_export_failed code=%s http_status=%s", code, status)
                finally:
                    for _ in events:
                        self._queue.task_done()
        finally:
            if exporter is not None and hasattr(exporter, "close"):
                try:
                    exporter.close(timeout=0)
                except Exception:
                    with self._lock:
                        self._last_error = "TRACE_EXPORT_FAILED"

    def health(self):
        with self._lock:
            return {
                "enabled": self.enabled, "configured": bool(self._config or self._exporter),
                "queued": self._counts["queued"], "exported": self._counts["exported"],
                "failed": self._counts["failed"], "dropped": self._counts["dropped"],
                "pending": self._queue.unfinished_tasks,
                "workerAlive": bool(self._thread and self._thread.is_alive()),
                "lastErrorCode": self._last_error,
                "lastHttpStatus": self._last_http_status,
                "lastFailedRunId": self._last_failed_run_id,
                "lastFailedOperation": self._last_failed_operation,
                "lastFailedEvents": list(self._last_failed_events),
                "lastBatchSize": self._last_batch_size,
                "exportBatches": self._counts["exportBatches"],
                "errorCounts": {k.removeprefix("error:"): v for k, v in self._counts.items() if k.startswith("error:")},
            }

    def flush(self, timeout=5):
        """有界等待队列处理；返回值只代表已处理，不保证每项成功上传。"""
        deadline = time.monotonic() + max(0, timeout)
        with self._queue.all_tasks_done:
            while self._queue.unfinished_tasks:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._queue.all_tasks_done.wait(remaining)
        return True

    def close(self, timeout=5):
        """超时后丢弃尚未发送的摘要；不阻塞业务退出，不在重启时补发。"""
        deadline = time.monotonic() + max(0, timeout)
        self._closed = True
        drained = self.flush(max(0, deadline - time.monotonic()))
        self._stop.set()
        while True:
            try:
                self._queue.get_nowait()
                self._queue.task_done()
                with self._lock:
                    self._counts["dropped"] += 1
            except Empty:
                break
        if self._thread:
            self._thread.join(max(0, deadline - time.monotonic()))
        return drained and not (self._thread and self._thread.is_alive())


@contextmanager
def current_span(name, run_type="chain", inputs=None, metadata=None):
    """模型传输等底层能力只挂接已有受控根，不自行发起包含原文的新 trace。"""
    active = _ACTIVE.get()
    if active is None:
        yield _Span()
    else:
        with active[0].span(active[1], name, run_type, inputs, metadata) as span:
            yield span


@contextmanager
def disable_automatic_tracing():
    """关闭 LangGraph 对原始 state 的自动采集；手工摘要仍由独立开关控制。"""
    from langsmith import tracing_context

    with tracing_context(enabled=False):
        yield
