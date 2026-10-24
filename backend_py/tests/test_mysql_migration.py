"""只在自建 PG 与 runner 自建 MySQL 上验迁移；真实 profile 和用户库不参与。"""
import asyncio
from decimal import Decimal
import json
import os
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
from urllib.parse import urlsplit, urlunsplit, unquote
from uuid import uuid4
import pytest
from tango_backend.compat import now_ms, js_json, digest
from tango_backend.store import PostgresStore, validateCredentials
from tango_backend.mysql_store import MySQLStore
from tango_backend.task_store import TaskStore
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.conversation_store import ConversationStore
from tango_backend.conversation_route_store import ConversationRouteStore
from tango_backend.development_session import SCHEMA as DEVELOPMENT_SCHEMA
from tango_backend.invite_store import InviteStore
from tango_backend.mysql_infra import private_write, clean_environment
from tango_backend import migrate_mysql as migration
from tango_backend.errors import ApiError

MYSQL_URL = os.environ.get('TANGO_MYSQL_TEST_URL')
DEVICE = 'mysql_migration_fixture_device'


def test_canonical_digest_keeps_bigints_decimals_and_json_types():
    columns = ['snapshot_json', 'enabled', 'request_count']
    first = {'snapshot_json': '{"a":9007199254740993,"b":1.00,"c":true}', 'enabled': True, 'request_count': 9007199254740993}
    same = {'snapshot_json': '{"c":true,"b":1,"a":9007199254740993}', 'enabled': 1, 'request_count': 9007199254740993}
    changed = {**same, 'snapshot_json': '{"c":true,"b":1,"a":9007199254740992}'}
    assert migration.row_digest(first, columns) == migration.row_digest(same, columns)
    assert migration.row_digest(first, columns) != migration.row_digest(changed, columns)
    assert migration.canonical(Decimal('0.12345678901234567890123456789')) != migration.canonical(Decimal('0.12345678901234568'))
    assert migration.canonical(True) != migration.canonical(1)

@pytest.fixture(scope='module')
def owned_pg():
    if not MYSQL_URL: pytest.skip('只在 owned MySQL runner 中启动迁移用临时 PG')
    from run_storage_suite import binary, command, free_port
    directory = Path(tempfile.mkdtemp(prefix='tgmxpg-', dir='/tmp')); directory.chmod(0o700)
    pg_ctl, initdb, createdb = [binary(n) for n in ['pg_ctl', 'initdb', 'createdb']]
    password = uuid4().hex + uuid4().hex
    port = free_port(); env = clean_environment(); data = directory / 'data'
    private_write(directory / 'password', password + '\n')
    private_write(directory / 'pgpass', f'127.0.0.1:{port}:*:tango:{password}\n')
    (directory / 'socket').mkdir(mode=0o700)
    try:
        command([initdb, '-D', str(data), '--username=tango', '--auth-local=scram-sha-256', '--auth-host=scram-sha-256', '--encoding=UTF8', '--locale=C', '--pwfile=' + str(directory / 'password')], env)
        with (data / 'postgresql.conf').open('a') as file:
            file.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\nunix_socket_directories='{directory / 'socket'}'\nshared_buffers='32MB'\nmax_connections=60\n")
        command([pg_ctl, '-D', str(data), '-l', str(directory / 'postgres.log'), '-w', 'start'], env)
        command([createdb, '-h', '127.0.0.1', '-p', str(port), '-U', 'tango', 'tango'], {**env, 'PGPASSFILE': str(directory / 'pgpass')})
        yield f'postgresql://tango:{password}@127.0.0.1:{port}/tango'
    finally:
        if (data / 'postmaster.pid').is_file(): command([pg_ctl, '-D', str(data), '-m', 'fast', '-w', 'stop'], env, timeout=15)
        shutil.rmtree(directory)

