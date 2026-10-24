"""真实后台装配的离线测试：私有配置不读取，模型端点只声明为环回假地址。"""

import json
import os

import httpx
import pytest

from tango_backend import server


def environment(monkeypatch, tmp_path, **extra):
    # 不继承调用者的密钥、数据库或开发身份；只操作本测试进程环境。
    for key in list(os.environ):
        if key.startswith(("QWEN_", "QUIET_")):
            monkeypatch.delenv(key)
    values = {
        "QUIET_STORAGE_DRIVER": "sqlite",
        "QUIET_DB_PATH": str(tmp_path / "owned.sqlite"),
        "QUIET_REGISTRATION_MODE": "open",
        "QWEN_API_KEY": "fixture-only-flash-secret",
        "QWEN_BASE_URL": "http://127.0.0.1:9/flash",
        "QWEN_MODEL": "qwen-flash-fixture",
        **extra,
    }
    for key, value in values.items():
        monkeypatch.setenv(key, value)


@pytest.mark.parametrize("strategy", ["flash_only", "task_split", "plus_only"])
async def test_configured_server_separates_roles_and_reports_safe_status(monkeypatch, tmp_path, strategy):
    environment(
        monkeypatch, tmp_path,
        QWEN_MODEL_STRATEGY=strategy,
        QWEN_PLUS_ENABLED="true",
        QWEN_PLUS_MODEL="qwen3.7-plus",
        QWEN_PLUS_API_KEY="fixture-only-plus-secret",
        QWEN_PLUS_BASE_URL="http://127.0.0.1:9/plus",
        QWEN_PLUS_TIMEOUT_MS="60000",
    )
    app, store, redis = await server.configured_app()
    try:
        assert redis is None
        await app.initialize()
        services = app.services
        assert services.health()["modelRoutingVersion"] == 1
        assert services.router.model.model == "qwen-flash-fixture"
        assert services.tasks.model.policySnapshot()["strategy"] == strategy
        assert services.tasks.model.transports["plus"].model == "qwen3.7-plus"
        assert services.tasks.model.transports["plus"].apiKey == "fixture-only-plus-secret"
        assert services.model.transport.apiKey == "fixture-only-flash-secret"
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://127.0.0.1") as client:
            registration = await client.post("/v1/auth/register", json={"username": "model-role-user", "password": "fixture-password-only"})
            assert registration.status_code == 201
            client.headers["authorization"] = "Bearer " + registration.json()["token"]
            response = await client.get("/v1/agent/status")
            assert response.status_code == 200
            assert response.json()["modelRouting"] == {
                "version": 1, "strategy": strategy, "plusEnabled": True,
                "selection": "server_policy",
            }
            assert "fixture-only" not in response.text
            assert ":9/" not in response.text
            assert (await client.get("/v1/account/usage")).json()["daily"]["used"] == 0
    finally:
        await app.close()
        await store.close()


async def test_plus_credentials_can_be_prepared_without_enabling_calls(monkeypatch, tmp_path):
    environment(monkeypatch, tmp_path, QWEN_PLUS_MODEL="qwen3.7-plus", QWEN_PLUS_ENABLED="false", QWEN_PLUS_API_KEY="fixture-only-plus-secret")
    app, store, _ = await server.configured_app()
    try:
        assert app.services.tasks.model.policySnapshot()["strategy"] == "flash_only"
        assert "plus" not in app.services.tasks.model.transports
        assert app.services.options["modelRouting"]["plusEnabled"] is False
        snapshot = json.dumps(app.services.tasks.model.policySnapshot())
        assert "fixture-only" not in snapshot and "127.0.0.1" not in snapshot
    finally:
        await app.close()
        await store.close()


@pytest.mark.parametrize("extra", [
    {"QWEN_MODEL_STRATEGY": "task_split", "QWEN_PLUS_ENABLED": "false"},
    {"QWEN_MODEL_STRATEGY": "unknown"},
    {"QWEN_PLUS_ENABLED": "true", "QWEN_PLUS_MODEL": "qwen3.7-plus", "QWEN_PLUS_ENABLE_THINKING": "false"},
    {"QWEN_PLUS_ENABLED": "true", "QWEN_PLUS_MODEL": "qwen3.7-plus", "QWEN_PLUS_STREAM": "false"},
])
async def test_invalid_model_configuration_fails_before_database_open(monkeypatch, tmp_path, extra):
    environment(monkeypatch, tmp_path, **extra)

    def forbidden_store(*args, **kwargs):
        pytest.fail("模型配置无效时不应先创建数据库或开始请求")

    monkeypatch.setattr(server, "Store", forbidden_store)
    with pytest.raises(ValueError):
        await server.configured_app()
