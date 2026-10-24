"""多模型配置和流式边界均用假上游；不读 .env，不调用真实模型。"""

import asyncio
import json

import httpx
import pytest

from tango_backend.errors import ApiError
from tango_backend.model_profiles import modelProfilesConfiguration
from tango_backend.model_transport import RESPONSE_LIMIT, createChatTransport


def configured(**extra):
    return modelProfilesConfiguration({
        "QWEN_API_KEY": "fixture-flash-secret",
        "QWEN_BASE_URL": "https://fixture.invalid/v1",
        "QWEN_PLUS_ENABLED": "true",
        "QWEN_PLUS_MODEL": "qwen3.7-plus",
        **extra,
    })


def delta(content=None, *, finish=None, **extra):
    return {
        "id": "chatcmpl-fixture",
        "model": "qwen3.7-plus-actual",
        "choices": [{"index": 0, "delta": {"content": content, **extra}, "finish_reason": finish}],
    }


def event(value):
    data = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
    return ("data: " + data + "\n\n").encode("utf-8")


def valid_stream(*, usage=True, finish="stop"):
    chunks = [
        event(delta(role="assistant", reasoning_content="private fixture thought")),
        event(delta('{"title":"')),
        event(delta('中文😀"}')),
        event(delta(finish=finish)),
    ]
    if usage:
        chunks.append(event({"choices": [], "usage": {"prompt_tokens": 7, "completion_tokens": 19, "total_tokens": 26}}))
    chunks.append(event("[DONE]"))
    return b"".join(chunks)


def stream_options(handler, **extra):
    return {
        "apiKey": "fixture-private-key",
        "baseUrl": "https://fixture.invalid/v1",
        "model": "qwen3.7-plus",
        "enableThinking": True,
        "stream": True,
        "maxTokens": 8192,
        "client": httpx.AsyncClient(transport=httpx.MockTransport(handler)),
        **extra,
    }


def response(content):
    return httpx.Response(200, content=content, headers={"content-type": "text/event-stream; charset=utf-8"})


def test_default_profiles_keep_flash_and_disabled_plus():
    config = modelProfilesConfiguration({})
    assert config["strategy"] == "flash_only"
    flash, plus = config["profiles"]["flash"], config["profiles"]["plus"]
    assert flash.enabled and not flash.configured
    assert flash.model == "qwen3.7-flash-2026-07-15"
    assert flash.enableThinking is False and flash.stream is False
    assert flash.timeoutMs == 15000 and flash.maxTokens == 4096
    assert not plus.enabled and not plus.configured and plus.model == ""


def test_plus_can_be_filled_without_activation_or_new_key():
    config = configured(QWEN_PLUS_ENABLED="false")
    plus = config["profiles"]["plus"]
    assert not plus.configured and plus.model == "qwen3.7-plus"
    assert plus.apiKey == "fixture-flash-secret"
    assert plus.baseUrl == "https://fixture.invalid/v1"
    assert plus.transportOptions()["apiKey"] == ""


def test_plus_configuration_capabilities_and_independent_endpoint():
    config = configured(
        QWEN_PLUS_API_KEY="fixture-plus-secret", QWEN_PLUS_BASE_URL="https://plus.invalid/compatible-mode/v1",
        QWEN_MODEL_STRATEGY="task_split", QWEN_PLUS_TIMEOUT_MS="60000",
    )
    plus = config["profiles"]["plus"]
    assert plus.configured and plus.enabled
    assert plus.apiKey == "fixture-plus-secret"
    assert plus.enableThinking is True and plus.stream is True and plus.jsonMode is True
    assert plus.timeoutMs == 60000 and plus.maxTokens == 8192
    assert config["strategy"] == "task_split"
    assert createChatTransport(plus.transportOptions()).model == "qwen3.7-plus"


