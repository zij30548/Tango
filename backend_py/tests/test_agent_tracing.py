"""观测边界验收：默认无出站、完整关联、摘要白名单、失败隔离及真实 SDK 请求替身。"""

import asyncio
from copy import deepcopy
import json
from threading import Event, get_ident
import time
from types import SimpleNamespace
from uuid import uuid4

import pytest

from tango_backend.agent_tracing import (
    AgentTracing, LangSmithExporter, current_span, disable_automatic_tracing, safe_summary,
)
from tango_backend.errors import ApiError

_FIXTURE_KEY = "lsv2_tr_0123456789abcdef0123456789abcdef"


class Collector:
    """仅收集独立内存事件，允许注入堵塞和失败，不触及云服务。"""

    def __init__(self, *, fail=False, gate=None):
        self.rows = []
        self.threads = []
        self.fail = fail
        self.gate = gate
        self.entered = Event()
        self.closed = False

    def export(self, operation, payload):
        self.entered.set()
        if self.gate:
            self.gate.wait(2)
        if self.fail:
            raise RuntimeError("Bearer PRIVATE_COOKIE sk-PRIVATE_EXCEPTION https://private.invalid")
        self.rows.append((operation, deepcopy(payload)))
        self.threads.append(get_ident())

    def close(self, timeout=0):
        self.closed = True


@pytest.fixture
def observed():
    collector = Collector()
    observer = AgentTracing(enabled=True, exporter=collector)
    yield observer, collector
    observer.close(timeout=1)


def test_default_disabled_ignores_standard_auto_tracing_flags_and_never_constructs_sdk(monkeypatch):
    import langsmith

    monkeypatch.setattr(langsmith, "Client", lambda **kwargs: pytest.fail("默认关闭不得构造 SDK"))
    observer = AgentTracing.from_env({"LANGSMITH_TRACING": "true", "LANGSMITH_API_KEY": "private"})
    context = observer.new_context("secret conversation seed")
    assert observer.begin_root(context) is False
    with observer.span(context, "conversation.route") as span:
        span.outputs = {"status": "completed"}
    assert observer.update_root(context, {"status": "awaiting_task"}) is False
    assert observer.finish_root(context, {"status": "completed"}) is False
    assert observer.health()["workerAlive"] is False
    assert observer.health()["queued"] == 0
    assert observer.close(timeout=0) is True


@pytest.mark.parametrize("config", [
    {"LANGSMITH_API_KEY": "secret", "LANGSMITH_ENDPOINT": "https://evil.invalid"},
    {"LANGSMITH_API_KEY": "secret", "LANGSMITH_ENDPOINT": "https://api.smith.langchain.com?secret=1"},
    {"LANGSMITH_API_KEY": "secret", "LANGSMITH_PROJECT": "聊天原文不能当项目名"},
    {"LANGSMITH_API_KEY": "secret", "LANGSMITH_WORKSPACE_ID": "Cookie private"},
    {"LANGSMITH_API_KEY": "secret\nAuthorization: test"},
    {"LANGSMITH_ENDPOINT": None},
])
def test_invalid_configuration_disables_export_without_echoing_secrets(config):
    observer = AgentTracing.from_env({"QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": _FIXTURE_KEY, **config})
    assert observer.health()["enabled"] is False
    assert observer.health()["lastErrorCode"] == "TRACE_CONFIGURATION_INVALID"
    assert "secret" not in json.dumps(observer.health())


def test_context_is_json_persistent_deterministic_and_does_not_expose_seed(observed):
    observer, _ = observed
    ident = str(uuid4())
    context = observer.new_context("PRIVATE_USER_DEVICE_MESSAGE", {"message_id": ident, "prompt": "PRIVATE_PROMPT"}, 1700000000000)
    assert json.loads(json.dumps(context)) == context
    assert context == observer.new_context("PRIVATE_USER_DEVICE_MESSAGE", {"message_id": ident}, 1700000000000)
    assert context["root_id"] == context["trace_id"]
    assert context["metadata"] == {"message_id": ident}
    assert "PRIVATE" not in json.dumps(context)


