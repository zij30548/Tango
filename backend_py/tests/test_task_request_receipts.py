"""原操作事实查询的 HTTP 契约；使用隔离数据库与模拟模型。"""

import json
from uuid import uuid4

import pytest

from test_app import fx, until
from tango_backend.compat import clone, iso, now_ms
from tango_backend.store import ensureTaskRequestMetadataColumn
from tango_backend.task_store import TaskStore, requestMetadataJson, readRequestFacts
from tango_backend import migrate_mysql as migration


async def assert_receipt_finds_create_and_control_without_reexecution(fx):
    device, request = str(uuid4()), str(uuid4())
    result = await fx.client.post("/v1/tasks", json={
        "deviceId": device, "requestId": request, "instruction": "读书二十分钟",
        "preference": "不要现场版",
    })
    assert result.status_code == 201, result.text
    task = await until(fx.client, result.json()["task"]["id"], {"waiting_tools"})
    control = "route-" + uuid4().hex
    paused = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json={
        "deviceId": device, "requestId": control,
        "expectedRevision": task["revision"], "action": "pause",
    })
    assert paused.status_code == 200, paused.text
    saved = await fx.app.tasks.store.load(task["id"], fx.session["user"]["id"])
    before = json.dumps(saved, sort_keys=True)
    calls = list(fx.model.calls)
    usage = (await fx.client.get("/v1/account/usage")).json()
    # 查创建和控制均读同一份当前快照，不重新执行创建或暂停。
    for identifier in [request, control, request]:
        result = await fx.client.get(f"/v1/tasks/requests/{device}/{identifier}")
        assert result.status_code == 200, result.text
        body = result.json()
        assert body["receipt"] == {
            "deviceId": device, "requestId": identifier,
            "status": "recorded", "taskId": task["id"],
            **({"action": "create"} if identifier == request else {
                "action": "pause", "expectedRevision": task["revision"],
            }),
        }
        assert body["task"]["id"] == task["id"]
        assert body["task"]["status"] == "paused"
        for private in ["userId", "preference", "instruction", "routingContext", "_observation"]:
            assert private not in body["task"]
        assert "payload_hash" not in result.text
    assert list(fx.model.calls) == calls
    assert (await fx.client.get("/v1/account/usage")).json() == usage
    assert json.dumps(await fx.app.tasks.store.load(task["id"], fx.session["user"]["id"]), sort_keys=True) == before


async def assert_receipt_unknown_and_account_device_isolation(fx):
    device, request = str(uuid4()), str(uuid4())
    result = await fx.client.post("/v1/tasks", json={
        "deviceId": device, "requestId": request, "instruction": "读书二十分钟",
    })
    assert result.status_code == 201
    task = await until(fx.client, result.json()["task"]["id"], {"waiting_tools"})
    for requested_device, identifier in [(device, str(uuid4())), (str(uuid4()), request)]:
        result = await fx.client.get(f"/v1/tasks/requests/{requested_device}/{identifier}")
        assert result.status_code == 200
        assert result.json() == {"receipt": {
            "deviceId": requested_device, "requestId": identifier, "status": "unknown",
        }, "task": None}
        assert task["id"] not in result.text
    registered = await fx.client.post("/v1/auth/register", json={
        "username": "other-receipt-user", "password": "fixture-private-password",
    })
    assert registered.status_code == 201, registered.text
    result = await fx.client.get(f"/v1/tasks/requests/{device}/{request}", headers={
        "authorization": "Bearer " + registered.json()["token"],
    })
    assert result.status_code == 200
    assert result.json()["receipt"]["status"] == "unknown"
    assert result.json()["task"] is None


