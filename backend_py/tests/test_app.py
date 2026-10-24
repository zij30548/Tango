"""HTTP 契约与跨模块联调，使用临时数据库和假模型/曲库。"""
import asyncio
import json
from types import SimpleNamespace
from uuid import uuid4
import httpx
import pytest
from tango_backend.app import createApp
from tango_backend.store import Store
from tango_backend.compat import iso,now_ms


def brief(**patch):
    return {'kind':'research','title':'阅读时刻','scene':'读书','styles':[],'queries':['读书'],'artist':'','artistId':0,'requiredArtist':'','artistOnly':False,'since':'','until':'','targetMinutes':20,'excludeLive':True,'avoidPrevious':False,'excludedArtists':[],'requireReleases':False,'retainFirst':0,'removeIds':[],'question':'','adjustment':'',**patch}
class TaskModel:
    configured=True;model='fixture-task'
    def __init__(self):self.calls=[]
    async def call(self,kind,input,options=None):
        self.calls.append(kind)
        value=brief(**(input.get('currentBrief') or {})) if kind=='plan' else {'queries':['读书'],'artist':''} if kind=='research' else {'decision':'done','queries':[],'note':'来源已核对'} if kind=='observe' else {'ids':[t['id'] for t in input['candidates']]}
        return {'value':value,'usage':{'inputTokens':2,'outputTokens':1}}
    async def plan(self,*args):return await self.call('plan',*args)
    async def research(self,*args):return await self.call('research',*args)
    async def observe(self,*args):return await self.call('observe',*args)
    async def compose(self,*args):return await self.call('compose',*args)

@pytest.fixture
async def fx(tmp_path):
    store=Store(str(tmp_path/'test.sqlite'));model=TaskModel();route=SimpleNamespace(configured=False,model='disabled')
    app=createApp(store=store,model=model,taskModel=model,routingModel=route,registrationMode='open',aiPerMinute=100,authPerTenMinutes=100)
    async with app.router.lifespan_context(app):
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app,client=('127.0.0.1',32123)),base_url='http://127.0.0.1:32123') as client:
            r=await client.post('/v1/auth/register',json={'username':'test-user','password':'fixture-private-password'})
            assert r.status_code==201,r.text
            session=r.json();client.headers['authorization']='Bearer '+session['token']
            yield SimpleNamespace(client=client,app=app,store=store,model=model,session=session)
    await store.close()

async def test_health_and_original_client_protocol(fx):
    r=await fx.client.get('/health');assert r.status_code==200
    assert r.json()['runtimeLanguage']=='python' and r.json()['routingFeaturesVersion']==1
    assert r.json().get('lookupContextVersion') == 1
    assert (await fx.client.get('/v1/auth/me')).json()['user']==fx.session['user']
    assert (await fx.client.get('/v1/agent/status')).json()['taskRuntime'] is True
    assert (await fx.client.get('/v1/account/usage')).json()['daily']['used']==0
    assert (await fx.client.get('/v1/tasks/skills')).json()['runtime']=='langgraph'
    assert (await fx.client.get('/v1/agent/capabilities')).status_code==200

@pytest.mark.parametrize('path,method,content,headers,status,code',[
    ('/v1/tasks','POST','{}',{'content-type':'text/plain'},415,'JSON_REQUIRED'),
    ('/v1/tasks','POST','{bad',{'content-type':'application/json'},400,'INVALID_JSON'),
    ('/v1/tasks','POST','{"x":NaN}',{'content-type':'application/json'},400,'INVALID_JSON'),
    ('/v1/tasks','POST','x'*17000,{'content-type':'application/json'},413,'BODY_TOO_LARGE'),
    ('/v1/auth/me?bad=1','GET',None,{},400,'INVALID_ROUTE'),
    ('/v1/auth/me','GET',None,{'origin':'https://evil.invalid'},403,'ORIGIN_NOT_ALLOWED'),
    ('/v1/missing','GET',None,{},404,'NOT_FOUND')])
async def test_http_boundaries(fx,path,method,content,headers,status,code):
    r=await fx.client.request(method,path,content=content,headers=headers)
    assert (r.status_code,r.json()['error']['code'])==(status,code),r.text

async def test_conversation_receipts_route_restore_and_account_isolation(fx):
    device=str(uuid4());rid=str(uuid4());mid=str(uuid4())
    c=await fx.client.post('/v1/conversations',json={'deviceId':device,'requestId':rid});assert c.status_code==201,c.text
    cid=c.json()['conversation']['id'];base='/v1/conversations/'+cid
    message={'deviceId':device,'messageId':mid,'content':'你能做什么'}
    accepted=await fx.client.post(base+'/messages',json=message);assert accepted.status_code==200,accepted.text
    assert accepted.json()['receipt']['dispatchHint']=='router'
    duplicate=await fx.client.post(base+'/messages',json=message);assert duplicate.json()==accepted.json()
    assert (await fx.client.get(base+'/messages/'+mid)).json()==accepted.json()
    route=await fx.client.post(base+'/messages/'+mid+'/route',json={'deviceId':device,'preference':''})
    assert route.status_code==200,route.text
    assert route.json()['route']['status']=='completed' and route.json()['route']['paid'] is False
    replay=await fx.client.post(base+'/messages/'+mid+'/route',json={'deviceId':device,'preference':''});assert replay.json()==route.json()
    restored=await fx.client.get('/v1/conversations/latest/'+device);assert len(restored.json()['conversation']['messages'])==2
    assert fx.model.calls==[]
    other=await fx.client.post('/v1/auth/register',json={'username':'other-user','password':'fixture-private-password'})
    r=await fx.client.get(base,headers={'authorization':'Bearer '+other.json()['token']});assert r.status_code==404
    r=await fx.client.post(base+'/messages',json={**message,'content':'不同'},headers={'authorization':'Bearer '+fx.session['token']});assert r.status_code==409