def test_root_update_stays_open_and_later_preview_finishes_same_root(observed):
    observer, collector = observed
    context = observer.new_context("route")
    assert observer.begin_root(context)
    assert observer.begin_root(context)
    assert observer.update_root(context, {"status": "awaiting_task", "action_count": 1})
    assert observer.finish_root(json.loads(json.dumps(context)), {"status": "preview", "track_count": 6})
    assert observer.finish_root(context, {"status": "adopted", "applied": True})
    assert observer.flush(timeout=1)
    assert [op for op, _ in collector.rows] == ["create", "create", "patch", "patch", "patch"]
    assert collector.rows[1][1]["name"] == "agent.progress"
    assert collector.rows[1][1]["parent_run_id"] == context["root_id"]
    assert collector.rows[2][1]["outputs"] == {"status": "awaiting_task", "action_count": 1, "root_complete": False}
    root_rows = [(op, row) for op, row in collector.rows if row["id"] == context["root_id"]]
    assert [op for op, _ in root_rows] == ["create", "patch", "patch"]
    assert "end_time" not in root_rows[0][1]
    assert root_rows[1][1]["outputs"]["status"] == "preview"
    assert all("end_time" in row for op, row in collector.rows if op == "patch")


def test_nested_spans_are_parented_and_model_usage_is_not_fabricated(observed):
    observer, collector = observed
    context = observer.new_context("nested")
    observer.begin_root(context)
    with observer.span(context, "task.research") as parent:
        with current_span("model.request", "llm", inputs={"model": "qwen3.7-flash-2026-07-15"}) as child:
            child.outputs = {"usage_metadata": {"input_tokens": 13, "output_tokens": None}, "content": "PRIVATE"}
            child.metadata = {"purpose": "research"}
    assert observer.flush(timeout=1)
    creates = [row for op, row in collector.rows if op == "create"]
    assert creates[1]["parent_run_id"] == context["root_id"]
    assert creates[2]["parent_run_id"] == parent.id
    assert creates[2]["dotted_order"].startswith(parent.dotted_order + ".")
    child_patch = next(row for op, row in collector.rows if op == "patch" and row["id"] == child.id)
    assert child_patch["outputs"] == {"usage_metadata": {"input_tokens": 13}}
    assert "output_tokens" not in json.dumps(child_patch)


def test_span_with_no_or_malformed_context_is_noop(observed):
    observer, collector = observed
    for context in (None, {}, {"schemaVersion": 1, "root_id": "secret"}):
        with observer.span(context, "task.run") as span:
            assert span.id is None
        assert observer.begin_root(context) is False
        assert observer.finish_root(context) is False
    with current_span("model.request") as span:
        assert span.id is None
    assert observer.flush(timeout=1)
    assert collector.rows == []


def test_safe_summary_rejects_raw_data_type_smuggling_and_unbounded_usage():
    ident = str(uuid4())
    value = safe_summary({
        "task_id": ident, "previous_trace_id": ident, "state_version": 2, "fixture": True,
        "upstream_mode": "scripted", "evaluation_scope": "integration_wiring",
        "status": "completed", "planner": "model", "intents": ["scene_research", "PRIVATE"],
        "capabilities": ["music.task", "PRIVATE"], "model": "qwen3.7-plus",
        "actual_model": "qwen-secret-PRIVATE", "error_code": {"raw": "PRIVATE"},
        "track_count": True, "source_count": 10 ** 1000, "elapsed_ms": float("inf"),
        "usage_metadata": {"input_tokens": 10, "output_tokens": None, "raw": "PRIVATE"},
        "checks": [{"key": "locks", "status": "satisfied", "detail": "PRIVATE"},
                   {"key": "style", "status": "unverified"}, {"key": "PRIVATE", "status": "satisfied"}],
        "prompt": "PRIVATE", "messages": ["PRIVATE"], "headers": {"Cookie": "PRIVATE"},
        "error": "PRIVATE", "environment": {"KEY": "PRIVATE"}, "serialized": {"state": "PRIVATE"},
    })
    assert value["task_id"] == ident and value["previous_trace_id"] == ident and value["fixture"] is True
    assert value["checks"] == [{"key": "locks", "status": "satisfied"}, {"key": "style", "status": "unverified"}]
    assert value["usage_metadata"] == {"input_tokens": 10}
    assert "PRIVATE" not in json.dumps(value)
    assert "track_count" not in value and "source_count" not in value


