"""真实 Python/MySQL/Redis 进程重启保留开发身份、会话及额度；不接真实模型。"""

import os
import secrets
from uuid import uuid4
from helpers.mysql_fixture import mysql_database
from helpers.process_fixture import backend, provider
from tango_backend.store import MysqlStore
from tango_backend.compat import now_ms


async def test_mysql_process_restart_preserves_identity_session_and_quota(tmp_path):
    async with mysql_database() as url:
        token = secrets.token_hex(32)
        path = tmp_path / 'dev.token'
        path.write_text(token + '\n');path.chmod(0o600)
        env = {'QUIET_STORAGE_DRIVER':'mysql', 'QUIET_DATABASE_URL':url,
               'QUIET_REDIS_URL':os.environ['TANGO_TEST_REDIS_URL'],
               'QUIET_REDIS_NAMESPACE':'tango:mysql_process:' + uuid4().hex,
               'QUIET_DEVELOPMENT_MODE':'1', 'QUIET_DEVELOPMENT_TOKEN_FILE':str(path),
               'QUIET_REGISTRATION_MODE':'invite_only'}
        with provider() as (upstream, calls):
            with backend(tmp_path, upstream, env) as (client, proc, port):
                assert client.get('/health').json()['storage'] == 'mysql'
                assert client.post('/v1/dev/session',json={}).status_code == 403
                session = client.post('/v1/dev/session',json={},headers={'x-tango-development-token':token}).json()
                client.headers['authorization'] = 'Bearer ' + session['token']
                assert client.get('/v1/account/usage').json()['daily']['used'] == 0
                store = MysqlStore(url)
                try:
                    await store.reserveDailyUsage(session['user']['id'], now_ms(), 10000)
                finally:
                    await store.close()
            with backend(tmp_path, upstream, env) as (client, proc, port):
                client.headers['authorization'] = 'Bearer ' + session['token']
                assert client.get('/v1/auth/me').json()['user'] == session['user']
                usage = client.get('/v1/account/usage').json()
                assert usage['daily']['used'] == usage['total']['used'] == 1
                again = client.post('/v1/dev/session',json={},headers={'x-tango-development-token':token}).json()
                assert again['user'] == session['user'] and again['token'] != session['token']
                assert client.get('/v1/account/usage').json()['total']['used'] == 1
                assert calls == []
