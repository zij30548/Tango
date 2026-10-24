"""统一路由契约的模拟HTTP、权限与诊断回归；不访问真实模型。"""

import json

import httpx
import pytest

from tango_backend.errors import ApiError
from tango_backend.intent_routing import PROMPT, RoutingModel
from tango_backend.model_transport import createChatTransport
from tango_backend.route_contract import (
    ROUTE_OUTPUT_CONTRACT, ROUTE_OUTPUT_SCHEMA, routeResponseFormat,
    routeSchemaPrompt, routeStructureDiagnostic,
)


@pytest.mark.parametrize("model", ["qwen3.7-flash-2026-07-15", "qwen3.7-plus", "qwen3.7-max-latest", "qwen3.8-flash", "qwen3.8-max-2026-09-22"])
def test_known_model_uses_isolated_strict_schema(model):
    first = routeResponseFormat(model)
    assert first == {"type": "json_schema", "json_schema": {
        "name": ROUTE_OUTPUT_CONTRACT, "strict": True, "schema": ROUTE_OUTPUT_SCHEMA}}
    first["json_schema"]["schema"]["properties"].clear()
    assert routeResponseFormat(model)["json_schema"]["schema"] == ROUTE_OUTPUT_SCHEMA


@pytest.mark.parametrize("model", ["qwen-fixture", "qwen-turbo", "qwen-plus", "qwen3.7-plus-arbitrary", "prefix-qwen3.7-flash", "qwen3.8-plus", "qwen3.7-flash-2026-07-15\n", None])
def test_unknown_model_does_not_claim_schema_support(model):
    assert routeResponseFormat(model) is None


def test_schema_and_prompt_share_one_contract_without_unsupported_keywords():
    assert PROMPT.count(routeSchemaPrompt()) == 1
    assert json.loads(routeSchemaPrompt()) == ROUTE_OUTPUT_SCHEMA

    def visit(value):
        if isinstance(value, dict):
            assert "uniqueItems" not in value
            if value.get("type") == "object":
                assert value["additionalProperties"] is False
            for item in value.values():
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)
    visit(ROUTE_OUTPUT_SCHEMA)


@pytest.mark.parametrize("model,contract,json_mode,expected", [
    ("qwen3.7-flash-2026-07-15", ROUTE_OUTPUT_CONTRACT, True, "json_schema"),
    ("qwen3.7-plus", ROUTE_OUTPUT_CONTRACT, True, "json_schema"),
    ("qwen-fixture", ROUTE_OUTPUT_CONTRACT, True, "json_object"),
    ("qwen3.7-plus", None, True, "json_object"),
    ("qwen3.7-plus", ROUTE_OUTPUT_CONTRACT, False, None),
])
async def test_only_routing_request_changes_http_format(model, contract, json_mode, expected):
    seen = []
    def handle(request):
        seen.append(json.loads(request.content))
        if seen[-1].get("stream"):
            event = {"choices": [{"index": 0, "delta": {"content": "{}"}, "finish_reason": "stop"}]}
            return httpx.Response(200, headers={"content-type": "text/event-stream"},
                                  text="data: " + json.dumps(event) + "\n\ndata: [DONE]\n\n")
        return httpx.Response(200, json={"choices": []})
    async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
        transport = createChatTransport({"baseUrl": "http://127.0.0.1:9/v1", "apiKey": "fixture-only-key",
            "model": model, "jsonMode": json_mode, "maxTokens": 2400, "client": client})
        payload = {"prompt": "只输出JSON", "input": {}, "maxTokens": 2200}
        if contract:
            payload["outputContract"] = contract
        await transport.complete(payload)
    assert len(seen) == 1
    assert seen[0].get("response_format", {}).get("type") == expected
    assert seen[0]["max_tokens"] <= 2400
    assert "outputContract" not in seen[0]


async def test_provider_rejection_does_not_retry_or_change_contract():
    seen = []
    def handle(request):
        seen.append(json.loads(request.content))
        return httpx.Response(400, json={"error": "private-provider-value"})
    async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
        transport = createChatTransport({"baseUrl": "http://127.0.0.1:9/v1", "apiKey": "fixture-only-key",
            "model": "qwen3.7-flash-2026-07-15", "client": client})
        with pytest.raises(ApiError) as error:
            await transport.complete({"prompt": "JSON", "input": {}, "outputContract": ROUTE_OUTPUT_CONTRACT})
    assert error.value.code == "MODEL_UNAVAILABLE"
    assert "private-provider-value" not in str(error.value)
    assert len(seen) == 1 and seen[0]["response_format"]["type"] == "json_schema"


async def test_unknown_contract_is_rejected_before_any_http():
    seen = []
    def handle(request):
        seen.append(request)
        return httpx.Response(200, json={})
    async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
        transport = createChatTransport({"baseUrl": "http://127.0.0.1:9/v1", "apiKey": "fixture-only-key", "client": client})
        with pytest.raises(ValueError):
            await transport.complete({"prompt": "JSON", "input": {}, "outputContract": "unknown"})
    assert seen == []


def test_rejection_diagnostic_contains_counts_without_untrusted_keys_or_values():
    value = {"intents": [], "actions": [{"private-key-in-action": "private-value"}], "missingSlots": [],
             "slots": {"artist": 4, "targetMinutes": True, "private-slot-key": "private-slot-value"},
             "private-root-key": "private-root-value"}
    result = routeStructureDiagnostic(value)
    assert result == {"invalidRoot": 0, "missingRootFields": 0, "extraRootFields": 1,
                      "missingSlotFields": 0, "extraSlotFields": 1, "invalidSlotTypes": 2,
                      "invalidActionShapes": 1}
    assert "private" not in json.dumps(result)
    assert all(type(v) is int and 0 <= v <= 100 for v in result.values())


@pytest.mark.parametrize("value", [None, [], "private-input", 4, False])
def test_nonobject_diagnostic_remains_fixed_numeric(value):
    result = routeStructureDiagnostic(value)
    assert result["invalidRoot"] == 1
    assert set(result) == set(routeStructureDiagnostic({}))
    assert all(type(v) is int for v in result.values())


@pytest.mark.parametrize("reason", ["length", "content_filter", "tool_calls"])
def test_structured_output_does_not_accept_incomplete_envelopes(reason):
    model = RoutingModel({"model": "qwen-fixture"})
    with pytest.raises(ApiError) as error:
        model.parse({"choices": [{"finish_reason": reason, "message": {"content": "{}"}}]})
    assert error.value.code == "INVALID_ROUTE_PLAN"