def test_public_descriptor_and_repr_never_expose_credentials_or_url():
    plus = configured(QWEN_PLUS_API_KEY="fixture-plus-secret")["profiles"]["plus"]
    value = plus.publicDescriptor()
    text = json.dumps(value) + repr(plus)
    assert value["profile"] == "plus" and value["model"] == "qwen3.7-plus"
    assert len(value["fingerprint"]) == 64 and len(value["endpointId"]) == 64
    for private in (plus.apiKey, plus.baseUrl, "https://", "fixture.invalid"):
        assert private not in text


def test_key_rotation_preserves_fingerprint_but_endpoint_or_capability_change_does_not():
    before = configured()["profiles"]["plus"].publicDescriptor()
    rotated = configured(QWEN_PLUS_API_KEY="other-fixture-key")["profiles"]["plus"].publicDescriptor()
    assert before == rotated
    for change in ({"QWEN_PLUS_TIMEOUT_MS": "50000"}, {"QWEN_PLUS_BASE_URL": "https://elsewhere.invalid/v1", "QWEN_PLUS_API_KEY": "fixture-plus-key"}, {"QWEN_PLUS_MAX_TOKENS": "4096"}):
        assert configured(**change)["profiles"]["plus"].publicDescriptor()["fingerprint"] != before["fingerprint"]


@pytest.mark.parametrize("base", ["", "https://FIXTURE.invalid/v1/", "https://fixture.invalid:443/v1"])
def test_plus_inherits_key_only_for_the_same_normalized_endpoint(base):
    plus = configured(QWEN_PLUS_API_KEY="", QWEN_PLUS_BASE_URL=base)["profiles"]["plus"]
    assert plus.configured and plus.apiKey == "fixture-flash-secret"
    assert plus.baseUrl == "https://fixture.invalid/v1"


@pytest.mark.parametrize("explicitKey", [None, ""])
def test_different_plus_endpoint_without_explicit_key_fails_before_any_request(explicitKey):
    env = {"QWEN_PLUS_BASE_URL": "https://different-platform.invalid/v1"}
    if explicitKey is not None:
        env["QWEN_PLUS_API_KEY"] = explicitKey
    with pytest.raises(ValueError) as error:
        configured(**env)
    assert "explicit QWEN_PLUS_API_KEY" in str(error.value)
    assert "fixture-flash-secret" not in str(error.value)
    assert "different-platform.invalid" not in str(error.value)


def test_disabled_plus_can_stage_a_different_endpoint_without_inheriting_flash_key():
    plus = configured(
        QWEN_PLUS_ENABLED="false", QWEN_PLUS_BASE_URL="https://different-platform.invalid/v1",
        QWEN_PLUS_API_KEY="",
    )["profiles"]["plus"]
    assert not plus.configured and plus.apiKey == ""
    assert plus.transportOptions()["apiKey"] == ""


def test_different_endpoint_can_use_an_explicitly_supplied_key():
    plus = configured(
        QWEN_PLUS_BASE_URL="https://different-platform.invalid/v1",
        QWEN_PLUS_API_KEY="fixture-flash-secret",
    )["profiles"]["plus"]
    assert plus.configured and plus.apiKey == "fixture-flash-secret"
    assert plus.baseUrl == "https://different-platform.invalid/v1"


