"""原生 LangGraph 与真实临时存储的任务边界回归；模型和曲库全部为夹具。"""
import asyncio
import os
import uuid
from types import SimpleNamespace
import pytest
from tango_backend.compat import clone,iso,now_ms
from tango_backend.errors import ApiError
from tango_backend.store import Store,PostgresStore
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.task_runtime import MusicTaskRuntime
from tango_backend.task_schema import validateBrief
from tango_backend.task_pending_request import preparePendingContinuation,validatePendingBrief


def raw_brief(**updates):
    return {'kind':'research','title':'阅读时刻','scene':'读书','styles':['R&B'],'queries':['读书 R&B'],'artist':'','requiredArtist':'','artistOnly':False,
            'since':None,'until':None,'targetMinutes':20,'excludeLive':True,'avoidPrevious':False,'excludedArtists':[],'requireReleases':False,
            'retainFirst':0,'removeIds':[],'question':'','adjustment':'',**updates}


class Model:
    configured=True
    model='owned-task-fixture'
    def __init__(self):
        self.calls=[];self.gate=None;self.entered=asyncio.Event();self.fail=None;self.invalid_observer=False;self.initial=raw_brief();self.followup=None;self.clarify=False
    async def call(self,kind,inputs,options=None):
        self.calls.append((kind,clone(inputs)));self.entered.set()
        if self.gate is not None:await self.gate.wait()
        if self.fail==kind:raise ApiError(502,'FIXTURE_MODEL_FAILURE','夹具模型失败，未调用外部服务。')
        if kind=='plan':
            prepared=preparePendingContinuation(inputs)
            if prepared:
                value=validatePendingBrief(raw_brief(artist='邓紫棋',requireReleases=True),prepared)
            elif self.clarify and not inputs.get('currentBrief'):
                value=validateBrief({'kind':'clarify','question':'哪位歌手？'},inputs)
            else:
                source=clone(inputs.get('currentBrief') or self.initial)
                if self.followup:source.update(self.followup(inputs))
                value=validateBrief(source,inputs)
        elif kind=='research':value={'queries':inputs['scope']['queries'],'artist':inputs['scope']['artist'],'artistId':0,'since':inputs['scope']['since'],'until':inputs['scope']['until'],'note':'合成研究计划'}
        elif kind=='observe':value={'decision':'run-arbitrary-code','queries':[]} if self.invalid_observer else {'decision':'done','queries':[],'note':'继续核验过的候选'}
        elif kind=='compose':value={'ids':[t['id'] for t in inputs['candidates']]}
        else:raise AssertionError(kind)
        return {'value':value,'usage':{'inputTokens':3,'outputTokens':2}}


@pytest.fixture(params=['sqlite','postgres'])
async def f(request,tmp_path):
    admin=schema=None;stores=[];runtimes=[]
    if request.param=='postgres':
        url=os.environ.get('TANGO_TEST_DATABASE_URL')
        if not url:pytest.skip('须通过独立runner显式提供临时 PostgreSQL')
        import asyncpg
        admin=await asyncpg.connect(url);schema='task_runtime_'+uuid.uuid4().hex
        await admin.execute('CREATE SCHEMA '+schema)
        async def new_store():
            store=PostgresStore(connectionString=url,serverSettings={'search_path':schema});stores.append(store);await store.initialize();return store
    else:
        async def new_store():
            store=Store(tmp_path/'isolated.sqlite');stores.append(store);await store.initialize();return store
    store=await new_store();user=str(uuid.uuid4());time=[now_ms()]
    await store.db.execute('INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)',[user,'fixture-user','fixture-user','fixture-salt','fixture-hash',time[0]])
    store.accountPolicy=AccountPolicyStore(store)
    model=Model()
    async def runtime(**options):
        account=options.pop('store',store);value=MusicTaskRuntime(store=account,model=options.pop('model',model),now=lambda:time[0],**options)
        runtimes.append(value);await value.initialize();return value
    try:yield SimpleNamespace(store=store,user=user,model=model,runtime=runtime,new_store=new_store,time=time,postgres=request.param=='postgres')
    finally:
        model.gate.set() if model.gate else None
        for runtime in runtimes:await runtime.close()
        for store in stores:await store.close()
        if admin:
            await admin.execute('DROP SCHEMA '+schema+' CASCADE');await admin.close()


