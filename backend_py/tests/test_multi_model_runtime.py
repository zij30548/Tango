"""确定性模型分工、配置恢复及共享账本；HTTP 上游和曲库均为隔离夹具。"""

import asyncio
import json

import httpx
import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.intent_routing import createRoutingModel
from tango_backend.model_profiles import modelProfilesConfiguration
from tango_backend.task_model import ModelRoutingPolicy, createTaskModel, TaskModel, jsonEnvelope
from test_task_runtime_migration import Model, body, f, finish, raw_brief, until
from test_conversation_routing import fx, music


def configuration(strategy="task_split", **overrides):
    return modelProfilesConfiguration({
        "QWEN_BASE_URL": "http://127.0.0.1:9/v1",
        "QWEN_API_KEY": "isolated-flash-key",
        "QWEN_MODEL": "qwen-flash-fixture",
        "QWEN_PLUS_ENABLED": "true",
        "QWEN_PLUS_MODEL": "qwen-plus-fixture",
        "QWEN_PLUS_API_KEY": "isolated-plus-key",
        "QWEN_PLUS_ENABLE_THINKING": "false",
        "QWEN_PLUS_STREAM": "false",
        "QWEN_MODEL_STRATEGY": strategy,
        **overrides,
    })


@pytest.mark.parametrize("purpose,inputs,profile,reason", [
    ("route", {"conversationContext": {"intents": ["scene_research", "release_research"]}}, "flash", "routing_flash"),
    ("plan", {"instruction": "喝酒听点歌"}, "flash", "simple_task"),
    ("plan", {"instruction": "普通说明" * 400, "confidence": 0.01, "difficulty": "hard"}, "flash", "simple_task"),
    ("plan", {"instruction": "短", "conversationContext": {"intents": ["scene_research", "release_research"]}}, "plus", "multi_intent"),
    ("plan", {"conversationContext": {"dependencyCount": 1}}, "plus", "dependent_actions"),
    ("plan", {"pendingRequest": {"sources": [{}, {}]}}, "plus", "pending_continuation"),
    ("plan", {"currentBrief": {"scene": "阅读"}, "preview": [{"id": 1}]}, "plus", "confirmed_revision"),
    ("research", {"scope": {"scene": "阅读", "styles": ["R&B"], "targetMinutes": 40}}, "plus", "multiple_constraints"),
    ("research", {"scope": {"artist": "邓紫棋", "requireReleases": True}}, "flash", "simple_task"),
    ("observe", {"scope": raw_brief()}, "flash", "simple_task"),
    ("compose", {"currentBrief": raw_brief()}, "flash", "simple_task"),
])
def test_task_split_uses_supported_structure_not_length_or_confidence(purpose, inputs, profile, reason):
    policy = ModelRoutingPolicy(configuration())
    selection = policy.select(purpose, inputs, policy.snapshot())
    assert selection["profile"] == profile and selection["reason"] == reason
    assert selection["purpose"] == purpose
    assert set(selection) == {"profile", "configuredModel", "configFingerprint", "policyVersion", "strategy", "purpose", "reason"}
    assert "key" not in json.dumps(selection).lower()


@pytest.mark.parametrize("strategy,expected", [("flash_only", "flash"), ("plus_only", "plus")])
def test_comparison_strategies_route_stays_flash_and_task_calls_are_fixed(strategy, expected):
    policy = ModelRoutingPolicy(configuration(strategy))
    assert policy.select("route", {})["profile"] == "flash"
    for purpose in ("plan", "research", "observe", "compose"):
        assert policy.select(purpose, {"scope": raw_brief()})["profile"] == expected


@pytest.mark.parametrize("change", [
    {"QWEN_PLUS_MODEL": "qwen-other-fixture"},
    {"QWEN_PLUS_BASE_URL": "https://other.invalid/v1"},
    {"QWEN_PLUS_TIMEOUT_MS": "20000"},
    {"QWEN_PLUS_MAX_TOKENS": "3000"},
    {"QWEN_MODEL_STRATEGY": "flash_only", "QWEN_PLUS_ENABLED": "false"},
])
def test_saved_policy_rejects_model_endpoint_parameters_and_strategy_drift(change):
    saved = ModelRoutingPolicy(configuration()).snapshot()
    with pytest.raises(ApiError) as error:
        ModelRoutingPolicy(configuration(**change)).select("research", {"scope": raw_brief()}, saved)
    assert error.value.code == "MODEL_POLICY_MISMATCH"