def test_business_exceptions_propagate_but_only_fixed_code_is_exported(observed):
    observer, collector = observed
    context = observer.new_context("error")
    with pytest.raises(ApiError):
        with observer.span(context, "model.request", "llm"):
            raise ApiError(504, "MODEL_TIMEOUT", "PRIVATE_COOKIE Authorization: private")
    with pytest.raises(RuntimeError):
        with observer.span(context, "task.run"):
            raise RuntimeError("PRIVATE_EXCEPTION")
    assert observer.flush(timeout=1)
    errors = [row["error"] for op, row in collector.rows if op == "patch"]
    assert errors == ["MODEL_TIMEOUT", "TRACE_OPERATION_FAILED"]
    assert "PRIVATE" not in json.dumps(collector.rows)


def test_exporter_failure_is_counted_without_leaking_error_or_changing_business(caplog):
    collector = Collector(fail=True)
    observer = AgentTracing(enabled=True, exporter=collector)
    context = observer.new_context("offline")
    for _ in range(4):
        observer.finish_root(context, {"status": "preview"})
    assert observer.flush(timeout=1)
    assert observer.health()["failed"] == 4
    assert observer.health()["exported"] == 0
    assert observer.health()["lastErrorCode"] == "TRACE_EXPORT_FAILED"
    assert len(caplog.records) == 3
    assert "PRIVATE" not in caplog.text
    assert observer.close(timeout=1)


def test_slow_export_is_on_worker_and_queue_pressure_and_close_are_bounded():
    gate = Event()
    collector = Collector(gate=gate)
    observer = AgentTracing(enabled=True, exporter=collector, queue_capacity=1)
    context = observer.new_context("pressure")
    observer.begin_root(context)
    assert collector.entered.wait(1)
    start = time.monotonic()
    assert observer.begin_root(observer.new_context("second-pressure-root"))
    assert observer.finish_root(context, {"status": "preview"}) is False
    assert time.monotonic() - start < 0.1
    assert observer.health()["dropped"] == 1
    assert observer.close(timeout=0.01) is False
    assert time.monotonic() - start < 0.3
    gate.set()
    observer._thread.join(1)
    assert collector.threads == [observer._thread.ident]
    assert collector.threads[0] != get_ident()
    assert observer.health()["pending"] == 0
    assert observer.health()["dropped"] == 2


@pytest.mark.asyncio
async def test_concurrent_roots_cannot_inherit_each_others_parent(observed):
    observer, collector = observed
    first, second = observer.new_context("first"), observer.new_context("second")

    async def run(context):
        with observer.span(context, "task.run") as parent:
            await asyncio.sleep(0)
            with current_span("model.request", "llm") as child:
                return parent.id, child.id

    rows = await asyncio.gather(run(first), run(second))
    assert observer.flush(timeout=1)
    creates = {row["id"]: row for op, row in collector.rows if op == "create"}
    for context, (parent, child) in zip((first, second), rows):
        assert creates[child]["parent_run_id"] == parent
        assert creates[child]["trace_id"] == context["root_id"]


