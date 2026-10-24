"""拥有者临时 MySQL 上核验只读诊断；不读取应用配置、用户库或外部模型。"""

import json
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import unquote, urlsplit

import pytest

from helpers.mysql_fixture import mysql_database
from tango_backend.store import MysqlStore


NOW = 1792742400000
USER = "diagnostic-user-one"
DEVICE = "diagnostic-device-one"
CONVERSATION = "diagnostic-conversation-one"
TASK = "diagnostic-task-one"
ROUTE = "diagnostic-route-one"
MESSAGE = "diagnostic-message-one"


def _configuration(directory, url):
    """只向临时文件写入本例随机库入口，不继承真实后台环境。"""
    path = Path(directory) / "diagnostic-storage.env"
    path.write_text("QUIET_STORAGE_DRIVER=mysql\nQUIET_DATABASE_URL=" + url + "\n")
    path.chmod(0o600)
    return path


async def _connection(url):
    import asyncmy

    parsed = urlsplit(url)
    return await asyncmy.connect(
        host=parsed.hostname,
        port=parsed.port or 3306,
        user=unquote(parsed.username or ""),
        password=unquote(parsed.password or ""),
        db=parsed.path.lstrip("/"),
        autocommit=True,
        charset="utf8mb4",
    )


@pytest.fixture
async def mysql_diagnostic_store(tmp_path):
    async with mysql_database() as url:
        store = MysqlStore(connectionString=url)
        try:
            # 建表属于测试准备，诊断入口本身不能初始化或迁移数据库。
            await store.initialize()
            await store.db.execute(
                "INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)",
                [USER, "fixture-user", "fixture-user", "fixture-salt", "fixture-hash", NOW],
            )
            yield SimpleNamespace(store=store, env=_configuration(tmp_path, url), url=url)
        finally:
            await store.close()


async def test_mysql_diagnostics_snapshot_is_consistent_and_rejects_writes(mysql_diagnostic_store):
    from tango_backend.diagnostics import DiagnosticError, open_database

    fixture = mysql_diagnostic_store
    async with open_database(env_file=fixture.env) as db:
        assert db.dialect == "mysql"
        original = await db.fetchall("SELECT username FROM users WHERE id=?", [USER])
        await fixture.store.db.execute("UPDATE users SET username=? WHERE id=?", ["changed-by-writer", USER])
        assert await db.fetchall("SELECT username FROM users WHERE id=?", [USER]) == original
        with pytest.raises(DiagnosticError):
            await db.fetchall("UPDATE users SET username=? WHERE id=?", ["forbidden-write", USER])
        with pytest.raises(DiagnosticError):
            await db.fetchall("SELECT username FROM users WHERE id=? FOR UPDATE", [USER])
    async with open_database(env_file=fixture.env) as db:
        assert (await db.fetchall("SELECT username FROM users WHERE id=?", [USER])) == [
            {"username": "changed-by-writer"}
        ]


async def _seed_report_fixture(store):
    """构造互相关联的路由、检查点与未知调用，不让诊断器执行任务。"""
    journal = {
        "id": CONVERSATION,
        "deviceId": DEVICE,
        "version": 3,
        "messages": [
            {"id": MESSAGE, "role": "user", "content": "推荐20分钟跑步歌单，不要现场版", "sequence": 1, "createdAt": "2026-10-23T00:00:00.000Z"}
        ],
        "taskLinks": {TASK: {"turnId": MESSAGE, "revision": 1}},
        "latestTaskId": TASK,
        "receipts": {MESSAGE: {"sequence": 1}},
    }
    snapshot = {
        "id": TASK,
        "userId": USER,
        "deviceId": DEVICE,
        "revision": 1,
        "stateVersion": 5,
        "status": "paused",
        "phase": "paused",
        "createdAt": NOW,
        "updatedAt": NOW + 20,
        "runId": "diagnostic-run-one",
        "instruction": "推荐20分钟跑步歌单，不要现场版",
        "preference": "",
        "brief": {"targetMinutes": 20, "excludeLive": True, "scene": "跑步"},
        "pendingSteps": [],
        "lockedIds": [],
        "trace": [{"node": "paused", "revision": 1, "at": NOW + 20}],
        "toolHistory": [{
            "stepId": "diagnostic-step-one", "skill": "scene.research", "version": 1,
            "runId": "diagnostic-run-one", "revision": 1, "attempt": 1,
            "status": "completed", "dispatchedAt": NOW + 5, "completedAt": NOW + 10,
            "input": {"queries": ["跑步20分钟"]},
            "result": {"tracks": [], "sources": []},
        }],
        "preview": {"tracks": [], "totalDurationMs": 0, "checks": [
            {"key": "duration", "status": "unverified", "text": "候选不足，无法核验目标时长"}
        ]},
        "budget": {"runCalls": 1, "totalCalls": 1, "maxRunCalls": 6, "maxTaskCalls": 24,
                   "toolCalls": 1, "maxToolCalls": 4, "inputTokens": 0, "outputTokens": 0},
        "messages": journal["messages"],
        "degradations": [],
        "error": {"code": "SERVER_RESTART", "message": "未知调用须人工检查"},
    }
    await store.db.execute(
        "INSERT INTO music_conversations(id,user_id,device_id,request_id,payload_hash,journal_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)",
        [CONVERSATION, USER, DEVICE, "diagnostic-create-one", "a" * 64, json.dumps(journal), NOW, NOW + 20],
    )
    await store.db.execute(
        "INSERT INTO music_tasks(id,user_id,device_id,revision,state_version,status,snapshot_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)",
        [TASK, USER, DEVICE, 1, 5, "paused", json.dumps(snapshot), NOW, NOW + 20],
    )
    await store.db.execute(
        "INSERT INTO music_conversation_routes(id,conversation_id,user_id,device_id,message_id,input_hash,input_json,capability_version,status,paid,attempt_id,owner_id,owner_token,owner_epoch,lease_until,result_json,error_code,created_at,finished_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        [ROUTE, CONVERSATION, USER, DEVICE, MESSAGE, "b" * 64,
         json.dumps({"instruction": "推荐20分钟跑步歌单，不要现场版"}), 2, "running", 1,
         "diagnostic-attempt-one", "fixture-worker", "private-owner-token", 1, NOW + 10,
         None, None, NOW, None],
    )
    await store.db.execute(
        "INSERT INTO music_conversation_route_attempts(id,route_id,status,paid,started_at,finished_at,input_tokens,output_tokens,error_code) VALUES(?,?,?,?,?,?,?,?,?)",
        ["diagnostic-attempt-one", ROUTE, "reserved", 1, NOW, None, None, None, None],
    )
    await store.db.execute(
        "INSERT INTO music_task_calls(id,task_id,revision,run_id,node,status,started_at,finished_at,input_tokens,output_tokens,error_code) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        ["diagnostic-call-one", TASK, 1, "diagnostic-run-one", "compose", "uncertain", NOW, NOW + 20,
         None, None, "SERVER_RESTART"],
    )


