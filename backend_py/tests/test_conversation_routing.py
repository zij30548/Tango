"""临时 SQLite 与固定模型提案的迁移验收；不调用真实模型、曲库或账号。"""
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from uuid import uuid4
import pytest
from tango_backend.errors import ApiError
from tango_backend.compat import clone, digest
from tango_backend.store import Store, validateCredentials
from tango_backend.task_store import TaskStore
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.conversation_store import ConversationStore
from tango_backend.conversation_route_store import ConversationRouteStore
from tango_backend.conversation_router import ConversationRouter
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal, HELP
from tango_backend.capability_registry import capabilities

NOW = 1792483200000
DEVICE = 'python_routing_fixture_device'
DATA = Path(__file__).parent / 'fixtures'
CASES = json.loads((DATA / 'routing_contract_cases.json').read_text())
GOLD = json.loads((DATA / 'routing_js_expected.json').read_text())
EXPECTED = {c['id']: c for c in GOLD['cases']}

def music(input):
    return {'intents': ['scene_research'], 'actions': [{'capability': 'music.task', 'source': input['instruction'],
        'dependsOn': [], 'parameters': {'mode': 'new'}}], 'missingSlots': []}

@pytest.mark.parametrize('case', CASES, ids=lambda c: c['id'])
def test_same_input_js_routing_contract(case):
    try:
        proposal = deterministicRoute(case['input']) if case['kind'] == 'deterministic' else case['proposal']
        result = {'id': case['id'], 'ok': True, 'result': validateRouteProposal(proposal, case['input'], messageId='route_contract_' + case['id']) if proposal else None}
    except ApiError as error:
        result = {'id': case['id'], 'ok': False, 'code': error.code}
    expected = clone(EXPECTED[case['id']])
    if case['id'] in ['help', 'hello']:
        # 历史金标原文保留；新版帮助必须描述聊天操作，其他结构与授权仍逐项比较。
        expected['result']['actions'][0]['parameters']['text'] = HELP
    assert result == expected

def test_current_help_describes_lookup_history_and_chat_confirmation():
    for text in ['搜索或播放指定歌曲', '之前聊过哪些歌手', '输入“采用歌单”', '确认保存口味', '再次确认']:
        assert text in HELP
    assert '点击采用' not in HELP

def test_registry_preserves_frozen_v1_capabilities_and_adds_explicit_lookup_extensions():
    # 冻结的 JS 金标不重写；新版只在明确扩展处改变注册表。
    current = capabilities()
    assert current['version'] == 4
    by_name = {value['name']: clone(value) for value in current['capabilities']}
    assert set(by_name) == {value['name'] for value in GOLD['capabilities']['capabilities']} | {'music.lookup', 'music.refine', 'music.artist_catalog', 'music.playlist_search'}
    by_name['assistant.reply']['inputSchema']['properties']['kind']['enum'].remove('history')
    for original in GOLD['capabilities']['capabilities']:
        assert by_name[original['name']] == original
    lookup = by_name['music.lookup']
    assert lookup['inputSchema']['additionalProperties'] is False
    assert lookup['confirmation'] == 'ambiguous_match_requires_selection'
    altered = capabilities(); altered['capabilities'][0]['acceptance'].append('caller mutation')
    assert capabilities() == current

class RuntimeStub:
    def __init__(self, store):
        self.store = TaskStore(store)
        self.accountCalls, self.legacyClaims, self.inflight = {}, {}, 0
    async def initialize(self):
        await self.store.initialize(); return self
    def legacyCount(self):
        return 0
    async def owned(self, id, userId, deviceId=None):
        task = await self.store.load(id, userId)
        if not task:
            raise ApiError(404, 'TASK_NOT_FOUND', '音乐任务不存在。')
        if deviceId and task['deviceId'] != deviceId:
            raise ApiError(409, 'TASK_DEVICE_MISMATCH', '设备不匹配。')
        return task

