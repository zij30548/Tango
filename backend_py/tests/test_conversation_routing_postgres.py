"""只接受显式隔离 PostgreSQL 地址；验证跨实例锁、槽位和旧账本语义。"""
import asyncio
import os
from uuid import uuid4
import pytest
from tango_backend.store import PostgresStore, validateCredentials
from tango_backend.task_store import TaskStore
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.conversation_store import ConversationStore
from tango_backend.conversation_route_store import ConversationRouteStore
from tango_backend.compat import digest
from tango_backend.errors import ApiError

URL = os.environ.get('TANGO_TEST_DATABASE_URL') or os.environ.get('TANGO_PYTHON_CONVERSATION_PG_URL')
pytestmark = pytest.mark.skipif(not URL, reason='需要本次测试独立持有的 PostgreSQL，不能读取本机用户库')
NOW, DEVICE = 1792483200000, 'python_pg_conversation_device'

@pytest.fixture
async def pg():
    import asyncpg
    admin = await asyncpg.connect(URL)
    schema = 'tango_conv_' + uuid4().hex
    await admin.execute('CREATE SCHEMA ' + schema)
    stores = [PostgresStore(URL, serverSettings={'search_path': schema}), PostgresStore(URL, serverSettings={'search_path': schema})]
    for store in stores:
        store.accountPolicy = AccountPolicyStore(store)
    tasks = [TaskStore(store) for store in stores]
    conversations = [ConversationStore(store, now=lambda: NOW) for store in stores]
    clock = {'now': NOW}
    repos = [ConversationRouteStore(store=store, taskStore=task, now=lambda: clock['now'], leaseMs=1000, dailyLimit=100, globalDailyLimit=1000) for store, task in zip(stores, tasks)]
    await asyncio.gather(*(repo.initialize() for repo in repos))
    user = await stores[0].register(validateCredentials({'username': 'owned-pg-user', 'password': 'isolated-pg-fixture-password'}), NOW)
    yield stores, tasks, conversations, repos, user, clock
    for store in stores:
        await store.close()
    await admin.execute('DROP SCHEMA ' + schema + ' CASCADE'); await admin.close()

async def request(pg, text='读书听纯音乐。'):
    stores, tasks, convs, repos, user, clock = pg
    conv = await convs[0].create(user['id'], {'deviceId': DEVICE, 'requestId': str(uuid4())})
    body = {'deviceId': DEVICE, 'messageId': str(uuid4()), 'content': text}
    await convs[0].message(conv['id'], user['id'], body, {'dispatchHint': 'router'})
    context = await convs[0].routeContext(conv['id'], user['id'], DEVICE, body['messageId'])
    input = {k: context[k] for k in ['conversationId', 'messageId', 'deviceId', 'instruction', 'history', 'task']}; input['preference'] = ''
    return {'conversationId': conv['id'], 'messageId': body['messageId'], 'deviceId': DEVICE, 'userId': user['id'], 'input': input, 'inputHash': digest(input), 'capabilityVersion': 1}

async def test_pg_concurrent_same_turn_reserves_one_attempt_and_shared_model_slot(pg):
    stores, tasks, convs, repos, user, clock = pg
    req = await request(pg)
    results = await asyncio.gather(*(repos[n % 2].begin(req) for n in range(12)))
    assert sum(result['acquired'] for result in results) == 1
    assert len({r['route']['id'] for r in results}) == 1
    assert (await stores[0].accountPolicy.snapshot(user['id'], NOW))['total']['used'] == 1
    assert (await stores[0].db.fetchone('SELECT count(*) AS n FROM music_task_model_slots'))['n'] == 1
    acquired = next(r for r in results if r['acquired'])
    completed = await repos[0].finish(acquired['claim'], status='completed', result={'fixture': True})
    assert completed['status'] == 'completed'
    assert (await stores[0].db.fetchone('SELECT count(*) AS n FROM music_task_model_slots'))['n'] == 0

async def test_pg_account_slot_conflict_rolls_back_route_and_quota(pg):
    stores, tasks, convs, repos, user, clock = pg
    first, second = await request(pg, '第一项音乐需求。'), await request(pg, '第二项音乐需求。')
    acquired = await repos[0].begin(first)
    with pytest.raises(ApiError) as error:
        await repos[1].begin(second)
    assert error.value.code == 'AI_BUSY'
    assert await repos[1].get(second) is None
    assert (await stores[0].accountPolicy.snapshot(user['id'], NOW))['total']['used'] == 1
    await repos[0].finish(acquired['claim'], status='failed', errorCode='MODEL_UNAVAILABLE')
    accepted = await repos[1].begin(second)
    assert accepted['acquired']
    await repos[1].finish(accepted['claim'], status='uncertain', errorCode='MODEL_TIMEOUT')
    assert (await stores[0].accountPolicy.snapshot(user['id'], NOW))['total']['used'] == 2

async def test_pg_failed_and_unknown_attempts_keep_total_across_days_and_restarts(pg):
    stores, tasks, convs, repos, user, clock = pg
    await stores[0].accountPolicy.adminset(user['id'], {'dailyLimit': 30, 'totalLimit': 2, 'enabled': True}, {'now': NOW})
    for index, status in enumerate(['failed', 'uncertain']):
        req = await request(pg)
        acquired = await repos[index].begin(req)
        await repos[index].finish(acquired['claim'], status=status, errorCode='MODEL_TIMEOUT')
        clock['now'] += 86400000
    req = await request(pg)
    with pytest.raises(ApiError) as error:
        await repos[1].begin(req)
    assert error.value.code == 'AI_TOTAL_LIMIT'
    assert (await stores[1].accountPolicy.snapshot(user['id'], clock['now']))['total']['used'] == 2
    assert await repos[1].get(req) is None

async def test_pg_expiry_and_latest_turn_checks_fence_late_completion(pg):
    stores, tasks, convs, repos, user, clock = pg
    req = await request(pg)
    acquired = await repos[0].begin(req)
    clock['now'] += 1001
    assert (await repos[1].get(req))['status'] == 'uncertain'
    result = await repos[0].finish(acquired['claim'], status='completed', result={'mustNotCommit': True})
    assert result['status'] == 'uncertain' and result['result'] is None
    assert not (await repos[1].begin(req))['acquired']
    fresh = await request(pg)
    active = await repos[0].begin(fresh)
    await convs[1].message(fresh['conversationId'], user['id'], {'deviceId': DEVICE, 'messageId': str(uuid4()), 'content': '这条才是最新需求。'})
    result = await repos[0].finish(active['claim'], status='completed', result={'mustNotCommit': True})
    assert result['status'] == 'failed' and result['errorCode'] == 'CONVERSATION_STALE' and result['result'] is None
