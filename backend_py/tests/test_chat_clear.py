"""聊天清除保留审计和幂等边界；仅使用拥有者临时数据库与固定模型。"""

import asyncio
import json
from types import SimpleNamespace
from uuid import uuid4

import pytest

from tango_backend.account_quota import AccountPolicyStore
from tango_backend.compat import digest
from tango_backend.conversation_store import ConversationStore
from tango_backend.conversation_router import ConversationRouter
from tango_backend.errors import ApiError
from tango_backend.store import validateCredentials
from tango_backend.store import MysqlStore
from test_conversation_routing import RuntimeStub
from test_route_reply_atomicity import isolated_store
from test_app import fx as http_fx


NOW, DEVICE = 1792843200000, "chat_clear_fixture_device"


@pytest.fixture(params=["sqlite", "mysql"])
async def clear_fx(request, tmp_path):
    async with isolated_store(request.param, tmp_path) as store:
        store.accountPolicy = AccountPolicyStore(store)
        await store.initialize()
        users = [await store.register(validateCredentials({"username": name, "password": "owned-clear-fixture"}), NOW) for name in ("clear-owner", "clear-other")]
        convs, runtime = ConversationStore(store, now=lambda: NOW), RuntimeStub(store)
        router = ConversationRouter(store=store, conversations=convs, tasks=runtime, now=lambda: NOW)
        await router.initialize()
        try:
            yield SimpleNamespace(store=store, convs=convs, router=router, runtime=runtime, user=users[0]["id"], other=users[1]["id"])
        finally:
            await router.close()


async def conversation(fx, user=None, device=DEVICE):
    body = {"deviceId": device, "requestId": str(uuid4())}
    return await fx.convs.create(user or fx.user, body), body


async def anchor(fx, conv, content="你能做什么？"):
    body = {"deviceId": conv["deviceId"], "messageId": str(uuid4()), "content": content}
    await fx.convs.message(conv["id"], fx.user, body, dispatchHint="router")
    return body


async def test_clear_all_devices_account_isolation_raw_scrub_and_ledger_kept(clear_fx):
    fx = clear_fx
    own, _ = await conversation(fx)
    second, _ = await conversation(fx, device="chat_clear_second_device")
    other, _ = await conversation(fx, user=fx.other)
    message = await anchor(fx, own)
    routed = await fx.router.route(own["id"], message["messageId"], fx.user, {"deviceId": DEVICE, "preference": ""})
    attempts = await fx.store.db.fetchall("SELECT * FROM music_conversation_route_attempts")
    usage = await fx.store.accountPolicy.snapshot(fx.user, NOW)
    receipt = await fx.convs.clearAll(fx.user, {"requestId": str(uuid4())})
    assert receipt["cleared"] is True and receipt["conversationCount"] == 2
    assert await fx.convs.latest(fx.user, DEVICE) is None
    assert await fx.convs.latest(fx.user, second["deviceId"]) is None
    assert (await fx.convs.get(other["id"], fx.other))["id"] == other["id"]
    row = await fx.store.db.fetchone("SELECT journal_json FROM music_conversations WHERE id=?", (own["id"],))
    assert "你能做什么" not in str(row["journal_json"])
    route = await fx.store.db.fetchone("SELECT input_json,result_json FROM music_conversation_routes WHERE id=?", (routed["route"]["id"],))
    assert json.loads(route["input_json"]) == {} if isinstance(route["input_json"], str) else route["input_json"] == {}
    assert route["result_json"] is None
    assert await fx.store.db.fetchall("SELECT * FROM music_conversation_route_attempts") == attempts
    assert await fx.store.accountPolicy.snapshot(fx.user, NOW) == usage


async def test_clear_replay_never_deletes_new_conversation(clear_fx):
    fx = clear_fx
    old, old_body = await conversation(fx)
    req = {"requestId": str(uuid4())}
    first = await fx.convs.clearAll(fx.user, req)
    new, _ = await conversation(fx)
    await anchor(fx, new)
    assert await fx.convs.clearAll(fx.user, req) == first
    assert (await fx.convs.latest(fx.user, DEVICE))["id"] == new["id"]
    with pytest.raises(ApiError) as caught:
        await fx.convs.create(fx.user, old_body)
    assert caught.value.code == "CONVERSATION_NOT_FOUND"
    another = await fx.convs.clearAll(fx.user, {"requestId": str(uuid4())})
    assert another["conversationCount"] == 1 and another["cancelledTaskCount"] == 0