def test_rebuilt_tool_wait_span_uses_persisted_dispatch_time(observed):
    observer, collector = observed
    context = observer.new_context("tool", start_ms=1700000000000)
    with observer.span(context, "task.tool", "tool", start_ms=1700000001000):
        pass
    assert observer.flush(timeout=1)
    assert collector.rows[0][1]["start_time"] == "2023-11-14T22:13:21+00:00"
    assert collector.rows[1][1]["end_time"] > collector.rows[0][1]["start_time"]


def test_automatic_langgraph_tracing_is_disabled_even_with_global_env(monkeypatch):
    from langsmith import utils

    monkeypatch.setenv("LANGSMITH_TRACING", "true")
    monkeypatch.setenv("LANGCHAIN_TRACING_V2", "true")
    with disable_automatic_tracing():
        assert utils.tracing_is_enabled() is False


def test_real_sdk_batch_ingestion_preserves_safe_payload_and_pending_root(monkeypatch):
    import requests
    from langsmith._internal import _profiles

    monkeypatch.setattr(_profiles, "load_profile_client_config", lambda: SimpleNamespace(
        api_url=None, api_key=None, workspace_id=None, has_oauth=False, oauth_access_token=None,
    ))
    for key in ("LANGSMITH_ENDPOINT", "LANGCHAIN_ENDPOINT", "LANGSMITH_RUNS_ENDPOINTS"):
        monkeypatch.delenv(key, raising=False)
    requests_seen = []

    def send(session, request, **kwargs):
        requests_seen.append((request, kwargs))
        response = requests.Response()
        response.status_code = 202
        response._content = b"{}"
        response.request = request
        return response

    monkeypatch.setattr(requests.Session, "send", send)
    monkeypatch.setenv("HTTPS_PROXY", "http://private-proxy.invalid:9000")
    monkeypatch.setattr(requests.sessions, "get_netrc_auth", lambda *args, **kwargs: pytest.fail("不得继承 netrc 身份"))
    exporter = LangSmithExporter.from_env({
        "QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": _FIXTURE_KEY,
        "LANGSMITH_ENDPOINT": "https://eu.api.smith.langchain.com", "LANGSMITH_PROJECT": "tango-fixture",
    })
    assert exporter.client is None and requests_seen == []
    collector = Collector()
    observer = AgentTracing(enabled=True, exporter=collector)
    context = observer.new_context("sdk", {"fixture": True, "prompt": "PRIVATE"})
    observer.begin_root(context)
    observer.update_root(context, {"status": "awaiting_task", "prompt": "PRIVATE"})
    observer.finish_root(context, {"status": "preview"})
    assert observer.flush(timeout=2)
    assert observer.health()["failed"] == 0
    assert observer.close(timeout=1)
    for operation, payload in collector.rows:
        exporter.export(operation, payload)
    exporter.close()
    assert [r.method for r, _ in requests_seen] == ["POST"] * 4
    assert all(r.url.endswith("/runs/batch") for r, _ in requests_seen)
    assert {r.url.split("/runs")[0] for r, _ in requests_seen} == {"https://eu.api.smith.langchain.com"}
    assert all(kwargs["allow_redirects"] is False for _, kwargs in requests_seen)
    assert all(kwargs["timeout"] == (2.0, 2.0) for _, kwargs in requests_seen)
    assert all(r.headers["x-api-key"] == _FIXTURE_KEY for r, _ in requests_seen)
    assert all(not kwargs.get("proxies") for _, kwargs in requests_seen)
    assert all("Authorization" not in r.headers and "Cookie" not in r.headers for r, _ in requests_seen)
    batches = [json.loads(r.body) for r, _ in requests_seen]
    assert [set(batch) for batch in batches] == [{"post"}, {"post"}, {"patch"}, {"patch"}]
    payloads = [batch[key][0] for batch, key in zip(batches, ("post", "post", "patch", "patch"))]
    assert payloads[0]["session_name"] == "tango-fixture"
    assert payloads[1]["name"] == "agent.progress" and "end_time" not in payloads[1]
    assert "end_time" in payloads[2] and payloads[2]["outputs"]["root_complete"] is False
    assert payloads[3]["id"] == context["root_id"] and payloads[3]["outputs"]["status"] == "preview"
    assert "PRIVATE" not in json.dumps(payloads)
    assert "runtime" not in json.dumps(payloads)