@pytest.fixture
async def pair(tmp_path, owned_pg):
    import asyncpg
    import asyncmy
    pg_admin = await asyncpg.connect(owned_pg)
    dbname = 'tango_migration_' + uuid4().hex
    await pg_admin.execute('CREATE DATABASE ' + dbname)
    src = urlsplit(owned_pg); source_url = urlunsplit((src.scheme, src.netloc, '/' + dbname, '', ''))
    u = urlsplit(MYSQL_URL)
    mysql_admin = await asyncmy.connect(host=u.hostname, port=u.port or 3306, user=unquote(u.username), password=unquote(u.password), autocommit=True)
    async with mysql_admin.cursor() as cursor:
        await cursor.execute(f'CREATE DATABASE `{dbname}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin')
    target_url = urlunsplit((u.scheme, u.netloc, '/' + dbname, '', ''))
    source, target = PostgresStore(source_url), MySQLStore(target_url)
    source.accountPolicy = AccountPolicyStore(source)
    await source.initialize(); tasks = TaskStore(source); await tasks.initialize()
    conversations = ConversationStore(source); await conversations.initialize()
    routes = ConversationRouteStore(store=source, taskStore=tasks); await routes.initialize()
    await source.db.executescript(DEVELOPMENT_SCHEMA)
    await source.db.execute('CREATE TABLE tango_storage_imports(migration_id text PRIMARY KEY,source_sha256 text NOT NULL,target_sha256 text NOT NULL,counts_json jsonb NOT NULL,applied_at bigint NOT NULL)')
    now = now_ms()
    credentials = validateCredentials({'username': 'migration-fixture', 'password': 'isolated-migration-password'})
    user = await source.register(credentials, now)
    session = await source.createSession(user, now)
    invite = await InviteStore(source).issue({'label': '离线邀请 😀', 'dailyLimit': 12, 'totalLimit': 120}, now)
    second = await source.register(validateCredentials({'username': 'migration-invited', 'password': 'isolated-invited-password'}), now, {'registrationMode': 'invite_only', 'inviteCode': invite['code']})
    await source.db.execute('INSERT INTO ai_usage(user_id,utc_day,request_count) VALUES(?,?,?)', (user['id'], '2026-10-21', 9))
    await source.db.execute('INSERT INTO music_task_global_usage(utc_day,request_count) VALUES(?,?)', ('2026-10-21', 9))
    conv = await conversations.create(user['id'], {'deviceId': DEVICE, 'requestId': str(uuid4())})
    message = {'deviceId': DEVICE, 'messageId': str(uuid4()), 'content': '离线阅读音乐 😀'}
    await conversations.message(conv['id'], user['id'], message, {'dispatchHint': 'router'})
    task = {'id': str(uuid4()), 'userId': user['id'], 'deviceId': DEVICE, 'revision': 1, 'stateVersion': 1, 'status': 'waiting_tools',
            'createdAt': now, 'updatedAt': now, 'messages': [{'messageId': str(uuid4()), 'role': 'user', 'content': message['content'], 'revision': 1}],
            'preference': '', 'excludedIds': [], 'pendingSteps': [], 'migrationProbe': {'big': 9007199254740993, 'text': '粤语😀', 'flag': True}}
    # 离线等待工具允许保留；没有有效租约或任何 reserved 上游尝试。
    await tasks.commit(task, message['messageId'], {'deviceId': DEVICE, 'requestId': message['messageId'], 'instruction': message['content'], 'preference': '', 'excludedIds': []}, {'expectedStateVersion': 0, 'now': now})
    call_ids = [str(uuid4()), str(uuid4())]
    for id, status in zip(call_ids, ['failed', 'uncertain']):
        await source.db.execute('INSERT INTO music_task_calls(id,task_id,revision,run_id,node,status,started_at,finished_at,error_code) VALUES(?,?,?,?,?,?,?,?,?)', (id, task['id'], 1, 'fixture_run', 'plan', status, now - 10, now - 1, 'MODEL_TIMEOUT'))
    await source.db.execute('INSERT INTO music_task_account_leases(user_id,task_id,owner_id,owner_token,owner_epoch,lease_until) VALUES(?,?,?,?,?,?)', (user['id'], task['id'], 'old-owner', 'old-token', 3, now - 1))
    await source.db.execute('INSERT INTO music_task_model_slots(call_id,user_id,task_id,owner_id,owner_token,owner_epoch,lease_until) VALUES(?,?,?,?,?,?,?)', (call_ids[0], user['id'], task['id'], 'old-owner', 'old-token', 3, now - 1))
    await source.db.execute('INSERT INTO development_identities(identity_key,user_id,created_at) VALUES(?,?,?)', ('tango_local_development_v1', second['id'], now))
    await source.db.execute('INSERT INTO tango_storage_imports(migration_id,source_sha256,target_sha256,counts_json,applied_at) VALUES(?,?,?,?,?)',
                            ('isolated_prior_migration', 'a'*64, 'b'*64, '{"users":2,"historicalId":9007199254740993}', now))
    context = await conversations.routeContext(conv['id'], user['id'], DEVICE, message['messageId'])
    inp = {k: context[k] for k in ['conversationId','messageId','deviceId','instruction','history','task']}; inp['preference'] = ''
    request = {k: inp[k] for k in ['conversationId','messageId','deviceId']}; request.update(userId=user['id'],input=inp,inputHash=digest(inp),capabilityVersion=1)
    claim = (await routes.begin(request, False))['claim']
    await routes.finish(claim, status='completed', result={'fixture': True})
    # 第二账号准备真实清除墓碑和回执；第一账号任务及账本保持原样参与逐表核验。
    clear_device = 'migration_cleared_fixture_device'
    cleared_conv = await conversations.create(second['id'], {'deviceId': clear_device, 'requestId': str(uuid4())})
    await conversations.message(cleared_conv['id'], second['id'], {'deviceId': clear_device, 'messageId': str(uuid4()), 'content': '迁移后不应恢复的旧聊天。'})
    clear_request = {'requestId': str(uuid4())}
    clear_receipt = await conversations.clearAll(second['id'], clear_request)
    source_env, target_env = tmp_path / 'source.env', tmp_path / 'target.env'
    private_write(source_env, 'QUIET_STORAGE_DRIVER=postgres\nQUIET_DATABASE_URL=' + source_url + '\n')
    private_write(target_env, 'QUIET_STORAGE_DRIVER=mysql\nQUIET_DATABASE_URL=' + target_url + '\n')
    value = SimpleNamespace(source=source,target=target,tasks=tasks,user=user,credentials=credentials,session=session,task=task,
                            source_env=source_env,target_env=target_env,backup=tmp_path / 'backups',calls=call_ids,now=now,
                            conv=conv,cleared_conv=cleared_conv,clear_user=second,clear_request=clear_request,clear_receipt=clear_receipt)
    try: yield value
    finally:
        await source.close(); await target.close()
        await pg_admin.execute('DROP DATABASE ' + dbname); await pg_admin.close()
        async with mysql_admin.cursor() as cursor: await cursor.execute(f'DROP DATABASE `{dbname}`')
        mysql_admin.close(); await mysql_admin.ensure_closed()

