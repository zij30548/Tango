"""Redis 只作缓存和通知；真实临时 Redis 与独立 SSE 故障夹具验证降级。"""
import asyncio
from contextlib import suppress
import json
import os
from types import SimpleNamespace
from uuid import uuid4
import pytest
from starlette.requests import ClientDisconnect
from tango_backend.compat import clone, iso
from tango_backend.errors import ApiError
from tango_backend.task_tool_cache import RedisTaskToolCache, scopeDigest
from tango_backend.task_progress import TaskProgressHub, TaskProgressConnections, taskProgress

NOW = 1792454400000


@pytest.fixture
async def redis_fixture():
    url = os.environ.get("TANGO_TEST_REDIS_URL") or os.environ.get("TANGO_REDIS_TEST_URL")
    if not url:
        pytest.skip("只接受最终运行器提供的独立临时 Redis")
    from redis.asyncio import Redis
    client = Redis.from_url(url, decode_responses=True)
    namespace = "tango:features_" + uuid4().hex
    await client.ping()
    try:
        yield client, namespace
    finally:
        keys = [key async for key in client.scan_iter(namespace + ":*")]
        if keys:
            await client.delete(*keys)
        await client.aclose()


def scope(**patch):
    return {"userId": "cache_user_fixture", "deviceId": "cache_device_fixture", "catalogContext": "catalog_epoch_fixture",
        "skill": "scene.research", "skillVersion": 1, "input": {"queries": ["阅读"], "excludedIds": [1], "excludeLive": True}, **patch}


def catalog(now=NOW, sourceAge=0):
    return {"tracks": [{"id": 1001, "title": "合成晚风", "artist": "合成艺人", "album": "测试专辑", "durationMs": 180000,
        "sourceId": 77, "sourceKind": "playlist", "sourceName": "阅读", "sourceTags": ["纯音乐"], "playUrl": "must-not-cache"}],
        "sources": [{"id": 77, "name": "阅读", "checkedAt": iso(now - sourceAge), "tags": ["纯音乐"]}],
        "checkedAt": iso(now), "cookie": "must-not-cache", "messages": ["must-not-cache"]}


async def test_cache_exact_scope_hit_and_user_device_epoch_input_isolation(redis_fixture):
    client, namespace = redis_fixture
    cache = RedisTaskToolCache(client=client, namespace=namespace, now=lambda: NOW)
    original = scope()
    assert await cache.put({**original, "result": catalog()}) == {"stored": True}
    hit = await cache.get(original)
    assert hit["cache"]["hit"] and hit["result"]["tracks"][0]["id"] == 1001
    reordered = {**original, "input": {"excludeLive": True, "excludedIds": [1], "queries": ["阅读"]}}
    assert await cache.get(reordered) == hit
    for changed in [scope(userId="other_user"), scope(deviceId="other_device"), scope(catalogContext="new_epoch"),
        scope(input={**original["input"], "excludedIds": [1001]})]:
        assert await cache.get(changed) is None
    raw = await client.get(namespace + ":toolcache:v1:" + scopeDigest(original))
    assert "must-not-cache" not in raw and "playUrl" not in raw and "cookie" not in raw
    assert original["userId"] not in raw and original["deviceId"] not in raw


async def test_cache_ttl_is_bounded_by_original_source_time_and_never_refreshes_evidence(redis_fixture):
    client, namespace = redis_fixture
    clock = {"now": NOW}
    cache = RedisTaskToolCache(client=client, namespace=namespace, now=lambda: clock["now"])
    result = catalog(sourceAge=299000)
    assert (await cache.put({**scope(), "result": result}))["stored"]
    key = namespace + ":toolcache:v1:" + scopeDigest(scope())
    assert 0 < await client.pttl(key) <= 1000
    hit = await cache.get(scope())
    assert hit["cache"]["expiresAt"] == NOW + 1000
    assert hit["result"]["checkedAt"] == result["checkedAt"]
    assert hit["result"]["sources"][0]["checkedAt"] == result["sources"][0]["checkedAt"]
    clock["now"] += 1001
    assert await cache.get(scope()) is None


