"""独立原生评测后台的隔离、控制权限、真实接口与预算工程测试；不付费。"""

import asyncio
import json
import os
import secrets
from uuid import uuid4

import httpx
import pytest

from evaluation import calibration
from evaluation import independent_server as server_module
from tango_backend.errors import ApiError


def prepared(tmp_path, *, mode="offline", maximum=0, authorization="", cases=None):
    run = tmp_path / secrets.token_hex(5)
    run.mkdir(mode=0o700)
    suite = {"clock": "2026-10-24T04:00:00Z", "cases": cases or [
        {"id": "help", "turns": [{"text": "你能做什么？"}]},
        {"id": "scene", "turns": [{"text": "推荐20分钟读书纯音乐，不要现场版。"}]},
    ]}
    manifest = {"mode": mode, "maximumProviderRequests": maximum, "authorization": authorization,
                "suiteDigest": calibration.sha(calibration.encode(suite).encode())}
    manifest["seal"] = calibration.sha(calibration.encode(manifest).encode())
    calibration.write_new(run / "manifest.json", manifest)
    calibration.write_new(run / "suite.json", suite)
    token = secrets.token_urlsafe(32)
    descriptor = os.open(run / "control.token", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(token)
    return run, token


async def open_client(run):
    server = server_module.IndependentEvaluationServer(run)
    await server.initialize()
    client = httpx.AsyncClient(transport=httpx.ASGITransport(app=server), base_url="http://127.0.0.1")
    return server, client


async def register(client):
    response = await client.post("/v1/auth/register", json={"username": "evaluation-user", "password": "owned-test-password-only"})
    assert response.status_code == 201, response.text
    client.headers["authorization"] = "Bearer " + response.json()["token"]
    response = await client.post("/v1/conversations", json={"deviceId": "owned-evaluation-device", "requestId": str(uuid4())})
    assert response.status_code == 201, response.text
    return response.json()["conversation"]["id"]


def control(token):
    return {"X-Tango-Evaluation-Control": token}


@pytest.mark.parametrize("mode,maximum,authorization", [
    ("offline", 1, ""), ("live", 0, "new user authorization"), ("live", 2, ""),
    ("live", True, "new user authorization"), ("other", 0, ""),
])
def test_new_live_authorization_and_positive_shared_budget_required(tmp_path, mode, maximum, authorization, monkeypatch):
    run, _ = prepared(tmp_path, mode=mode, maximum=maximum, authorization=authorization)
    monkeypatch.setattr(calibration, "live_models", lambda *args: pytest.fail("无效授权不能加载真实模型配置"))
    with pytest.raises(server_module.IndependentServerError):
        server_module.IndependentEvaluationServer(run)
    assert not (run / "owned.sqlite").exists()


@pytest.mark.parametrize("change", ["manifest", "suite", "public_token", "symlink_token"])
def test_frozen_files_and_control_token_permissions_are_checked(tmp_path, change):
    run, _ = prepared(tmp_path)
    if change == "manifest":
        value = calibration.read_json(run / "manifest.json")
        value["maximumProviderRequests"] = 2
        (run / "manifest.json").write_text(json.dumps(value))
    elif change == "suite":
        value = calibration.read_json(run / "suite.json")
        value["cases"][0]["turns"][0]["text"] = "替换后的题目"
        (run / "suite.json").write_text(json.dumps(value))
    elif change == "public_token":
        (run / "control.token").chmod(0o644)
    else:
        (run / "control.token").rename(run / "real.token")
        (run / "control.token").symlink_to(run / "real.token")
    with pytest.raises((server_module.IndependentServerError, calibration.CalibrationError)):
        server_module.IndependentEvaluationServer(run)
    assert not (run / "owned.sqlite").exists()


async def test_offline_startup_never_loads_private_configuration_or_network(tmp_path, monkeypatch):
    run, token = prepared(tmp_path)
    monkeypatch.setenv("LANGSMITH_TRACING", "true")
    monkeypatch.setenv("LANGSMITH_API_KEY", "must-not-read-environment-key")
    monkeypatch.setattr(calibration, "live_models", lambda *args: pytest.fail("离线模式不能加载模型配置"))
    async def forbidden(*args, **kwargs):
        pytest.fail("离线后台不能出站请求")
    monkeypatch.setattr(httpx.AsyncHTTPTransport, "handle_async_request", forbidden)
    server, client = await open_client(run)
    try:
        health = (await client.get("/health")).json()
        assert health["storage"] == "sqlite" and health["tracing"]["enabled"] is False
        response = await client.get("/__evaluation__/status", headers=control(token))
        assert response.status_code == 200
        value = response.json()
        assert value["providerRequests"] == 0 and value["providerOutcomes"] == {}
        assert value["semanticEvidence"] is False and value["modelSnapshot"]["semanticEvidence"] is False
        assert not server.product.tracing.enabled
        assert "must-not-read" not in response.text and token not in response.text
        assert (run / "owned.sqlite").stat().st_mode & 0o077 == 0
    finally:
        await client.aclose()
        await server.close()


async def test_sidechannel_requires_separate_capability_and_rejects_unknown_context(tmp_path):
    run, token = prepared(tmp_path)
    server, client = await open_client(run)
    try:
        for headers in ({}, {"authorization": "Bearer " + token}, control("wrong-token")):
            response = await client.get("/__evaluation__/status", headers=headers)
            assert response.status_code == 403
        for body in ({}, {"caseId": "missing", "turn": 1}, {"caseId": "help", "turn": 0},
                     {"caseId": "help", "turn": True}, {"caseId": "help", "turn": 1, "extra": True}):
            response = await client.post("/__evaluation__/context", headers=control(token), json=body)
            assert response.status_code == 400
        assert (await client.post("/__evaluation__/observation", headers=control(token), json={})).status_code == 400
        assert (await client.get("/__evaluation__/not-a-route", headers=control(token))).status_code == 404
        assert server.ledger.started == 0
    finally:
        await client.aclose()
        await server.close()


async def test_real_asgi_help_route_and_readonly_observation_are_context_bound(tmp_path):
    run, token = prepared(tmp_path)
    server, client = await open_client(run)
    try:
        cid = await register(client)
        assert (await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "help", "turn": 1})).status_code == 200
        mid = str(uuid4())
        base = "/v1/conversations/" + cid
        message = {"deviceId": "owned-evaluation-device", "messageId": mid, "content": "你能做什么？"}
        assert (await client.post(base + "/messages", json=message)).status_code == 200
        response = await client.post(base + "/messages/" + mid + "/route", json={"deviceId": "owned-evaluation-device", "preference": ""})
        assert response.status_code == 200, response.text
        first = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert first["context"] == {"caseId": "help", "turn": 1, "phase": "evaluation"}
        assert first["route"]["result"]["intents"] == ["help"]
        assert first["route"]["result"]["planner"] == "deterministic"
        assert first["task"] is None and first["providerRequests"] == 0
        assert first["messages"][0]["content"] == "你能做什么？"
        assert "input" not in first["route"]
        assert first["callLedger"][0]["paid"] == 0
        assert client.headers["authorization"].removeprefix("Bearer ") not in json.dumps(first)
        second = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert second == first
        mismatch = await client.post("/__evaluation__/observation", headers=control(token), json={"conversationId": str(uuid4()), "messageId": mid})
        assert mismatch.status_code == 400
    finally:
        await client.aclose()
        await server.close()


