"""存储迁移回归：临时库、合成凭据和模型调用账本，不调用外部模型。"""
import asyncio
import json
import os
import secrets
import stat
import uuid
from contextlib import suppress
from types import SimpleNamespace

import pytest
import pytest_asyncio

from tango_backend.store import Store, PostgresStore, validateCredentials
from tango_backend.storage_auth import passwordRecord, passwordMatches, passwordNeedsUpgrade
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.invite_store import InviteStore, inviteCodeHash
from tango_backend.registration_service import RegistrationService
from tango_backend.task_store import TaskStore
from tango_backend.development_config import ensureDevelopmentToken, readDevelopmentToken, developmentConfiguration, developmentTokenFingerprint
from tango_backend.development_session import DevelopmentSession
from tango_backend.redis_limiter import MemoryRateLimiter, RedisRateLimiter
from tango_backend.errors import ApiError
from tango_backend.compat import clone, digest

NOW = 1792454400000
PASSWORD = 'fixture-password-12345'
SALT = '00112233445566778899aabbccddeeff'
# 此固定向量由 Node crypto.scryptSync 生成，用于跨语言格式校验。
LEGACY = '45962d35b12efda03846132b638cf8fa99377151b402cc4b4f6059ad43d0974afb3b8d856505550dc270517f50af47acbc6aa87d0741e9907ba2a78065d85218'
V2 = 'scrypt-v2$a80822527d0a013616dd33e0cd83b121ca249faa0c10dcff4e3aac02ea28a0b2895e1bbd05f54e07709bafb795611d5becda71fe911c0af846d513298ef34780'


@pytest_asyncio.fixture(params=['sqlite', 'postgres'])
async def storage(request, tmp_path):
    stores, admin, schema = [], None, None
    if request.param == 'postgres':
        url = os.environ.get('TANGO_TEST_DATABASE_URL') or os.environ.get('TANGO_POSTGRES_TEST_URL')
        if not url:
            pytest.skip('需要显式的临时 PostgreSQL 测试连接')
        import asyncpg
        admin = await asyncpg.connect(url)
        schema = 'python_storage_' + uuid.uuid4().hex
        await admin.execute('CREATE SCHEMA ' + schema)
        async def newStore():
            store = PostgresStore(connectionString=url, serverSettings={'search_path': schema})
            stores.append(store)
            await store.initialize()
            return store
    else:
        async def newStore():
            store = Store(str(tmp_path / 'isolated.sqlite'))
            stores.append(store)
            await store.initialize()
            return store
    store = await newStore()
    user = {'id': str(uuid.uuid4()), 'username': 'isolated_user'}
    await store.db.execute('INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)',
        [user['id'], user['username'], user['username'], SALT, LEGACY, NOW])
    policy = store.accountPolicy = AccountPolicyStore(store)
    tasks = TaskStore(store)
    await tasks.initialize()
    try:
        yield SimpleNamespace(store=store, user=user, quota=policy, tasks=tasks, newStore=newStore, postgres=request.param == 'postgres')
    finally:
        for value in stores:
            await value.close()
        if admin:
            await admin.execute('DROP SCHEMA ' + schema + ' CASCADE')
            await admin.close()


def task(f, **updates):
    return {'id': str(uuid.uuid4()), 'userId': f.user['id'], 'deviceId': 'storage_fixture_device', 'revision': 1,
        'stateVersion': 1, 'status': 'planning', 'phase': 'plan', 'createdAt': NOW, 'updatedAt': NOW,
        'runId': str(uuid.uuid4()), 'pendingSteps': [], 'budget': {'totalCalls': 0, 'runCalls': 0}, 'trace': [], **updates}


async def test_node_password_vectors_and_new_format():
    for encoded in [LEGACY, V2]:
        row = {'password_salt': SALT, 'password_hash': encoded}
        assert await passwordMatches('fixture-password-长期', row)
        assert not await passwordMatches('wrong-password', row)
    assert passwordNeedsUpgrade({'password_salt': SALT, 'password_hash': LEGACY})
    assert not passwordNeedsUpgrade({'password_salt': SALT, 'password_hash': V2})
    for row in [None, {'password_salt': 'bad', 'password_hash': V2}, {'password_salt': SALT, 'password_hash': 'scrypt-v999$expensive'}]:
        assert not await passwordMatches(PASSWORD, row)
    fresh = await passwordRecord(PASSWORD)
    assert fresh['hash'].startswith('scrypt-v2$')
    assert await passwordMatches(PASSWORD, {'password_salt': fresh['salt'], 'password_hash': fresh['hash']})


