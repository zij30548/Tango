"""读取冻结的旧 Node 合成数据，验证当前存储格式；不再运行已退役后台。"""

import hashlib
import hmac
import json
import os
from pathlib import Path
from urllib.parse import unquote, urlsplit, urlunsplit
import uuid

import pytest

from tango_backend.errors import ApiError
from tango_backend.store import Store, PostgresStore, MySQLStore, validateCredentials
from tango_backend.task_store import TaskStore

FIXTURE = json.loads((Path(__file__).parent / 'fixtures/storage_js_v1.json').read_text())


@pytest.fixture(params=['sqlite', 'postgres', 'mysql'])
async def legacy_store(request, tmp_path):
    """只创建运行器显式提供的临时库，不访问应用配置或用户数据库。"""
    admin = schema = name = store = None
    driver = request.param
    try:
        if driver == 'postgres':
            url = os.environ.get('TANGO_TEST_DATABASE_URL') or os.environ.get('TANGO_POSTGRES_TEST_URL')
            if not url:
                pytest.skip('需要显式临时 PostgreSQL 地址')
            import asyncpg
            admin = await asyncpg.connect(url)
            schema = 'interop_' + uuid.uuid4().hex
            await admin.execute('CREATE SCHEMA ' + schema)
            store = PostgresStore(connectionString=url, serverSettings={'search_path': schema})
        elif driver == 'mysql':
            url = os.environ.get('TANGO_MYSQL_TEST_URL')
            if not url:
                pytest.skip('需要独立运行器的临时 MySQL 地址')
            import asyncmy
            parsed = urlsplit(url)
            admin = await asyncmy.connect(host=parsed.hostname, port=parsed.port or 3306,
                                         user=unquote(parsed.username), password=unquote(parsed.password),
                                         autocommit=True, charset='utf8mb4')
            name = 'tango_interop_' + uuid.uuid4().hex
            async with admin.cursor() as cursor:
                await cursor.execute(f'CREATE DATABASE `{name}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin')
            store = MySQLStore(connectionString=urlunsplit((parsed.scheme, parsed.netloc, '/' + name, '', '')))
        else:
            store = Store(tmp_path / 'legacy-fixture.sqlite')
        await store.initialize()
        await TaskStore(store).initialize()
        # 合成记录直接来自旧 SQLite Store；共享库把快照内版本投影到独立 CAS 列。
        async with store.db.transaction() as db:
            for table, rows in FIXTURE['tables'].items():
                for original in rows:
                    row = dict(original)
                    if table == 'music_tasks' and driver in ('mysql', 'postgres'):
                        row['state_version'] = json.loads(row['snapshot_json'])['stateVersion']
                    if table == 'music_account_quota' and driver == 'postgres':
                        row['enabled'] = bool(row['enabled'])
                    columns = ','.join(row)
                    placeholders = ','.join('?' for _ in row)
                    await db.execute(f'INSERT INTO {table}({columns}) VALUES({placeholders})', list(row.values()))
        yield store
    finally:
        if store:
            await store.close()
        if schema and admin:
            await admin.execute('DROP SCHEMA ' + schema + ' CASCADE')
            await admin.close()
        elif name and admin:
            async with admin.cursor() as cursor:
                await cursor.execute(f'DROP DATABASE `{name}`')
            admin.close()


async def test_frozen_node_password_session_snapshot_and_request_ledger_compatibility(legacy_store):
    store, now, password = legacy_store, FIXTURE['now'], FIXTURE['password']
    original = FIXTURE['original']
    user = await store.authenticate(validateCredentials({'username': 'node_fixture', 'password': password}))
    assert user == original['user']
    assert (await store.session(original['sessionToken'], now))['user'] == user
    tasks = TaskStore(store)
    await tasks.initialize()
    request = original['request']
    restored = await tasks.replay(user['id'], request['deviceId'], request['requestId'], request)
    assert restored['id'] == original['taskId'] and restored['preview']['tracks'][0]['title'] == '晚风'
    with pytest.raises(ApiError) as error:
        await tasks.replay(user['id'], request['deviceId'], request['requestId'], {**request, 'instruction': '不同要求'})
    assert error.value.code == 'IDEMPOTENCY_CONFLICT'
    restored['stateVersion'] = 2
    restored['updatedAt'] = now + 1
    restored['preview']['title'] = 'Python 更新歌单'
    await tasks.save(restored, expectedStateVersion=1, now=now + 1)
    assert await tasks.load(restored['id'], user['id']) == FIXTURE['expectedUpdatedSnapshot']
    raw = await store.db.fetchone('SELECT snapshot_json FROM music_tasks WHERE id=?', [restored['id']])
    assert json.loads(raw['snapshot_json']) == FIXTURE['expectedUpdatedSnapshot']

    # 独立按冻结的旧 scrypt/令牌协议验算当前写入；无需旧 JS 或 Node 运行时。
    created = await store.register(validateCredentials({'username': 'python_fixture', 'password': password}), now)
    row = await store.db.fetchone('SELECT password_salt,password_hash FROM users WHERE id=?', [created['id']])
    spec = FIXTURE['passwordFormat']
    assert len(row['password_salt']) == spec['saltHexLength']
    assert row['password_hash'].startswith(spec['hashPrefix'])
    expected = hashlib.scrypt(password.encode(), salt=bytes.fromhex(row['password_salt']), n=spec['N'],
                              r=spec['r'], p=spec['p'], dklen=spec['derivedBytes'], maxmem=64 * 1024 * 1024).hex()
    assert len(expected) == spec['derivedHexLength']
    assert hmac.compare_digest(row['password_hash'][len(spec['hashPrefix']):], expected)
    session = await store.createSession(created, now)
    assert len(session['token']) == FIXTURE['sessionFormat']['rawTokenLength']
    raw = await store.db.fetchone('SELECT token_hash,expires_at FROM sessions WHERE user_id=?', [created['id']])
    assert raw['token_hash'] == hashlib.sha256(session['token'].encode()).hexdigest()
    assert int(raw['expires_at']) == now + FIXTURE['sessionFormat']['defaultDurationMs']
    assert (await store.session(session['token'], now))['user'] == created