def test_sdk_outer_envelope_drops_attachments_state_and_raw_exception(monkeypatch):
    exporter = LangSmithExporter({"project": "owned", "key": _FIXTURE_KEY, "endpoint": "https://api.smith.langchain.com"})
    sent = []
    exporter.client = SimpleNamespace(request_with_retries=lambda *args, **kwargs: (
        sent.append((args, kwargs)) or SimpleNamespace(status_code=202)))
    exporter.export("create", {
        "id": str(uuid4()), "name": "PRIVATE_NAME", "run_type": "PRIVATE_TYPE",
        "inputs": {"prompt": "PRIVATE"}, "extra": {"metadata": {"fixture": True, "raw": "PRIVATE"}, "runtime": {"PRIVATE": True}},
        "error": "PRIVATE_COOKIE", "serialized": {"state": "PRIVATE"}, "attachments": {"file": "PRIVATE"},
    })
    payload = json.loads(sent[0][1]["data"])["post"][0]
    assert payload["name"] == "agent.step" and payload["run_type"] == "chain"
    assert payload["error"] == "TRACE_OPERATION_FAILED"
    assert payload["extra"] == {"metadata": {"fixture": True}}
    assert "PRIVATE" not in json.dumps(payload)


def test_real_sdk_does_not_retry_or_log_private_response_on_cloud_failure(monkeypatch, caplog):
    import requests
    from langsmith._internal import _profiles

    monkeypatch.setattr(_profiles, "load_profile_client_config", lambda: SimpleNamespace(
        api_url=None, api_key=None, workspace_id=None, has_oauth=False, oauth_access_token=None,
    ))
    for key in ("LANGSMITH_ENDPOINT", "LANGCHAIN_ENDPOINT", "LANGSMITH_RUNS_ENDPOINTS"):
        monkeypatch.delenv(key, raising=False)
    attempts = []

    def send(session, request, **kwargs):
        attempts.append(request)
        response = requests.Response()
        response.status_code = 429
        response._content = b'{"detail":"PRIVATE_KEY PRIVATE_COOKIE"}'
        response.headers["retry-after"] = "60"
        response.request = request
        return response

    monkeypatch.setattr(requests.Session, "send", send)
    observer = AgentTracing.from_env({"QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": _FIXTURE_KEY})
    observer.begin_root(observer.new_context("offline-rate-limit"))
    assert observer.flush(timeout=1)
    assert observer.health()["failed"] == 1
    assert len(attempts) == 1
    assert "PRIVATE" not in caplog.text
    assert observer.close(timeout=1)


def test_usage_tokens_require_known_integers_without_derived_total():
    result = safe_summary({"usage_metadata": {"input_tokens": 4, "output_tokens": 2.5, "total_tokens": None}})
    assert result == {"usage_metadata": {"input_tokens": 4}}


@pytest.mark.parametrize("status,expected", [(401, "TRACE_AUTH_FAILED"), (403, "TRACE_FORBIDDEN"), (429, "TRACE_RATE_LIMITED"), (500, "TRACE_HTTP_FAILED")])
def test_sdk_wrapped_http_errors_keep_only_fixed_classification_and_numeric_status(status, expected, caplog):
    import requests

    response = requests.Response()
    response.status_code = status
    response._content = b"PRIVATE_RESPONSE Cookie=PRIVATE_KEY"
    cause = requests.HTTPError("PRIVATE_ERROR", response=response)
    outer = RuntimeError("PRIVATE_WRAPPER")
    outer.__cause__ = cause

    class FailingExporter:
        def export(self, operation, payload):
            raise outer

    observer = AgentTracing(enabled=True, exporter=FailingExporter())
    observer.begin_root(observer.new_context("http-failure"))
    assert observer.flush(timeout=1)
    health = observer.health()
    assert health["lastErrorCode"] == expected
    assert health["lastHttpStatus"] == status
    assert health["errorCounts"] == {expected: 1}
    assert "PRIVATE" not in json.dumps(health) and "PRIVATE" not in caplog.text
    assert observer.close(timeout=1)


