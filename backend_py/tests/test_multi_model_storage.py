"""模型选择与上游回执的事务账本；只使用随机测试库和合成模型标识。"""

import json
import os
from contextlib import AsyncExitStack
from types import SimpleNamespace
from uuid import uuid4

import pytest

from helpers.mysql_fixture import mysql_database
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.compat import digest, iso
from tango_backend.conversation_route_store import ConversationRouteStore
from tango_backend.conversation_store import ConversationStore
from tango_backend.errors import ApiError
from tango_backend import migrate_mysql as migration
from tango_backend.store import Store, PostgresStore, MysqlStore, ensureModelLedgerColumns
from tango_backend.task_store import (
    TaskStore, modelSelectionJson, modelMetadataJson, readModelLedger,
)
from test_mysql_migration import pair, owned_pg  # 复用仅由 runner 创建的迁移源与目标夹具。

NOW = 1792483200000
DEVICE = "isolated_model_ledger_device"
SELECTION = {
    "profile": "plus", "configuredModel": "qwen-plus-fixture",
    "configFingerprint": "b" * 64, "policyVersion": "qwen-task-routing-v1",
    "strategy": "task_split", "purpose": "plan", "reason": "multiple_constraints",
}
METADATA = {"providerModel": "qwen-plus-fixture-response", "providerResponseId": "chatcmpl-fixture-001"}


@pytest.mark.parametrize("field,value", [
    ("profile", "admin"), ("configuredModel", "https://internal/private"),
    ("configuredModel", "sk-private-key"), ("configuredModel", "x" * 201),
    ("configFingerprint", "b" * 63), ("configFingerprint", "B" * 64),
    ("policyVersion", "unvalidated-future"), ("strategy", "force-free"),
    ("purpose", "arbitrary_code"), ("reason", "full user content must not be logged"),
])
def test_selection_contract_rejects_untrusted_fields(field, value):
    with pytest.raises(TypeError):
        modelSelectionJson({**SELECTION, field: value})


def test_selection_contract_has_exact_keys_and_detached_bounded_json():
    encoded = modelSelectionJson(SELECTION)
    assert json.loads(encoded) == SELECTION
    assert len(encoded) < 1024
    for invalid in ({**SELECTION, "apiKey": "secret"}, {key: value for key, value in SELECTION.items() if key != "profile"}):
        with pytest.raises(TypeError):
            modelSelectionJson(invalid)
    assert modelSelectionJson(None) is None


@pytest.mark.parametrize("value", [
    {"apiKey": "secret"}, {"providerModel": "https://upstream/private"},
    {"providerResponseId": "Bearer token"}, {"providerModel": "sk-private"},
    {"providerResponseId": "x" * 201}, {"providerResponseId": "model\nsecret"},
    {"providerModel": 123}, {"providerResponseId": ["id"]},
])
def test_response_metadata_is_not_a_log_dump(value):
    with pytest.raises(TypeError):
        modelMetadataJson(value)


def test_response_metadata_allows_missing_provider_evidence_without_guessing():
    assert modelMetadataJson(None) is None
    assert modelMetadataJson({"providerModel": None, "providerResponseId": None}) is None
    assert json.loads(modelMetadataJson(METADATA)) == METADATA
    for row in ({}, {"model_selection_json": None, "model_metadata_json": None}):
        assert readModelLedger(row) == {"selectionStatus": "legacy_unknown", "modelSelection": None, "modelMetadata": None}
    for selection in (SELECTION, modelSelectionJson(SELECTION)):
        view = readModelLedger({"model_selection_json": selection, "model_metadata_json": modelMetadataJson(METADATA)})
        assert view == {"selectionStatus": "recorded", "modelSelection": SELECTION, "modelMetadata": METADATA}
        view["modelSelection"]["profile"] = "mutated"
        assert SELECTION["profile"] == "plus"