async def test_clear_blocks_old_receipts_routes_messages_and_late_reply(clear_fx):
    fx = clear_fx
    conv, _ = await conversation(fx)
    msg = await anchor(fx, conv)
    await fx.convs.clearAll(fx.user, {"requestId": str(uuid4())})
    operations = [fx.convs.get(conv["id"], fx.user), fx.convs.receipt(conv["id"], fx.user, msg["messageId"]), fx.convs.message(conv["id"], fx.user, {**msg, "messageId": str(uuid4())}), fx.router.get(conv["id"], msg["messageId"], fx.user), fx.convs.appendRouteReply(conv["id"], fx.user, DEVICE, {"turnId": msg["messageId"], "replyId": str(uuid4()), "content": "迟到回复不能恢复"})]
    for operation in operations:
        with pytest.raises(ApiError) as caught:
            await operation
        assert caught.value.code == "CONVERSATION_NOT_FOUND"
    assert await fx.convs.latest(fx.user, DEVICE) is None


async def test_late_paid_route_only_updates_cost_fact_not_deleted_content(clear_fx):
    fx = clear_fx
    conv, _ = await conversation(fx)
    msg = await anchor(fx, conv, "不应留在墓碑中的私人正文")
    context = await fx.convs.routeContext(conv["id"], fx.user, DEVICE, msg["messageId"])
    value = {"conversationId": conv["id"], "messageId": msg["messageId"], "deviceId": DEVICE, "instruction": msg["content"], "history": [], "task": None, "preference": ""}
    request = {"conversationId": conv["id"], "messageId": msg["messageId"], "deviceId": DEVICE, "userId": fx.user, "input": value, "inputHash": digest(value), "capabilityVersion": 3}
    acquired = await fx.router.repository.begin(request, paid=True)
    await fx.convs.clearAll(fx.user, {"requestId": str(uuid4())})
    outcome = await fx.router.repository.finish(acquired["claim"], status="completed", result={"actions": [{"capability": "assistant.reply", "id": str(uuid4()), "parameters": {"text": "不能写回的晚回复"}}]}, usage={"inputTokens": 9, "outputTokens": 4})
    assert outcome["errorCode"] == "CONVERSATION_CLEARED" and outcome["result"] is None
    attempt = await fx.store.db.fetchone("SELECT * FROM music_conversation_route_attempts WHERE id=?", (acquired["claim"]["attemptId"],))
    assert attempt["input_tokens"] == 9 and attempt["output_tokens"] == 4
    assert (await fx.store.accountPolicy.snapshot(fx.user, NOW))["total"]["used"] == 1
    assert not fx.router.repository.localClaims
    assert await fx.convs.latest(fx.user, DEVICE) is None


async def test_clear_rejects_unbounded_or_extra_scope(clear_fx):
    for body in ({}, {"requestId": "x"}, {"requestId": str(uuid4()), "userId": clear_fx.other}, {"requestId": str(uuid4()), "deviceId": DEVICE}):
        with pytest.raises(ApiError) as caught:
            await clear_fx.convs.clearAll(clear_fx.user, body)
        assert caught.value.status == 400


async def test_mysql_other_connection_create_waits_for_clear_commit(clear_fx):
    fx = clear_fx
    if fx.store.dialect != "mysql":
        pytest.skip("此断言需要两个独立 MySQL 连接")
    other_store = MysqlStore(fx.store.db.connectionString)
    other_convs = ConversationStore(other_store, now=lambda: NOW)
    await other_convs.initialize()
    await conversation(fx)
    reached, finish = asyncio.Event(), asyncio.Event()
    original = fx.store.db.execute
    async def pause_clear(sql, *args, **kwargs):
        value = await original(sql, *args, **kwargs)
        if sql.startswith("UPDATE music_conversations SET journal_json=?,cleared_at="):
            reached.set()
            await finish.wait()
        return value
    fx.store.db.execute = pause_clear
    req = {"requestId": str(uuid4())}
    clearing = asyncio.create_task(fx.convs.clearAll(fx.user, req))
    creating = None
    try:
        await asyncio.wait_for(reached.wait(), 2)
        creating = asyncio.create_task(other_convs.create(fx.user, {"deviceId": DEVICE, "requestId": str(uuid4())}))
        await asyncio.sleep(0.05)
        assert not creating.done()
        finish.set()
        result = await clearing
        created = await creating
        assert result["conversationCount"] == 1
        assert await other_convs.clearAll(fx.user, req) == result
        assert (await other_convs.latest(fx.user, DEVICE))["id"] == created["id"]
    finally:
        finish.set()
        await asyncio.gather(clearing, *([creating] if creating else []), return_exceptions=True)
        fx.store.db.execute = original
        await other_store.close()


