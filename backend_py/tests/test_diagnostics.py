"""只读诊断的隔离验收：核对关联、未知状态、脱敏、无写入及无额外模型调用。"""

import json
from pathlib import Path
import sqlite3
import subprocess
import sys
from uuid import uuid4

import pytest

from test_conversation_routing import fx, DEVICE, NOW
from tango_backend import diagnostics as d


@pytest.fixture
async def records(fx):
    """用真实持久会话和路由播种，模型是现有离线替身，不使用应用数据。"""
    help_body, _ = await fx.submit("你能做什么")
    await fx.route(help_body)
    body, _ = await fx.submit()
    await fx.route(body)
    task_id = str(uuid4())
    task = {
        "id": task_id, "userId": fx.user["id"], "deviceId": DEVICE,
        "revision": 2, "stateVersion": 7, "status": "preview", "phase": "done", "runId": "owned_run_01",
        "instruction": "不要输出这段私密原文", "preference": "不要输出这段画像",
        "ownerToken": "secret-owner-token", "brief": {"targetMinutes": 20, "excludeLive": True},
        "lockedIds": [101], "budget": {"modelCalls": 3, "toolCalls": 1},
        "trace": [{"node": "compose", "status": "preview", "revision": 2, "at": NOW, "note": "Bearer secret-note"}],
        "pendingSteps": [{"id": "owned_step_01", "skill": "scene.research", "revision": 2, "leaseId": "secret-lease-token"}],
        "toolHistory": [{"stepId": "owned_step_01", "skill": "scene.research", "revision": 2, "status": "completed",
            "input": {"queries": ["粤语 跑步"], "queryCoverage": ["不要输出这段覆盖原文"]},
            "result": {"tracks": [{"id": 101, "url": "https://private.example/audio"}],
                "sources": [{"name": "不要输出这段来源原文"}], "checkedAt": NOW}}],
        "preview": {"version": 2, "tracks": [{"id": 101}], "totalDurationMs": 1200000,
            "checks": [{"key": "duration", "status": "satisfied", "detail": "不要输出这段检查原文"},
                {"key": "style", "status": "unverified"}]},
    }
    await fx.store.db.execute("INSERT INTO music_tasks(id,user_id,device_id,revision,status,snapshot_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)",
        (task_id, fx.user["id"], DEVICE, 2, "preview", json.dumps(task, ensure_ascii=False), NOW, NOW))
    journal = await fx.store.db.fetchone("SELECT journal_json FROM music_conversations WHERE id=?", (fx.conversation["id"],))
    state = json.loads(journal["journal_json"])
    state["latestTaskId"] = task_id
    state["taskLinks"][task_id] = {"turnId": body["messageId"], "revision": 2}
    await fx.store.db.execute("UPDATE music_conversations SET journal_json=? WHERE id=?", (json.dumps(state), fx.conversation["id"]))
    for n, status, tokens in [(1, "completed", 11), (2, "uncertain", None)]:
        await fx.store.db.execute("INSERT INTO music_task_calls(id,task_id,revision,run_id,node,status,started_at,input_tokens,output_tokens,error_code) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (str(uuid4()), task_id, 2, "owned_run_01", "compose", status, NOW, tokens, tokens, "MODEL_TIMEOUT" if n == 2 else None))
    return fx, task_id, body["messageId"]


async def view(fx, scope, ident=None, **options):
    async with d.open_database(sqlite=str(fx.store.db.path)) as database:
        return await d.report(database, scope, identifier=ident, now_ms=NOW, **options)


async def test_recent_lists_ids_without_user_messages_or_any_model_call(records):
    fx, task_id, _ = records
    before = len(fx.calls)
    result = await view(fx, "recent")
    assert result["readOnly"] is True and result["scope"] == "recent"
    assert any(t["id"] == task_id for t in result["tasks"])
    assert any(c["id"] == fx.conversation["id"] for c in result["conversations"])
    assert len(fx.calls) == before


async def test_conversation_distinguishes_free_rule_and_paid_route(records):
    fx, task_id, _ = records
    result = await view(fx, "conversation", fx.conversation["id"])
    assert len(result["routes"]) == 2
    assert sorted(r["paid"] for r in result["routes"]) == [False, True]
    assert any(t["id"] == task_id for t in result["tasks"])


async def test_task_preserves_unknown_tokens_and_unverified_constraints(records):
    fx, task_id, _ = records
    result = await view(fx, "task", task_id)
    assert result["task"]["id"] == task_id and result["task"]["status"] == "preview"
    assert len(result["calls"]) == 2
    assert any(c["inputTokens"] is None and c["outputTokens"] is None for c in result["calls"])
    assert {c["key"]: c["status"] for c in result["checks"]} == {"duration": "satisfied", "style": "unverified"}
    assert result["toolHistory"] and result["trace"]


async def test_trace_identifier_does_not_claim_cloud_upload_or_expose_private_context(records):
    fx, task_id, _ = records
    row = await fx.store.db.fetchone("SELECT snapshot_json FROM music_tasks WHERE id=?", (task_id,))
    state = json.loads(row["snapshot_json"])
    trace_id = str(uuid4())
    state["_observation"] = {"trace_id": trace_id, "metadata": {"secret": "secret-private-context"}}
    await fx.store.db.execute("UPDATE music_tasks SET snapshot_json=? WHERE id=?", (json.dumps(state), task_id))
    result = await view(fx, "task", task_id)
    assert result["task"]["observation"] == {"traceId": trace_id, "uploadStatus": "unknown"}
    assert "secret-private-context" not in json.dumps(result)


async def test_default_report_excludes_raw_text_and_capability_tokens(records):
    fx, task_id, _ = records
    for scope, ident in [("task", task_id), ("conversation", fx.conversation["id"])]:
        rendered = json.dumps(await view(fx, scope, ident), ensure_ascii=False)
        for sensitive in ["不要输出这段", "secret-owner-token", "secret-lease-token", "secret-note", "private.example", "粤语 跑步"]:
            assert sensitive not in rendered


async def test_explicit_queries_only_exposes_bounded_music_query(records):
    fx, task_id, _ = records
    rendered = json.dumps(await view(fx, "task", task_id, show_queries=True), ensure_ascii=False)
    assert "粤语 跑步" in rendered
    assert "不要输出这段" not in rendered and "secret-lease-token" not in rendered


async def test_expired_route_is_observed_as_uncertain_without_modification(records):
    fx, _, message_id = records
    await fx.store.db.execute("UPDATE music_conversation_routes SET status='running',lease_until=? WHERE message_id=?", (NOW - 1, message_id))
    result = await view(fx, "conversation", fx.conversation["id"])
    observed = next(r for r in result["routes"] if r["messageId"] == message_id)
    assert observed["effectiveStatus"] == "uncertain"
    row = await fx.store.db.fetchone("SELECT status FROM music_conversation_routes WHERE message_id=?", (message_id,))
    assert row["status"] == "running"


async def test_cross_owner_task_link_is_not_followed(records):
    fx, task_id, _ = records
    await fx.store.db.execute("UPDATE music_tasks SET user_id=? WHERE id=?", (fx.other["id"], task_id))
    result = await view(fx, "conversation", fx.conversation["id"])
    assert not any(t["id"] == task_id for t in result["tasks"])
    assert result["warnings"]


async def test_reader_rejects_writes_and_existing_database_is_unchanged(records):
    fx, _, _ = records
    before = await fx.store.db.fetchall("SELECT id,status FROM music_tasks ORDER BY id")
    async with d.open_database(sqlite=str(fx.store.db.path)) as database:
        with pytest.raises(Exception):
            await database.fetchall("UPDATE music_tasks SET status='cancelled'")
    assert await fx.store.db.fetchall("SELECT id,status FROM music_tasks ORDER BY id") == before


async def test_missing_sqlite_is_not_created(tmp_path):
    target = tmp_path / "missing.sqlite"
    with pytest.raises(d.DiagnosticError):
        await d.run_diagnostics(["recent", "--sqlite", str(target)])
    assert not target.exists()


async def test_missing_schema_is_not_initialized(tmp_path):
    target = tmp_path / "empty.sqlite"
    sqlite3.connect(target).close()
    before = target.read_bytes()
    with pytest.raises(d.DiagnosticError):
        await d.run_diagnostics(["recent", "--sqlite", str(target)])
    assert target.read_bytes() == before
    with sqlite3.connect(target) as connection:
        assert not connection.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()


async def test_target_identifier_cannot_become_sql(records):
    fx, _, _ = records
    with pytest.raises(d.DiagnosticError):
        await view(fx, "task", "' OR 1=1 --")


def test_help_does_not_read_configuration_or_connect_to_database():
    result = subprocess.run([sys.executable, "-m", "tango_backend.diagnostics", "--help"],
        cwd=Path(__file__).parents[1], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0 and "recent" in result.stdout
    assert "Traceback" not in result.stderr


async def test_refine_route_is_named_without_exposing_candidate_context(records):
    """诊断显示只读操作类别，不能把原池、歌名或任意参数完整输出。"""
    fx, _, message_id = records
    row = await fx.store.db.fetchone("SELECT result_json FROM music_conversation_routes WHERE message_id=?", (message_id,))
    plan = json.loads(row["result_json"])
    plan["actions"] = [{"id": "refine_action_01", "capability": "music.refine", "dependsOn": [],
        "source": "PRIVATE_INSTRUCTION", "parameters": {"operation": "select", "index": 2,
            "candidateSetId": "PRIVATE_CANDIDATE_SET", "rows": [{"title": "PRIVATE_TITLE"}]}}]
    await fx.store.db.execute("UPDATE music_conversation_routes SET result_json=? WHERE message_id=?", (json.dumps(plan), message_id))
    result = await view(fx, "conversation", fx.conversation["id"])
    action = next(item for route in result["routes"] for item in route["actions"] if item["id"] == "refine_action_01")
    assert action["capability"] == "music.refine" and action["operation"] == "select"
    assert "PRIVATE_" not in json.dumps(result)