@pytest.mark.parametrize("table", ["music_task_calls", "music_conversation_route_attempts"])
def test_old_source_schema_may_omit_only_known_nullable_model_columns(table):
    assert migration.migration_columns(table, ["id", "status"], ["id", "status", *migration.MODEL_LEDGER_COLUMNS]) == sorted(["id", "status", *migration.MODEL_LEDGER_COLUMNS])
    for source, target in [(["id"], ["id", "new_unknown"]), (["id", "lost_source"], ["id"])]:
        with pytest.raises(migration.MigrationError, match="COLUMN_SCHEMA_MISMATCH"):
            migration.migration_columns(table, source, target)
    with pytest.raises(migration.MigrationError, match="COLUMN_SCHEMA_MISMATCH"):
        migration.migration_columns("users", ["id"], ["id", "model_selection_json"])


def test_new_model_json_columns_use_canonical_migration_hashing():
    for column, value in [("model_selection_json", SELECTION), ("model_metadata_json", METADATA)]:
        assert migration.row_digest({column: value}, [column]) == migration.row_digest({column: json.dumps(value, indent=2)}, [column])
        assert migration.row_digest({column: None}, [column]) != migration.row_digest({column: "null"}, [column])
        assert json.loads(migration._insert_value(column, value)) == value


@pytest.fixture(params=["sqlite", "postgres", "mysql"])
async def ledger(request, tmp_path):
    stores = []
    async with AsyncExitStack() as cleanup:
        if request.param == "postgres":
            url = os.environ.get("TANGO_TEST_DATABASE_URL")
            if not url:
                pytest.skip("需要 owned PG runner 提供临时实例；不读取用户配置")
            import asyncpg
            admin = await asyncpg.connect(url)
            schema = "tango_model_ledger_" + uuid4().hex
            await admin.execute("CREATE SCHEMA " + schema)
            async def remove_schema():
                await admin.execute("DROP SCHEMA " + schema + " CASCADE")
                await admin.close()
            cleanup.push_async_callback(remove_schema)
            factory = lambda: PostgresStore(url, serverSettings={"search_path": schema})
        elif request.param == "mysql":
            url = await cleanup.enter_async_context(mysql_database())
            factory = lambda: MysqlStore(url)
        else:
            factory = lambda: Store(tmp_path / "model-ledger.sqlite")

        async def opened():
            store = factory()
            stores.append(store)
            store.accountPolicy = AccountPolicyStore(store)
            await store.initialize()
            return store

        store = await opened()
        user = str(uuid4())
        await store.db.execute("INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)",
                               [user, "ledger-fixture", "ledger-fixture", "fixture-salt", "fixture-hash", NOW])
        tasks = TaskStore(store)
        await tasks.initialize()
        conversations = ConversationStore(store, now=lambda: NOW)
        routes = ConversationRouteStore(store=store, taskStore=tasks, conversations=conversations, now=lambda: NOW)
        await routes.initialize()
        try:
            yield SimpleNamespace(store=store, tasks=tasks, routes=routes, conversations=conversations,
                                  user=user, opened=opened, dialect=request.param)
        finally:
            for value in stores:
                await value.close()


async def prepared_task(ledger):
    task = {"id": str(uuid4()), "userId": ledger.user, "deviceId": DEVICE, "revision": 1, "stateVersion": 1,
            "status": "planning", "runId": "model-fixture-run", "createdAt": NOW, "updatedAt": NOW}
    await ledger.tasks.save(task, expectedStateVersion=0, now=NOW)
    claim = await ledger.tasks.claimTask(task["id"], ledger.user, workerId="model-fixture-worker", now=NOW) if ledger.dialect != "sqlite" else None
    return task, claim


async def route_request(ledger):
    conversation = await ledger.conversations.create(ledger.user, {"deviceId": DEVICE, "requestId": str(uuid4())})
    message = {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "读书时听音乐。"}
    await ledger.conversations.message(conversation["id"], ledger.user, message, {"dispatchHint": "router"})
    context = await ledger.conversations.routeContext(conversation["id"], ledger.user, DEVICE, message["messageId"])
    frozen = {key: context[key] for key in ["conversationId", "messageId", "deviceId", "instruction", "history", "task"]}
    frozen["preference"] = ""
    return {"conversationId": conversation["id"], "messageId": message["messageId"], "deviceId": DEVICE,
            "userId": ledger.user, "input": frozen, "inputHash": digest(frozen), "capabilityVersion": 1}