async def test_clear_http_auth_idempotency_and_reopen(http_fx):
    fx = http_fx
    cid = (await fx.client.post("/v1/conversations", json={"deviceId": DEVICE, "requestId": str(uuid4())})).json()["conversation"]["id"]
    body = {"requestId": str(uuid4())}
    denied = await fx.client.request("DELETE", "/v1/conversations", json=body, headers={"authorization": ""})
    assert denied.status_code == 401
    response = await fx.client.request("DELETE", "/v1/conversations", json=body)
    assert response.status_code == 200, response.text
    assert response.json() == {"cleared": True, "requestId": body["requestId"], "conversationCount": 1, "clearedAt": response.json()["clearedAt"], "cancelledTaskCount": 0}
    assert (await fx.client.get("/v1/conversations/" + cid)).status_code == 404
    assert (await fx.client.get("/v1/conversations/latest/" + DEVICE)).json() == {"conversation": None}
    assert (await fx.client.request("DELETE", "/v1/conversations", json=body)).json() == response.json()


async def routed_scene(fx):
    uid = fx.session["user"]["id"]
    conv = await fx.app.conversations.create(uid, {"deviceId": DEVICE, "requestId": str(uuid4())})
    msg = {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "给我一份读书时听的歌单，20分钟，只做预览。"}
    await fx.app.conversations.message(conv["id"], uid, msg, dispatchHint="router")
    routed = await fx.app.conversationRouter.route(conv["id"], msg["messageId"], uid, {"deviceId": DEVICE, "preference": ""})
    assert routed["route"]["status"] == "completed", routed
    return uid, conv, msg, {"deviceId": DEVICE, "requestId": msg["messageId"], "instruction": msg["content"]}


async def test_clear_cancels_linked_waiting_task_and_blocks_resume_adoption(http_fx):
    fx = http_fx
    uid, conv, msg, raw = await routed_scene(fx)
    task = await fx.app.tasks.create(uid, raw)
    for _ in range(100):
        current = await fx.app.tasks.store.load(task["id"], uid)
        if current["status"] == "waiting_tools":
            break
        await asyncio.sleep(0.01)
    assert current["status"] == "waiting_tools"
    calls_before = await fx.store.db.fetchall("SELECT * FROM music_task_calls ORDER BY id")
    usage_before = await fx.store.accountPolicy.snapshot(uid, fx.app.services.now())
    cleared = await fx.app.conversations.clearAll(uid, {"requestId": str(uuid4())}, fx.app.tasks)
    assert cleared["cancelledTaskCount"] == 1
    cancelled = await fx.app.tasks.store.load(task["id"], uid)
    assert cancelled["status"] == "cancelled" and not cancelled["pendingSteps"]
    assert await fx.store.db.fetchall("SELECT * FROM music_task_calls ORDER BY id") == calls_before
    assert await fx.store.accountPolicy.snapshot(uid, fx.app.services.now()) == usage_before
    for action in ("resume", "adopt"):
        response = await fx.client.post("/v1/tasks/" + task["id"] + "/actions", json={"deviceId": DEVICE, "requestId": str(uuid4()), "action": action, "expectedRevision": cancelled["revision"]})
        assert response.status_code == 409, response.text
        assert response.json()["error"]["code"] == "CONVERSATION_CLEARED", response.text
    assert (await fx.client.get("/v1/tasks/" + task["id"])).status_code == 200


async def test_late_task_create_after_clear_never_calls_model(http_fx):
    fx = http_fx
    uid, conv, msg, raw = await routed_scene(fx)
    await fx.app.conversations.clearAll(uid, {"requestId": str(uuid4())}, fx.app.tasks)
    with pytest.raises(ApiError) as caught:
        await fx.app.tasks.create(uid, raw)
    assert caught.value.code == "CONVERSATION_CLEARED"
    assert fx.model.calls == []
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0


async def test_clear_between_task_context_and_first_checkpoint_is_fenced(http_fx):
    fx = http_fx
    uid, conv, msg, raw = await routed_scene(fx)
    entered, finish = asyncio.Event(), asyncio.Event()
    original = fx.app.tasks.checkpoint
    async def slow(task, node, *args, **kwargs):
        if node == "created":
            entered.set()
            await finish.wait()
        return await original(task, node, *args, **kwargs)
    fx.app.tasks.checkpoint = slow
    pending = asyncio.create_task(fx.app.tasks.create(uid, raw))
    await asyncio.wait_for(entered.wait(), 1)
    await fx.app.conversations.clearAll(uid, {"requestId": str(uuid4())}, fx.app.tasks)
    finish.set()
    with pytest.raises(ApiError) as caught:
        await pending
    assert caught.value.code == "CONVERSATION_CLEARED"
    assert fx.model.calls == []
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0


