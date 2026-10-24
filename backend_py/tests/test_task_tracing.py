"""任务追踪真实 HTTP 适配、恢复与隐私回归；所有网络响应均为隔离夹具。"""

import asyncio
import json
from uuid import uuid4

import httpx
import pytest
import requests

from evaluation.langsmith_demo import MemoryExporter, merged_runs
from tango_backend.agent_tracing import AgentTracing, LangSmithExporter
from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.task_model import createTaskModel
from test_multi_model_runtime import configuration
from test_task_runtime_migration import body, catalog, f, finish, raw_brief, until


def no_context_leak(value):
    """观测关联属于后台私有状态，不能成为模型指令、曲库数据或客户端契约。"""
    encoded = json.dumps(value, ensure_ascii=False)
    for forbidden in ("_observation", "trace_id", "root_id", "dotted_order", "previous_trace_id", "completed_state_version"):
        assert forbidden not in encoded


def no_external_http(monkeypatch):
    """允许显式 MockTransport，阻止模型、追踪 SDK 意外使用真实 HTTP。"""
    async def async_forbidden(*args, **kwargs):
        pytest.fail("测试发起了真实异步 HTTP")
    def sync_forbidden(*args, **kwargs):
        pytest.fail("测试发起了真实同步 HTTP")
    monkeypatch.setattr(httpx.AsyncHTTPTransport, "handle_async_request", async_forbidden)
    monkeypatch.setattr(requests.Session, "request", sync_forbidden)