@pytest.mark.parametrize('username', ['〇〇', '々々'])
def test_username_han_script_matches_existing_node_boundary(username):
    # Node 的 Script=Han 接受这些非基本 CJK 区段字符，旧账号仍须能够登录。
    credentials = validateCredentials({'username': username, 'password': PASSWORD})
    assert credentials['username'] == username
    assert credentials['usernameKey'] == username


async def test_auth_upgrade_session_isolation_and_expiry(storage):
    f = storage
    user = await f.store.authenticate(validateCredentials({'username': f.user['username'], 'password': 'fixture-password-长期'}))
    assert user == f.user
    record = await f.store.db.fetchone('SELECT password_hash FROM users WHERE id=?', [user['id']])
    assert record['password_hash'].startswith('scrypt-v2$')
    session = await f.store.createSession(user, NOW)
    assert (await f.store.session(session['token'], NOW))['user'] == user
    assert await f.store.session(session['token'], NOW + 8 * 86400000) is None
    assert await f.store.session('not-a-token', NOW) is None
    await f.store.logout(session['token'])
    assert await f.store.session(session['token'], NOW) is None


async def test_database_placeholders_rollback_nested_and_ddl(storage):
    db = storage.store.db
    if not storage.postgres:
        tables = {row["name"]: row for row in await db.fetchall("PRAGMA table_list")}
        assert all(tables[name]["strict"] == 1 for name in ["users", "sessions", "ai_usage"])
        assert (await db.fetchone("SELECT type FROM pragma_table_info('users') WHERE name='created_at'"))["type"] == "INTEGER"
    assert (await db.fetchone('SELECT $2 AS second,$1 AS first,$2 AS again', ['a', 'b'])) == {'second': 'b', 'first': 'a', 'again': 'b'}
    async with db.transaction():
        await db.executescript('CREATE TABLE migration_fixture(id TEXT PRIMARY KEY); INSERT INTO migration_fixture VALUES(\'kept\');')
        with pytest.raises(RuntimeError):
            async with db.transaction():
                await db.execute('INSERT INTO migration_fixture VALUES(?)', ['rolled_back'])
                raise RuntimeError('fixture rollback')
    assert [r['id'] for r in await db.fetchall('SELECT id FROM migration_fixture')] == ['kept']
    with pytest.raises(RuntimeError):
        async with db.transaction():
            await db.execute('INSERT INTO migration_fixture VALUES(?)', ['whole_transaction'])
            raise RuntimeError('fixture rollback')
    assert len(await db.fetchall('SELECT id FROM migration_fixture')) == 1


async def test_invite_concurrent_consumption_and_failed_registration_rollback(storage):
    f = storage
    other = await f.newStore()
    issued = await InviteStore(f.store).issue({'maxUses': 2, 'dailyLimit': 7, 'totalLimit': 21}, NOW)
    code = issued['code']
    services = [RegistrationService(store=s, now=lambda: NOW) for s in [f.store, other]]
    results = await asyncio.gather(*[services[n % 2].register({'username': 'invited_' + str(n), 'password': PASSWORD, 'inviteCode': code}) for n in range(4)], return_exceptions=True)
    assert sum(isinstance(value, dict) for value in results) == 2
    assert all(isinstance(value, dict) or isinstance(value, ApiError) and value.code == 'INVITE_INVALID' for value in results)
    listed = (await InviteStore(f.store).list())[0]
    assert listed['usedCount'] == 2 and 'code' not in listed and 'codeHash' not in listed
    raw = await f.store.db.fetchone('SELECT * FROM music_registration_invites WHERE code_hash=?', [inviteCodeHash(code)])
    assert code not in json.dumps(raw)
    for value in results:
        if isinstance(value, dict):
            usage = await f.quota.snapshot(value['id'], NOW)
            assert usage['daily']['limit'] == 7 and usage['total']['limit'] == 21
    rollback = await InviteStore(f.store).issue({'maxUses': 1}, NOW)
    with pytest.raises(ApiError, match='用户名'):
        await services[0].register({'username': f.user['username'], 'password': PASSWORD, 'inviteCode': rollback['code']})
    record = await f.store.db.fetchone('SELECT used_count FROM music_registration_invites WHERE id=?', [rollback['invite']['id']])
    assert record['used_count'] == 0
    await InviteStore(f.store).disable(rollback['invite']['id'])
    with pytest.raises(ApiError) as rejected:
        await services[0].register({'username': 'disabled_code', 'password': PASSWORD, 'inviteCode': rollback['code']})
    assert rejected.value.code == 'INVITE_INVALID'