@pytest.mark.parametrize("change", [
    {"QWEN_MODEL_STRATEGY": "automatic"},
    {"QWEN_PLUS_ENABLED": "yes"},
    {"QWEN_PLUS_API_KEY": "private\nkey"},
    {"QWEN_PLUS_BASE_URL": "http://external.invalid/v1"},
    {"QWEN_PLUS_BASE_URL": "https://user:secret@fixture.invalid/v1"},
    {"QWEN_PLUS_BASE_URL": "https://fixture.invalid/v1?key=private"},
    {"QWEN_PLUS_MODEL": "https://private.invalid/model"},
    {"QWEN_PLUS_MODEL": "qwen\nprivate"},
    {"QWEN_PLUS_MODEL": ""},
    {"QWEN_PLUS_API_KEY": "", "QWEN_API_KEY": ""},
    {"QWEN_PLUS_ENABLE_THINKING": "false"},
    {"QWEN_PLUS_ENABLE_THINKING": "maybe"},
    {"QWEN_PLUS_STREAM": "false"},
    {"QWEN_PLUS_JSON_MODE": "maybe"},
    {"QWEN_PLUS_TIMEOUT_MS": "60001"},
    {"QWEN_PLUS_TIMEOUT_MS": "0"},
    {"QWEN_PLUS_TIMEOUT_MS": "1.5"},
    {"QWEN_PLUS_MAX_TOKENS": "8193"},
    {"QWEN_PLUS_MAX_TOKENS": "0"},
    {"QWEN_MODEL_STRATEGY": "task_split", "QWEN_PLUS_ENABLED": "false"},
    {"QWEN_MODEL_STRATEGY": "plus_only", "QWEN_PLUS_ENABLED": "false"},
    {"QWEN_MODEL_STRATEGY": "task_split", "QWEN_API_KEY": "", "QWEN_PLUS_API_KEY": "fixture-plus-key"},
    {"QWEN_MODEL_STRATEGY": "plus_only", "QWEN_API_KEY": "", "QWEN_PLUS_API_KEY": "fixture-plus-key"},
])
def test_invalid_profile_configuration_fails_closed_without_value_echo(change):
    with pytest.raises(ValueError) as error:
        configured(**change)
    assert "fixture-flash-secret" not in str(error.value)
    assert "private.invalid" not in str(error.value)


@pytest.mark.parametrize("setting,expected", [("omit", None), ("true", True), ("false", False)])
def test_thinking_three_states_are_preserved(setting, expected):
    plus = configured(QWEN_PLUS_MODEL="qwen-compatible-fixture", QWEN_PLUS_ENABLE_THINKING=setting)["profiles"]["plus"]
    assert plus.enableThinking is expected


async def test_streaming_thinking_collects_only_answer_and_actual_metadata():
    seen = []
    def handle(request):
        seen.append(json.loads(request.content))
        return response(valid_stream())
    opts = stream_options(handle)
    try:
        result = await createChatTransport(opts).complete({"prompt": "仅输出JSON", "input": {}, "maxTokens": 1200})
        body = seen[0]
        assert body["model"] == "qwen3.7-plus" and body["enable_thinking"] is True
        assert body["stream"] is True and body["stream_options"] == {"include_usage": True}
        assert body["response_format"] == {"type": "json_object"} and body["max_tokens"] == 8192
        assert json.loads(result["choices"][0]["message"]["content"]) == {"title": "中文😀"}
        assert result["usage"] == {"prompt_tokens": 7, "completion_tokens": 19, "total_tokens": 26}
        assert result["_transport"] == {"providerModel": "qwen3.7-plus-actual", "providerResponseId": "chatcmpl-fixture"}
        assert "private fixture thought" not in json.dumps(result)
    finally:
        await opts["client"].aclose()


async def test_omit_thinking_and_json_mode_off_do_not_sneak_back_into_payload():
    seen = []
    def handle(request):
        seen.append(json.loads(request.content))
        return response(valid_stream(usage=False))
    opts = stream_options(handle, enableThinking=None, jsonMode=False)
    try:
        result = await createChatTransport(opts).complete({"prompt": "仅输出JSON", "input": {}})
        assert "enable_thinking" not in seen[0] and "response_format" not in seen[0]
        assert result["usage"] is None
    finally:
        await opts["client"].aclose()


class ByteStream(httpx.AsyncByteStream):
    def __init__(self, content, *, wait=False):
        self.content = content
        self.wait = wait
        self.closed = False

    async def __aiter__(self):
        for byte in self.content:
            yield bytes([byte])
        if self.wait:
            await asyncio.sleep(60)

    async def aclose(self):
        self.closed = True