async def assert_receipt_does_not_enter_runtime_recovery(fx, monkeypatch):
    device, request = str(uuid4()), str(uuid4())
    result = await fx.client.post("/v1/tasks", json={
        "deviceId": device, "requestId": request, "instruction": "读书二十分钟",
    })
    task = await until(fx.client, result.json()["task"]["id"], {"waiting_tools"})
    saved = await fx.app.tasks.store.load(task["id"], fx.session["user"]["id"])
    before = json.dumps(saved, sort_keys=True)

    async def forbidden(*args, **kwargs):
        pytest.fail("只读原操作查询进入了执行或恢复路径")

    for name in ["get", "create", "action", "result"]:
        monkeypatch.setattr(fx.app.tasks, name, forbidden)
    monkeypatch.setattr(fx.app.tasks.store, "recoverInterrupted", forbidden)
    calls = list(fx.model.calls)
    result = await fx.client.get(f"/v1/tasks/requests/{device}/{request}")
    assert result.status_code == 200, result.text
    assert result.json()["receipt"]["status"] == "recorded"
    assert list(fx.model.calls) == calls
    assert json.dumps(await fx.app.tasks.store.load(task["id"], fx.session["user"]["id"]), sort_keys=True) == before


async def assert_receipt_http_boundaries(fx):
    device, request = str(uuid4()), str(uuid4())
    path = f"/v1/tasks/requests/{device}/{request}"
    result = await fx.client.get(path, headers={"authorization": ""})
    assert result.status_code == 401
    for invalid in ["short", "x" * 101, "invalid/message"]:
        result = await fx.client.get(f"/v1/tasks/requests/{device}/{invalid}")
        assert result.status_code == 404
    result = await fx.client.get(path + "?override=1")
    assert result.status_code == 400
    result = await fx.client.post(path, json={})
    assert result.status_code == 404
    assert fx.model.calls == []


async def test_receipt_finds_create_and_control_without_reexecution(fx):
    await assert_receipt_finds_create_and_control_without_reexecution(fx)


async def test_receipt_unknown_and_account_device_isolation(fx):
    await assert_receipt_unknown_and_account_device_isolation(fx)


async def test_receipt_does_not_enter_runtime_recovery(fx, monkeypatch):
    await assert_receipt_does_not_enter_runtime_recovery(fx, monkeypatch)


async def test_receipt_http_boundaries(fx):
    await assert_receipt_http_boundaries(fx)


async def assert_original_adopt_facts_survive_newer_task_revision(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={
        "deviceId": device, "requestId": request, "instruction": "读书20分钟",
    })
    assert created.status_code == 201, created.text
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    stamp = iso(now_ms())
    catalog = {"checkedAt": stamp, "sources": [{"id": 77, "name": "读书", "tags": [], "checkedAt": stamp}],
               "tracks": [{"id": index + 1, "title": f"合成歌曲{index + 1}", "artist": "合成歌手",
                           "album": "合成专辑", "durationMs": 240000, "sourceId": 77,
                           "sourceKind": "playlist", "sourceName": "读书", "sourceTags": []} for index in range(10)]}
    for step in task["pendingSteps"]:
        result = await fx.client.post(f'/v1/tasks/{task["id"]}/steps/{step["id"]}/result', json={
            "deviceId": device, "leaseId": step["leaseId"], "revision": step["revision"], "result": catalog,
        })
        assert result.status_code == 200, result.text
    task = await until(fx.client, task["id"], {"preview", "failed"})
    assert task["status"] == "preview", task.get("error")
    adoption = {"deviceId": device, "requestId": str(uuid4()), "action": "adopt", "expectedRevision": task["revision"]}
    adopted = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json=adoption)
    assert adopted.status_code == 200, adopted.text
    current = adopted.json()["task"]
    applied = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json={
        "deviceId": device, "requestId": str(uuid4()), "action": "applied",
        "expectedRevision": current["revision"], "adoptionId": current["adoption"]["id"],
    })
    assert applied.status_code == 200, applied.text
    current = applied.json()["task"]
    assert current["status"] == "adopted"
    # 采用和设备回执不改变revision；后续锁定才产生新版本，原采用事实仍须保留。
    changed = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json={
        "deviceId": device, "requestId": str(uuid4()), "action": "lock",
        "expectedRevision": current["revision"], "lockedIds": [],
    })
    assert changed.status_code == 200, changed.text
    current = changed.json()["task"]
    assert current["status"] == "preview" and current["revision"] > adoption["expectedRevision"]
    calls = list(fx.model.calls)
    usage = (await fx.client.get("/v1/account/usage")).json()
    for _ in range(2):
        result = await fx.client.get(f'/v1/tasks/requests/{device}/{adoption["requestId"]}')
        assert result.status_code == 200, result.text
        assert result.json()["receipt"] == {
            "deviceId": device, "requestId": adoption["requestId"], "status": "recorded",
            "taskId": task["id"], "action": "adopt", "expectedRevision": adoption["expectedRevision"],
        }
        assert result.json()["task"] == current
    user = fx.session["user"]["id"]
    assert await fx.app.tasks.store.requestReceipt(user, device, adoption["requestId"]) == await fx.app.tasks.store.load(task["id"], user)
    assert list(fx.model.calls) == calls
    assert (await fx.client.get("/v1/account/usage")).json() == usage