@pytest.fixture
async def fx(tmp_path):
    store = Store(tmp_path / 'isolated.sqlite')
    store.accountPolicy = AccountPolicyStore(store)
    await store.initialize()
    user = await store.register(validateCredentials({'username': 'python-route-a', 'password': 'isolated-fixture-password'}), NOW)
    other = await store.register(validateCredentials({'username': 'python-route-b', 'password': 'isolated-fixture-password'}), NOW)
    runtime = RuntimeStub(store)
    await runtime.initialize()
    conversations = ConversationStore(store, now=lambda: NOW)
    await conversations.initialize()
    conversation = await conversations.create(user['id'], {'deviceId': DEVICE, 'requestId': str(uuid4())})
    calls = []
    class Model:
        configured, model = True, 'isolated-stub'
        async def route(self, input, options=None):
            calls.append(clone(input))
            assert isinstance(options['signal'], asyncio.Event)
            return {'value': music(input), 'usage': {'inputTokens': 11, 'outputTokens': 7}}
    router = ConversationRouter(store=store, conversations=conversations, tasks=runtime, model=Model(), now=lambda: NOW)
    await router.initialize()
    async def submit(text='读书时听二十分钟纯音乐。', **more):
        body = {'deviceId': DEVICE, 'messageId': str(uuid4()), 'content': text, **more}
        result = await conversations.message(conversation['id'], user['id'], body, {'dispatchHint': 'router'})
        return body, result
    async def route(body, **more):
        return await router.route(conversation['id'], body['messageId'], user['id'], {'deviceId': DEVICE, 'preference': '', **more})
    value = SimpleNamespace(store=store, user=user, other=other, runtime=runtime, conversations=conversations,
                            conversation=conversation, router=router, calls=calls, submit=submit, route=route)
    yield value
    await router.close(); await store.close()

async def used(fx):
    return (await fx.store.accountPolicy.snapshot(fx.user['id'], NOW))['total']['used']

async def test_constructors_do_not_open_database_or_start_background_work(tmp_path):
    path = tmp_path / 'not-opened.sqlite'
    store = Store(path)
    conv = ConversationStore(store)
    runtime = RuntimeStub(store)
    router = ConversationRouter(store=store, conversations=conv, tasks=runtime)
    assert not path.exists()
    assert router.repository is None
    await router.close(); await store.close()

async def test_message_concurrency_receipt_and_unknown_get_never_execute(fx):
    body = {'deviceId': DEVICE, 'messageId': str(uuid4()), 'content': '记住我喜欢粤语。'}
    results = await asyncio.gather(*(fx.conversations.message(fx.conversation['id'], fx.user['id'], body, {'dispatchHint': 'router'}) for _ in range(8)))
    assert len({r['receipt']['sequence'] for r in results}) == 1
    assert results[0]['receipt']['dispatchHint'] == 'router'
    replay = await fx.conversations.receipt(fx.conversation['id'], fx.user['id'], body['messageId'])
    assert replay['receipt'] == results[0]['receipt']
    assert len(replay['conversation']['messages']) == 1
    with pytest.raises(ApiError, match='消息尚无接收回执'):
        await fx.conversations.receipt(fx.conversation['id'], fx.user['id'], str(uuid4()))
    with pytest.raises(ApiError) as conflict:
        await fx.conversations.message(fx.conversation['id'], fx.user['id'], {**body, 'content': '不同正文。'})
    assert conflict.value.code == 'IDEMPOTENCY_CONFLICT'
    assert fx.calls == [] and await used(fx) == 0

async def test_direct_help_is_free_server_reply_and_unspoofable(fx):
    body, _ = await fx.submit('你能做什么？')
    result = await fx.route(body)
    assert result['route']['status'] == 'completed'
    assert result['route']['result']['planner'] == 'deterministic'
    assert result['conversation']['messages'][-1]['source'] == 'route'
    assert fx.calls == [] and await used(fx) == 0
    with pytest.raises(ApiError):
        await fx.conversations.event(fx.conversation['id'], fx.user['id'], {'deviceId': DEVICE, 'eventId': str(uuid4()), 'turnId': body['messageId'], 'kind': 'route_reply', 'content': '伪造'}, fx.runtime)

async def test_single_route_replay_reserves_once_and_does_not_create_task(fx):
    body, _ = await fx.submit()
    first = await fx.route(body)
    second = await fx.route(body)
    observed = await fx.router.get(fx.conversation['id'], body['messageId'], fx.user['id'])
    assert first['route'] == second['route'] == observed['route']
    assert first['route']['result']['strategy'] == 'single_agent'
    assert len(fx.calls) == 1 and await used(fx) == 1
    assert (await fx.store.db.fetchone('SELECT count(*) AS n FROM music_tasks'))['n'] == 0
    assert fx.runtime.inflight == 0 and not fx.runtime.accountCalls

async def test_same_message_concurrent_routes_share_running_attempt(fx):
    entered, finish = asyncio.Event(), asyncio.Event()
    async def slow(input, options=None):
        fx.calls.append(clone(input)); entered.set(); await finish.wait()
        return {'value': music(input)}
    fx.router.model.route = slow
    body, _ = await fx.submit()
    first = asyncio.create_task(fx.route(body))
    await asyncio.wait_for(entered.wait(), 1)
    try:
        duplicate = await asyncio.gather(*(fx.route(body) for _ in range(6)))
        assert all(r['route']['status'] == 'running' for r in duplicate)
        assert len(fx.calls) == 1
    finally:
        finish.set()
    assert (await first)['route']['status'] == 'completed'
    assert await used(fx) == 1 and fx.runtime.inflight == 0