async def test_task_selection_and_quota_share_one_reservation(ledger):
    task, claim = await prepared_task(ledger)
    call = await ledger.tasks.reserveCall(task, "plan", NOW, ledger.store, 30, 1000, claim=claim, modelSelection=SELECTION)
    row = await ledger.store.db.fetchone("SELECT * FROM music_task_calls WHERE id=?", [call])
    assert readModelLedger(row)["modelSelection"] == SELECTION
    assert row["status"] == "reserved" and readModelLedger(row)["modelMetadata"] is None
    assert (await ledger.store.accountPolicy.snapshot(ledger.user, NOW))["total"]["used"] == 1
    await ledger.tasks.finishCall(call, "completed", NOW + 1, usage={"inputTokens": 5, "outputTokens": 3}, modelMetadata=METADATA)
    await ledger.tasks.finishCall(call, "failed", NOW + 2, modelMetadata={"providerModel": "must-not-overwrite"})
    row = await ledger.store.db.fetchone("SELECT * FROM music_task_calls WHERE id=?", [call])
    assert row["status"] == "completed" and row["input_tokens"] == 5 and row["output_tokens"] == 3
    assert readModelLedger(row)["modelMetadata"] == METADATA


async def test_invalid_selection_or_budget_failure_cannot_create_partial_attempt(ledger):
    task, claim = await prepared_task(ledger)
    with pytest.raises(TypeError):
        await ledger.tasks.reserveCall(task, "plan", NOW, ledger.store, 30, 1000, claim=claim, modelSelection={**SELECTION, "apiKey": "forbidden"})
    assert (await ledger.store.accountPolicy.snapshot(ledger.user, NOW))["total"]["used"] == 0
    await ledger.store.accountPolicy.adminset(ledger.user, {"dailyLimit": 30, "totalLimit": 0, "enabled": True}, now=NOW)
    with pytest.raises(ApiError) as error:
        await ledger.tasks.reserveCall(task, "plan", NOW, ledger.store, 30, 1000, claim=claim, modelSelection=SELECTION)
    assert error.value.code == "AI_TOTAL_LIMIT"
    assert (await ledger.store.db.fetchone("SELECT count(*) AS n FROM music_task_calls"))["n"] == 0
    assert await ledger.tasks.getGlobalDailyUsage(iso(NOW)[:10]) == 0


async def test_route_reservation_records_model_not_request_metadata_and_is_once(ledger):
    request = await route_request(ledger)
    request["modelSelection"] = {"profile": "untrusted-client"}
    selected = {**SELECTION, "profile": "flash", "purpose": "route", "reason": "routing_flash"}
    acquired = await ledger.routes.begin(request, modelSelection=selected)
    replay = await ledger.routes.begin(request, modelSelection={**selected, "configuredModel": "later-config"})
    assert acquired["acquired"] and not replay["acquired"]
    await ledger.routes.finish(acquired["claim"], status="completed", result={"fixture": True}, modelMetadata=METADATA)
    await ledger.routes.finish(acquired["claim"], status="failed", modelMetadata={"providerModel": "must-not-overwrite"})
    row = await ledger.store.db.fetchone("SELECT * FROM music_conversation_route_attempts WHERE id=?", [acquired["claim"]["attemptId"]])
    assert readModelLedger(row)["modelSelection"] == selected
    assert readModelLedger(row)["modelMetadata"] == METADATA and row["status"] == "completed"
    assert (await ledger.store.accountPolicy.snapshot(ledger.user, NOW))["total"]["used"] == 1


