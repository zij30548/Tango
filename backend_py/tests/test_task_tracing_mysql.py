"""拥有者临时 MySQL 上核验追踪持久性与提交边界；不读取真实配置或用户库。"""

import asyncio
from uuid import uuid4

import pytest

from evaluation.langsmith_demo import MemoryExporter, merged_runs
from tango_backend.agent_tracing import AgentTracing
from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.task_store import TaskStore
from test_mysql_task_runtime import f
from test_task_runtime_migration import body, catalog, until
from test_task_tracing import no_context_leak, no_external_http


async def test_mysql_trace_context_survives_new_connection_and_explicit_resume_without_replay(f, monkeypatch):
    """以独立连接读回私有上下文，恢复仅在用户显式要求后续跑。"""
    no_external_http(monkeypatch)
    collector = MemoryExporter()
    observer = AgentTracing(enabled=True, exporter=collector)
    try:
        runtime = await f.runtime(tracing=observer)
        thread_id = str(uuid4())

        async def context(user, device, request_id, instruction):
            return {"intents": ["scene_research"], "dependencyCount": 0,
                "_observation": observer.new_context("mysql-fixture:" + request_id,
                    metadata={"thread_id": thread_id, "conversation_id": thread_id})}

        runtime.conversationContextProvider = context
        created = await runtime.create(f.user, body())
        waiting = await until(runtime, created["id"], f.user, "waiting_tools")
        stored = await runtime.store.load(created["id"], f.user)
        original_context = clone(stored["_observation"])
        old_step = clone(waiting["pendingSteps"][0])
        no_context_leak(waiting)
        no_context_leak(stored["routingContext"])
        no_context_leak(f.model.calls)
        assert stored["_observation"]["metadata"]["thread_id"] == thread_id

        paused = await runtime.action(created["id"], f.user,
            body(action="pause", expectedRevision=waiting["revision"]))
        assert paused["status"] == "paused"
        paused_private = await runtime.store.load(created["id"], f.user)
        paused_context = clone(paused_private["_observation"])
        assert paused_context == {**original_context, "completed_state_version": paused_private["stateVersion"]}
        await runtime.close()
        assert observer.flush(timeout=5)
        event_count = len(collector.events())
        calls_before = len(f.model.calls)
        usage_before = await f.store.db.fetchone("SELECT request_count FROM ai_usage WHERE user_id=?", [f.user])

        new_account_store = await f.new_store()
        assert new_account_store is not f.store and new_account_store.dialect == "mysql"
        restored_runtime = await f.runtime(store=new_account_store, tracing=observer)
        for _ in range(2):
            restored = await restored_runtime.get(created["id"], f.user)
        await asyncio.sleep(.02)
        assert observer.flush(timeout=5)
        assert len(f.model.calls) == calls_before and len(collector.events()) == event_count
        assert restored["status"] == "paused"
        no_context_leak(restored)
        durable = await restored_runtime.store.load(created["id"], f.user)
        assert durable["_observation"] == paused_context
        assert await new_account_store.db.fetchone("SELECT request_count FROM ai_usage WHERE user_id=?", [f.user]) == usage_before

        await restored_runtime.action(created["id"], f.user,
            body(action="resume", expectedRevision=restored["revision"]))
        waiting_again = await until(restored_runtime, created["id"], f.user, "waiting_tools")
        durable_resumed = await restored_runtime.store.load(created["id"], f.user)
        resumed_context = durable_resumed["_observation"]
        assert resumed_context["trace_id"] != original_context["trace_id"]
        assert resumed_context["metadata"]["previous_trace_id"] == original_context["trace_id"]
        assert resumed_context["metadata"]["thread_id"] == thread_id
        assert len(f.model.calls) == calls_before
        assert waiting_again["pendingSteps"][0]["leaseId"] != old_step["leaseId"]
        assert observer.flush(timeout=5)
        before_rejection_events = len(collector.events())
        before_rejection_state = await restored_runtime.store.load(created["id"], f.user)
        with pytest.raises(ApiError) as rejected:
            await restored_runtime.result(created["id"], old_step["id"], f.user, catalog(old_step, f.time[0]))
        assert rejected.value.code == "STALE_TASK_STEP"
        assert await restored_runtime.store.load(created["id"], f.user) == before_rejection_state
        assert observer.flush(timeout=5)
        rejected_events = collector.events()[before_rejection_events:]
        assert not any(event["payload"].get("name") == "task.checks" for event in rejected_events)
        assert not any(event["payload"].get("outputs", {}).get("status") in {"completed", "preview", "adopted"} for event in rejected_events)

        for step in waiting_again["pendingSteps"]:
            await restored_runtime.result(created["id"], step["id"], f.user, catalog(step, f.time[0]))
        preview = await until(restored_runtime, created["id"], f.user, "preview")
        no_context_leak(preview)
        no_context_leak(f.model.calls)
        assert preview["preview"]["totalDurationMs"] == 1200000
        durable_preview = await restored_runtime.store.load(created["id"], f.user)
        completion_version = durable_preview["_observation"]["completed_state_version"]
        assert completion_version == durable_preview["stateVersion"]
        calls_at_preview = len(f.model.calls)
        requested = await restored_runtime.action(created["id"], f.user,
            body(action="adopt", expectedRevision=preview["revision"]))
        assert requested["status"] == "awaiting_apply"
        applied = await restored_runtime.action(created["id"], f.user,
            body(action="applied", expectedRevision=requested["revision"], adoptionId=requested["adoption"]["id"]))
        assert applied["status"] == "adopted" and len(f.model.calls) == calls_at_preview
        durable_applied = await restored_runtime.store.load(created["id"], f.user)
        assert durable_applied["_observation"]["completed_state_version"] == completion_version
        assert completion_version < durable_applied["stateVersion"]
        no_context_leak(requested)
        no_context_leak(applied)
        no_context_leak(f.model.calls)
        assert observer.flush(timeout=5)
        runs = merged_runs(collector.events())
        roots = {row["id"]: row for row in runs if not row.get("parent_run_id")}
        assert len(roots) == 2
        assert roots[original_context["trace_id"]]["outputs"]["status"] == "paused"
        assert roots[resumed_context["trace_id"]]["outputs"]["status"] == "preview"
        assert roots[resumed_context["trace_id"]]["outputs"]["state_version"] == completion_version
        root_patches = [event["payload"] for event in collector.events()
                        if event["operation"] == "patch" and event["payload"]["id"] == resumed_context["trace_id"]]
        assert len(root_patches) == 1
        assert root_patches[0]["outputs"]["status"] == "preview"
        assert root_patches[0]["outputs"]["state_version"] == completion_version
        assert root_patches[0]["outputs"]["checkpoint"] == "preview"
        assert any(row["name"] == "task.adopt" and row["outputs"]["status"] == "awaiting_apply" for row in runs)
        assert any(row["name"] == "task.applied" and row["outputs"]["status"] == "adopted" for row in runs)
        assert any(row["name"] == "task.tool" and row["trace_id"] == resumed_context["trace_id"] for row in runs)
        assert any(row["name"] == "task.checks" and row["trace_id"] == resumed_context["trace_id"] for row in runs)
    finally:
        assert observer.close(timeout=5)