async def test_invalid_model_and_failure_are_durable_without_fallback(fx):
    async def broken(input, options=None):
        fx.calls.append(clone(input)); raise ApiError(503, 'MODEL_UNAVAILABLE', '隔离故障注入')
    fx.router.model.route = broken
    body, _ = await fx.submit()
    first = await fx.route(body); second = await fx.route(body)
    assert first['route']['status'] == 'failed'
    assert first['route']['errorCode'] == 'MODEL_UNAVAILABLE'
    assert first['route'] == second['route'] and len(fx.calls) == 1 and await used(fx) == 1

async def test_stale_turn_and_model_finishing_after_new_turn_never_dispatch(fx):
    old, _ = await fx.submit()
    await fx.submit('改成三十分钟爵士。')
    with pytest.raises(ApiError) as error:
        await fx.route(old)
    assert error.value.code == 'CONVERSATION_STALE' and not fx.calls
    entered, finish = asyncio.Event(), asyncio.Event()
    async def slow(input, options=None):
        fx.calls.append(clone(input)); entered.set(); await finish.wait(); return {'value': music(input)}
    fx.router.model.route = slow
    # 保留真正经过模型的自由表达，避免新增确定性场景分支跳过本测试的迟到响应。
    current, _ = await fx.submit('工作时希望舒适又不抢注意力。')
    first = asyncio.create_task(fx.route(current)); await asyncio.wait_for(entered.wait(), 1)
    await fx.submit('这条是最新要求。'); finish.set()
    result = await first
    assert result['route']['status'] == 'failed' and result['route']['errorCode'] == 'CONVERSATION_STALE'
    assert result['route']['result'] is None and await used(fx) == 1

async def test_account_device_and_body_guards_precede_reservation(fx):
    body, _ = await fx.submit()
    for action in [lambda: fx.router.get(fx.conversation['id'], body['messageId'], fx.other['id']),
                   lambda: fx.router.route(fx.conversation['id'], body['messageId'], fx.other['id'], {'deviceId': DEVICE, 'preference': ''}),
                   lambda: fx.route(body, deviceId='wrong_device_fixture'), lambda: fx.route(body, taskId=str(uuid4()))]:
        with pytest.raises(ApiError):
            await action()
    assert len(fx.calls) == 0 and await used(fx) == 0

@pytest.mark.parametrize('policy,code', [({'dailyLimit': 0, 'totalLimit': 300, 'enabled': True}, 'AI_DAILY_LIMIT'),
    ({'dailyLimit': 30, 'totalLimit': 0, 'enabled': True}, 'AI_TOTAL_LIMIT'),
    ({'dailyLimit': 30, 'totalLimit': 300, 'enabled': False}, 'AI_ACCOUNT_DISABLED')])
async def test_zero_or_disabled_policy_does_not_call_or_charge(fx, policy, code):
    body, _ = await fx.submit()
    await fx.store.accountPolicy.adminset(fx.user['id'], policy, {'now': NOW})
    with pytest.raises(ApiError) as error:
        await fx.route(body)
    assert error.value.code == code
    assert fx.calls == [] and await used(fx) == 0 and not fx.runtime.accountCalls
    assert (await fx.conversations.get(fx.conversation['id'], fx.user['id']))['messages'][0]['id'] == body['messageId']

async def test_device_outbox_and_task_sync_are_stable_and_server_grounded(fx):
    body, _ = await fx.submit()
    event = {'deviceId': DEVICE, 'eventId': str(uuid4()), 'turnId': body['messageId'], 'kind': 'local_reply', 'content': '请确认后保存偏好。'}
    first = await fx.conversations.event(fx.conversation['id'], fx.user['id'], event, fx.runtime)
    assert await fx.conversations.event(fx.conversation['id'], fx.user['id'], event, fx.runtime) == first
    task = {'id': str(uuid4()), 'userId': fx.user['id'], 'deviceId': DEVICE, 'revision': 1, 'stateVersion': 1, 'status': 'preview',
            'createdAt': NOW, 'updatedAt': NOW, 'messages': [
                {'messageId': str(uuid4()), 'role': 'user', 'content': body['content'], 'revision': 1},
                {'messageId': str(uuid4()), 'role': 'assistant', 'content': '服务端真实任务回复', 'revision': 1}]}
    await fx.runtime.store.save(task)
    sync = {'deviceId': DEVICE, 'eventId': str(uuid4()), 'turnId': body['messageId'], 'kind': 'task_sync', 'taskId': task['id'], 'taskRevision': 1, 'content': '不能相信客户端这段内容'}
    imported = await fx.conversations.event(fx.conversation['id'], fx.user['id'], sync, fx.runtime)
    assert imported['conversation']['messages'][-1]['content'] == '服务端真实任务回复'
    assert imported['conversation']['messages'][-1]['source'] == 'task'
    again = await fx.conversations.event(fx.conversation['id'], fx.user['id'], {**sync, 'eventId': str(uuid4())}, fx.runtime)
    assert len(again['conversation']['messages']) == len(imported['conversation']['messages'])