async def test_task_model_real_transport_has_nested_llm_usage_without_raw_content(f, monkeypatch):
    no_external_http(monkeypatch)
    seen = []
    private_text = "trace-private-fixture-body"
    private_preference = "trace-private-fixture-preference"
    fixture_key = "isolated-flash-key"

    def handler(request):
        payload = json.loads(request.content)
        inputs = json.loads(payload["messages"][1]["content"])
        no_context_leak(inputs)
        seen.append(clone(payload))
        maximum = payload["max_tokens"]
        values = {
            2200: raw_brief(),
            1000: {"queries": ["读书 R&B"], "artist": ""},
            700: {"decision": "done", "queries": [], "note": "固定观察"},
            1200: {"order": list(range(len(inputs.get("candidates", []))))},
        }
        assert maximum in values
        return httpx.Response(200, json={
            "model": payload["model"], "id": "isolated-response-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": json.dumps(values[maximum])}}],
            "usage": {"prompt_tokens": 7, "completion_tokens": 3},
        })

    collector = MemoryExporter()
    observer = AgentTracing(enabled=True, exporter=collector)
    try:
        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            model = createTaskModel({"modelProfiles": configuration("flash_only", QWEN_MODEL="qwen3.7-flash-2026-07-15"), "client": client})
            runtime = await f.runtime(model=model, tracing=observer)
            thread = str(uuid4())
            async def context(user, device, request, instruction):
                return {"intents": ["scene_research"], "dependencyCount": 0,
                    "_observation": observer.new_context("fixture:" + request,
                        metadata={"thread_id": thread, "conversation_id": thread})}
            runtime.conversationContextProvider = context
            created = await runtime.create(f.user, body(instruction="读书20分钟 R&B，不要现场版；" + private_text,
                                                        preference=private_preference))
            preview, _ = await finish(runtime, created, f.user, f.time[0])
            no_context_leak(preview)
            persisted = await runtime.store.load(created["id"], f.user)
            assert persisted["_observation"]["metadata"]["thread_id"] == thread
            no_context_leak(persisted["routingContext"])
            assert private_text in json.dumps(seen, ensure_ascii=False)
            assert private_preference in json.dumps(seen, ensure_ascii=False)
            assert observer.flush(timeout=5)
            runs = merged_runs(collector.events())
            llms = [row for row in runs if row["name"] == "model.request"]
            calls = {row["id"]: row for row in runs if row["name"] == "task.model_call"}
            assert len(llms) == len(seen) == 3
            for row in llms:
                assert row["run_type"] == "llm" and row["parent_run_id"] in calls
                assert row["outputs"]["usage_metadata"] == {"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}
                assert row["extra"]["metadata"]["model"] == "qwen3.7-flash-2026-07-15"
                assert row["extra"]["metadata"]["actual_model"] == "qwen3.7-flash-2026-07-15"
            encoded = json.dumps(runs, ensure_ascii=False)
            for forbidden in (private_text, private_preference, fixture_key, "http://127.0.0.1:9", "system", "prompt", "Bearer", "Cookie"):
                assert forbidden not in encoded
            ledger = await f.store.db.fetchall("SELECT input_tokens,output_tokens FROM music_task_calls")
            assert len(ledger) == len(llms) and all(row["input_tokens"] == 7 and row["output_tokens"] == 3 for row in ledger)
    finally:
        assert observer.close(timeout=5)


async def test_trace_disabled_executes_business_without_sdk_or_trace_context(f, monkeypatch):
    no_external_http(monkeypatch)
    monkeypatch.setenv("LANGSMITH_TRACING", "true")
    monkeypatch.setenv("LANGCHAIN_TRACING_V2", "true")
    monkeypatch.setenv("LANGSMITH_API_KEY", "ambient-fixture-key-do-not-use")
    def forbidden_sdk(*args, **kwargs):
        pytest.fail("追踪关闭时构造了云 SDK")
    monkeypatch.setattr(LangSmithExporter, "_open", forbidden_sdk)
    collector = MemoryExporter()
    observer = AgentTracing.from_env({"LANGSMITH_TRACING": "true", "LANGSMITH_API_KEY": "ambient-fixture-key-do-not-use"}, exporter=collector)
    assert observer.enabled is False
    runtime = await f.runtime(tracing=observer)
    created = await runtime.create(f.user, body())
    preview, _ = await finish(runtime, created, f.user, f.time[0])
    assert preview["status"] == "preview"
    assert "_observation" not in await runtime.store.load(created["id"], f.user)
    no_context_leak(preview)
    assert collector.events() == []
    assert observer.health()["queued"] == 0 and observer.health()["workerAlive"] is False
    assert observer.close(timeout=5)


async def test_pause_restart_read_resume_and_turn_keep_private_durable_trace_boundaries(f, monkeypatch):
    no_external_http(monkeypatch)
    collector = MemoryExporter()
    first = AgentTracing(enabled=True, exporter=collector)
    second = None
    try:
        runtime = await f.runtime(tracing=first)
        thread = str(uuid4())
        async def context(user, device, request, instruction):
            return {"intents": ["scene_research"], "dependencyCount": 0,
                "_observation": runtime.tracing.new_context("fixture:" + request,
                    metadata={"thread_id": thread, "conversation_id": thread})}
        runtime.conversationContextProvider = context
        created = await runtime.create(f.user, body())
        waiting = await until(runtime, created["id"], f.user, "waiting_tools")
        original = await runtime.store.load(created["id"], f.user)
        original_context = clone(original["_observation"])
        original_step = clone(waiting["pendingSteps"][0])
        assert first.flush(timeout=5)
        root_waiting = next(row for row in merged_runs(collector.events()) if row["id"] == original_context["trace_id"])
        assert "end_time" not in root_waiting
        progress_waiting = [row for row in merged_runs(collector.events())
                            if row["trace_id"] == original_context["trace_id"] and row["name"] == "agent.progress"]
        assert progress_waiting and "end_time" in progress_waiting[-1]
        assert progress_waiting[-1]["outputs"]["status"] == "waiting_tools"
        assert progress_waiting[-1]["outputs"]["root_complete"] is False
        paused = await runtime.action(created["id"], f.user, body(action="pause", expectedRevision=waiting["revision"]))
        assert paused["status"] == "paused"
        paused_private = await runtime.store.load(created["id"], f.user)
        paused_context = clone(paused_private["_observation"])
        assert paused_context == {**original_context, "completed_state_version": paused_private["stateVersion"]}
        calls_before = len(f.model.calls)
        await runtime.close()
        assert first.flush(timeout=5) and first.close(timeout=5)
        first_events = len(collector.events())
        second = AgentTracing(enabled=True, exporter=collector)
        runtime = await f.runtime(tracing=second)
        runtime.conversationContextProvider = context
        for _ in range(2):
            restored = await runtime.get(created["id"], f.user)
        await asyncio.sleep(.02)
        assert restored["status"] == "paused" and len(f.model.calls) == calls_before
        assert len(collector.events()) == first_events
        restored_private = await runtime.store.load(created["id"], f.user)
        assert restored_private["_observation"] == paused_context
        no_context_leak(restored)

        resumed = await runtime.action(created["id"], f.user, body(action="resume", expectedRevision=restored["revision"]))
        waiting_again = await until(runtime, created["id"], f.user, "waiting_tools")
        resumed_private = await runtime.store.load(created["id"], f.user)
        resumed_context = resumed_private["_observation"]
        assert resumed_context["trace_id"] != original_context["trace_id"]
        assert resumed_context["metadata"]["previous_trace_id"] == original_context["trace_id"]
        assert resumed_context["metadata"]["thread_id"] == thread
        assert len(f.model.calls) == calls_before
        assert second.flush(timeout=5)
        root_resuming = next(row for row in merged_runs(collector.events()) if row["id"] == resumed_context["trace_id"])
        assert "end_time" not in root_resuming
        progress_resuming = [row for row in merged_runs(collector.events())
                             if row["trace_id"] == resumed_context["trace_id"] and row["name"] == "agent.progress"]
        assert progress_resuming and "end_time" in progress_resuming[-1]
        assert progress_resuming[-1]["outputs"]["status"] == "waiting_tools"
        assert progress_resuming[-1]["outputs"]["root_complete"] is False
        assert waiting_again["pendingSteps"][0]["leaseId"] != original_step["leaseId"]
        with pytest.raises(ApiError) as stale:
            await runtime.result(created["id"], original_step["id"], f.user, catalog(original_step, f.time[0]))
        assert stale.value.code == "STALE_TASK_STEP"
        for step in waiting_again["pendingSteps"]:
            await runtime.result(created["id"], step["id"], f.user, catalog(step, f.time[0]))
        preview = await until(runtime, created["id"], f.user, "preview")
        no_context_leak(preview)
        assert second.flush(timeout=5)
        prior_run_id = (await runtime.store.load(created["id"], f.user))["runId"]
        turn = await runtime.action(created["id"], f.user, body(action="turn", expectedRevision=preview["revision"],
            instruction="保留已经确认的要求，把后半段调得轻快一些"))
        revised, _ = await finish(runtime, turn, f.user, f.time[0])
        private_turn = await runtime.store.load(created["id"], f.user)
        assert private_turn["runId"] != prior_run_id
        assert private_turn["_observation"]["trace_id"] not in {original_context["trace_id"], resumed_context["trace_id"]}
        assert private_turn["_observation"]["metadata"]["thread_id"] == thread
        no_context_leak(private_turn["routingContext"])
        no_context_leak(revised)
        no_context_leak(f.model.calls)
        assert second.flush(timeout=5)
        runs = merged_runs(collector.events())
        by_trace = {row["id"]: row for row in runs if not row.get("parent_run_id")}
        assert len(by_trace) == 3
        assert by_trace[original_context["trace_id"]]["outputs"]["status"] == "paused"
        assert by_trace[resumed_context["trace_id"]]["outputs"]["status"] == "preview"
        turn_trace = private_turn["_observation"]["trace_id"]
        assert by_trace[turn_trace]["outputs"]["run_id"] == private_turn["runId"]
        for row in runs:
            if row["trace_id"] == turn_trace and row["name"] == "task.run":
                assert row["extra"]["metadata"]["run_id"] == private_turn["runId"]
    finally:
        first.close(timeout=5)
        if second:
            assert second.close(timeout=5)