async def test_real_pg_to_mysql_preserves_all_tables_auth_json_and_ledger(pair):
    report = pair.backup.parent / 'report.json'
    result = await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup,report=report)
    assert result['ok'] and result['verified'] and not result['profileSwitched'] and result['sourceRetained']
    assert set(result['tables']) == set(migration.TABLES)
    assert all(summary['count'] > 0 for summary in result['tables'].values())
    assert Path(result['backup']['path']).is_file() and Path(result['backup']['path']).stat().st_mode & 0o077 == 0
    assert report.stat().st_mode & 0o077 == 0
    assert result['reportWritten'] and json.loads(report.read_text())['verified']
    await pair.target.initialize()
    assert await pair.target.authenticate(pair.credentials) == pair.user
    assert (await pair.target.session(pair.session['token'], now_ms()))['user'] == pair.user
    target_tasks = TaskStore(pair.target)
    loaded = await target_tasks.load(pair.task['id'],pair.user['id'])
    assert loaded['migrationProbe']['big'] == 9007199254740993 and loaded['migrationProbe']['text'] == '粤语😀'
    assert loaded['status'] == 'waiting_tools' and loaded['pendingSteps'] == []
    assert (await pair.target.db.fetchone('SELECT SUM(request_count) AS n FROM ai_usage'))['n'] == 9
    assert (await pair.source.db.fetchone('SELECT SUM(request_count) AS n FROM ai_usage'))['n'] == 9
    assert [row['status'] for row in await pair.target.db.fetchall('SELECT status FROM music_task_calls ORDER BY status')] == ['failed','uncertain']
    audit = await pair.target.db.fetchone('SELECT * FROM tango_storage_imports')
    assert audit['migration_id'] == 'isolated_prior_migration'
    assert json.loads(audit['counts_json'])['historicalId'] == 9007199254740993
    target_conversations = ConversationStore(pair.target)
    await target_conversations.initialize()
    assert (await pair.target.db.fetchone('SELECT cleared_at FROM music_conversations WHERE id=?', (pair.conv['id'],)))['cleared_at'] is None
    with pytest.raises(ApiError) as cleared:
        await target_conversations.get(pair.cleared_conv['id'], pair.clear_user['id'])
    assert cleared.value.code == 'CONVERSATION_NOT_FOUND'
    assert await target_conversations.clearAll(pair.clear_user['id'], pair.clear_request) == pair.clear_receipt
    new_conv = await target_conversations.create(pair.clear_user['id'], {'deviceId': pair.cleared_conv['deviceId'], 'requestId': str(uuid4())})
    assert await target_conversations.clearAll(pair.clear_user['id'], pair.clear_request) == pair.clear_receipt
    assert (await target_conversations.latest(pair.clear_user['id'], new_conv['deviceId']))['id'] == new_conv['id']


