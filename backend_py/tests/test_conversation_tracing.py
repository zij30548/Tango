"""路由观测验收：仅使用隔离数据库、模型替身和内存导出器。"""

import json
import asyncio
from copy import deepcopy
from datetime import datetime
from uuid import uuid4

import pytest

from test_conversation_routing import fx, DEVICE, NOW, used
from tango_backend.agent_tracing import AgentTracing
from tango_backend.errors import ApiError
from test_conversation_routing import music


class MemoryExporter:
    """保留实际准备出站的投影，便于核对隐私和父子关系。"""

    def __init__(self, *, fail=False):
        self.records = []
        self.fail = fail

    def export(self, operation, payload):
        if self.fail:
            raise RuntimeError("Bearer 不应进入日志的导出故障")
        self.records.append((operation, deepcopy(payload)))


@pytest.fixture
def tracing():
    exporter = MemoryExporter()
    tracer = AgentTracing(enabled=True, exporter=exporter)
    yield tracer, exporter
    assert tracer.close(timeout=2)


def records(tracing, *, operation=None, name=None, root=None):
    tracer, exporter = tracing
    assert tracer.flush(timeout=2)
    return [payload for op, payload in exporter.records
            if (operation is None or operation == op)
            and (name is None or payload.get("name") == name)
            and (root is None or payload.get("id") == root)]


async def test_free_help_has_route_span_and_closed_root_without_raw_text(fx, tracing):
    tracer, exporter = tracing
    fx.router.tracing = tracer
    body, _ = await fx.submit("你能做什么？")
    result = await fx.route(body)
    root = records(tracing, operation="create", name="agent.conversation")[0]
    route = records(tracing, operation="create", name="conversation.route")[0]
    assert route["parent_run_id"] == root["id"] == route["trace_id"]
    assert root["extra"]["metadata"]["conversation_id"] == fx.conversation["id"]
    assert root["extra"]["metadata"]["route_id"] == result["route"]["id"]
    assert any(row.get("end_time") and row["outputs"]["status"] == "completed"
               for row in records(tracing, operation="patch", root=root["id"]))
    route_end = records(tracing, operation="patch", root=route["id"])[0]
    assert route_end["outputs"]["capabilities"] == ["assistant.reply"]
    assert route_end["outputs"]["paid"] is False
    rendered = json.dumps(exporter.records, ensure_ascii=False)
    for secret in [body["content"], fx.user["id"], DEVICE, "instruction", "preference", "owner_token"]:
        assert secret not in rendered
    assert fx.calls == [] and await used(fx) == 0


async def test_music_route_waits_for_task_and_rebuilds_same_root_from_persistent_time(fx, tracing):
    tracer, _ = tracing
    fx.router.tracing = tracer
    body, _ = await fx.submit()
    first = await fx.route(body)
    root = records(tracing, operation="create", name="agent.conversation")[0]
    updates = records(tracing, operation="patch", root=root["id"])
    # 云端不接受缺少 end_time 的 PATCH；等待状态用独立进度节点表达，根不能提前结算。
    assert "end_time" not in root
    assert updates == []
    assert all("end_time" not in row for row in records(tracing, root=root["id"]))
    progress = records(tracing, operation="create", name="agent.progress")
    assert len(progress) == 1 and progress[0]["parent_run_id"] == root["id"]
    assert progress[0]["trace_id"] == root["id"]
    progress_end = records(tracing, operation="patch", root=progress[0]["id"])
    assert len(progress_end) == 1 and isinstance(progress_end[0]["end_time"], str)
    progress_start = datetime.fromisoformat(progress[0]["start_time"])
    progress_finished = datetime.fromisoformat(progress_end[0]["end_time"])
    assert progress_start.tzinfo is not None and progress_finished >= progress_start
    assert progress_end[0]["outputs"]["status"] == "awaiting_task"
    assert progress_end[0]["outputs"]["root_complete"] is False
    linked = await fx.router.taskContext(fx.user["id"], DEVICE, body["messageId"], body["content"])
    context = linked.pop("_observation")
    assert context["root_id"] == root["id"] and context["dotted_order"] == root["dotted_order"]
    assert context["start_time"] == root["start_time"]
    fx.router.now = lambda: NOW + 1000
    rebuilt = await fx.router.taskContext(fx.user["id"], DEVICE, body["messageId"], body["content"])
    assert rebuilt.pop("_observation") == context and rebuilt == linked
    before = len(records(tracing))
    second = await fx.route(body)
    assert first["route"] == second["route"] and len(records(tracing)) == before
    assert len(fx.calls) == 1 and await used(fx) == 1
    assert "_observation" not in fx.calls[0]
    assert fx.runtime.inflight == 0 and not fx.runtime.accountCalls


async def test_concurrent_duplicate_requests_share_one_route_and_one_trace(fx, tracing):
    fx.router.tracing = tracing[0]
    entered, finish = asyncio.Event(), asyncio.Event()

    async def slow(inputs, options=None):
        fx.calls.append(inputs)
        entered.set()
        await finish.wait()
        return {"value": music(inputs)}

    fx.router.model.route = slow
    body, _ = await fx.submit()
    first = asyncio.create_task(fx.route(body))
    await asyncio.wait_for(entered.wait(), timeout=2)
    try:
        duplicates = await asyncio.gather(*(fx.route(body) for _ in range(3)))
        assert all(row["route"]["status"] == "running" for row in duplicates)
    finally:
        finish.set()
    assert (await first)["route"]["status"] == "completed"
    assert len(records(tracing, operation="create", name="agent.conversation")) == 1
    assert len(records(tracing, operation="create", name="conversation.route")) == 1
    assert len(fx.calls) == 1 and await used(fx) == 1