async def assert_create_extra_fields_cannot_forge_adopt_facts(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={
        "deviceId": device, "requestId": request, "instruction": "读书二十分钟",
        "action": "adopt", "expectedRevision": 777, "preference": "不要现场版",
        "apiKey": "fixture-not-a-real-secret", "catalogContext": "private_fixture_catalog_context",
    })
    assert created.status_code == 201, created.text
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    result = await fx.client.get(f"/v1/tasks/requests/{device}/{request}")
    assert result.json()["receipt"]["action"] == "create"
    assert "expectedRevision" not in result.json()["receipt"]
    row = await fx.store.db.fetchone("SELECT request_metadata_json FROM music_task_requests WHERE request_id=?", [request])
    metadata = row["request_metadata_json"]
    assert (json.loads(metadata) if isinstance(metadata, str) else metadata) == {"action": "create"}
    assert task["status"] == "waiting_tools"


async def assert_legacy_and_malformed_facts_remain_unknown(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={"deviceId": device, "requestId": request, "instruction": "读书二十分钟"})
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    for value in [None, "null", "[]", "true", '{"action":"adopt"}',
                  '{"action":"adopt","expectedRevision":true}',
                  '{"action":"adopt","expectedRevision":-1}',
                  '{"action":"create","expectedRevision":1}',
                  '{"action":"adopt","expectedRevision":1,"apiKey":"private-fixture"}']:
        await fx.store.db.execute("UPDATE music_task_requests SET request_metadata_json=? WHERE request_id=?", [value, request])
        result = await fx.client.get(f"/v1/tasks/requests/{device}/{request}")
        assert result.json()["receipt"] == {
            "deviceId": device, "requestId": request, "status": "recorded", "taskId": task["id"],
        }
    assert await fx.app.tasks.store.requestFacts(fx.session["user"]["id"], str(uuid4()), request) == {}