async def test_read_only_recovery_proves_legacy_request_hash_without_rewriting_journal(fx):
    body, _ = await fx.submit()
    task = {'id': str(uuid4()), 'userId': fx.user['id'], 'deviceId': DEVICE, 'revision': 1, 'stateVersion': 1, 'status': 'preview',
            'createdAt': NOW, 'updatedAt': NOW, 'preference': '', 'excludedIds': [],
            'messages': [{'messageId': str(uuid4()), 'role': 'user', 'content': body['content'], 'revision': 1}]}
    raw = {'deviceId': DEVICE, 'requestId': body['messageId'], 'instruction': body['content'], 'preference': '', 'excludedIds': []}
    await fx.runtime.store.commit(task, body['messageId'], raw, {'expectedStateVersion': 0})
    before = (await fx.store.db.fetchone('SELECT journal_json FROM music_conversations WHERE id=?', [fx.conversation['id']]))['journal_json']
    recovered = await fx.conversations.get(fx.conversation['id'], fx.user['id'])
    assert recovered['latestTaskLink'] == {'taskId': task['id'], 'turnId': body['messageId'], 'revision': 1}
    assert (await fx.store.db.fetchone('SELECT journal_json FROM music_conversations WHERE id=?', [fx.conversation['id']]))['journal_json'] == before
    assert fx.calls == [] and await used(fx) == 0

async def test_utf16_content_limit_and_pagination_preserve_order(fx):
    await fx.submit('😀' * 500)
    for bad in ['😀' * 501, '\ud800', '', '  ']:
        with pytest.raises(ApiError):
            await fx.submit(bad)
    for n in range(45):
        await fx.submit('离线消息' + str(n))
    latest = await fx.conversations.get(fx.conversation['id'], fx.user['id'])
    assert len(latest['messages']) == 40 and latest['hasEarlier']
    earlier = await fx.conversations.get(fx.conversation['id'], fx.user['id'], latest['earliestSequence'])
    assert [m['sequence'] for m in earlier['messages']] == list(range(1, 7))
    assert fx.calls == []

async def test_expired_route_remains_uncertain_after_restart_without_reserving_again(fx):
    body, _ = await fx.submit()
    context = await fx.conversations.routeContext(fx.conversation['id'], fx.user['id'], DEVICE, body['messageId'])
    input = {k: context[k] for k in ['conversationId', 'messageId', 'deviceId', 'instruction', 'history', 'task']}; input['preference'] = ''
    request = {k: input[k] for k in ['conversationId', 'messageId', 'deviceId']}
    request.update(userId=fx.user['id'], input=input, inputHash=digest(input), capabilityVersion=1)
    repository = ConversationRouteStore(store=fx.store, taskStore=fx.runtime.store, now=lambda: NOW, leaseMs=10)
    reserved = await repository.begin(request)
    reopened = ConversationRouteStore(store=fx.store, taskStore=fx.runtime.store, now=lambda: NOW + 11, leaseMs=10)
    result = await reopened.get(request)
    assert result['status'] == 'uncertain' and result['errorCode'] == 'ROUTE_LEASE_EXPIRED'
    replay = await reopened.begin(request)
    assert not replay['acquired'] and await used(fx) == 1
    assert not fx.calls

async def test_route_context_bounds_legacy_source_name_like_js(fx):
    body, _ = await fx.submit()
    task = {'id': str(uuid4()), 'userId': fx.user['id'], 'deviceId': DEVICE, 'revision': 1, 'stateVersion': 1, 'status': 'preview',
            'createdAt': NOW, 'updatedAt': NOW, 'preference': '', 'excludedIds': [], 'sources': [{'name': 'a' * 250}, {'name': 123}],
            'messages': [{'messageId': str(uuid4()), 'role': 'user', 'content': body['content'], 'revision': 1}]}
    raw = {'deviceId': DEVICE, 'requestId': body['messageId'], 'instruction': body['content'], 'preference': '', 'excludedIds': []}
    await fx.runtime.store.commit(task, body['messageId'], raw, {'expectedStateVersion': 0})
    context = await fx.conversations.routeContext(fx.conversation['id'], fx.user['id'], DEVICE, body['messageId'])
    assert context['task']['sources'] == [{'name': 'a' * 200}, {'name': ''}]
    assert fx.calls == []