def body(**updates):return {'deviceId':'fixture-device','requestId':str(uuid.uuid4()),'instruction':'读书20分钟 R&B，不要现场版','preference':'',**updates}
async def until(runtime,taskId,user,*statuses):
    for _ in range(300):
        task=await runtime.get(taskId,user)
        if task['status'] in statuses:return task
        await asyncio.sleep(.01)
    pytest.fail('未进入预期状态：'+task['status']+' '+str(task.get('error')))


def catalog(step,now,*,label='读书 R&B',offset=0):
    release=step['skill']=='releases.research';source_id=88 if release else 77;name='近期发行' if release else label
    tracks=[]
    for i in range(2 if release else 10):
        row={'id':9000+i if release else 1000+offset+i,'title':'录音室曲目'+str(i),'artist':'邓紫棋' if release else '陶喆','album':'专辑','coverUrl':'','durationMs':240000,
             'sourceId':source_id,'sourceKind':'album' if release else 'playlist','sourceName':name,'sourceTags':[] if release else ['R&B','华语']}
        if release:row.update(publishedAt=iso(now-1000),artistId=7763,albumId=88)
        tracks.append(row)
    result={'tracks':tracks,'sources':[{'id':source_id,'name':name,'checkedAt':iso(now),'sourceKind':'album' if release else 'playlist','tags':[]}],'checkedAt':iso(now),'artists':[]}
    if release:result['resolvedArtist']={'id':7763,'name':'邓紫棋'}
    return {'deviceId':'fixture-device','leaseId':step['leaseId'],'revision':step['revision'],'result':result}


async def finish(runtime,task,user,now,**kwargs):
    waiting=await until(runtime,task['id'],user,'waiting_tools','failed','needs_clarification','budget_exhausted')
    assert waiting['status']=='waiting_tools',waiting.get('error')
    await asyncio.gather(*(runtime.result(task['id'],step['id'],user,catalog(step,now,**kwargs)) for step in waiting['pendingSteps']))
    preview=await until(runtime,task['id'],user,'preview','failed','needs_clarification','budget_exhausted')
    assert preview['status']=='preview',preview.get('error')
    return preview,waiting


@pytest.mark.parametrize('strategy',['fixed','single_agent','dual_agent'])
async def test_actual_langgraph_complete_lock_turn_undo_and_adopt(f,strategy):
    runtime=await f.runtime(strategy=strategy);created=await runtime.create(f.user,body())
    first,_=await finish(runtime,created,f.user,f.time[0]);assert first['preview']['totalDurationMs']==1200000
    assert set(runtime.graph.get_graph().nodes)>={'entry','understand','research','dispatch','observe','compose'}
    lock=await runtime.action(first['id'],f.user,body(action='lock',expectedRevision=first['revision'],lockedIds=[first['preview']['tracks'][0]['id']]))
    assert lock['lockedIds']==[first['preview']['tracks'][0]['id']]
    f.model.followup=lambda _: {'adjustment':'轻快','styles':['R&B','轻快']}
    turn=await runtime.action(first['id'],f.user,body(action='turn',expectedRevision=lock['revision'],instruction='保留锁定曲目，后半段轻快一点'))
    revised,_=await finish(runtime,turn,f.user,f.time[0],label='读书 R&B 轻快',offset=100)
    assert revised['preview']['tracks'][0]['id']==lock['lockedIds'][0]
    assert revised['preview']['adjustment']['status']=='source_supported'
    calls=len(f.model.calls)
    undone=await runtime.action(first['id'],f.user,body(action='undo',expectedRevision=revised['revision']))
    assert [t['id'] for t in undone['preview']['tracks']]==[t['id'] for t in first['preview']['tracks']]
    assert len(f.model.calls)==calls
    adopt_body=body(action='adopt',expectedRevision=undone['revision'])
    adopted=await runtime.action(first['id'],f.user,adopt_body)
    assert adopted['status']=='awaiting_apply' and adopted['adoption']['id']
    assert (await runtime.action(first['id'],f.user,adopt_body))['adoption']==adopted['adoption']
    applied=await runtime.action(first['id'],f.user,body(action='applied',expectedRevision=adopted['revision'],adoptionId=adopted['adoption']['id']))
    assert applied['status']=='adopted'
    ledger=await f.store.db.fetchall('SELECT status FROM music_task_calls')
    assert ledger and all(r['status']=='completed' for r in ledger)
    assert 'userId' not in applied and 'preference' not in applied