async def test_additive_upgrade_preserves_old_unknown_attempts_and_is_repeatable(ledger):
    task, claim = await prepared_task(ledger)
    call = await ledger.tasks.reserveCall(task, "plan", NOW, ledger.store, 30, 1000, claim=claim)
    await ledger.tasks.finishCall(call, "uncertain", NOW + 1, errorCode="MODEL_TIMEOUT")
    # 模拟本阶段以前的表形状；只改本例自建库，不操作运行中的用户库。
    for table in ["music_task_calls", "music_conversation_route_attempts"]:
        for column in ["model_selection_json", "model_metadata_json"]:
            await ledger.store.db.execute(f"ALTER TABLE {table} DROP COLUMN {column}")
    reopened = await ledger.opened()
    tasks = TaskStore(reopened)
    await tasks.initialize()
    routes = ConversationRouteStore(store=reopened, taskStore=tasks)
    await routes.initialize()
    for table in ["music_task_calls", "music_conversation_route_attempts"]:
        await ensureModelLedgerColumns(reopened.db, table, reopened.dialect)
    row = await reopened.db.fetchone("SELECT * FROM music_task_calls WHERE id=?", [call])
    assert row["status"] == "uncertain" and row["error_code"] == "MODEL_TIMEOUT"
    assert readModelLedger(row)["selectionStatus"] == "legacy_unknown"
    assert (await reopened.accountPolicy.snapshot(ledger.user, NOW))["total"]["used"] == 1


async def test_route_invalid_metadata_does_not_finish_or_release_quota(ledger):
    request = await route_request(ledger)
    with pytest.raises(TypeError):
        await ledger.routes.begin(request, paid=False, modelSelection=SELECTION)
    assert await ledger.routes.get(request) is None
    acquired = await ledger.routes.begin(request, modelSelection={**SELECTION, "purpose": "route"})
    with pytest.raises(TypeError):
        await ledger.routes.finish(acquired["claim"], status="completed", modelMetadata={"endpoint": "forbidden"})
    row = await ledger.store.db.fetchone("SELECT * FROM music_conversation_route_attempts WHERE id=?", [acquired["claim"]["attemptId"]])
    assert row["status"] == "reserved" and readModelLedger(row)["modelMetadata"] is None
    await ledger.routes.finish(acquired["claim"], status="uncertain", errorCode="MODEL_TIMEOUT")
    assert (await ledger.store.accountPolicy.snapshot(ledger.user, NOW))["total"]["used"] == 1


@pytest.mark.parametrize("historical", [True, False], ids=["legacy-nullable", "recorded-metadata"])
async def test_real_pg_to_mysql_retains_old_unknown_or_recorded_model_evidence(pair, historical):
    if historical:
        for table in ["music_task_calls", "music_conversation_route_attempts"]:
            for column in migration.MODEL_LEDGER_COLUMNS:
                await pair.source.db.execute(f"ALTER TABLE {table} DROP COLUMN {column}")
    else:
        for table in ["music_task_calls", "music_conversation_route_attempts"]:
            await pair.source.db.execute(
                f"UPDATE {table} SET model_selection_json=?,model_metadata_json=?",
                [modelSelectionJson(SELECTION), modelMetadataJson(METADATA)],
            )
    result = await migration.migrate(pair.source_env, pair.target_env, execute=True, backup_directory=pair.backup)
    assert result["ok"] and result["verified"] and result["sourceRetained"]
    await pair.target.initialize()
    for table in ["music_task_calls", "music_conversation_route_attempts"]:
        rows = await pair.target.db.fetchall(f"SELECT * FROM {table}")
        assert rows
        for row in rows:
            evidence = readModelLedger(row)
            assert evidence["selectionStatus"] == ("legacy_unknown" if historical else "recorded")
            assert evidence["modelSelection"] == (None if historical else SELECTION)
            assert evidence["modelMetadata"] == (None if historical else METADATA)
        source_columns = await migration._columns(pair.source.db, table, "postgres")
        assert bool(set(source_columns) & migration.MODEL_LEDGER_COLUMNS) is not historical