async def test_sse_utf8_and_crlf_split_across_every_byte_are_supported():
    stream = ByteStream(valid_stream().replace(b"\n", b"\r\n"))
    opts = stream_options(lambda _: httpx.Response(200, stream=stream, headers={"content-type": "text/event-stream"}))
    try:
        result = await createChatTransport(opts).complete({"prompt": "JSON", "input": {}})
        assert "中文😀" in result["choices"][0]["message"]["content"]
        assert stream.closed
    finally:
        await opts["client"].aclose()


@pytest.mark.parametrize("content,code", [
    (event("[DONE]"), "INVALID_MODEL_STREAM"),
    (event(delta("{}")), "INVALID_MODEL_STREAM"),
    (valid_stream().replace(b"data: [DONE]\n\n", b""), "INVALID_MODEL_STREAM"),
    (valid_stream()[:-1], "INVALID_MODEL_STREAM"),
    (valid_stream() + event(delta("extra")), "INVALID_MODEL_STREAM"),
    (event("not-json"), "INVALID_MODEL_STREAM"),
    (event('{"choices":[],"usage":{"prompt_tokens":NaN}}'), "INVALID_MODEL_STREAM"),
    (event({"error": {"message": "fixture-private-upstream-detail"}}), "INVALID_MODEL_STREAM"),
    (event({"choices": [{"index": 1, "delta": {"content": "{}"}}]}), "INVALID_MODEL_STREAM"),
    (event({"choices": [{"index": True, "delta": {"content": "{}"}}]}), "INVALID_MODEL_STREAM"),
    (event({"choices": [{"index": 0, "delta": {"content": {}}}]}), "INVALID_MODEL_STREAM"),
    (event(delta(tool_calls=[])), "INVALID_MODEL_STREAM"),
    (event(delta(refusal="fixture-private-upstream-detail")), "INVALID_MODEL_STREAM"),
    (event(delta(reasoning_content=[])), "INVALID_MODEL_STREAM"),
    (valid_stream().replace(b"chatcmpl-fixture", b"different-id", 1), "INVALID_MODEL_STREAM"),
    (b"event: unexpected\ndata: {}\n\n", "INVALID_MODEL_STREAM"),
    (b"data: \xff\n\n", "INVALID_MODEL_STREAM"),
    (valid_stream(finish="length"), "MODEL_OUTPUT_TRUNCATED"),
    (b" " * (RESPONSE_LIMIT + 1), "MODEL_RESPONSE_TOO_LARGE"),
])
async def test_malformed_incomplete_and_oversized_streams_rejected_without_retry(content, code):
    calls = []
    def handle(request):
        calls.append(request)
        return response(content)
    opts = stream_options(handle)
    try:
        with pytest.raises(ApiError) as error:
            await createChatTransport(opts).complete({"prompt": "JSON", "input": {}})
        assert error.value.code == code and len(calls) == 1
        assert "fixture-private" not in str(error.value)
    finally:
        await opts["client"].aclose()


async def test_stream_requires_sse_content_type():
    opts = stream_options(lambda _: httpx.Response(200, json={"choices": []}))
    try:
        with pytest.raises(ApiError) as error:
            await createChatTransport(opts).complete({"prompt": "JSON", "input": {}})
        assert error.value.code == "INVALID_MODEL_STREAM"
    finally:
        await opts["client"].aclose()