async def test_scene_uses_real_graph_and_control_observation_cannot_leak_tool_lease(tmp_path):
    run, token = prepared(tmp_path)
    server, client = await open_client(run)
    try:
        cid = await register(client)
        await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "scene", "turn": 1})
        mid = str(uuid4())
        text = "推荐20分钟读书纯音乐，不要现场版。"
        await client.post(f"/v1/conversations/{cid}/messages", json={"deviceId": "owned-evaluation-device", "messageId": mid, "content": text})
        routed = await client.post(f"/v1/conversations/{cid}/messages/{mid}/route", json={"deviceId": "owned-evaluation-device", "preference": ""})
        assert routed.status_code == 200
        response = await client.post("/v1/tasks", json={"deviceId": "owned-evaluation-device", "requestId": mid, "instruction": text, "preference": ""})
        assert response.status_code == 201, response.text
        task_id = response.json()["task"]["id"]
        for _ in range(100):
            task = (await client.get("/v1/tasks/" + task_id)).json()["task"]
            if task["pendingSteps"]:
                break
            await asyncio.sleep(.01)
        assert task["status"] == "waiting_tools"
        lease = task["pendingSteps"][0]["leaseId"]
        observation = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert observation["task"]["id"] == task_id
        assert observation["task"]["brief"]["targetMinutes"] == 20
        assert observation["task"]["brief"]["excludeLive"] is True
        assert observation["task"]["pendingSteps"]
        assert lease not in json.dumps(observation)
        assert "leaseId" not in observation["task"]["pendingSteps"][0]
        assert observation["providerRequests"] == 0
        assert server.product.tasks.strategy == "single_agent"
    finally:
        await client.aclose()
        await server.close()