async def assert_metadata_and_original_operation_rollback_together(fx, monkeypatch):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={"deviceId": device, "requestId": request, "instruction": "读书二十分钟"})
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    paused = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json={
        "deviceId": device, "requestId": str(uuid4()), "action": "pause", "expectedRevision": task["revision"],
    })
    assert paused.status_code == 200, paused.text
    store = fx.app.tasks.store
    before = await store.load(task["id"], fx.session["user"]["id"])
    changed = clone(before)
    changed.update(status="cancelled", revision=before["revision"] + 1, stateVersion=before["stateVersion"] + 1,
                   updatedAt=before["updatedAt"] + 1)
    original = store.remember

    async def fail_after_insert(*args, **kwargs):
        await original(*args, **kwargs)
        raise RuntimeError("fixture-after-request-insert")

    failed_request = str(uuid4())
    body = {"deviceId": device, "requestId": failed_request, "action": "cancel", "expectedRevision": before["revision"]}
    monkeypatch.setattr(store, "remember", fail_after_insert)
    with pytest.raises(RuntimeError, match="fixture-after-request-insert"):
        await store.commit(changed, failed_request, body, {"expectedStateVersion": before["stateVersion"], "requestAction": "cancel"})
    assert await store.load(task["id"], fx.session["user"]["id"]) == before
    assert await store.requestReceipt(fx.session["user"]["id"], device, failed_request) is None
    assert await store.requestFacts(fx.session["user"]["id"], device, failed_request) == {}
    monkeypatch.setattr(store, "remember", original)
    with pytest.raises(TypeError, match="Invalid original request metadata"):
        await store.commit(changed, failed_request, body, {"expectedStateVersion": before["stateVersion"], "requestAction": "adopt"})
    assert await store.load(task["id"], fx.session["user"]["id"]) == before
    # 没有可信入口类型的直接旧提交只能记录未知，不能从正文猜采用授权。
    await store.commit(changed, failed_request, body, {"expectedStateVersion": before["stateVersion"]})
    assert await store.requestFacts(fx.session["user"]["id"], device, failed_request) == {}
    assert (await store.requestReceipt(fx.session["user"]["id"], device, failed_request))["status"] == "cancelled"


async def assert_additive_request_upgrade_preserves_old_rows(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={"deviceId": device, "requestId": request, "instruction": "读书二十分钟"})
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    old = await fx.store.db.fetchone("SELECT * FROM music_task_requests WHERE request_id=?", [request])
    await fx.store.db.execute("ALTER TABLE music_task_requests DROP COLUMN request_metadata_json")
    # 只重开本例自建库，模拟升级前已有请求；初始化不碰应用服务或真实用户库。
    reopened = type(fx.store)(fx.store.db.connectionString if fx.store.dialect == "mysql" else fx.store.path)
    try:
        upgraded = TaskStore(reopened)
        await upgraded.initialize()
        await ensureTaskRequestMetadataColumn(reopened.db, reopened.dialect)
        row = await reopened.db.fetchone("SELECT * FROM music_task_requests WHERE request_id=?", [request])
        assert row["request_metadata_json"] is None
        assert {key: value for key, value in row.items() if key != "request_metadata_json"} == {
            key: value for key, value in old.items() if key != "request_metadata_json"
        }
        assert await upgraded.requestFacts(fx.session["user"]["id"], device, request) == {}
        assert (await upgraded.requestReceipt(fx.session["user"]["id"], device, request))["id"] == task["id"]
    finally:
        await reopened.close()


async def assert_receipt_join_rejects_spoofed_request_owner_and_device(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={"deviceId": device, "requestId": request, "instruction": "读书二十分钟"})
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    registered = await fx.client.post("/v1/auth/register", json={"username": "spoofed-receipt-user", "password": "fixture-private-password"})
    assert registered.status_code == 201, registered.text
    owner = fx.session["user"]["id"]
    other = registered.json()["user"]["id"]
    for claimed_user, claimed_device in [(owner, str(uuid4())), (other, device)]:
        spoofed = str(uuid4())
        await fx.store.db.execute(
            "INSERT INTO music_task_requests(user_id,device_id,request_id,payload_hash,task_id,request_metadata_json) VALUES(?,?,?,?,?,?)",
            [claimed_user, claimed_device, spoofed, "0" * 64, task["id"], '{"action":"adopt","expectedRevision":1}'],
        )
        assert await fx.app.tasks.store.requestReceipt(claimed_user, claimed_device, spoofed) is None
        assert await fx.app.tasks.store.requestFacts(claimed_user, claimed_device, spoofed) == {}


async def assert_accepted_integral_revision_is_recorded_as_integer(fx):
    device, request = str(uuid4()), str(uuid4())
    created = await fx.client.post("/v1/tasks", json={"deviceId": device, "requestId": request, "instruction": "读书二十分钟"})
    task = await until(fx.client, created.json()["task"]["id"], {"waiting_tools"})
    control = str(uuid4())
    result = await fx.client.post(f'/v1/tasks/{task["id"]}/actions', json={
        "deviceId": device, "requestId": control, "action": "pause", "expectedRevision": float(task["revision"]),
    })
    assert result.status_code == 200, result.text
    facts = await fx.app.tasks.store.requestFacts(fx.session["user"]["id"], device, control)
    assert facts == {"action": "pause", "expectedRevision": task["revision"]}
    assert type(facts["expectedRevision"]) is int