async def test_cancel_keeps_paid_slot_until_model_settles_and_rejects_old_completion(f):
    runtime=await f.runtime();f.model.gate=asyncio.Event();created=await runtime.create(f.user,body())
    await asyncio.wait_for(f.model.entered.wait(),2)
    cancelled=await runtime.action(created['id'],f.user,body(action='cancel',expectedRevision=1))
    assert cancelled['status']=='cancelled'
    with pytest.raises(ApiError) as error:await runtime.create(f.user,body(instruction='另找新歌单'))
    assert error.value.code=='AI_BUSY'
    f.model.gate.set()
    for _ in range(200):
        if not runtime.accountCalls:break
        await asyncio.sleep(.01)
    assert (await runtime.get(created['id'],f.user))['status']=='cancelled'
    assert len(f.model.calls)==1
    assert (await f.store.db.fetchone('SELECT request_count FROM ai_usage'))['request_count']==1


async def test_budget_reservation_failure_and_explicit_resume_do_not_reset_total(f):
    runtime=await f.runtime(maxRunCalls=1)
    created=await runtime.create(f.user,body());stopped=await until(runtime,created['id'],f.user,'budget_exhausted','failed')
    assert stopped['status']=='budget_exhausted' and stopped['budget']['totalCalls']==1
    with pytest.raises(ApiError) as error:await runtime.action(created['id'],f.user,body(action='resume',expectedRevision=1))
    assert error.value.code=='TASK_BUDGET' and len(f.model.calls)==1


async def test_tool_lease_result_injection_and_idempotency_guards(f):
    runtime=await f.runtime();request=body();created=await runtime.create(f.user,request)
    assert (await runtime.create(f.user,request))['id']==created['id']
    with pytest.raises(ApiError) as conflict:await runtime.create(f.user,{**request,'instruction':'不同内容'})
    assert conflict.value.code=='IDEMPOTENCY_CONFLICT'
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');assert waiting['status']=='waiting_tools'
    step=waiting['pendingSteps'][0];valid=catalog(step,f.time[0])
    for changed,code in [({'leaseId':'another-valid-lease'},'STALE_TASK_STEP'),({'revision':step['revision']+1},'STALE_TASK_STEP'),({'deviceId':'other-device'},'TASK_DEVICE_MISMATCH')]:
        with pytest.raises(ApiError) as error:await runtime.result(created['id'],step['id'],f.user,{**valid,**changed})
        assert error.value.code==code
    with pytest.raises(ApiError) as owner:await runtime.get(created['id'],'other-user')
    assert owner.value.code=='TASK_NOT_FOUND'
    await runtime.result(created['id'],step['id'],f.user,valid)
    await runtime.result(created['id'],step['id'],f.user,valid)
    changed=clone(valid);changed['result']['tracks'][0]['title']='恶意变更'
    with pytest.raises(ApiError) as conflict:await runtime.result(created['id'],step['id'],f.user,changed)
    assert conflict.value.code=='STEP_RESULT_CONFLICT'
    assert (await until(runtime,created['id'],f.user,'preview','failed'))['status']=='preview'


async def test_tool_timeout_and_restart_are_observation_only_until_resume(f):
    runtime=await f.runtime();created=await runtime.create(f.user,body());waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed')
    assert waiting['status']=='waiting_tools';calls=len(f.model.calls);old_lease=waiting['pendingSteps'][0]['leaseId']
    await runtime.close();restored_runtime=await f.runtime()
    restored=await restored_runtime.get(created['id'],f.user)
    assert restored['status']==('waiting_tools' if f.postgres else 'paused') and len(f.model.calls)==calls
    await asyncio.sleep(.05);assert len(f.model.calls)==calls
    resumed=await restored_runtime.action(created['id'],f.user,body(action='resume',expectedRevision=restored['revision']))
    waiting=await until(restored_runtime,created['id'],f.user,'waiting_tools','failed')
    assert waiting['pendingSteps'][0]['leaseId']!=old_lease and len(f.model.calls)==calls
    f.time[0]+=45001
    timed_out=await restored_runtime.get(created['id'],f.user)
    assert timed_out['status']=='paused' and timed_out['pendingSteps']==[]