async def test_mysql_diagnostics_reports_linked_rows_without_fabricating_tokens(mysql_diagnostic_store):
    from tango_backend.diagnostics import open_database, report

    fixture = mysql_diagnostic_store
    await _seed_report_fixture(fixture.store)
    async with open_database(env_file=fixture.env) as db:
        recent = await report(db, "recent", limit=2, now_ms=NOW + 100)
        conversation = await report(db, "conversation", CONVERSATION, now_ms=NOW + 100)
        task = await report(db, "task", TASK, now_ms=NOW + 100)
    assert recent["readOnly"] is True
    assert any(value["id"] == CONVERSATION for value in recent["conversations"])
    assert any(value["id"] == TASK for value in recent["tasks"])
    assert conversation["conversation"]["id"] == CONVERSATION
    assert conversation["routes"][0]["status"] == "running"
    assert conversation["routes"][0]["effectiveStatus"] == "uncertain"
    assert conversation["routes"][0]["attempt"]["inputTokens"] is None
    assert task["task"]["id"] == TASK
    assert task["calls"][0]["status"] == "uncertain"
    assert task["calls"][0]["inputTokens"] is None
    assert task["calls"][0]["outputTokens"] is None
    assert task["checks"][0]["status"] == "unverified"
    assert task["toolHistory"][0]["skill"] == "scene.research"
    assert any(value["id"] == CONVERSATION for value in task["conversations"])
    assert "private-owner-token" not in json.dumps([recent, conversation, task])
    # 查看过期记录不能代替业务恢复，更不能将 running/NULL 原地改写。
    route = await fixture.store.db.fetchone("SELECT status,finished_at FROM music_conversation_routes WHERE id=?", [ROUTE])
    assert route == {"status": "running", "finished_at": None}


async def test_mysql_diagnostics_missing_schema_never_creates_tables(tmp_path):
    from tango_backend.diagnostics import DiagnosticError, open_database, report

    async with mysql_database() as url:
        env = _configuration(tmp_path, url)
        async with open_database(env_file=env) as db:
            with pytest.raises(DiagnosticError):
                await report(db, "recent", now_ms=NOW)
        connection = await _connection(url)
        try:
            async with connection.cursor() as cursor:
                await cursor.execute("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE()")
                assert (await cursor.fetchone())[0] == 0
        finally:
            await connection.ensure_closed()


async def test_mysql_diagnostics_identifier_cannot_expand_target_scope(mysql_diagnostic_store):
    from tango_backend.diagnostics import DiagnosticError, open_database, report

    fixture = mysql_diagnostic_store
    await _seed_report_fixture(fixture.store)
    async with open_database(env_file=fixture.env) as db:
        with pytest.raises(DiagnosticError):
            await report(db, "conversation", CONVERSATION + "' OR '1'='1", now_ms=NOW)
        with pytest.raises(DiagnosticError):
            await report(db, "task", "missing-task-one", now_ms=NOW)
        actual = await report(db, "task", TASK, now_ms=NOW + 100)
        assert actual["task"]["id"] == TASK