def test_api_key_rotation_does_not_change_public_policy_or_record_credentials():
    original = ModelRoutingPolicy(configuration())
    rotated = ModelRoutingPolicy(configuration(QWEN_API_KEY="rotated-flash-key", QWEN_PLUS_API_KEY="rotated-plus-key"))
    assert original.snapshot() == rotated.snapshot()
    rotated.select("plan", {}, original.snapshot())
    public = json.dumps(original.snapshot())
    assert "isolated-" not in public and "127.0.0.1" not in public


def test_staging_unused_plus_configuration_does_not_invalidate_flash_tasks():
    before = ModelRoutingPolicy(configuration("flash_only", QWEN_PLUS_ENABLED="false"))
    after = ModelRoutingPolicy(configuration(
        "flash_only", QWEN_PLUS_ENABLED="false",
        QWEN_PLUS_MODEL="qwen-staged-plus", QWEN_PLUS_BASE_URL="https://staging.invalid/v1",
        QWEN_PLUS_TIMEOUT_MS="20000", QWEN_PLUS_MAX_TOKENS="6000",
    ))
    assert set(before.snapshot()["profiles"]) == {"flash"}
    assert before.snapshot() == after.snapshot()
    assert after.select("plan", {}, before.snapshot())["profile"] == "flash"
    with pytest.raises(ApiError) as error:
        ModelRoutingPolicy(configuration("task_split")).select("plan", {}, before.snapshot())
    assert error.value.code == "MODEL_POLICY_MISMATCH"


@pytest.mark.parametrize("usage", [None, {}, {"prompt_tokens": 5}, {"completion_tokens": 4}, {"prompt_tokens": -1, "completion_tokens": 4}, {"prompt_tokens": True, "completion_tokens": 4}])
def test_missing_partial_or_invalid_usage_is_unknown_instead_of_zero(usage):
    envelope = {"choices": [{"finish_reason": "stop", "message": {"content": "{}"}}]}
    if usage is not None:
        envelope["usage"] = usage
    assert jsonEnvelope(envelope)["usage"] is None


def test_explicit_zero_usage_is_distinct_from_unknown_usage():
    envelope = {"choices": [{"finish_reason": "stop", "message": {"content": "{}"}}], "usage": {"prompt_tokens": 0, "completion_tokens": 0}}
    assert jsonEnvelope(envelope)["usage"] == {"inputTokens": 0, "outputTokens": 0}


def test_disabled_plus_is_default_and_never_selected_or_used_as_fallback():
    config = modelProfilesConfiguration({"QWEN_API_KEY": "isolated-key"})
    policy = ModelRoutingPolicy(config)
    assert policy.strategy == "flash_only" and not config["profiles"]["plus"].enabled
    assert policy.select("research", {"scope": raw_brief()})["profile"] == "flash"
    unavailable = clone(configuration())
    from dataclasses import replace
    unavailable["profiles"]["plus"] = replace(unavailable["profiles"]["plus"], enabled=False)
    with pytest.raises(ApiError) as error:
        ModelRoutingPolicy(unavailable).select("research", {"scope": raw_brief()})
    assert error.value.code == "MODEL_PROFILE_UNAVAILABLE"


async def test_selected_profile_owns_request_and_metadata_is_returned_without_mutating_flash():
    seen = []

    def handler(request):
        payload = json.loads(request.content)
        seen.append((payload["model"], request.headers["authorization"]))
        return httpx.Response(200, json={
            "model": payload["model"], "id": "chatcmpl-owned-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": json.dumps({"queries": ["阅读 R&B"], "artist": ""})}}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 4},
        })

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        model = createTaskModel({"modelProfiles": configuration(), "client": client})
        inputs = {"scope": raw_brief()}
        selected = model.selectCall("research", inputs, model.policySnapshot())
        result = await model.call("research", inputs, {"modelSelection": selected})
        assert seen == [("qwen-plus-fixture", "Bearer isolated-plus-key")]
        assert model.transport.model == "qwen-flash-fixture"
        assert result["modelMetadata"] == {"providerModel": "qwen-plus-fixture", "providerResponseId": "chatcmpl-owned-fixture"}
        assert result["usage"] == {"inputTokens": 5, "outputTokens": 4}