async def test_completed_preview_pause_restart_resume_never_schedules_model(f):
    runtime = await f.runtime()
    created = await runtime.create(f.user, body())
    preview, _ = await finish(runtime, created, f.user, f.time[0])
    paused = await runtime.action(created['id'], f.user, body(action='pause', expectedRevision=preview['revision']))
    await runtime.close()
    restored = await f.runtime()
    current = await restored.get(created['id'], f.user)
    calls = len(f.model.calls)
    resumed = await restored.action(created['id'], f.user, body(action='resume', expectedRevision=current['revision']))
    assert resumed['status'] == 'preview'
    assert [t['id'] for t in resumed['preview']['tracks']] == [t['id'] for t in preview['preview']['tracks']]
    assert resumed['adoption'] is None and resumed['pendingSteps'] == []
    await asyncio.sleep(.02)
    assert len(f.model.calls) == calls and not restored.jobs

    # 只恢复已有预览时，模型预算和账号禁用不能触发新的付费请求。
    paused = await restored.action(created['id'], f.user, body(action='pause', expectedRevision=resumed['revision']))
    raw = await restored.store.load(created['id'], f.user)
    raw['budget'].update(runCalls=raw['budget']['maxRunCalls'], totalCalls=raw['budget']['maxTaskCalls'])
    await restored.checkpoint(raw, 'fixture_budget_exhausted')
    await f.store.accountPolicy.adminset(f.user, {'dailyLimit': 0, 'totalLimit': 0, 'enabled': False}, now=f.time[0])
    observed = await restored.get(created['id'], f.user)
    assert observed['recovery']['canResume'] is True
    resumed = await restored.action(created['id'], f.user, body(action='resume', expectedRevision=observed['revision']))
    assert resumed['status'] == 'preview' and len(f.model.calls) == calls


async def test_invalid_observer_is_explicitly_degraded_but_provider_errors_fail(f):
    runtime=await f.runtime();f.model.invalid_observer=True
    created=await runtime.create(f.user,body());preview,_=await finish(runtime,created,f.user,f.time[0],label='来源未标明场景')
    assert any(c['key']=='observer' and c['status']=='unverified' for c in preview['preview']['checks'])
    assert preview['preview']['degradations'][0]['code']=='INVALID_TASK_MODEL'
    f.model.invalid_observer=False;f.model.fail='observe'
    second=await runtime.create(f.user,body(requestId=str(uuid.uuid4())))
    waiting=await until(runtime,second['id'],f.user,'waiting_tools','failed')
    for step in waiting['pendingSteps']:await runtime.result(second['id'],step['id'],f.user,catalog(step,f.time[0],label='仍未标明场景'))
    failed=await until(runtime,second['id'],f.user,'failed','preview')
    assert failed['status']=='failed' and failed['error']['code']=='FIXTURE_MODEL_FAILURE'


async def test_release_branches_concurrent_handoff_and_verified_artist(f):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True,since=iso(f.time[0]-86400000))
    runtime=await f.runtime(strategy='dual_agent',maxRunCalls=8)
    created=await runtime.create(f.user,body(instruction='读书20分钟 R&B，加入邓紫棋最近的新歌，不要现场版'))
    preview,waiting=await finish(runtime,created,f.user,f.time[0])
    assert len(waiting['pendingSteps'])==2
    assert preview['brief']['artistId']==7763
    assert any(c['key']=='releases' and c['status']=='satisfied' for c in preview['preview']['checks'])
    assert len([kind for kind,_ in f.model.calls if kind=='compose'])==1


@pytest.mark.parametrize('strategy',['fixed','single_agent','dual_agent'])
async def test_pure_release_query_never_opens_generic_scene_lane(f,strategy):
    f.model.initial=raw_brief(scene='邓紫棋的发行查询',styles=[],queries=['邓紫棋 新歌'],artist='邓紫棋',requireReleases=True,targetMinutes=0)
    runtime=await f.runtime(strategy=strategy)
    created=await runtime.create(f.user,body(instruction='你能查询邓紫棋的新歌吗'))
    preview,waiting=await finish(runtime,created,f.user,f.time[0])
    assert [s['skill'] for s in waiting['pendingSteps']]==['releases.research']
    assert {t['sourceSkill'] for t in preview['preview']['tracks']}=={'releases.research'}
    assert {t['id'] for t in preview['preview']['tracks']}=={9000,9001}