async def test_original_adopt_facts_survive_newer_task_revision(fx):
    await assert_original_adopt_facts_survive_newer_task_revision(fx)


async def test_create_extra_fields_cannot_forge_adopt_facts(fx):
    await assert_create_extra_fields_cannot_forge_adopt_facts(fx)


async def test_legacy_and_malformed_facts_remain_unknown(fx):
    await assert_legacy_and_malformed_facts_remain_unknown(fx)


async def test_metadata_and_original_operation_rollback_together(fx, monkeypatch):
    await assert_metadata_and_original_operation_rollback_together(fx, monkeypatch)


async def test_additive_request_upgrade_preserves_old_rows(fx):
    await assert_additive_request_upgrade_preserves_old_rows(fx)


async def test_receipt_join_rejects_spoofed_request_owner_and_device(fx):
    await assert_receipt_join_rejects_spoofed_request_owner_and_device(fx)


async def test_accepted_integral_revision_is_recorded_as_integer(fx):
    await assert_accepted_integral_revision_is_recorded_as_integer(fx)


@pytest.mark.parametrize("value", [None, "not-json", "x" * 1000, 1, [],
                                 {"action": "adopt"}, {"action": "adopt", "expectedRevision": 1.0},
                                 {"action": "adopt", "expectedRevision": 9007199254740992},
                                 {"action": "adopt", "expectedRevision": True},
                                 {"action": "unknown", "expectedRevision": 1},
                                 {"action": "create", "apiKey": "fixture-secret"}])
def test_request_facts_reject_malformed_metadata_without_guessing(value):
    assert readRequestFacts(value) == {}


def test_request_metadata_uses_explicit_validated_action_and_minimal_projection():
    body = {"action": "adopt", "expectedRevision": 2, "instruction": "fixture text", "preference": "fixture preference",
            "catalogContext": "private-context", "apiKey": "fixture-not-a-secret"}
    assert json.loads(requestMetadataJson("adopt", body)) == {"action": "adopt", "expectedRevision": 2}
    assert json.loads(requestMetadataJson("create", body)) == {"action": "create"}
    assert requestMetadataJson(None, body) is None
    for action, request in [("unsupported", body), ("pause", body), ("adopt", {**body, "expectedRevision": True}),
                            ("adopt", {**body, "expectedRevision": 0}), ("adopt", None)]:
        with pytest.raises(TypeError):
            requestMetadataJson(action, request)
    assert readRequestFacts(requestMetadataJson("adopt", body)) == {"action": "adopt", "expectedRevision": 2}
    normalized = json.loads(requestMetadataJson("adopt", {**body, "expectedRevision": 2.0}))
    assert normalized == {"action": "adopt", "expectedRevision": 2} and type(normalized["expectedRevision"]) is int


def test_legacy_request_migration_allows_only_nullable_facts_and_canonical_json():
    assert migration.migration_columns("music_task_requests", ["request_id"], ["request_id", "request_metadata_json"]) == ["request_id", "request_metadata_json"]
    for table in ["users", "music_tasks", "music_task_calls"]:
        with pytest.raises(migration.MigrationError, match="COLUMN_SCHEMA_MISMATCH"):
            migration.migration_columns(table, ["id"], ["id", "request_metadata_json"])
    value = {"action": "adopt", "expectedRevision": 2}
    assert migration.row_digest({"request_metadata_json": value}, ["request_metadata_json"]) == migration.row_digest({"request_metadata_json": json.dumps(value)}, ["request_metadata_json"])
    assert json.loads(migration._insert_value("request_metadata_json", value)) == value