def test_network_failure_has_fixed_code_and_unknown_http_status(caplog):
    class OfflineExporter:
        def export(self, operation, payload):
            raise TimeoutError("PRIVATE_PROXY Cookie=PRIVATE")

    observer = AgentTracing(enabled=True, exporter=OfflineExporter())
    observer.begin_root(observer.new_context("network-failure"))
    assert observer.flush(timeout=1)
    assert observer.health()["lastErrorCode"] == "TRACE_NETWORK_FAILED"
    assert observer.health()["lastHttpStatus"] is None
    assert "PRIVATE" not in caplog.text
    assert observer.close(timeout=1)


@pytest.mark.parametrize("key", [
    "sk-0123456789abcdef0123456789abcdef", "https://secret.invalid/private-key",
    "${LANGSMITH_API_KEY}", "lsv2_pt_${LANGSMITH_API_KEY}", "lsv2_tr_",
    "lsv2_tr_your_api_key_here", "ls__legacy0123456789abcdef",
    "lsv2_bad_0123456789abcdef0123456789abcdef", "lsv2_pt_<replace-with-real-key>",
])
def test_wrong_service_or_placeholder_key_cannot_construct_sdk_or_send(key, monkeypatch):
    import langsmith
    import requests

    monkeypatch.setattr(langsmith, "Client", lambda **kwargs: pytest.fail("无效格式不得构造 SDK"))
    monkeypatch.setattr(requests.Session, "send", lambda *args, **kwargs: pytest.fail("无效格式不得发网"))
    env = {"QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": key}
    observer = AgentTracing.from_env(env)
    assert observer.health()["enabled"] is False
    assert observer.health()["lastErrorCode"] == "TRACE_CONFIGURATION_INVALID"
    with pytest.raises(ValueError, match="^TRACE_CONFIGURATION_INVALID$"):
        LangSmithExporter.from_env(env)
    with pytest.raises(ValueError, match="^TRACE_CONFIGURATION_INVALID$"):
        LangSmithExporter({"key": key, "project": "owned", "endpoint": "https://api.smith.langchain.com"})
    assert key not in json.dumps(observer.health())


@pytest.mark.parametrize("kind", ["tr", "pt", "sk"])
def test_all_three_current_key_formats_are_accepted_without_automatic_network(kind, monkeypatch):
    import langsmith

    monkeypatch.setattr(langsmith, "Client", lambda **kwargs: pytest.fail("尚未提交事件不得构造 SDK"))
    key = f"lsv2_{kind}_0123456789abcdef0123456789abcdef"
    env = {"QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": key}
    exporter = LangSmithExporter.from_env(env)
    assert exporter.client is None
    observer = AgentTracing.from_env(env)
    assert observer.health()["enabled"] is True
    assert observer.health()["queued"] == 0
    assert observer.close(timeout=0.2)


def test_all_registered_task_skills_survive_summary_projection():
    from tango_backend.task_schema import TASK_SKILLS

    for skill in TASK_SKILLS:
        assert safe_summary({"skill": skill, "skillId": skill}) == {"skill": skill, "skillId": skill}