async def test_routing_model_uses_flash_even_with_plus_task_baseline():
    seen = []

    def handler(request):
        payload = json.loads(request.content)
        seen.append(payload["model"])
        return httpx.Response(200, json={
            "model": payload["model"], "id": "route-owned-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": "{}"}}],
        })

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        model = createRoutingModel({"modelProfiles": configuration("plus_only"), "client": client})
        result = await model.route({"instruction": "阅读"})
        assert seen == ["qwen-flash-fixture"]
        assert result["modelMetadata"]["providerResponseId"] == "route-owned-fixture"


@pytest.mark.parametrize("invalid", [False, True])
async def test_paid_route_records_selected_and_provider_models_even_if_output_is_invalid(fx, invalid):
    def handler(request):
        payload = json.loads(request.content)
        inputs = json.loads(payload["messages"][1]["content"])
        return httpx.Response(200, json={
            "model": payload["model"], "id": "route-owned-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": "invalid-json" if invalid else json.dumps(music(inputs))}}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 4},
        })

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        fx.router.model = createRoutingModel({"modelProfiles": configuration(), "client": client})
        message, _ = await fx.submit()
        result = await fx.route(message)
        assert result["route"]["status"] == ("failed" if invalid else "completed")
        row = await fx.store.db.fetchone("SELECT model_selection_json,model_metadata_json,input_tokens,output_tokens FROM music_conversation_route_attempts")
        assert json.loads(row["model_selection_json"])["profile"] == "flash"
        assert json.loads(row["model_metadata_json"]) == {"providerModel": "qwen-flash-fixture", "providerResponseId": "route-owned-fixture"}
        assert row["input_tokens"] == 5 and row["output_tokens"] == 4


class ProfiledModel(TaskModel):
    """保留真实策略边界，模型生成部分复用已校验的固定夹具。"""

    def __init__(self, strategy="task_split"):
        super().__init__({"modelProfiles": configuration(strategy)})
        self.fake, self.selected, self.error = Model(), [], None

    async def call(self, kind, inputs, options=None):
        self.selected.append(clone(options["modelSelection"]))
        if self.error:
            raise self.error
        result = await self.fake.call(kind, inputs, options)
        result["modelMetadata"] = {"providerModel": options["modelSelection"]["configuredModel"], "providerResponseId": "fixture-response"}
        return result


async def test_runtime_shared_budget_and_pre_call_ledger_cover_flash_and_plus(f):
    model = ProfiledModel()
    runtime = await f.runtime(model=model)
    reserved = []
    original = runtime.store.reserveCall

    async def reserve(*args, **kwargs):
        value = await original(*args, **kwargs)
        reserved.append(clone(args[6]["modelSelection"]))
        assert len(reserved) == len(model.selected) + 1
        return value

    runtime.store.reserveCall = reserve
    created = await runtime.create(f.user, body())
    preview, _ = await finish(runtime, created, f.user, f.time[0])
    assert created["modelPolicy"]["strategy"] == "task_split"
    assert {e["profile"] for e in reserved} == {"flash", "plus"}
    assert reserved == model.selected
    assert preview["budget"]["totalCalls"] == len(reserved)
    assert (await f.store.db.fetchone("SELECT request_count FROM ai_usage"))["request_count"] == len(reserved)
    rows = await f.store.db.fetchall("SELECT status,model_selection_json,model_metadata_json FROM music_task_calls")
    assert len(rows) == len(reserved) and all(row["status"] == "completed" for row in rows)
    assert all(row["model_selection_json"] and row["model_metadata_json"] for row in rows)