async def test_legacy_pg_without_clear_schema_migrates_to_nullable_tombstone_and_empty_receipts(pair):
    # 还原上线前的旧源表形状，只操作本用例自建库；迁移不得回写源模式。
    await pair.source.db.execute('DELETE FROM music_conversations WHERE id=?', (pair.cleared_conv['id'],))
    await pair.source.db.execute('DROP TABLE music_conversation_clears')
    await pair.source.db.execute('ALTER TABLE music_conversations DROP COLUMN cleared_at')
    result = await migration.migrate(pair.source_env, pair.target_env, execute=True, backup_directory=pair.backup)
    assert result['ok'] and result['verified']
    assert result['tables']['music_conversation_clears']['count'] == 0
    assert (await pair.target.db.fetchone('SELECT cleared_at FROM music_conversations WHERE id=?', (pair.conv['id'],)))['cleared_at'] is None
    assert (await pair.target.db.fetchone('SELECT count(*) AS n FROM music_conversation_clears'))['n'] == 0
    assert await pair.source.db.fetchone("SELECT 1 FROM information_schema.columns WHERE table_schema=current_schema() AND table_name='music_conversations' AND column_name='cleared_at'") is None
    loaded = await TaskStore(pair.target).load(pair.task['id'], pair.user['id'])
    assert loaded['migrationProbe'] == pair.task['migrationProbe']

async def test_dry_run_never_writes_business_rows_or_backup(pair):
    result = await migration.migrate(pair.source_env,pair.target_env)
    assert result['verified'] and not result['executed'] and 'backup' not in result
    for table in migration.TABLES:
        assert (await pair.target.db.fetchone(f'SELECT count(*) AS n FROM `{table}`'))['n'] == 0
    assert not pair.backup.exists()

async def test_older_source_without_prior_migration_audit_is_supported(pair):
    await pair.source.db.execute('DROP TABLE tango_storage_imports')
    result = await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup)
    assert result['verified'] and result['tables']['tango_storage_imports']['count'] == 0
    assert (await pair.target.db.fetchone('SELECT count(*) AS n FROM tango_storage_imports'))['n'] == 0

