"""在拥有者临时 MySQL 上复用原操作回执的只读与隔离验收。"""

from test_mysql_app import fx
import test_task_request_receipts as contract
import pytest
from uuid import uuid4

from test_mysql_migration import pair, owned_pg
from tango_backend import migrate_mysql as migration
from tango_backend.compat import clone
from tango_backend.task_store import TaskStore


async def test_mysql_receipt_finds_create_and_control_without_reexecution(fx):
    await contract.assert_receipt_finds_create_and_control_without_reexecution(fx)


async def test_mysql_receipt_unknown_and_account_device_isolation(fx):
    await contract.assert_receipt_unknown_and_account_device_isolation(fx)


async def test_mysql_receipt_does_not_enter_runtime_recovery(fx, monkeypatch):
    await contract.assert_receipt_does_not_enter_runtime_recovery(fx, monkeypatch)


async def test_mysql_receipt_http_boundaries(fx):
    await contract.assert_receipt_http_boundaries(fx)


async def test_mysql_original_adopt_facts_survive_newer_task_revision(fx):
    await contract.assert_original_adopt_facts_survive_newer_task_revision(fx)


async def test_mysql_create_extra_fields_cannot_forge_adopt_facts(fx):
    await contract.assert_create_extra_fields_cannot_forge_adopt_facts(fx)


async def test_mysql_legacy_and_malformed_facts_remain_unknown(fx):
    await contract.assert_legacy_and_malformed_facts_remain_unknown(fx)


async def test_mysql_metadata_and_original_operation_rollback_together(fx, monkeypatch):
    await contract.assert_metadata_and_original_operation_rollback_together(fx, monkeypatch)


async def test_mysql_additive_request_upgrade_preserves_old_rows(fx):
    await contract.assert_additive_request_upgrade_preserves_old_rows(fx)


async def test_mysql_receipt_join_rejects_spoofed_request_owner_and_device(fx):
    await contract.assert_receipt_join_rejects_spoofed_request_owner_and_device(fx)


async def test_mysql_accepted_integral_revision_is_recorded_as_integer(fx):
    await contract.assert_accepted_integral_revision_is_recorded_as_integer(fx)


@pytest.mark.parametrize("historical", [True, False], ids=["legacy-null", "recorded-original"])
async def test_real_pg_to_mysql_retains_original_request_facts_or_unknown(pair, historical):
    task = clone(pair.task)
    task.update(status="paused", revision=2, stateVersion=2, updatedAt=pair.now + 1)
    request = str(uuid4())
    body = {"deviceId": task["deviceId"], "requestId": request, "action": "pause", "expectedRevision": 1}
    await pair.tasks.commit(task, request, body, {"expectedStateVersion": 1, "requestAction": "pause"})
    if historical:
        await pair.source.db.execute("ALTER TABLE music_task_requests DROP COLUMN request_metadata_json")
    result = await migration.migrate(pair.source_env, pair.target_env, execute=True, backup_directory=pair.backup)
    assert result["ok"] and result["verified"] and result["sourceRetained"]
    target = TaskStore(pair.target)
    assert await target.requestFacts(pair.user["id"], task["deviceId"], request) == (
        {} if historical else {"action": "pause", "expectedRevision": 1}
    )
    assert (await target.requestReceipt(pair.user["id"], task["deviceId"], request))["revision"] == 2
    columns = await migration._columns(pair.source.db, "music_task_requests", "postgres")
    assert ("request_metadata_json" in columns) is not historical