async def test_clear_cancellation_failure_rolls_back_chat_routes_and_receipt(http_fx):
    fx = http_fx
    uid, conv, msg, raw = await routed_scene(fx)
    task = await fx.app.tasks.create(uid, raw)
    for _ in range(100):
        if (await fx.app.tasks.store.load(task["id"], uid))["status"] == "waiting_tools":
            break
        await asyncio.sleep(0.01)
    before = await fx.app.conversations.get(conv["id"], uid)
    async def fail_cancel(*args, **kwargs):
        raise ApiError(503, "FIXTURE_CANCEL_FAILURE", "固定取消故障")
    fx.app.tasks.action = fail_cancel
    request = {"requestId": str(uuid4())}
    with pytest.raises(ApiError) as caught:
        await fx.app.conversations.clearAll(uid, request, fx.app.tasks)
    assert caught.value.code == "FIXTURE_CANCEL_FAILURE"
    assert await fx.app.conversations.get(conv["id"], uid) == before
    assert await fx.store.db.fetchone("SELECT 1 FROM music_conversation_clears WHERE user_id=? AND request_id=?", (uid, request["requestId"])) is None
    assert (await fx.store.db.fetchone("SELECT result_json FROM music_conversation_routes WHERE conversation_id=?", (conv["id"],)))["result_json"] is not None


async def test_clear_keeps_pending_adoption_for_original_ack_only(http_fx):
    fx = http_fx
    uid, conv, msg, raw = await routed_scene(fx)
    task = await fx.app.tasks.create(uid, raw)
    for _ in range(100):
        current = await fx.app.tasks.store.load(task["id"], uid)
        if current["status"] == "waiting_tools":
            break
        await asyncio.sleep(0.01)
    # 此夹具只准备“已授权、设备回执待到”的持久检查点，不模拟一次新的采用。
    current["status"], current["phase"], current["pendingSteps"] = "awaiting_apply", "done", []
    current["adoption"] = {"id": str(uuid4()), "previewVersion": 1}
    await fx.app.tasks.checkpoint(current, "fixture_waiting_adoption", options={"invalidateLease": True})
    cleared = await fx.app.conversations.clearAll(uid, {"requestId": str(uuid4())}, fx.app.tasks)
    assert cleared["cancelledTaskCount"] == 0
    wrong = {"deviceId": DEVICE, "requestId": str(uuid4()), "action": "applied", "expectedRevision": current["revision"], "adoptionId": str(uuid4())}
    with pytest.raises(ApiError) as caught:
        await fx.app.tasks.action(task["id"], uid, wrong)
    assert caught.value.code == "ADOPTION_CONFLICT"
    ack = {**wrong, "requestId": str(uuid4()), "adoptionId": current["adoption"]["id"]}
    other_response = await fx.client.post("/v1/auth/register", json={"username": "clear-ack-other", "password": "owned-other-ack-fixture"})
    assert other_response.status_code == 201
    calls_before = list(fx.model.calls)
    other_ack = await fx.client.post("/v1/tasks/" + task["id"] + "/actions", json=ack, headers={"authorization": "Bearer " + other_response.json()["token"]})
    assert other_ack.status_code == 404
    assert other_ack.json()["error"]["code"] == "TASK_NOT_FOUND"
    assert (await fx.app.tasks.store.load(task["id"], uid))["status"] == "awaiting_apply"
    result = await fx.app.tasks.action(task["id"], uid, ack)
    assert result["status"] == "adopted"
    assert await fx.app.tasks.action(task["id"], uid, ack) == result
    receipt_count = await fx.store.db.fetchone("SELECT count(*) AS n FROM music_task_requests WHERE user_id=? AND device_id=? AND request_id=?", (uid, DEVICE, ack["requestId"]))
    assert receipt_count["n"] == 1
    assert fx.model.calls == calls_before and result["pendingSteps"] == []
    with pytest.raises(ApiError) as caught:
        await fx.app.tasks.action(task["id"], uid, {"deviceId": DEVICE, "requestId": str(uuid4()), "action": "adopt", "expectedRevision": result["revision"]})
    assert caught.value.code == "CONVERSATION_CLEARED"
    assert await fx.app.conversations.latest(uid, DEVICE) is None