async def test_cache_rejects_releases_errors_overflow_and_boolean_version(redis_fixture):
    client, namespace = redis_fixture
    cache = RedisTaskToolCache(client=client, namespace=namespace, now=lambda: NOW)
    for value in [scope(skill="releases.research"), scope(skillVersion=True), scope(input={"large": "x" * (128 * 1024 + 1)})]:
        assert await cache.put({**value, "result": catalog()}) == {"stored": False}
    for result in [{**catalog(), "error": {"code": "FAILED"}}, {**catalog(), "question": "尚需确认"},
        {**catalog(), "tracks": [*catalog()["tracks"]] * 41}]:
        assert await cache.put({**scope(), "result": result}) == {"stored": False}
    tiny = RedisTaskToolCache(client=client, namespace=namespace, now=lambda: NOW, maxBytes=128)
    assert await tiny.put({**scope(), "result": catalog()}) == {"stored": False}
    assert not [key async for key in client.scan_iter(namespace + ":*")]
    assert (await cache.put({**scope(), "result": catalog()}))["stored"]
    key = namespace + ":toolcache:v1:" + scopeDigest(scope())
    envelope = json.loads(await client.get(key))
    envelope["version"] = True
    await client.set(key, json.dumps(envelope), px=1000)
    assert await cache.get(scope()) is None


async def test_cache_redis_failure_and_timeout_are_misses():
    class Broken:
        async def get(self, key):
            raise ConnectionError("synthetic Redis failure")
        async def set(self, *args, **kwargs):
            raise ConnectionError("synthetic Redis failure")
    cache = RedisTaskToolCache(client=Broken(), namespace="tango:cache_fault", now=lambda: NOW)
    assert await cache.get(scope()) is None
    assert await cache.put({**scope(), "result": catalog()}) == {"stored": False}
    class Stalled:
        async def get(self, key):
            await asyncio.Event().wait()
        async def set(self, *args, **kwargs):
            await asyncio.Event().wait()
    cache = RedisTaskToolCache(client=Stalled(), namespace="tango:cache_stalled", now=lambda: NOW, commandTimeoutMs=10)
    assert await asyncio.wait_for(cache.get(scope()), .2) is None
    assert await asyncio.wait_for(cache.put({**scope(), "result": catalog()}), .2) == {"stored": False}


def full_task(version=7):
    return {"id": "progress_task_fixture", "userId": "progress_user_fixture", "revision": 2, "stateVersion": version,
        "status": "researching", "phase": "observe", "updatedAt": NOW, "instruction": "must-not-publish",
        "messages": [{"content": "must-not-publish"}], "preference": "must-not-publish", "pendingSteps": [{"body": "must-not-publish"}]}


async def test_progress_real_redis_stream_contains_version_only_and_has_atomic_ttl(redis_fixture):
    client, namespace = redis_fixture
    hub = TaskProgressHub(client=client, namespace=namespace)
    seen = []
    unsubscribe = hub.subscribe("progress_user_fixture", "progress_task_fixture", lambda: seen.append("changed"))
    hub.publish(full_task())
    await asyncio.gather(*tuple(hub.pending))
    key = hub.key("progress_user_fixture", "progress_task_fixture")
    values = await client.xrange(key)
    assert len(values) == 1 and values[0][1] == {"version": "7"}
    assert 3590 <= await client.ttl(key) <= 3600
    assert "must-not-publish" not in json.dumps(values) and "progress_user_fixture" not in key
    note = await hub.read("progress_user_fixture", "progress_task_fixture")
    assert note == {"cursor": values[0][0], "changed": True, "delivery": "redis"}
    assert not (await hub.read("progress_user_fixture", "progress_task_fixture", note["cursor"]))["changed"]
    assert not (await hub.read("another_user", "progress_task_fixture"))["changed"]
    assert seen == ["changed"]
    assert set(taskProgress(full_task())) == {"taskId", "revision", "stateVersion", "status", "stage", "message", "at"}
    unsubscribe()
    await hub.close()


async def test_progress_redis_failure_falls_back_to_authoritative_database():
    class Broken:
        async def xread(self, *args, **kwargs):
            raise ConnectionError("synthetic failure")
        async def eval(self, *args, **kwargs):
            raise ConnectionError("synthetic failure")
    hub = TaskProgressHub(client=Broken(), namespace="tango:progress_fault", timeoutMs=10)
    hub.publish(full_task())
    await asyncio.gather(*tuple(hub.pending))
    assert await hub.read("progress_user_fixture", "progress_task_fixture", "1-0") == {"cursor": "1-0", "changed": False, "delivery": "database"}
    await hub.close()