async def test_mysql_failed_cas_cannot_emit_successful_checkpoint_or_checks(f, monkeypatch):
    """真实 MySQL CAS 拒绝旧版本；观测只能在提交成功后产生。"""
    no_external_http(monkeypatch)
    collector = MemoryExporter()
    observer = AgentTracing(enabled=True, exporter=collector)
    try:
        runtime = await f.runtime(tracing=observer)
        runtime.schedule = lambda task: None
        created = await runtime.create(f.user, body())
        stale = await runtime.store.load(created["id"], f.user)
        other_store = TaskStore(await f.new_store())
        current = clone(stale)
        current.update(status="paused", stateVersion=stale["stateVersion"] + 1,
                       updatedAt=stale["updatedAt"] + 1)
        await other_store.save(current, expectedStateVersion=stale["stateVersion"], now=f.time[0])
        committed_before = await other_store.load(created["id"], f.user)
        assert committed_before["status"] == "paused"
        assert observer.flush(timeout=5)
        events_before = collector.events()

        stale.update(status="preview", preview={"tracks": [], "checks": [{"key": "duration", "status": "satisfied"}]})
        with pytest.raises(ApiError) as rejected:
            await runtime.checkpoint(stale, "preview")
        assert rejected.value.code == "TASK_WRITE_CONFLICT"
        assert await other_store.load(created["id"], f.user) == committed_before
        assert observer.flush(timeout=5)
        assert collector.events() == events_before
        assert not any(row["name"] == "task.checks" for row in merged_runs(collector.events()))
        assert f.model.calls == []
        assert await f.store.db.fetchone("SELECT request_count FROM ai_usage WHERE user_id=?", [f.user]) is None
    finally:
        assert observer.close(timeout=5)