async def test_live_entry_reuses_counted_adapters_without_reading_private_file(tmp_path, monkeypatch):
    run, token = prepared(tmp_path, mode="live", maximum=1, authorization="explicit-new-fixture-authorization")
    made = []
    class FailingDelegate:
        async def complete(self, *args, **kwargs):
            raise ApiError(504, "MODEL_TIMEOUT", "must-not-save-provider-text")
    def models(ledger, now):
        made.append(ledger)
        model = server_module.WiringModel(now)
        model.counted_fixture = calibration.CountedTransport(FailingDelegate(), ledger, "route:flash")
        return model, model, ["private-fixture-key"], {"strategy": "flash_only", "model": "fixture"}
    monkeypatch.setattr(calibration, "live_models", models)
    server, client = await open_client(run)
    try:
        await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "help", "turn": 1})
        with pytest.raises(ApiError):
            await server.product.services.taskModel.counted_fixture.complete({})
        with pytest.raises(ApiError, match="上限"):
            await server.product.services.taskModel.counted_fixture.complete({})
        status = (await client.get("/__evaluation__/status", headers=control(token))).json()
        assert status["providerRequests"] == 1 and status["providerOutcomes"] == {"uncertain": 1}
        assert made == [server.ledger]
        ledger = (run / "provider-requests.jsonl").read_text()
        assert '"caseId":"help"' in ledger and '"turn":1' in ledger
        assert "must-not-save" not in ledger and "private-fixture-key" not in json.dumps(status)
    finally:
        await client.aclose()
        await server.close()


async def test_same_run_cannot_restart_or_spend_again(tmp_path):
    run, _ = prepared(tmp_path)
    server, client = await open_client(run)
    await client.aclose()
    await server.close()
    with pytest.raises(FileExistsError):
        server_module.IndependentEvaluationServer(run)
    assert (run / "provider-requests.jsonl").read_text() == ""


def test_private_fields_and_configured_keys_are_removed_recursively():
    value = server_module._safe({"token": "session-value", "leaseId": "lease-value",
                                 "nested": {"ownerToken": "owner-value", "apiKey": "secret"},
                                 "text": "fixture-key", "tracks": [{"id": 1, "url": "https://private-play-url"}]}, ["fixture-key"])
    encoded = json.dumps(value)
    assert not any(text in encoded for text in ("session-value", "lease-value", "owner-value", "secret", "fixture-key", "private-play-url"))
    assert value["tracks"] == [{"id": 1}]


async def test_real_lifespan_writes_only_safe_ready_metadata_and_closes(tmp_path):
    run, token = prepared(tmp_path)
    server = server_module.IndependentEvaluationServer(run, port=16399)
    inbox, responses = asyncio.Queue(), []
    async def send(message):
        responses.append(message)
    worker = asyncio.create_task(server({"type": "lifespan"}, inbox.get, send))
    await inbox.put({"type": "lifespan.startup"})
    for _ in range(100):
        if responses:
            break
        await asyncio.sleep(.01)
    assert responses == [{"type": "lifespan.startup.complete"}]
    ready = calibration.read_json(run / "ready.json")
    assert ready == {"pid": os.getpid(), "port": 16399, "mode": "offline", "ready": True}
    assert token not in (run / "ready.json").read_text()
    await inbox.put({"type": "lifespan.shutdown"})
    await worker
    assert server.closed and responses[-1] == {"type": "lifespan.shutdown.complete"}


async def test_remote_control_peer_and_unfinished_provider_attempt_stay_closed(tmp_path, monkeypatch):
    run, token = prepared(tmp_path, mode="live", maximum=1, authorization="new-isolated-test-authorization")
    def models(ledger, now):
        model = server_module.WiringModel(now)
        return model, model, [], {"strategy": "fixture"}
    monkeypatch.setattr(calibration, "live_models", models)
    server, client = await open_client(run)
    remote = httpx.AsyncClient(transport=httpx.ASGITransport(app=server, client=("203.0.113.1", 9000)), base_url="http://127.0.0.1")
    try:
        assert (await remote.get("/__evaluation__/status", headers=control(token))).status_code == 403
        await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "help", "turn": 1})
        server.ledger.begin("route:flash")
        value = (await client.get("/__evaluation__/status", headers=control(token))).json()
        assert value["providerRequests"] == 1 and value["providerOutcomes"] == {"uncertain": 1}
        observation = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert observation["providerLedger"] == [{"event": "started", "number": 1, "role": "route:flash", "caseId": "help", "turn": 1, "phase": "evaluation"}]
        assert server.ledger.started == 1
    finally:
        await remote.aclose()
        await client.aclose()
        await server.close()