async def until(client,tid,states):
    for _ in range(200):
        r=await client.get('/v1/tasks/'+tid);assert r.status_code==200,r.text
        task=r.json()['task']
        if task['status'] in states:return task
        await asyncio.sleep(.01)
    pytest.fail('任务未进入预期状态: '+json.dumps(task,ensure_ascii=False))

async def test_full_task_tools_preview_sse_adoption_and_restart_restore(fx):
    device=str(uuid4());rid=str(uuid4());body={'deviceId':device,'requestId':rid,'instruction':'读书20分钟，不要现场版','preference':''}
    r=await fx.client.post('/v1/tasks',json=body);assert r.status_code==201,r.text
    tid=r.json()['task']['id']
    replay=await fx.client.post('/v1/tasks',json=body);assert replay.json()['task']['id']==tid
    task=await until(fx.client,tid,{'waiting_tools','failed','needs_clarification'});assert task['status']=='waiting_tools',task.get('error')
    stamp=iso(now_ms());catalog={'tracks':[{'id':i+1,'title':f'曲目{i+1}','artist':'歌手','album':'专辑','durationMs':240000,'sourceId':77,'sourceKind':'playlist','sourceName':'读书','sourceTags':[]} for i in range(10)],'sources':[{'id':77,'name':'读书','checkedAt':stamp,'tags':[]}],'checkedAt':stamp}
    for step in task['pendingSteps']:
        result={'deviceId':device,'leaseId':step['leaseId'],'revision':step['revision'],'result':catalog}
        r=await fx.client.post(f'/v1/tasks/{tid}/steps/{step["id"]}/result',json=result);assert r.status_code==200,r.text
    task=await until(fx.client,tid,{'preview','failed','needs_clarification'});assert task['status']=='preview',task.get('error')
    assert task['preview']['totalDurationMs']==20*60000
    assert 'userId' not in task and fx.model.calls.count('compose')==1
    event=await fx.client.get(f'/v1/tasks/{tid}/events');assert event.status_code==200,event.text
    assert 'event: progress' in event.text and '"stage":"ready"' in event.text and 'fixture-private-password' not in event.text
    usage=(await fx.client.get('/v1/account/usage')).json();assert usage['daily']['used']==len(fx.model.calls)
    action={'deviceId':device,'requestId':str(uuid4()),'expectedRevision':task['revision'],'action':'adopt'}
    adopted=await fx.client.post(f'/v1/tasks/{tid}/actions',json=action);assert adopted.status_code==200,adopted.text
    current=adopted.json()['task'];assert current['status']=='awaiting_apply'
    applied={**action,'requestId':str(uuid4()),'expectedRevision':current['revision'],'action':'applied','adoptionId':current['adoption']['id']}
    applied=(await fx.client.post(f'/v1/tasks/{tid}/actions',json=applied));assert applied.status_code==200,applied.text
    assert applied.json()['task']['status']=='adopted'
    assert (await fx.client.get('/v1/tasks/latest/'+device)).json()['task']['id']==tid
    count=len(fx.model.calls)
    assert (await fx.client.get('/v1/tasks/'+tid)).status_code==200
    assert len(fx.model.calls)==count

async def test_logout_invalidates_session_and_pauses_task(fx):
    r=await fx.client.post('/v1/tasks',json={'deviceId':str(uuid4()),'requestId':str(uuid4()),'instruction':'读书'})
    assert r.status_code==201,r.text
    tid=r.json()['task']['id'];await until(fx.client,tid,{'waiting_tools','failed'})
    assert (await fx.client.post('/v1/auth/logout',json={})).status_code==200
    assert (await fx.client.get('/v1/auth/me')).status_code==401
    task=await fx.app.tasks.store.load(tid,fx.session['user']['id']);assert task['status']=='paused'

async def test_legacy_lease_loss_cancels_active_upstream(fx):
    signal=asyncio.Event();seen=asyncio.Event();claim={'signal':signal}
    async def acquire(userId):return claim
    async def reserve(*args):pass
    async def release(*args):pass
    async def plan(input,options=None):
        seen.set();await options['signal'].wait()
        from tango_backend.errors import ApiError
        raise ApiError(499,'REQUEST_CANCELLED','请求已取消。')
    fx.app.tasks.acquireLegacy=acquire;fx.app.tasks.reserveLegacyUsage=reserve;fx.app.tasks.releaseLegacy=release
    fx.app.services.radioModel=SimpleNamespace(configured=True,plan=plan)
    pending=asyncio.create_task(fx.client.post('/v1/radio/plan',json={'instruction':'读书','history':[],'preference':''}))
    await asyncio.wait_for(seen.wait(),1);signal.set()
    response=await asyncio.wait_for(pending,1)
    assert response.status_code==499 and response.json()['error']['code']=='REQUEST_CANCELLED'