async def test_validated_release_intent_overrides_model_generated_scene(f):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True)
    runtime=await f.runtime(strategy='fixed')
    runtime.conversationContextProvider=lambda *args: {'intents':['release_research'],'musicMode':'new'}
    created=await runtime.create(f.user,body(instruction='读书时顺便查一下邓紫棋的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed')
    assert [s['skill'] for s in waiting['pendingSteps']]==['releases.research']


@pytest.mark.parametrize('release_first',[False,True])
async def test_parallel_scene_result_never_bypasses_pending_artist_choice(f,release_first):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True)
    runtime=await f.runtime(strategy='fixed')
    created=await runtime.create(f.user,body(instruction='读书20分钟，加入邓紫棋的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed')
    steps=sorted(waiting['pendingSteps'],key=lambda step: (step['skill']=='releases.research')!=release_first)
    for step in steps:
        result=catalog(step,f.time[0])
        if step['skill']=='releases.research':
            result['result']={'tracks':[],'sources':[],'checkedAt':iso(f.time[0]),'question':'请选择曲库歌手',
                              'artists':[{'id':7763,'name':'邓紫棋','description':'歌手一'},{'id':7764,'name':'邓紫棋','description':'同名歌手'}]}
        await runtime.result(created['id'],step['id'],f.user,result)
    clarify=await until(runtime,created['id'],f.user,'needs_clarification','preview','failed')
    assert clarify['status']=='needs_clarification' and not clarify.get('preview')
    assert clarify['question']=='请选择曲库歌手'
    assert {a['id'] for a in clarify['providedArtists']}=={7763,7764}
    assert not any(kind=='compose' for kind,_ in f.model.calls)
    # 旧检查点丢失顶层 question 时，分支证据仍必须阻止 compose。
    stored=await runtime.store.load(created['id'],f.user)
    stored.update(question='',providedArtists=[],phase='compose')
    await runtime.compose(stored,stored['revision'])
    assert stored['status']=='needs_clarification' and stored['question']=='请选择曲库歌手'
    assert not any(kind=='compose' for kind,_ in f.model.calls)


async def test_release_tracks_require_catalog_verified_identity(f):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True)
    runtime=await f.runtime();created=await runtime.create(f.user,body(instruction='查邓紫棋的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    unverified=catalog(step,f.time[0]);unverified['result'].pop('resolvedArtist')
    with pytest.raises(ApiError) as failure:
        await runtime.result(created['id'],step['id'],f.user,unverified)
    assert failure.value.code=='RELEASE_ARTIST_UNVERIFIED'
    saved=await runtime.store.load(created['id'],f.user)
    assert all(not branch['result'] and not branch.get('resultHash') for branch in saved['branches'].values())


@pytest.mark.parametrize('require_choice',[False,True])
async def test_catalog_resolved_alias_preserves_artist_only_release_tracks(f,require_choice):
    f.model.initial=raw_brief(scene='',styles=[],queries=['邓紫棋 新歌'],artist='邓紫棋',requiredArtist='邓紫棋',artistOnly=True,requireReleases=True,targetMinutes=0)
    runtime=await f.runtime(strategy='fixed');created=await runtime.create(f.user,body(instruction='只查邓紫棋的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    if require_choice:
        choice={'deviceId':'fixture-device','leaseId':step['leaseId'],'revision':step['revision'],
                'result':{'tracks':[],'sources':[],'checkedAt':iso(f.time[0]),'question':'请确认歌手身份',
                          'artists':[{'id':7763,'name':'G.E.M.邓紫棋','description':'曲库歌手'},{'id':7764,'name':'邓紫棋','description':'同名歌手'}]}}
        await runtime.result(created['id'],step['id'],f.user,choice)
        clarify=await until(runtime,created['id'],f.user,'needs_clarification','failed')
        await runtime.action(created['id'],f.user,body(action='select_artist',expectedRevision=clarify['revision'],artistId=7763))
        waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    verified=catalog(step,f.time[0]);verified['result']['resolvedArtist']['name']='G.E.M.邓紫棋'
    for track in verified['result']['tracks']:track['artist']='G.E.M.邓紫棋'
    await runtime.result(created['id'],step['id'],f.user,verified)
    preview=await until(runtime,created['id'],f.user,'preview','needs_clarification','failed')
    assert preview['status']=='preview' and len(preview['preview']['tracks'])==2
    assert preview['brief']['requiredArtist']=='G.E.M.邓紫棋'
    assert {t['artistId'] for t in preview['preview']['tracks']}=={7763}


async def test_pure_release_restore_ignores_stale_scene_candidates(f):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True,targetMinutes=0)
    runtime=await f.runtime(strategy='fixed');created=await runtime.create(f.user,body(instruction='查邓紫棋的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    schedule=runtime.schedule
    runtime.schedule=lambda task:None
    await runtime.result(created['id'],step['id'],f.user,catalog(step,f.time[0]))
    stored=await runtime.store.load(created['id'],f.user)
    scene_step={**step,'skill':'scene.research'}
    stored['branches']['old_scene']={'skill':'scene.research','result':catalog(scene_step,f.time[0])['result']}
    # 恢复检查点须交回真实 worker 获取租约，不能绕过 MySQL 付费调用的 claim 守卫。
    stored.update(phase='compose',status='composing')
    await runtime.checkpoint(stored,'legacy_scene_fixture')
    runtime.schedule=schedule;runtime.schedule(stored)
    preview=await until(runtime,created['id'],f.user,'preview','needs_clarification','failed')
    assert preview['status']=='preview',preview.get('error')
    assert {t['id'] for t in preview['preview']['tracks']}=={9000,9001}


async def test_out_of_range_release_does_not_report_empty_preview_as_success(f):
    f.model.initial=raw_brief(artist='邓紫棋',requireReleases=True,since=iso(f.time[0]-86400000))
    runtime=await f.runtime(strategy='fixed');created=await runtime.create(f.user,body(instruction='查邓紫棋最近一天的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    result=catalog(step,f.time[0])
    for track in result['result']['tracks']:track['publishedAt']=iso(f.time[0]-2*86400000)
    await runtime.result(created['id'],step['id'],f.user,result)
    clarify=await until(runtime,created['id'],f.user,'needs_clarification','preview','failed')
    assert clarify['status']=='needs_clarification' and not clarify.get('preview')
    assert not any('已生成 0 首' in entry['content'] for entry in clarify['messages'])


async def test_pg_two_workers_do_not_overlap_provider_or_recover_fresh_lease(f):
    if not f.postgres:pytest.skip('多worker持久租约由 PostgreSQL 验证')
    runtime=await f.runtime(workerId='worker-one');f.model.gate=asyncio.Event()
    created=await runtime.create(f.user,body());await asyncio.wait_for(f.model.entered.wait(),2)
    second_store=await f.new_store();other_model=Model();other=await f.runtime(store=second_store,model=other_model,workerId='worker-two')
    assert (await other.get(created['id'],f.user))['status']=='planning'
    with pytest.raises(ApiError) as error:await other.create(f.user,body(instruction='另一进程的新任务'))
    assert error.value.code=='AI_BUSY' and other_model.calls==[]
    f.model.gate.set();waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed')
    assert waiting['status']=='waiting_tools'
    f.time[0]+=45001
    paused=await other.get(created['id'],f.user);assert paused['status']=='paused'
    resumed=await other.action(created['id'],f.user,body(action='resume',expectedRevision=paused['revision']))
    assert (await until(other,resumed['id'],f.user,'waiting_tools','failed'))['status']=='waiting_tools'
    assert other_model.calls==[]


async def test_restart_preserves_unknown_call_charge_and_never_replays_without_resume(f):
    runtime=await f.runtime();runtime.schedule=lambda _:None
    created=await runtime.create(f.user,body());stored=await runtime.store.load(created['id'],f.user)
    claim=await runtime.store.claimTask(stored['id'],f.user,{'workerId':runtime.workerId,'now':f.time[0],'leaseMs':runtime.leaseMs})
    stored['budget'].update(runCalls=1,totalCalls=1)
    call_id=await runtime.store.reserveCall(stored,'understand',f.time[0],f.store,100,2000,{'claim':claim,'globalConcurrency':8,'leaseMs':runtime.leaseMs})
    await runtime.checkpoint(stored,'understand',options={'claim':claim})
    await runtime.close();f.time[0]+=runtime.leaseMs+1
    restored_runtime=await f.runtime();restored=await restored_runtime.get(stored['id'],f.user)
    assert restored['status']=='paused' and restored['budget']['totalCalls']==1
    assert (await f.store.db.fetchone('SELECT status FROM music_task_calls WHERE id=?',[call_id]))['status']=='uncertain'
    assert len(f.model.calls)==0
    await asyncio.sleep(.03);assert len(f.model.calls)==0
    resumed=await restored_runtime.action(stored['id'],f.user,body(action='resume',expectedRevision=restored['revision']))
    waiting=await until(restored_runtime,resumed['id'],f.user,'waiting_tools','failed')
    assert waiting['status']=='waiting_tools' and waiting['budget']['totalCalls']==3
    assert (await f.store.db.fetchone('SELECT status FROM music_task_calls WHERE id=?',[call_id]))['status']=='uncertain'


async def test_artist_choice_is_catalog_bound_and_mismatched_identity_cannot_enter_evidence(f):
    f.model.initial=raw_brief(queries=[],artist='邓紫棋',requireReleases=True)
    runtime=await f.runtime();created=await runtime.create(f.user,body(instruction='查邓紫棋最近的新歌'))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');assert len(waiting['pendingSteps'])==1
    step=waiting['pendingSteps'][0]
    choice={'deviceId':'fixture-device','leaseId':step['leaseId'],'revision':step['revision'],
            'result':{'tracks':[],'sources':[],'checkedAt':iso(f.time[0]),'question':'请选择曲库中的歌手',
                      'artists':[{'id':7763,'name':'邓紫棋','description':'歌手一'},{'id':7764,'name':'邓紫棋','description':'同名歌手'}]}}
    await runtime.result(created['id'],step['id'],f.user,choice)
    clarify=await until(runtime,created['id'],f.user,'needs_clarification','failed')
    with pytest.raises(ApiError) as error:await runtime.action(created['id'],f.user,body(action='select_artist',expectedRevision=clarify['revision'],artistId=9999))
    assert error.value.code=='INVALID_ARTIST_SELECTION'
    await runtime.action(created['id'],f.user,body(action='select_artist',expectedRevision=clarify['revision'],artistId=7763))
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed');step=waiting['pendingSteps'][0]
    assert step['input']['artistId']==7763
    injected=catalog(step,f.time[0]);injected['result']['resolvedArtist']['id']=42
    with pytest.raises(ApiError) as error:await runtime.result(created['id'],step['id'],f.user,injected)
    assert error.value.code=='ARTIST_ID_MISMATCH'
    stored=await runtime.store.load(created['id'],f.user)
    release_branch=next(b for b in stored['branches'].values() if b['skill']=='releases.research')
    assert release_branch['result'] is None and release_branch['resultHash'] is None


async def test_same_input_full_workflow_matches_frozen_js_langgraph_runtime(f):
    from pathlib import Path
    import json
    gold=json.loads((Path(__file__).parent/'fixtures/task_runtime_js_v1.json').read_text())
    f.time[0]=gold['now'];runtime=await f.runtime();created=await runtime.create(f.user,gold['request'])
    waiting=await until(runtime,created['id'],f.user,'waiting_tools','failed')
    assert waiting['status']=='waiting_tools'
    for step in waiting['pendingSteps']:
        await runtime.result(created['id'],step['id'],f.user,{'deviceId':'fixture-device','leaseId':step['leaseId'],'revision':step['revision'],'result':clone(gold['catalog'])})
    done=await until(runtime,created['id'],f.user,'preview','failed');assert done['status']=='preview',done.get('error')
    observed={'status':done['status'],'brief':done['brief'],'preview':done['preview'],'budget':done['budget'],'traceNodes':[e['node'] for e in done['trace']],'modelCalls':[kind for kind,_ in f.model.calls]}
    assert observed==gold['expected']