def sse_fixture(**patch):
    state = {"valid": True, "disconnected": False, "version": 3, "reads": 0}
    class Tasks:
        async def progress(self, taskId, userId):
            if userId != "owned_user":
                raise ApiError(404, "TASK_NOT_FOUND", "任务不存在。")
            state["reads"] += 1
            return taskProgress({**full_task(state["version"]), "id": taskId, "userId": userId, "status": "planning"})
    class Store:
        async def session(self, token, now):
            return {"user": {"id": "owned_user"}} if state["valid"] else None
    class Request:
        headers = {}
        async def is_disconnected(self):
            return state["disconnected"]
    hub = TaskProgressHub()
    connections = TaskProgressConnections(hub=hub, tasks=Tasks(), store=Store(), now=lambda: NOW,
        pollMs=5, sessionMs=100000, heartbeatMs=100000, lifetimeMs=1000, **patch)
    return SimpleNamespace(state=state, hub=hub, connections=connections, request=Request())


async def test_sse_cursor_ownership_and_connection_limits_are_bounded():
    f = sse_fixture(maxConnections=1, maxPerUser=1)
    for cursor in ["01", "-1", "1.0", "9007199254740992"]:
        f.request.headers = {"last-event-id": cursor}
        with pytest.raises(ApiError) as rejected:
            await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
        assert rejected.value.code == "INVALID_EVENT_ID"
    f.request.headers = {"last-event-id": "4"}
    with pytest.raises(ApiError) as ahead:
        await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    assert ahead.value.code == "EVENT_CURSOR_AHEAD"
    f.request.headers = {}
    with pytest.raises(ApiError) as foreign:
        await f.connections.open(f.request, userId="foreign_user", token="fixture", taskId="task_fixture")
    assert foreign.value.code == "TASK_NOT_FOUND"
    response = await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    with pytest.raises(ApiError) as capacity:
        await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    assert capacity.value.code == "PROGRESS_CONNECTION_LIMIT"
    assert "id: 3" in await anext(response.body_iterator)
    await response.body_iterator.aclose()
    assert not f.connections.connections and not f.connections.users and not f.hub.listeners


async def test_sse_revoked_session_stops_before_delivering_changed_state():
    f = sse_fixture()
    response = await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    assert "event: progress" in await anext(response.body_iterator)
    f.state.update(valid=False, version=4)
    f.hub.publish({**full_task(4), "userId": "owned_user", "id": "task_fixture"})
    frame = await asyncio.wait_for(anext(response.body_iterator), .2)
    assert "event: auth_expired" in frame and "AUTH_REQUIRED" in frame and "id: 4" not in frame
    await response.body_iterator.aclose()
    assert f.state["reads"] == 1 and not f.connections.connections and not f.hub.listeners


async def test_sse_response_header_backpressure_releases_even_unstarted_generator():
    f = sse_fixture(backpressureMs=10)
    response = await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    async def send(message):
        await asyncio.Event().wait()
    async def receive():
        await asyncio.Event().wait()
    # Starlette 将 response.start 的发送超时包装为 ClientDisconnect；资源仍须归还。
    with suppress(ClientDisconnect):
        await asyncio.wait_for(response({"type": "http", "asgi": {"spec_version": "2.4"}}, receive, send), .2)
    assert not f.connections.connections and not f.connections.users and not f.hub.listeners


async def test_sse_delivery_mode_change_never_regresses_state_version():
    f = sse_fixture()
    f.hub.client = object()
    async def read(*args):
        return {"cursor": "0-0", "changed": True, "delivery": "database"}
    f.hub.read = read
    calls = {"count": 0}
    async def progress(taskId, userId):
        calls["count"] += 1
        version = [3, 2, 4][min(calls["count"] - 1, 2)]
        return taskProgress({**full_task(version), "status": "preview" if version == 4 else "planning"})
    f.connections.tasks.progress = progress
    response = await f.connections.open(f.request, userId="owned_user", token="fixture", taskId="task_fixture")
    assert "id: 3" in await anext(response.body_iterator)
    next_frame = await asyncio.wait_for(anext(response.body_iterator), .2)
    assert "id: 4" in next_frame and "id: 2" not in next_frame
    await response.body_iterator.aclose()
    assert not f.connections.connections
