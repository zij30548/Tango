"""真实Python进程开发会话、重启恢复和启动守卫；不调用真实模型。"""
import secrets
import sqlite3
from helpers.process_fixture import backend,provider

def test_development_bootstrap_restart_and_host_guards(tmp_path):
    token=secrets.token_hex(32);path=tmp_path/'dev.token';path.write_text(token+'\n');path.chmod(0o600)
    env={'QUIET_DEVELOPMENT_MODE':'1','QUIET_DEVELOPMENT_TOKEN_FILE':str(path),'QUIET_REGISTRATION_MODE':'closed'}
    with provider() as (url,calls):
        with backend(tmp_path,url,env) as (client,proc,port):
            h={'x-tango-development-token':token}
            health=client.get('/health').json();assert health['developmentMode'] is True and token not in str(health)
            assert client.post('/v1/dev/session',json={}).status_code==403
            for extra in [{'host':f'localhost:{port}'},{'host':'evil.invalid'},{'x-forwarded-for':'127.0.0.1'},{'origin':'https://evil.invalid'},{'sec-fetch-site':'cross-site'}]:assert client.post('/v1/dev/session',json={},headers={**h,**extra}).status_code==403
            response=client.post('/v1/dev/session',json={},headers=h);assert response.status_code==200,response.text
            session=response.json();assert session['development'] is True
            client.headers['authorization']='Bearer '+session['token']
            usage=client.get('/v1/account/usage').json();assert usage['daily']['limit']==10000 and usage['total']['limit']==100000
            assert calls==[]
        with backend(tmp_path,url,env) as (client,proc,port):
            session2=client.post('/v1/dev/session',json={},headers={'x-tango-development-token':token}).json()
            assert session2['user']==session['user'] and session2['token']!=session['token'];assert calls==[]
            with sqlite3.connect(tmp_path/'accounts.sqlite') as db:assert db.execute('SELECT count(*) FROM users').fetchone()[0]==1

def test_production_entry_requires_auth_and_invite(tmp_path):
    with provider() as (url,calls):
        with backend(tmp_path,url,{'QUIET_REGISTRATION_MODE':'invite_only'}) as (client,proc,port):
            assert client.get('/v1/agent/status').status_code==401
            assert client.post('/v1/dev/session',json={}).status_code==404
            assert client.get('/v1/auth/config').json()['inviteRequired'] is True
            assert client.post('/v1/auth/register',json={'username':'attacker','password':'long-fixture-private-password'}).status_code==403
            assert calls==[]
