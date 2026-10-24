"""同一组路由契约在真实 MySQL 上运行，独立连接验证数据库锁而非进程内互斥。"""

import asyncio
from uuid import uuid4
import pytest
from helpers.mysql_fixture import mysql_database
from tango_backend.store import MysqlStore, validateCredentials
from tango_backend.task_store import TaskStore
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.conversation_store import ConversationStore
from tango_backend.conversation_route_store import ConversationRouteStore
import test_conversation_routing_postgres as contract


@pytest.fixture
async def mysql():
    async with mysql_database() as url:
        stores = [MysqlStore(url), MysqlStore(url)]
        for store in stores:
            store.accountPolicy = AccountPolicyStore(store)
        tasks = [TaskStore(store) for store in stores]
        conversations = [ConversationStore(store, now=lambda: contract.NOW) for store in stores]
        clock = {"now": contract.NOW}
        repos = [ConversationRouteStore(store=store, taskStore=task, now=lambda: clock["now"],
                                       leaseMs=1000, dailyLimit=100, globalDailyLimit=1000)
                 for store, task in zip(stores, tasks)]
        try:
            await asyncio.gather(*(repo.initialize() for repo in repos))
            user = await stores[0].register(validateCredentials({"username": "mysql-route-user", "password": "isolated-fixture-password"}), contract.NOW)
            yield stores, tasks, conversations, repos, user, clock
        finally:
            for store in stores:
                await store.close()


async def test_mysql_concurrent_same_turn_reserves_once(mysql):
    await contract.test_pg_concurrent_same_turn_reserves_one_attempt_and_shared_model_slot(mysql)


async def test_mysql_account_slot_conflict_rolls_back(mysql):
    await contract.test_pg_account_slot_conflict_rolls_back_route_and_quota(mysql)


async def test_mysql_failed_unknown_keep_lifetime_usage(mysql):
    await contract.test_pg_failed_and_unknown_attempts_keep_total_across_days_and_restarts(mysql)


async def test_mysql_expiry_and_latest_turn_fence_completion(mysql):
    await contract.test_pg_expiry_and_latest_turn_checks_fence_late_completion(mysql)


async def test_mysql_concurrent_conversation_receipts_and_same_millisecond_heartbeat(mysql):
    stores, tasks, convs, repos, user, clock = mysql
    body = {"deviceId": contract.DEVICE, "requestId": str(uuid4())}
    created = await asyncio.gather(*(convs[i % 2].create(user["id"], body) for i in range(8)))
    assert len({c["id"] for c in created}) == 1
    req = await contract.request(mysql)
    acquired = await repos[0].begin(req)
    assert await repos[0].heartbeat(acquired["claim"])
    assert await repos[1].heartbeat(acquired["claim"])
    await repos[0].finish(acquired["claim"], status="completed", result={"fixture": True})