def test_actual_fixed_checkpoint_names_survive_without_opening_arbitrary_text():
    for checkpoint in (
        "releases_planned", "releases_observed", "catalog_supplement_failed", "observer", "observer_degraded",
        "tool_timeout", "clarify_waiting", "understand_invalid", "supplement_skipped", "logout",
        "shared_plan", "scene_plan", "releases_plan", "shared_observe", "scene_observe", "releases_observe",
    ):
        assert safe_summary({"checkpoint": checkpoint}) == {"checkpoint": checkpoint}
    assert safe_summary({"checkpoint": "PRIVATE_CHECKPOINT_RAW"}) == {}


@pytest.mark.parametrize("key", ["catalog_supplement_failed", "observer", "observer_degraded"])
def test_fixed_degradation_checks_keep_unknown_status_without_detail(key):
    assert safe_summary({"checks": [{"key": key, "status": "unverified", "detail": "PRIVATE_RAW"}]}) == {
        "checks": [{"key": key, "status": "unverified"}],
    }


@pytest.mark.parametrize("code", [
    "TASK_BUDGET", "AI_DAILY_LIMIT", "AI_GLOBAL_DAILY_LIMIT", "AI_BUSY", "TASK_LEASE_LOST",
    "TASK_WRITE_CONFLICT", "TASK_CLAIMED", "STALE_TASK_STEP", "TOOL_LEASE_EXPIRED", "AUTH_REQUIRED", "INVALID_TASK_MODEL",
    "LOOKUP_CONTEXT_STALE", "INVALID_LOOKUP_CONTEXT",
])
def test_fixed_business_failure_codes_remain_diagnosable_without_exception_text(code, observed):
    observer, collector = observed
    with pytest.raises(ApiError):
        with observer.span(observer.new_context("fixed-code-" + code), "task.run"):
            raise ApiError(409, code, "PRIVATE_RESPONSE_BODY")
    assert observer.flush(timeout=1)
    assert collector.rows[-1][1]["error"] == code
    assert safe_summary({"error_code": code}) == {"error_code": code}
    assert "PRIVATE" not in json.dumps(collector.rows)


def test_export_many_merges_same_run_and_preserves_last_root_outcome(observed):
    observer, collector = observed
    context = observer.new_context("merged-root", {"fixture": True})
    observer.begin_root(context)
    observer.update_root(context, {"status": "awaiting_task"})
    observer.finish_root(context, {"status": "preview", "track_count": 5})
    observer.finish_root(context, {"status": "adopted", "track_count": 5})
    assert observer.flush(timeout=1)
    sent = []
    exporter = LangSmithExporter({"project": "owned", "key": _FIXTURE_KEY, "endpoint": "https://api.smith.langchain.com"})
    exporter.client = SimpleNamespace(request_with_retries=lambda *args, **kwargs: (
        sent.append((args, kwargs)) or SimpleNamespace(status_code=202)))
    exporter.export_many(collector.rows)
    assert len(sent) == 1
    payload = json.loads(sent[0][1]["data"])
    assert set(payload) == {"post"}
    assert len(payload["post"]) == 2
    root = next(row for row in payload["post"] if row["id"] == context["root_id"])
    progress = next(row for row in payload["post"] if row["id"] != context["root_id"])
    assert root["outputs"] == {"status": "adopted", "track_count": 5}
    assert root["extra"]["metadata"]["fixture"] is True
    assert "end_time" in root
    assert progress["outputs"] == {"status": "awaiting_task", "root_complete": False}
    assert "end_time" in progress