async def test_quota_reservation_atomic_across_instances_and_rollback(storage):
    f = storage
    await f.quota.adminset(f.user['id'], {'dailyLimit': 2, 'totalLimit': 3, 'enabled': True}, now=NOW)
    other = await f.newStore()
    second = TaskStore(other)
    await second.initialize()
    async def reserve(repository):
        async with repository.db.transaction() as db:
            await repository.lockUser(db, f.user['id'])
            await repository.reserveUsage(db, f.user['id'], NOW, 100, 100)
        return True
    results = await asyncio.gather(*[reserve([f.tasks, second][n % 2]) for n in range(6)], return_exceptions=True)
    assert results.count(True) == 2
    assert (await f.quota.snapshot(f.user['id'], NOW))['total']['used'] == 2
    assert await f.tasks.globalUsage('2026-10-20') == 2
    with pytest.raises(ApiError) as rejected:
        await reserve(f.tasks)
    assert rejected.value.code == 'AI_DAILY_LIMIT'
    assert await f.tasks.globalUsage('2026-10-20') == 2
    with pytest.raises(RuntimeError):
        async with f.store.db.transaction() as db:
            await f.tasks.lockUser(db, f.user['id'])
            await f.tasks.reserveUsage(db, f.user['id'], NOW + 86400000, 100, 100)
            raise RuntimeError('fixture rollback after usage')
    assert (await f.quota.snapshot(f.user['id'], NOW + 86400000))['total']['used'] == 2


async def test_task_cas_idempotency_and_unknown_recovery(storage):
    f = storage
    initial = task(f)
    body, requestId = {'deviceId': initial['deviceId'], 'instruction': '合成任务'}, str(uuid.uuid4())
    await f.tasks.commit(initial, requestId, body, expectedStateVersion=0)
    assert (await f.tasks.replay(f.user['id'], initial['deviceId'], requestId, body))['id'] == initial['id']
    with pytest.raises(ApiError) as conflict:
        await f.tasks.replay(f.user['id'], initial['deviceId'], requestId, {**body, 'instruction': '不同内容'})
    assert conflict.value.code == 'IDEMPOTENCY_CONFLICT'
    a, b = clone(initial), clone(initial)
    a.update(stateVersion=2, updatedAt=NOW + 1)
    b.update(stateVersion=2, updatedAt=NOW + 2)
    results = await asyncio.gather(f.tasks.save(a, expectedStateVersion=1), f.tasks.save(b, expectedStateVersion=1), return_exceptions=True)
    assert sum(isinstance(value, ApiError) and value.code == 'TASK_WRITE_CONFLICT' for value in results) == 1
    current = await f.tasks.load(initial['id'], f.user['id'])
    claim = await f.tasks.claimTask(current['id'], f.user['id'], workerId='worker_fixture_a', now=NOW + 3, leaseMs=1000)
    callId = await f.tasks.reserveCall(current, 'understand', NOW + 3, f.store, 30, 100, claim=claim)
    recovered = await f.tasks.recoverInterrupted(current, now=NOW + 2000, graceMs=1)
    assert recovered['status'] == 'paused' and recovered['budget']['totalCalls'] == 1
    ledger = await f.store.db.fetchone('SELECT status FROM music_task_calls WHERE id=?', [callId])
    assert ledger['status'] == 'uncertain'
    assert await f.tasks.load(current['id'], str(uuid.uuid4())) is None
    assert (await f.quota.snapshot(f.user['id'], NOW))['total']['used'] == 1


async def test_development_identity_is_stable_private_and_does_not_reuse_users(storage):
    f = storage
    config = {'store': f.store, 'quota': f.quota, 'enabled': True, 'token': 'a' * 64, 'now': lambda: NOW}
    request = SimpleNamespace(method='POST', client=SimpleNamespace(host='127.0.0.1'),
        scope={'server': ('127.0.0.1', 16383), 'headers': [(b'host', b'127.0.0.1:16383'), (b'x-tango-development-token', b'a' * 64)]})
    sessions = await asyncio.gather(DevelopmentSession(config).create(request), DevelopmentSession(config).create(request))
    assert sessions[0]['user'] == sessions[1]['user'] and sessions[0]['user']['id'] != f.user['id']
    assert sessions[0]['development'] is True
    usage = await f.quota.snapshot(sessions[0]['user']['id'], NOW)
    assert usage['daily']['limit'] == 10000 and usage['total']['limit'] == 100000
    for key, value in [(b'origin', b'null'), (b'x-forwarded-for', b'127.0.0.1'), (b'sec-fetch-site', b'cross-site')]:
        bad = SimpleNamespace(method=request.method, client=request.client, scope={**request.scope, 'headers': request.scope['headers'] + [(key, value)]})
        with pytest.raises(ApiError):
            await DevelopmentSession(config).create(bad)


