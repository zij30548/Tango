"""维护 CLI 只操作显式临时库，明码不进入标准输出。"""
import asyncio
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import uuid
import pytest
from tango_backend.admin import runAdmin
from tango_backend.store import Store


async def test_invite_cli_private_delivery_no_overwrite_and_account_policy_preserves_usage(tmp_path):
    database = str(tmp_path / 'owned.sqlite')
    store = Store(database)
    await store.initialize()
    userId = str(uuid.uuid4())
    await store.db.execute('INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)',
        [userId, 'fixture', 'fixture', 'fixture-salt', 'fixture-hash', 1000])
    await store.reserveDailyUsage(userId, 1000, 10)
    try:
        output = tmp_path / 'beta.invite-codes.json'
        result = await runAdmin(['invite', 'issue', '--sqlite', database, '--output', str(output)], now=lambda: 1000)
        secret = json.loads(output.read_text())['code']
        assert secret not in json.dumps(result) and 'code_hash' not in json.dumps(result)
        assert stat.S_IMODE(output.stat().st_mode) == 0o600
        with pytest.raises(FileExistsError):
            await runAdmin(['invite', 'issue', '--sqlite', database, '--output', str(output)], now=lambda: 1000)
        assert (await store.db.fetchone('SELECT count(*) AS n FROM music_registration_invites'))['n'] == 1
        linked = tmp_path / 'linked.invite-codes.json'
        linked.symlink_to(output)
        with pytest.raises(OSError):
            await runAdmin(['invite', 'issue', '--sqlite', database, '--output', str(linked)], now=lambda: 1000)
        assert json.loads(output.read_text())['code'] == secret
        listing = await runAdmin(['invite', 'list', '--sqlite', database])
        assert secret not in json.dumps(listing) and listing['invites'][0]['usedCount'] == 0
        assert (await runAdmin(['invite', 'disable', '--sqlite', database, '--id', result['invite']['id']]))['invite']['disabled']
        policy = await runAdmin(['account', 'set', '--sqlite', database, '--user-id', userId, '--daily', '0', '--total', '0', '--enabled', 'false'], now=lambda: 1000)
        assert policy['disabled'] and policy['total']['used'] == 1 and policy['total']['limit'] == 0
        assert (await runAdmin(['account', 'users', '--sqlite', database]))['users'][0]['id'] == userId
    finally:
        await store.close()


async def test_cli_help_is_chinese_and_missing_selector_never_uses_ambient_database(tmp_path):
    root = Path(__file__).resolve().parents[1]
    trap = tmp_path / 'must-not-be-created.sqlite'
    env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'PYTHONPATH': str(root),
        'QUIET_DB_PATH': str(trap), 'QUIET_STORAGE_DRIVER': 'sqlite'}
    def invoke(*args):
        return subprocess.run([sys.executable, '-m', 'tango_backend.admin', *args], env=env, cwd=root, capture_output=True, text=True, timeout=10)
    help = await asyncio.to_thread(invoke, '--help')
    assert help.returncode == 0 and '用法' in help.stdout and '显式选择已有数据库' in help.stdout
    failed = await asyncio.to_thread(invoke, 'account', 'users')
    assert failed.returncode != 0 and not trap.exists()
    assert '未打印账号或配置内容' in failed.stderr and str(trap) not in failed.stderr