async def test_timeout_keeps_unknown_attempt_and_never_switches_model_or_replays(f):
    model = ProfiledModel()
    model.error = ApiError(504, "MODEL_TIMEOUT", "隔离超时")
    runtime = await f.runtime(model=model)
    created = await runtime.create(f.user, body())
    failed = await until(runtime, created["id"], f.user, "failed")
    assert failed["error"]["code"] == "MODEL_TIMEOUT" and len(model.selected) == 1
    await runtime.get(created["id"], f.user)
    await asyncio.sleep(0)
    assert len(model.selected) == 1 and failed["budget"]["totalCalls"] == 1
    assert (await f.store.db.fetchone("SELECT status FROM music_task_calls"))["status"] == "uncertain"


async def test_runtime_resume_rejects_changed_policy_before_reserving_or_calling(f):
    model = ProfiledModel()
    runtime = await f.runtime(model=model)
    created = await runtime.create(f.user, body())
    waiting = await until(runtime, created["id"], f.user, "waiting_tools")
    paused = await runtime.action(created["id"], f.user, body(action="pause", expectedRevision=waiting["revision"]))
    previous = len(model.selected)
    model.policy = ModelRoutingPolicy(configuration(QWEN_PLUS_MODEL="qwen-plus-new"))
    with pytest.raises(ApiError) as error:
        await runtime.action(created["id"], f.user, body(action="resume", expectedRevision=paused["revision"]))
    assert error.value.code == "MODEL_POLICY_MISMATCH" and len(model.selected) == previous


@pytest.mark.parametrize("stage,node", [("plan", "understand"), ("research", "shared_research"), ("observe", "shared_observe"), ("compose", "compose")])
async def test_business_invalid_task_output_retains_actual_usage_and_provider_in_paid_ledger(f, stage, node):
    targets = {"plan": 2200, "research": 1000, "observe": 700, "compose": 1200}

    def handler(request):
        payload = json.loads(request.content)
        maximum = payload["max_tokens"]
        valid = {
            2200: raw_brief(),
            1000: {"queries": ["读书 R&B"], "artist": ""},
            700: {"decision": "done", "queries": [], "note": "固定观察"},
            1200: {"order": [0, 1]},
        }
        return httpx.Response(200, json={
            "model": payload["model"], "id": "business-invalid-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": json.dumps({} if maximum == targets[stage] else valid[maximum])}}],
            "usage": {"prompt_tokens": 7, "completion_tokens": 3},
        })

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        model = createTaskModel({"modelProfiles": configuration(), "client": client})
        runtime = await f.runtime(model=model)
        created = await runtime.create(f.user, body())
        if stage in ("plan", "research"):
            completed = await until(runtime, created["id"], f.user, "failed")
        else:
            # 缺少场景来源证据才会进入观察模型；程序核验路径不冒充已调用。
            completed, _ = await finish(runtime, created, f.user, f.time[0], label="不相关来源")
        row = await f.store.db.fetchone("SELECT status,input_tokens,output_tokens,model_metadata_json FROM music_task_calls WHERE node=?", [node])
        assert row["input_tokens"] == 7 and row["output_tokens"] == 3
        metadata = json.loads(row["model_metadata_json"]) if isinstance(row["model_metadata_json"], str) else row["model_metadata_json"]
        assert metadata["providerResponseId"] == "business-invalid-fixture"
        assert metadata["providerModel"] == ("qwen-plus-fixture" if stage == "research" else "qwen-flash-fixture")
        assert row["status"] == ("completed" if stage == "compose" else "failed")
        assert completed["budget"]["inputTokens"] == completed["budget"]["totalCalls"] * 7


def test_legacy_task_is_not_silently_migrated_to_multi_model(tmp_path):
    from tango_backend.task_runtime import MusicTaskRuntime
    from tango_backend.store import Store
    # 构造器只接收合法 Store，未 initialize 就不会打开数据库。
    store = Store(tmp_path / "unopened.sqlite")
    legacy = {"budget": {"totalCalls": 2}}
    split = MusicTaskRuntime(store=store, model=ProfiledModel())
    with pytest.raises(ApiError) as error:
        split.modelPolicy(legacy)
    assert error.value.code == "MODEL_POLICY_MISSING" and "modelPolicy" not in legacy
    baseline = MusicTaskRuntime(store=store, model=ProfiledModel("flash_only"))
    assert baseline.modelPolicy(legacy)["legacyFlash"] is True