def test_patch_only_batch_merges_in_order_and_refuses_unfinished_patch_before_network(observed):
    observer, collector = observed
    context = observer.new_context("patch-only")
    observer.finish_root(context, {"status": "preview"})
    observer.finish_root(context, {"status": "adopted"})
    assert observer.flush(timeout=1)
    sent = []
    exporter = LangSmithExporter({"project": "owned", "key": _FIXTURE_KEY, "endpoint": "https://api.smith.langchain.com"})
    exporter.client = SimpleNamespace(request_with_retries=lambda *args, **kwargs: (
        sent.append((args, kwargs)) or SimpleNamespace(status_code=202)))
    exporter.export_many(collector.rows)
    payload = json.loads(sent[0][1]["data"])
    assert len(payload["patch"]) == 1
    assert payload["patch"][0]["outputs"] == {"status": "adopted"}
    invalid = {**collector.rows[0][1]}
    invalid.pop("end_time")
    with pytest.raises(ValueError, match="TRACE_OPERATION_FAILED"):
        exporter.export("patch", invalid)
    with pytest.raises(ValueError, match="TRACE_OPERATION_FAILED"):
        exporter.export("patch", {**invalid, "end_time": None})
    assert len(sent) == 1


def test_worker_batches_85_events_into_four_requests_without_losing_final_status():
    started, gate = Event(), Event()
    sent = []

    def request(*args, **kwargs):
        sent.append(json.loads(kwargs["data"]))
        if len(sent) == 1:
            started.set()
            gate.wait(2)
        return SimpleNamespace(status_code=202)

    exporter = LangSmithExporter({"project": "owned", "key": _FIXTURE_KEY, "endpoint": "https://api.smith.langchain.com"})
    exporter.client = SimpleNamespace(request_with_retries=request, close=lambda **kwargs: None)
    observer = AgentTracing(enabled=True, exporter=exporter)
    try:
        context = observer.new_context("large-batch")
        observer.begin_root(context)
        assert started.wait(1)
        for index in range(84):
            observer.finish_root(context, {"status": "adopted" if index == 83 else "preview", "track_count": 5})
        gate.set()
        assert observer.flush(timeout=2)
        health = observer.health()
        assert health["queued"] == health["exported"] == 85
        assert health["failed"] == health["dropped"] == health["pending"] == 0
        assert health["exportBatches"] == len(sent) == 4
        assert 1 <= health["lastBatchSize"] <= 32
        assert sent[-1]["patch"][0]["outputs"]["status"] == "adopted"
    finally:
        gate.set()
        observer.close(timeout=1)


def test_lookup_refine_summary_keeps_registered_capability_without_candidates():
    """新路由能被追踪，同时候选内容和选择器原文仍不进入云摘要。"""
    assert safe_summary({"capabilities": ["music.refine", "invented.execute"],
                         "lookupContext": {"rows": [{"title": "PRIVATE_CANDIDATE"}]},
                         "selector": "PRIVATE_INSTRUCTION"}) == {"capabilities": ["music.refine"]}


def test_failed_batch_counts_every_unacknowledged_event_and_reports_only_bounded_ids(caplog):
    import requests

    gate, started = Event(), Event()

    class FailingBatchExporter:
        def export_many(self, events):
            started.set()
            gate.wait(2)
            response = requests.Response()
            response.status_code = 422
            raise requests.HTTPError("PRIVATE_BODY", response=response)

    observer = AgentTracing(enabled=True, exporter=FailingBatchExporter())
    context = observer.new_context("failed-batch")
    try:
        observer.begin_root(context)
        assert started.wait(1)
        for _ in range(20):
            observer.finish_root(context, {"status": "preview"})
        gate.set()
        assert observer.flush(timeout=2)
        health = observer.health()
        assert health["failed"] == 21 and health["exported"] == 0
        assert health["lastHttpStatus"] == 422
        assert health["lastErrorCode"] == "TRACE_HTTP_FAILED"
        assert health["errorCounts"] == {"TRACE_HTTP_FAILED": 21}
        assert health["lastFailedRunId"] == context["root_id"]
        assert health["lastFailedOperation"] == "patch"
        assert len(health["lastFailedEvents"]) == 8
        assert all(item == {"runId": context["root_id"], "operation": "patch"} for item in health["lastFailedEvents"])
        assert "PRIVATE" not in json.dumps(health) and "PRIVATE" not in caplog.text
    finally:
        gate.set()
        observer.close(timeout=1)