async def test_same_message_identifier_in_different_conversations_does_not_merge_traces(fx, tracing):
    fx.router.tracing = tracing[0]
    body, _ = await fx.submit("你能做什么？")
    await fx.route(body)
    other = await fx.conversations.create(
        fx.user["id"], {"deviceId": DEVICE, "requestId": str(uuid4())}
    )
    await fx.conversations.message(other["id"], fx.user["id"], body, {"dispatchHint": "router"})
    await fx.router.route(other["id"], body["messageId"], fx.user["id"], {"deviceId": DEVICE, "preference": ""})
    roots = records(tracing, operation="create", name="agent.conversation")
    assert len(roots) == 2 and roots[0]["id"] != roots[1]["id"]
    assert {row["extra"]["metadata"]["conversation_id"] for row in roots} == {fx.conversation["id"], other["id"]}
    assert fx.calls == [] and await used(fx) == 0


@pytest.mark.parametrize("code,status", [("MODEL_TIMEOUT", "uncertain"), ("MODEL_UNAVAILABLE", "failed")])
async def test_failed_and_unknown_model_attempts_keep_original_ledger_without_retry(fx, tracing, code, status):
    fx.router.tracing = tracing[0]

    async def broken(inputs, options=None):
        fx.calls.append(inputs)
        raise ApiError(504, code, "Bearer 私密上游故障正文")

    fx.router.model.route = broken
    body, _ = await fx.submit()
    first = await fx.route(body)
    second = await fx.route(body)
    assert first["route"] == second["route"] and first["route"]["status"] == status
    root = records(tracing, operation="create", name="agent.conversation")[0]
    terminal = records(tracing, operation="patch", root=root["id"])[-1]
    assert terminal["outputs"]["status"] == status and terminal.get("end_time")
    assert "私密上游故障正文" not in json.dumps(tracing[1].records, ensure_ascii=False)
    assert len(fx.calls) == 1 and await used(fx) == 1


async def test_invalid_plan_is_failed_in_trace_and_cannot_claim_task_completion(fx, tracing):
    fx.router.tracing = tracing[0]

    async def invalid(inputs, options=None):
        fx.calls.append(inputs)
        return {"value": {"intents": ["scene_research"], "actions": [], "missingSlots": []}}

    fx.router.model.route = invalid
    body, _ = await fx.submit()
    result = await fx.route(body)
    assert result["route"]["errorCode"] == "INVALID_ROUTE_PLAN"
    root = records(tracing, operation="create", name="agent.conversation")[0]
    terminal = records(tracing, operation="patch", root=root["id"])[-1]
    assert terminal["outputs"]["status"] == "failed"
    assert terminal["error"] == "INVALID_ROUTE_PLAN"
    assert terminal["outputs"]["capabilities"] == []


async def test_persistence_exception_closes_span_and_preserves_business_exception(fx, tracing, monkeypatch):
    fx.router.tracing = tracing[0]

    async def unavailable(*args, **kwargs):
        raise RuntimeError("数据库私密故障正文")

    monkeypatch.setattr(fx.router.repository, "finish", unavailable)
    body, _ = await fx.submit()
    with pytest.raises(RuntimeError, match="数据库私密故障正文"):
        await fx.route(body)
    route = records(tracing, operation="create", name="conversation.route")[0]
    assert records(tracing, operation="patch", root=route["id"])[0].get("end_time")
    root = records(tracing, operation="create", name="agent.conversation")[0]
    assert records(tracing, operation="patch", root=root["id"])[-1]["outputs"]["status"] == "uncertain"
    assert "数据库私密故障正文" not in json.dumps(tracing[1].records, ensure_ascii=False)
    assert fx.runtime.inflight == 0 and not fx.runtime.accountCalls
    assert not fx.router.controllers and not fx.router.running


async def test_export_failure_does_not_change_route_or_trigger_another_model_call(fx):
    tracer = AgentTracing(enabled=True, exporter=MemoryExporter(fail=True))
    fx.router.tracing = tracer
    try:
        body, _ = await fx.submit()
        first = await fx.route(body)
        second = await fx.route(body)
        assert first["route"] == second["route"] and first["route"]["status"] == "completed"
        assert tracer.flush(timeout=2) and tracer.health()["failed"] > 0
        assert len(fx.calls) == 1 and await used(fx) == 1
    finally:
        assert tracer.close(timeout=2)


async def test_context_failure_is_observability_only_and_disabled_keeps_old_task_context(fx, tracing, monkeypatch):
    fx.router.tracing = tracing[0]

    def unavailable(*args, **kwargs):
        raise RuntimeError("不应传播的追踪上下文故障")

    monkeypatch.setattr(tracing[0], "new_context", unavailable)
    body, _ = await fx.submit()
    result = await fx.route(body)
    assert result["route"]["status"] == "completed"
    assert records(tracing) == []
    fx.router.tracing = AgentTracing()
    linked = await fx.router.taskContext(fx.user["id"], DEVICE, body["messageId"], body["content"])
    assert "_observation" not in linked and len(fx.calls) == 1
