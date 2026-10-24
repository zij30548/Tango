"""MySQL+Redis+FastAPI 集成；复用客户端协议验收，模型/曲库均为夹具。"""

import os
from types import SimpleNamespace
from uuid import uuid4
import httpx
import pytest
from redis.asyncio import Redis
from helpers.mysql_fixture import mysql_database
from tango_backend.app import createApp
from tango_backend.store import MysqlStore
from tango_backend.redis_limiter import RedisRateLimiter
from tango_backend.task_progress import TaskProgressHub
import test_app as contract


@pytest.fixture
async def fx():
    async with mysql_database() as url:
        redis = Redis.from_url(os.environ["TANGO_TEST_REDIS_URL"], decode_responses=True)
        store = MysqlStore(url)
        model = contract.TaskModel()
        namespace = "tango:mysql_api:" + uuid4().hex
        app = createApp(store=store, model=model, taskModel=model,
                        routingModel=SimpleNamespace(configured=False, model="disabled"),
                        redis=redis, rateLimiter=RedisRateLimiter(client=redis, namespace=namespace),
                        progressHub=TaskProgressHub(client=redis, namespace=namespace),
                        registrationMode="open", aiPerMinute=100, authPerTenMinutes=100)
        try:
            async with app.router.lifespan_context(app):
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app, client=("127.0.0.1", 32123)), base_url="http://127.0.0.1:32123") as client:
                    r = await client.post("/v1/auth/register", json={"username": "test-user", "password": "fixture-private-password"})
                    assert r.status_code == 201, r.text
                    session = r.json()
                    client.headers["authorization"] = "Bearer " + session["token"]
                    yield SimpleNamespace(client=client, app=app, store=store, model=model, session=session)
        finally:
            await store.close()
            await redis.aclose()


async def test_mysql_health_and_client_protocol(fx):
    await contract.test_health_and_original_client_protocol(fx)
    assert (await fx.client.get("/health")).json()["storage"] == "mysql"
    assert (await fx.client.get("/ready")).json()["ready"] is True


async def test_mysql_receipts_routes_account_isolation(fx):
    await contract.test_conversation_receipts_route_restore_and_account_isolation(fx)


async def test_mysql_full_task_preview_sse_adoption_and_restore(fx):
    await contract.test_full_task_tools_preview_sse_adoption_and_restart_restore(fx)


async def test_mysql_logout_invalidates_and_pauses(fx):
    await contract.test_logout_invalidates_session_and_pauses_task(fx)


async def test_mysql_clear_http_auth_idempotency_and_reopen(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_clear_http_auth_idempotency_and_reopen(fx)


async def test_mysql_clear_cancels_linked_waiting_task(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_clear_cancels_linked_waiting_task_and_blocks_resume_adoption(fx)


async def test_mysql_clear_rejects_late_create(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_late_task_create_after_clear_never_calls_model(fx)


async def test_mysql_clear_fences_context_checkpoint_race(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_clear_between_task_context_and_first_checkpoint_is_fenced(fx)


async def test_mysql_clear_cancel_failure_rolls_back(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_clear_cancellation_failure_rolls_back_chat_routes_and_receipt(fx)


async def test_mysql_clear_pending_adoption_ack(fx):
    import test_chat_clear as clear_contract
    await clear_contract.test_clear_keeps_pending_adoption_for_original_ack_only(fx)
