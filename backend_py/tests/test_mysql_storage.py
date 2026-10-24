"""MySQL 8.4 账号与事务底座，全部操作运行器自建的随机临时库。"""
import asyncio
import json
import os
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlsplit, urlunsplit, unquote
from uuid import uuid4
import pytest
from tango_backend.store import MySQLStore, validateCredentials
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.invite_store import InviteStore, inviteCodeHash
from tango_backend.registration_service import RegistrationService
from tango_backend.development_session import DevelopmentSession
from tango_backend.task_store import TaskStore
from tango_backend.admin import runAdmin
from tango_backend.errors import ApiError
from tango_backend.conversation_store import ConversationStore

URL = os.environ.get("TANGO_MYSQL_TEST_URL")
pytestmark = pytest.mark.skipif(not URL, reason="需要独立运行器的临时 MySQL，不读取已有实例")
NOW = 1792454400000
PASSWORD = "mysql-fixture-password-123"
SALT = "00112233445566778899aabbccddeeff"
LEGACY = "45962d35b12efda03846132b638cf8fa99377151b402cc4b4f6059ad43d0974afb3b8d856505550dc270517f50af47acbc6aa87d0741e9907ba2a78065d85218"


@pytest.fixture
async def mysql():
    import asyncmy
    parsed = urlsplit(URL)
    admin = await asyncmy.connect(host=parsed.hostname, port=parsed.port or 3306, user=unquote(parsed.username), password=unquote(parsed.password), autocommit=True, charset="utf8mb4")
    name = "tango_storage_" + uuid4().hex
    async with admin.cursor() as cursor:
        await cursor.execute(f"CREATE DATABASE `{name}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
    url = urlunsplit((parsed.scheme, parsed.netloc, "/" + name, "", ""))
    stores = [MySQLStore(connectionString=url), MySQLStore(connectionString=url)]
    try:
        await asyncio.gather(*(store.initialize() for store in stores))
        user = {"id": str(uuid4()), "username": "mysql_fixture_user"}
        await stores[0].db.execute("INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)", [user["id"], user["username"], user["username"], SALT, LEGACY, NOW])
        for store in stores:
            store.accountPolicy = AccountPolicyStore(store)
        yield SimpleNamespace(stores=stores, store=stores[0], other=stores[1], user=user, url=url)
    finally:
        for store in stores:
            await store.close()
        async with admin.cursor() as cursor:
            await cursor.execute(f"DROP DATABASE `{name}`")
        admin.close()


async def test_mysql_schema_read_committed_exact_collation_and_parameter_contract(mysql):
    f = mysql
    row = await f.store.db.fetchone("SELECT @@transaction_isolation AS tx_level")
    assert row["tx_level"] == "READ-COMMITTED"
    assert await f.store.db.fetchone("SELECT $2 AS second,$1 AS first,$2 AS again,'100%' AS literal", ["甲", "乙"]) == {"second": "乙", "first": "甲", "again": "乙", "literal": "100%"}
    tables = await f.store.db.fetchall("SELECT TABLE_NAME AS table_name,ENGINE AS engine,TABLE_COLLATION AS collation_name FROM information_schema.tables WHERE TABLE_SCHEMA=DATABASE()")
    expected_tables = {"music_storage_locks", "users", "sessions", "ai_usage", "music_account_quota", "music_registration_invites", "music_registration_invite_uses", "music_tasks", "music_task_requests", "music_task_calls", "music_task_global_usage", "music_task_account_leases", "music_task_model_slots", "development_identities", "music_conversations", "music_conversation_clears", "music_conversation_routes", "music_conversation_route_attempts", "tango_storage_imports"}
    assert {r["table_name"] for r in tables} == expected_tables
    assert all(r["engine"] == "InnoDB" and r["collation_name"] == "utf8mb4_0900_bin" for r in tables)
    for identity, name in [("CaseID0001", "Upper"), ("caseid0001", "lower"), ("caseid0001 ", "space")]:
        await f.store.db.execute("INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)", [identity, name, name, SALT, LEGACY, NOW])
    assert (await f.store.db.fetchone("SELECT username FROM users WHERE id=?", ["CaseID0001"]))["username"] == "Upper"
    assert (await f.store.db.fetchone("SELECT username FROM users WHERE id=?", ["caseid0001 "]))["username"] == "space"


async def test_old_mysql_adds_clear_receipt_and_nullable_tombstone_without_rewriting_conversations(mysql):
    f = mysql
    convs = ConversationStore(f.store)
    conv = await convs.create(f.user["id"], {"deviceId": "legacy_mysql_clear_device", "requestId": str(uuid4())})
    before = await f.store.db.fetchone("SELECT journal_json FROM music_conversations WHERE id=?", (conv["id"],))
    await f.store.db.execute("DROP TABLE music_conversation_clears")
    await f.store.db.execute("ALTER TABLE music_conversations DROP COLUMN cleared_at")
    upgrades = [MySQLStore(f.url), MySQLStore(f.url)]
    try:
        await asyncio.gather(*(store.initialize() for store in upgrades))
        column = await upgrades[0].db.fetchone("SELECT IS_NULLABLE AS nullable,DATA_TYPE AS data_type FROM information_schema.columns WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='music_conversations' AND COLUMN_NAME='cleared_at'")
        assert column == {"nullable": "YES", "data_type": "bigint"}
        assert (await upgrades[1].db.fetchone("SELECT journal_json,cleared_at FROM music_conversations WHERE id=?", (conv["id"],))) == {**before, "cleared_at": None}
        assert (await upgrades[1].db.fetchone("SELECT count(*) AS n FROM music_conversation_clears"))["n"] == 0
    finally:
        for store in upgrades:
            await store.close()


async def test_mysql_old_password_upgrade_and_bounded_session_hashes(mysql):
    f = mysql
    user = await f.store.authenticate(validateCredentials({"username": "MYSQL_fixture_USER", "password": "fixture-password-长期"}))
    assert user == f.user
    assert (await f.store.db.fetchone("SELECT password_hash FROM users WHERE id=?", [user["id"]]))["password_hash"].startswith("scrypt-v2$")
    sessions = [await f.stores[n % 2].createSession(user, NOW + n) for n in range(12)]
    assert await f.store.session(sessions[0]["token"], NOW + 12) is None
    assert (await f.other.session(sessions[-1]["token"], NOW + 12))["user"] == user
    rows = await f.store.db.fetchall("SELECT token_hash FROM sessions WHERE user_id=?", [user["id"]])
    assert len(rows) == 10 and sessions[-1]["token"] not in json.dumps(rows)
    await f.other.logout(sessions[-1]["token"])
    assert await f.store.session(sessions[-1]["token"], NOW + 12) is None
    assert await f.store.session(sessions[-2]["token"], NOW + 8 * 86400000) is None


async def test_mysql_invite_consumption_is_atomic_across_instances_and_failure_does_not_spend(mysql):
    f = mysql
    issued = await InviteStore(f.store).issue({"maxUses": 2, "dailyLimit": 4, "totalLimit": 9}, NOW)
    services = [RegistrationService(store=s, now=lambda: NOW) for s in f.stores]
    results = await asyncio.gather(*[services[n % 2].register({"username": "mysql_invited_" + str(n), "password": PASSWORD, "inviteCode": issued["code"]}) for n in range(4)], return_exceptions=True)
    assert sum(isinstance(r, dict) for r in results) == 2
    assert all(isinstance(r, dict) or isinstance(r, ApiError) and r.code == "INVITE_INVALID" for r in results)
    assert (await InviteStore(f.store).list())[0]["usedCount"] == 2
    raw = await f.store.db.fetchone("SELECT * FROM music_registration_invites WHERE code_hash=?", [inviteCodeHash(issued["code"])])
    assert issued["code"] not in json.dumps(raw)
    for user in results:
        if isinstance(user, dict):
            usage = await f.store.accountPolicy.snapshot(user["id"], NOW)
            assert usage["daily"]["limit"] == 4 and usage["total"]["limit"] == 9
    next = await InviteStore(f.store).issue({"maxUses": 1}, NOW)
    with pytest.raises(ApiError) as duplicate:
        await services[0].register({"username": f.user["username"], "password": PASSWORD, "inviteCode": next["code"]})
    assert duplicate.value.code == "USERNAME_TAKEN"
    assert (await f.store.db.fetchone("SELECT used_count FROM music_registration_invites WHERE id=?", [next["invite"]["id"]]))["used_count"] == 0
    await InviteStore(f.store).disable(next["invite"]["id"])
    assert (await InviteStore(f.other).disable(next["invite"]["id"]))["disabled"]


async def test_mysql_account_global_quota_and_rollback_remain_one_transaction(mysql):
    f = mysql
    await f.store.accountPolicy.adminset(f.user["id"], {"dailyLimit": 2, "totalLimit": 3, "enabled": True}, now=NOW)
    repositories = [TaskStore(s) for s in f.stores]
    for repo in repositories:
        await repo.initialize()
    async def reserve(repo, now=NOW, globalLimit=100):
        async with repo.db.transaction() as db:
            await repo.lockUser(db, f.user["id"])
            await repo.reserveUsage(db, f.user["id"], now, 30, globalLimit)
        return True
    results = await asyncio.gather(*[reserve(repositories[n % 2]) for n in range(6)], return_exceptions=True)
    assert results.count(True) == 2
    assert all(r is True or isinstance(r, ApiError) and r.code == "AI_DAILY_LIMIT" for r in results)
    assert (await f.store.accountPolicy.snapshot(f.user["id"], NOW))["total"]["used"] == 2
    assert await repositories[0].globalUsage("2026-10-20") == 2
    with pytest.raises(RuntimeError):
        async with f.store.db.transaction() as db:
            await repositories[0].lockUser(db, f.user["id"])
            await repositories[0].reserveUsage(db, f.user["id"], NOW + 86400000, 30, 100)
            raise RuntimeError("synthetic rollback")
    assert (await f.store.accountPolicy.snapshot(f.user["id"], NOW + 86400000))["total"]["used"] == 2
    assert await repositories[0].globalUsage("2026-10-21") == 0


async def test_mysql_nested_savepoint_cancellation_and_ddl_never_fake_atomicity(mysql):
    f = mysql
    started = asyncio.Event()
    async def transaction():
        async with f.store.db.transaction() as db:
            await db.execute("UPDATE users SET created_at=? WHERE id=?", [NOW + 1, f.user["id"]])
            with pytest.raises(ValueError):
                async with db.transaction():
                    await db.execute("UPDATE users SET created_at=? WHERE id=?", [NOW + 2, f.user["id"]])
                    raise ValueError("nested rollback")
            assert (await db.fetchone("SELECT created_at FROM users WHERE id=?", [f.user["id"]]))["created_at"] == NOW + 1
            with pytest.raises(TypeError):
                await db.execute("CREATE TABLE forbidden_implicit_commit(id INTEGER)")
            started.set()
            await asyncio.Event().wait()
    work = asyncio.create_task(transaction())
    await asyncio.wait_for(started.wait(), 2)
    work.cancel()
    with pytest.raises(asyncio.CancelledError):
        await work
    assert (await f.other.db.fetchone("SELECT created_at FROM users WHERE id=?", [f.user["id"]]))["created_at"] == NOW
    assert await f.other.db.fetchone("SELECT TABLE_NAME FROM information_schema.tables WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='forbidden_implicit_commit'") is None


async def test_mysql_lock_key_is_cross_pool_transaction_scoped_and_cancel_releases(mysql):
    f = mysql
    held, secondEntered, release = asyncio.Event(), asyncio.Event(), asyncio.Event()
    async def first():
        async with f.store.db.transaction() as db:
            await db.lock_key("fixture:shared")
            held.set()
            await release.wait()
    async def second():
        async with f.other.db.transaction() as db:
            await db.lock_key("fixture:shared")
            secondEntered.set()
    with pytest.raises(TypeError):
        await f.store.db.lock_key("fixture:outside")
    a = asyncio.create_task(first())
    await asyncio.wait_for(held.wait(), 2)
    b = asyncio.create_task(second())
    await asyncio.sleep(.05)
    assert not secondEntered.is_set()
    a.cancel()
    with pytest.raises(asyncio.CancelledError):
        await a
    await asyncio.wait_for(b, 2)
    assert secondEntered.is_set()


async def test_mysql_json_checkpoint_and_cas_rowcount_contract(mysql):
    f = mysql
    id = str(uuid4())
    value = {"id": id, "title": "粤语 🎵", "nested": {"safe": False}, "number": 1}
    await f.store.db.execute("INSERT INTO music_tasks(id,user_id,device_id,revision,state_version,status,snapshot_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)", [id, f.user["id"], "mysql_device", 1, 1, "preview", json.dumps(value, ensure_ascii=False), NOW, NOW])
    rows = await asyncio.gather(*[s.db.execute("UPDATE music_tasks SET state_version=2 WHERE id=? AND state_version=1", [id]) for s in f.stores])
    assert sum(r.rowCount for r in rows) == 1
    assert json.loads((await f.other.db.fetchone("SELECT snapshot_json FROM music_tasks WHERE id=?", [id]))["snapshot_json"]) == value


async def test_mysql_development_mapping_and_admin_use_explicit_database_only(mysql, tmp_path):
    f = mysql
    request = SimpleNamespace(method="POST", client=SimpleNamespace(host="127.0.0.1"), scope={"server": ("127.0.0.1", 14001), "headers": [(b"host", b"127.0.0.1:14001"), (b"x-tango-development-token", b"a" * 64)]})
    sessions = await asyncio.gather(*[DevelopmentSession(store=s, quota=s.accountPolicy, enabled=True, token="a" * 64, now=lambda: NOW).create(request) for s in f.stores])
    assert sessions[0]["user"] == sessions[1]["user"] and sessions[0]["user"]["id"] != f.user["id"]
    assert (await f.other.session(sessions[0]["token"], NOW))["user"] == sessions[0]["user"]
    assert (await f.store.accountPolicy.snapshot(sessions[0]["user"]["id"], NOW))["daily"]["limit"] == 10000
    assert (await f.store.accountPolicy.snapshot(f.user["id"], NOW))["daily"]["limit"] == 30
    config = tmp_path / "explicit.env"
    config.write_text("QUIET_STORAGE_DRIVER=mysql\nQUIET_DATABASE_URL=" + f.url + "\n")
    config.chmod(0o600)
    output = tmp_path / "mysql.invite-codes.json"
    issued = await runAdmin(["invite", "issue", "--env-file", str(config), "--output", str(output)], now=lambda: NOW)
    assert json.loads(output.read_text())["code"] not in json.dumps(issued)
    assert (await runAdmin(["invite", "disable", "--env-file", str(config), "--id", issued["invite"]["id"]]))["invite"]["disabled"]