async def test_malformed_product_request_cannot_break_observation_wrapper(tmp_path):
    run, token = prepared(tmp_path)
    server, client = await open_client(run)
    try:
        cid = await register(client)
        await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "help", "turn": 1})
        response = await client.post(f"/v1/conversations/{cid}/messages", json=["invalid-request"])
        assert response.status_code == 400
        observation = await client.post("/__evaluation__/observation", headers=control(token), json={})
        assert observation.status_code == 200
        assert observation.json()["route"] is None and observation.json()["providerRequests"] == 0
    finally:
        await client.aclose()
        await server.close()


async def test_product_clock_advances_and_reissued_tool_lease_uses_wall_time(tmp_path, monkeypatch):
    run, _ = prepared(tmp_path)
    wall = [server_module.now_ms()]
    monkeypatch.setattr(server_module, "now_ms", lambda: wall[0])
    server, client = await open_client(run)
    try:
        assert server.product.services.now() == wall[0]
        wall[0] += 120000
        assert server.product.services.now() == wall[0]
        # 直接走产品任务接口检查租约；不通过固定题集时钟掩盖过期。
        await register(client)
        response = await client.post("/v1/tasks", json={"deviceId": "owned-evaluation-device", "requestId": str(uuid4()),
                                     "instruction": "推荐20分钟读书纯音乐，不要现场版。", "preference": ""})
        assert response.status_code == 201
        task_id = response.json()["task"]["id"]
        async def waiting():
            for _ in range(100):
                task = (await client.get("/v1/tasks/" + task_id)).json()["task"]
                if task["pendingSteps"]:
                    return task
                await asyncio.sleep(.01)
            pytest.fail("固定假模型未进入真实工具等待状态")
        task = await waiting()
        old_expiry = task["pendingSteps"][0]["expiresAt"]
        assert wall[0] < old_expiry <= wall[0] + 60000
        paused = await client.post(f"/v1/tasks/{task_id}/actions", json={"deviceId": "owned-evaluation-device", "requestId": str(uuid4()),
                                   "expectedRevision": task["revision"], "action": "pause"})
        assert paused.status_code == 200
        wall[0] += 120000
        resumed = await client.post(f"/v1/tasks/{task_id}/actions", json={"deviceId": "owned-evaluation-device", "requestId": str(uuid4()),
                                    "expectedRevision": paused.json()["task"]["revision"], "action": "resume"})
        assert resumed.status_code == 200, resumed.text
        task = await waiting()
        assert wall[0] < task["pendingSteps"][0]["expiresAt"] <= wall[0] + 60000
        assert task["pendingSteps"][0]["expiresAt"] >= old_expiry + 120000
        assert server.ledger.started == 0
    finally:
        await client.aclose()
        await server.close()


async def test_setup_observation_is_separate_and_formal_turn_sees_real_prior_task(tmp_path):
    setup_text = "推荐20分钟读书纯音乐，不要现场版。"
    run, token = prepared(tmp_path, cases=[{"id": "with_setup", "setupInstructions": [setup_text],
                                          "turns": [{"text": "你能做什么？"}]}])
    server, client = await open_client(run)
    try:
        cid = await register(client)
        setup = {"caseId": "with_setup", "turn": 1, "phase": "setup"}
        response = await client.post("/__evaluation__/context", headers=control(token), json=setup)
        assert response.status_code == 200 and response.json()["context"] == setup
        mid = str(uuid4())
        await client.post(f"/v1/conversations/{cid}/messages", json={"deviceId": "owned-evaluation-device", "messageId": mid, "content": setup_text})
        await client.post(f"/v1/conversations/{cid}/messages/{mid}/route", json={"deviceId": "owned-evaluation-device", "preference": ""})
        created = await client.post("/v1/tasks", json={"deviceId": "owned-evaluation-device", "requestId": mid,
                                    "instruction": setup_text, "preference": ""})
        assert created.status_code == 201
        task_id = created.json()["task"]["id"]
        cost_only = (await client.post("/__evaluation__/observation", headers=control(token), json=setup)).json()
        assert cost_only["context"] == setup and cost_only["providerLedger"] == []
        assert "route" not in cost_only and "task" not in cost_only and "messages" not in cost_only
        explicit = await client.post("/__evaluation__/observation", headers=control(token), json={**setup, "conversationId": cid, "messageId": mid})
        assert explicit.status_code == 200, explicit.text
        assert explicit.json()["task"]["id"] == task_id
        assert explicit.json()["route"]["messageId"] == mid
        assert (await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "with_setup", "turn": 1})).status_code == 200
        formal = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert formal["context"]["phase"] == "evaluation"
        assert formal["route"] is None and formal["messages"] == []
        assert formal["before"]["id"] == formal["task"]["id"] == task_id
        assert formal["providerLedger"] == [] and formal["providerRequests"] == 0
    finally:
        await client.aclose()
        await server.close()