@pytest.mark.parametrize("kind", ["timeout", "signal", "task"])
async def test_waiting_sse_body_is_closed_on_timeout_or_cancel(kind):
    stream = ByteStream(event(delta(reasoning_content="private thought")), wait=True)
    opts = stream_options(
        lambda _: httpx.Response(200, stream=stream, headers={"content-type": "text/event-stream"}),
        timeoutMs=15 if kind == "timeout" else 1000,
    )
    signal = asyncio.Event()
    try:
        work = asyncio.create_task(createChatTransport(opts).complete({"prompt": "JSON", "input": {}}, {"signal": signal}))
        if kind != "timeout":
            await asyncio.sleep(.01)
            signal.set() if kind == "signal" else work.cancel()
        with pytest.raises(asyncio.CancelledError if kind == "task" else ApiError) as error:
            await work
        if kind != "task":
            assert error.value.code == ("MODEL_TIMEOUT" if kind == "timeout" else "REQUEST_CANCELLED")
        assert stream.closed
    finally:
        await opts["client"].aclose()


@pytest.mark.parametrize("model,identifier", [
    ("https://private.invalid/model", "sk-fixture-private-key"),
    ("model\nprivate", "Bearer-private"),
    ("a" * 201, "b" * 201),
    ("fixture-private-key", "fixture-private-key"),
])
async def test_untrusted_provider_metadata_never_enters_safe_transport_diagnostics(model, identifier):
    opts = stream_options(lambda _: httpx.Response(200, json={
        "model": model, "id": identifier,
        "choices": [{"finish_reason": "stop", "message": {"content": "{}", "reasoning_content": "private thinking"}}],
    }), stream=False, model="qwen-fixture", enableThinking=False)
    try:
        result = await createChatTransport(opts).complete({"prompt": "JSON", "input": {}})
        assert result["_transport"] == {"providerModel": None, "providerResponseId": None}
        assert result["model"] is None and result["id"] is None
        assert "reasoning_content" not in result["choices"][0]["message"]
    finally:
        await opts["client"].aclose()


@pytest.mark.parametrize("streaming", [False, True])
@pytest.mark.parametrize("field", ["model", "id"])
@pytest.mark.parametrize("value", [
    "cookie-session", "COOKIEcredentials", "token=fixture", "sk-fixture",
    "bearer-fixture", "token-fixture", "api-key-fixture",
])
async def test_optional_dirty_provider_metadata_is_dropped_without_failing_valid_content(streaming, field, value):
    from tango_backend.task_store import modelMetadataJson
    if streaming:
        original = b"qwen3.7-plus-actual" if field == "model" else b"chatcmpl-fixture"
        content = valid_stream().replace(original, value.encode("ascii"))
        handler = lambda _: response(content)
    else:
        envelope = {
            "model": "qwen3.7-plus-actual", "id": "chatcmpl-fixture",
            "choices": [{"finish_reason": "stop", "message": {"content": '{"ok":true}'}}],
        }
        envelope[field] = value
        handler = lambda _: httpx.Response(200, json=envelope)
    opts = stream_options(handler, stream=streaming, model="qwen-fixture", enableThinking=False)
    try:
        result = await createChatTransport(opts).complete({"prompt": "JSON", "input": {}})
        key = "providerModel" if field == "model" else "providerResponseId"
        assert result["_transport"][key] is None and result[field] is None
        assert isinstance(json.loads(result["choices"][0]["message"]["content"]), dict)
        # 确认可选脏字段不会在账本第二次验证时把正常内容误写成 failed。
        stored = json.loads(modelMetadataJson(result["_transport"]))
        assert key not in stored and len(stored) == 1
    finally:
        await opts["client"].aclose()


async def test_non_thinking_payload_keeps_requested_bound_and_cap():
    bodies = []
    def handle(request):
        bodies.append(json.loads(request.content))
        return httpx.Response(200, json={"choices": []})
    opts = stream_options(handle, model="qwen-fixture", stream=False, enableThinking=False, maxTokens=400)
    try:
        transport = createChatTransport(opts)
        await transport.complete({"prompt": "JSON", "input": {}, "maxTokens": 200})
        await transport.complete({"prompt": "JSON", "input": {}, "maxTokens": 1000})
        assert [body["max_tokens"] for body in bodies] == [200, 400]
    finally:
        await opts["client"].aclose()