@pytest.mark.parametrize('unsafe', ['working','reserved_call','running_route','reserved_route_attempt','task_lease','account_lease','model_slot'])
async def test_migration_rejects_source_active_or_unknown_without_target_data(pair, unsafe):
    if unsafe == 'working': await pair.source.db.execute("UPDATE music_tasks SET status='planning'")
    if unsafe == 'reserved_call': await pair.source.db.execute("UPDATE music_task_calls SET status='reserved' WHERE id=?", (pair.calls[0],))
    if unsafe == 'running_route': await pair.source.db.execute("UPDATE music_conversation_routes SET status='running'")
    if unsafe == 'reserved_route_attempt': await pair.source.db.execute("UPDATE music_conversation_route_attempts SET status='reserved'")
    if unsafe == 'task_lease': await pair.source.db.execute("UPDATE music_tasks SET owner_id='owner',owner_token='token',lease_until=?", (now_ms()+60000,))
    if unsafe == 'account_lease': await pair.source.db.execute('UPDATE music_task_account_leases SET lease_until=?', (now_ms()+60000,))
    if unsafe == 'model_slot': await pair.source.db.execute('UPDATE music_task_model_slots SET lease_until=?', (now_ms()+60000,))
    with pytest.raises(migration.MigrationError) as error:
        await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup)
    assert error.value.code == 'SOURCE_ACTIVE_OR_UNCERTAIN'
    assert not pair.backup.exists()
    for table in migration.TABLES:
        assert (await pair.target.db.fetchone(f'SELECT count(*) AS n FROM `{table}`'))['n'] == 0

async def test_target_must_be_empty_and_stays_untouched(pair):
    await pair.target.initialize()
    await pair.target.register(validateCredentials({'username':'target-existing','password':'isolated-existing-password'}),now_ms())
    with pytest.raises(migration.MigrationError) as error:
        await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup)
    assert error.value.code == 'TARGET_NOT_EMPTY'
    assert (await pair.target.db.fetchone('SELECT count(*) AS n FROM users'))['n'] == 1
    assert not pair.backup.exists()

async def test_digest_failure_rolls_back_every_target_table_and_keeps_pg(pair,monkeypatch):
    original = migration._insert_value
    def corrupt(column,value):
        return original(column,value).replace('粤语😀','错误内容') if column=='snapshot_json' else original(column,value)
    monkeypatch.setattr(migration,'_insert_value',corrupt)
    with pytest.raises(migration.MigrationError) as error:
        await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup)
    assert error.value.code == 'ROW_DIGEST_MISMATCH'
    assert list(pair.backup.glob('*.dump'))
    for table in migration.TABLES:
        assert (await pair.target.db.fetchone(f'SELECT count(*) AS n FROM `{table}`'))['n'] == 0
    assert (await pair.source.db.fetchone('SELECT count(*) AS n FROM users'))['n'] == 2

async def test_existing_report_rejected_before_backup_or_target_write(pair):
    report = pair.backup.parent / 'existing-report.json'
    private_write(report, 'keep-existing-report\n')
    with pytest.raises(migration.MigrationError) as error:
        await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup,report=report)
    assert error.value.code == 'REPORT_UNAVAILABLE'
    assert report.read_text() == 'keep-existing-report\n' and not pair.backup.exists()
    await pair.target.initialize()
    assert (await pair.target.db.fetchone('SELECT count(*) AS n FROM users'))['n'] == 0

async def test_report_write_failure_after_commit_reports_committed_state(pair,monkeypatch):
    report = pair.backup.parent / 'report.json'
    original = migration.private_write
    def failing_final(path,value,**kwargs):
        if kwargs.get('replace'): raise OSError('isolated report write failure')
        return original(path,value,**kwargs)
    monkeypatch.setattr(migration,'private_write',failing_final)
    result = await migration.migrate(pair.source_env,pair.target_env,execute=True,backup_directory=pair.backup,report=report)
    assert result['ok'] and result['verified'] and not result['reportWritten']
    assert result['reportWarning'] == 'COMMITTED_REPORT_WRITE_FAILED'
    assert (await pair.target.db.fetchone('SELECT count(*) AS n FROM users'))['n'] == 2
    assert json.loads(report.read_text())['status'] == 'pending'