async def test_setup_and_formal_attempts_keep_distinct_ledger_and_one_total_cap(tmp_path, monkeypatch):
    run, token = prepared(tmp_path, mode="live", maximum=2, authorization="new-isolated-fixture-authorization")
    class Delegate:
        async def complete(self, *args, **kwargs):
            return {"usage": {"prompt_tokens": 2, "completion_tokens": 1}}
    def models(ledger, now):
        model = server_module.WiringModel(now)
        model.counted_fixture = calibration.CountedTransport(Delegate(), ledger, "task:flash")
        return model, model, [], {"strategy": "fixture"}
    monkeypatch.setattr(calibration, "live_models", models)
    server, client = await open_client(run)
    try:
        for phase in ("setup", "evaluation"):
            context = {"caseId": "help", "turn": 1, "phase": phase}
            assert (await client.post("/__evaluation__/context", headers=control(token), json=context)).status_code == 200
            await server.product.services.taskModel.counted_fixture.complete({})
            observation = (await client.post("/__evaluation__/observation", headers=control(token), json=context)).json()
            assert len(observation["providerLedger"]) == 2
            assert observation["providerLedger"][0]["phase"] == phase
            assert observation["providerRequests"] == (1 if phase == "setup" else 2)
        with pytest.raises(ApiError, match="上限"):
            await server.product.services.taskModel.counted_fixture.complete({})
        status = (await client.get("/__evaluation__/status", headers=control(token))).json()
        assert status["providerRequests"] == 2 and status["providerOutcomes"] == {"responded": 2}
        assert len(server.contexts) == 2
    finally:
        await client.aclose()
        await server.close()


async def test_repaired_instrumental_memory_is_only_a_proposal_without_task_dispatch(tmp_path, monkeypatch, caplog):
    """保留旧运行的拒绝证据；修复后只生成待确认提议，后台不能代替设备写偏好。"""
    text = "记住我喜欢原声器乐"
    run, token = prepared(tmp_path, cases=[{"id": "memory_smoke", "turns": [{"text": text}]}])
    server, client = await open_client(run)
    try:
        async def proposal(input, options=None):
            return {"value": {"intents": ["preference_proposal"], "slots": {"artist": "", "targetMinutes": 0},
                              "actions": [{"capability": "memory.propose", "source": text, "dependsOn": [],
                                           "parameters": {"operation": "set", "preference": "喜欢原声器乐"}}], "missingSlots": []}}
        # 仅在本诊断替换上游提案，验证产品守卫；通用离线模型不按这道题作答。
        monkeypatch.setattr(server.product.services.routingModel, "route", proposal)
        cid = await register(client)
        await client.post("/__evaluation__/context", headers=control(token), json={"caseId": "memory_smoke", "turn": 1})
        advertised = (await client.get("/v1/agent/status")).json()
        from tango_backend.capability_registry import CAPABILITY_REGISTRY_VERSION
        assert advertised["capabilityRegistryVersion"] == CAPABILITY_REGISTRY_VERSION
        mid = str(uuid4())
        await client.post(f"/v1/conversations/{cid}/messages", json={"deviceId": "owned-evaluation-device", "messageId": mid, "content": text})
        response = await client.post(f"/v1/conversations/{cid}/messages/{mid}/route", json={"deviceId": "owned-evaluation-device", "preference": ""})
        assert response.status_code == 200, response.text
        route = response.json()["route"]
        assert route["status"] == "completed"
        actions = route["result"]["actions"]
        assert len(actions) == 1 and actions[0]["capability"] == "memory.propose"
        assert actions[0]["parameters"] == {"operation": "set", "preference": "喜欢原声器乐"}
        assert route["errorCode"] is None
        assert "reason=memory_source_permission" not in caplog.text
        observation = (await client.post("/__evaluation__/observation", headers=control(token), json={})).json()
        assert observation["task"] is None and observation["providerRequests"] == 0
        assert (await server.store.db.fetchone("SELECT COUNT(*) AS n FROM music_tasks"))["n"] == 0
        assert (await server.store.db.fetchone("SELECT COUNT(*) AS n FROM music_task_requests"))["n"] == 0
        assert server.status()["semanticEvidence"] is False
    finally:
        await client.aclose()
        await server.close()