async def test_postgres_fencing_slots_heartbeat_and_takeover(storage):
    f = storage
    if not f.postgres:
        pytest.skip('SQLite 仅支持单进程运行时，不提供 PG 分布式租约')
    initial = task(f)
    await f.tasks.save(initial, expectedStateVersion=0)
    first = await f.tasks.claimTask(initial['id'], f.user['id'], workerId='first', now=NOW, leaseMs=1000)
    with pytest.raises(ApiError):
        await f.tasks.claimTask(initial['id'], f.user['id'], workerId='second', now=NOW, leaseMs=1000)
    assert await f.tasks.heartbeatTask(initial['id'], first, now=NOW + 500, leaseMs=1000)
    call = await f.tasks.reserveCall(initial, 'model', NOW + 600, f.store, 30, 100, claim=first)
    with pytest.raises(ApiError) as busy:
        await f.tasks.reserveCall(initial, 'model', NOW + 600, f.store, 30, 100, claim=first)
    assert busy.value.code == 'AI_BUSY'
    assert (await f.quota.snapshot(f.user['id'], NOW))['total']['used'] == 1
    await f.tasks.finishCall(call, 'failed', NOW + 700, None, 'MODEL_FAILED')
    second = await f.tasks.claimTask(initial['id'], f.user['id'], workerId='second', now=NOW + 1600, leaseMs=1000)
    assert second['epoch'] > first['epoch']
    assert not await f.tasks.heartbeatTask(initial['id'], first, now=NOW + 1600)
    assert not await f.tasks.releaseTask(initial['id'], first)
    assert await f.tasks.isTaskClaimCurrent(initial['id'], second, NOW + 1600)
    assert await f.tasks.releaseTask(initial['id'], second)


async def test_development_token_file_security_and_configuration(tmp_path):
    path = str(tmp_path / 'private' / 'token')
    await ensureDevelopmentToken(path)
    token = await readDevelopmentToken(path)
    await ensureDevelopmentToken(path)
    assert await readDevelopmentToken(path) == token
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    assert (await developmentConfiguration({'QUIET_DEVELOPMENT_MODE': '0', 'QUIET_DEVELOPMENT_TOKEN_FILE': '/does/not/exist'})) == {'enabled': False}
    env = {'QUIET_DEVELOPMENT_MODE': '1', 'QUIET_DEVELOPMENT_TOKEN_FILE': path}
    config = await developmentConfiguration(env, host='127.0.0.1')
    assert config['tokenFingerprint'] == developmentTokenFingerprint(token)
    for patch in [{'TANGO_ENV': 'production'}, {'NODE_ENV': 'production'}]:
        with pytest.raises(ValueError):
            await developmentConfiguration({**env, **patch}, host='127.0.0.1')
    link = tmp_path / 'link'
    link.symlink_to(path)
    with pytest.raises((OSError, ValueError)):
        await readDevelopmentToken(str(link))
    fifo = tmp_path / 'fifo'
    os.mkfifo(fifo, 0o600)
    with pytest.raises(ValueError):
        await asyncio.wait_for(readDevelopmentToken(str(fifo)), .5)


async def test_memory_rate_limiter_window_boundaries():
    window = MemoryRateLimiter().window('fixture', {'maximum': 2, 'periodMs': 100})
    assert await window.take('user', 0)
    assert await window.take('user', 0)
    assert not await window.take('user', 99)
    assert await window.take('user', 100)


async def test_redis_shared_atomic_window_and_failure_closed():
    url = os.environ.get('TANGO_TEST_REDIS_URL') or os.environ.get('TANGO_REDIS_TEST_URL')
    if not url:
        pytest.skip('需要显式的临时 Redis 测试连接')
    from redis.asyncio import Redis
    clients = [Redis.from_url(url, decode_responses=True) for _ in range(2)]
    namespace = 'tango:python_' + uuid.uuid4().hex
    try:
        windows = [RedisRateLimiter(client=client, namespace=namespace).window('fixture', {'maximum': 3, 'periodMs': 10000}) for client in clients]
        results = await asyncio.gather(*[windows[n % 2].take('same_user') for n in range(8)])
        assert results.count(True) == 3
        assert not await windows[1].allows('same_user')
        await windows[0].clear('same_user')
        assert await windows[1].take('same_user')
        class Broken:
            async def eval(self, *args):
                raise ConnectionError('synthetic failure')
        with pytest.raises(ApiError) as failed:
            await RedisRateLimiter(client=Broken(), namespace=namespace).window('fixture', {'maximum': 1, 'periodMs': 10}).take('user')
        assert failed.value.code == 'RATE_LIMIT_UNAVAILABLE'
    finally:
        keys = [key async for key in clients[0].scan_iter(namespace + ':*')]
        if keys:
            await clients[0].delete(*keys)
        for client in clients:
            await client.aclose()
