"""路由最终守卫与服务端回复必须原子提交；全部使用拥有者临时数据库。"""

import os
from contextlib import asynccontextmanager
from types import SimpleNamespace
from uuid import uuid4

import pytest

from helpers.mysql_fixture import mysql_database
from tango_backend.account_quota import AccountPolicyStore
from tango_backend.conversation_router import ConversationRouter
from tango_backend.conversation_store import ConversationStore
from tango_backend.errors import ApiError
from tango_backend.store import Store, MysqlStore, PostgresStore, validateCredentials
from test_conversation_routing import RuntimeStub

NOW, DEVICE = 1792483200000, "atomic_reply_device"


@asynccontextmanager
async def isolated_store(dialect, tmp_path):
    if dialect == "mysql":
        async with mysql_database() as url:
            store = MysqlStore(url)
            try:
                yield store
            finally:
                await store.close()
    elif dialect == "postgres":
        url = os.environ.get("TANGO_TEST_DATABASE_URL")
        if not url:
            pytest.skip("需要本次拥有者临时 PostgreSQL runner")
        import asyncpg
        admin = await asyncpg.connect(url)
        schema = "atomic_reply_" + uuid4().hex
        await admin.execute("CREATE SCHEMA " + schema)
        store = PostgresStore(url, serverSettings={"search_path": schema})
        try:
            yield store
        finally:
            await store.close()
            await admin.execute("DROP SCHEMA " + schema + " CASCADE")
            await admin.close()
    else:
        store = Store(tmp_path / "atomic-reply.sqlite")
        try:
            yield store
        finally:
            await store.close()


@pytest.fixture(params=["sqlite", "mysql", "postgres"])
async def fx(request, tmp_path):
    async with isolated_store(request.param, tmp_path) as store:
        store.accountPolicy = AccountPolicyStore(store)
        clock = {"now": NOW}
        await store.initialize()
        user = await store.register(validateCredentials({"username": "atomic-reply-user", "password": "owned-fixture-password"}), NOW)
        convs = ConversationStore(store, now=lambda: clock["now"])
        runtime = RuntimeStub(store)
        router = ConversationRouter(store=store, conversations=convs, tasks=runtime, now=lambda: clock["now"], leaseMs=1000)
        await router.initialize()
        conversation = await convs.create(user["id"], {"deviceId": DEVICE, "requestId": str(uuid4())})
        message = {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "你能做什么？"}
        await convs.message(conversation["id"], user["id"], message, dispatchHint="router")
        try:
            yield SimpleNamespace(store=store, clock=clock, user=user["id"], convs=convs, router=router, cid=conversation["id"], message=message)
        finally:
            await router.close()


async def route(fx):
    return await fx.router.route(fx.cid, fx.message["messageId"], fx.user, {"deviceId": DEVICE, "preference": ""})


async def no_replies(fx):
    journal = await fx.convs.get(fx.cid, fx.user)
    assert not [message for message in journal["messages"] if message["role"] == "assistant"]


async def test_lease_expiring_before_finish_does_not_persist_reply(fx):
    original = fx.router.repository.finish
    async def expire(claim, *args, **kwargs):
        fx.clock["now"] += 1001
        return await original(claim, *args, **kwargs)
    fx.router.repository.finish = expire
    response = await route(fx)
    assert response["route"]["status"] == "uncertain"
    assert response["route"]["result"] is None
    await no_replies(fx)


async def test_new_message_before_finish_does_not_persist_old_reply(fx):
    original = fx.router.repository.finish
    async def insert_new_message(claim, *args, **kwargs):
        await fx.convs.message(fx.cid, fx.user, {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "先别解释，换一个新需求。"})
        return await original(claim, *args, **kwargs)
    fx.router.repository.finish = insert_new_message
    response = await route(fx)
    assert response["route"]["status"] == "failed" and response["route"]["errorCode"] == "CONVERSATION_STALE"
    await no_replies(fx)


async def test_reply_failure_rolls_back_written_reply_and_route_completion(fx):
    original = fx.convs.appendRouteReply
    async def fail_after_write(*args, **kwargs):
        await original(*args, **kwargs)
        raise ApiError(503, "FIXTURE_REPLY_FAILURE", "仅用于验证同一事务回滚。")
    fx.convs.appendRouteReply = fail_after_write
    response = await route(fx)
    assert response["route"]["status"] == "failed" and response["route"]["errorCode"] == "FIXTURE_REPLY_FAILURE"
    await no_replies(fx)
    row = await fx.store.db.fetchone("SELECT status FROM music_conversation_route_attempts")
    assert row["status"] == "failed"
    # 失败尝试仍有账本，重复观察不能补写回复或重跑路由。
    restored = await fx.router.get(fx.cid, fx.message["messageId"], fx.user)
    assert restored["route"] == response["route"]
