"""后台专用模型 HTTP：能力适配、有界 JSON/SSE、超时和取消；绝不自动重试。"""

import asyncio
import re

import httpx

from .compat import js_json, strict_json_loads
from .errors import ApiError
from .model_profiles import validatedTransportOptions
from .agent_tracing import current_span

RESPONSE_LIMIT = 256 * 1024
SSE_EVENT_LIMIT = 4096


def _invalidStream():
    raise ApiError(422, "INVALID_MODEL_STREAM", "AI 流式回复不完整或格式无效。")


def _safeProviderIdentifier(value):
    # 上游字段是不可信数据，不能原样当日志标签，更不能回传 URL、Key 或控制符。
    return value if (
        isinstance(value, str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,199}", value)
        and "://" not in value
        and not value.lower().startswith(("sk-", "bearer", "cookie", "token=", "token-", "api-key"))
    ) else None


def _metadata(envelope, apiKey):
    def identifier(value):
        safe = _safeProviderIdentifier(value)
        return None if safe and apiKey and apiKey in safe else safe

    return {
        "providerModel": identifier(envelope.get("model")),
        "providerResponseId": identifier(envelope.get("id")),
    }


class _SSEEnvelope:
    """只接 OpenAI 兼容单候选文本流；思考片段计入体积但不保存。"""

    def __init__(self):
        self.buffer = b""
        self.data = []
        self.event = None
        self.eventCount = 0
        self.done = False
        self.finished = False
        self.reason = None
        self.content = []
        self.usage = None
        self.provider = {}

    def feed(self, chunk):
        self.buffer += chunk
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            try:
                line = raw.removesuffix(b"\r").decode("utf-8", errors="strict")
            except UnicodeError:
                _invalidStream()
            if line == "":
                self.dispatch()
            elif line.startswith(":"):
                continue
            else:
                name, separator, value = line.partition(":")
                if not separator:
                    _invalidStream()
                value = value.removeprefix(" ")
                if name == "data":
                    self.data.append(value)
                elif name == "event" and value in ("message", "") and self.event is None:
                    self.event = value
                elif name == "id" and len(value) <= 200 and "\x00" not in value:
                    continue
                elif name == "retry" and value.isascii() and value.isdigit() and len(value) <= 8:
                    continue
                else:
                    _invalidStream()

    def dispatch(self):
        if not self.data:
            self.event = None
            return
        self.eventCount += 1
        if self.done or self.eventCount > SSE_EVENT_LIMIT:
            _invalidStream()
        data = "\n".join(self.data)
        self.data = []
        self.event = None
        if data == "[DONE]":
            if not self.finished:
                _invalidStream()
            self.done = True
            return
        try:
            value = strict_json_loads(data)
        except (ValueError, UnicodeError, RecursionError):
            _invalidStream()
        if not isinstance(value, dict) or "error" in value:
            _invalidStream()
        for key in ("id", "model"):
            if key in value:
                raw = value[key]
                if not isinstance(raw, str) or len(raw) > 200:
                    _invalidStream()
                if key in self.provider and self.provider[key] != raw:
                    _invalidStream()
                self.provider[key] = raw
        usage = value.get("usage")
        if usage is not None:
            if not isinstance(usage, dict):
                _invalidStream()
            # 缺失 usage 保持 None；不得把未报告 Token 当成零消耗。
            self.usage = {
                k: usage[k] for k in ("prompt_tokens", "completion_tokens", "total_tokens")
                if isinstance(usage.get(k), int) and not isinstance(usage[k], bool)
                and 0 <= usage[k] <= 9007199254740991
            }
        choices = value.get("choices")
        if not isinstance(choices, list):
            _invalidStream()
        if not choices:
            if not self.finished or usage is None:
                _invalidStream()
            return
        if self.finished or len(choices) != 1 or not isinstance(choices[0], dict):
            _invalidStream()
        choice = choices[0]
        if choice.get("index") != 0 or isinstance(choice.get("index"), bool):
            _invalidStream()
        delta = choice.get("delta")
        if not isinstance(delta, dict) or delta.get("role") not in (None, "assistant"):
            _invalidStream()
        if any(delta.get(key) is not None for key in ("tool_calls", "function_call", "refusal")):
            _invalidStream()
        if delta.get("reasoning_content") is not None and not isinstance(delta["reasoning_content"], str):
            _invalidStream()
        content = delta.get("content")
        if content is not None:
            if not isinstance(content, str):
                _invalidStream()
            self.content.append(content)
        reason = choice.get("finish_reason")
        if reason is not None:
            if reason not in ("stop", "length", "content_filter", "tool_calls", "function_call"):
                _invalidStream()
            self.finished = True
            self.reason = reason

    def envelope(self):
        if self.buffer or self.data or not self.done or not self.finished:
            _invalidStream()
        if self.reason == "length":
            raise ApiError(422, "MODEL_OUTPUT_TRUNCATED", "AI 回复达到输出上限，未采用不完整结果。")
        return {
            **self.provider,
            "choices": [{"index": 0, "finish_reason": self.reason,
                         "message": {"role": "assistant", "content": "".join(self.content)}}],
            "usage": self.usage,
        }


class ChatTransport:
    def __init__(self, options=None, **kwargs):
        opts = validatedTransportOptions({**(options or {}), **kwargs})
        self.model = opts["model"]
        self.apiKey = opts["apiKey"]
        self.endpoint = opts["baseUrl"] + "/chat/completions" if self.apiKey else None
        self.configured = bool(self.endpoint)
        self.timeout = opts["timeoutMs"] / 1000
        self.maxTokens = opts["maxTokens"]
        self.enableThinking = opts["enableThinking"]
        self.jsonMode = opts["jsonMode"]
        self.stream = opts["stream"]
        self.client = opts.get("client")

    async def complete(self, payload, requestOptions=None):
        with current_span(
            "model.request", run_type="llm",
            metadata={"model": self.model, "ls_model_name": self.model, "ls_provider": "dashscope"},
        ) as span:
            result = await self._complete(payload, requestOptions)
            if isinstance(result, dict):
                span.metadata["actual_model"] = (result.get("_transport") or {}).get("providerModel")
                usage = result.get("usage") or {}
                values = [usage.get("prompt_tokens"), usage.get("completion_tokens")]
                # 缺失、局部或非法用量保持未知，不为追踪面板捏造零消耗。
                if all(isinstance(v, int) and not isinstance(v, bool) and v >= 0 for v in values):
                    known_usage = {
                        "input_tokens": values[0], "output_tokens": values[1], "total_tokens": sum(values),
                    }
                    span.outputs["usage_metadata"] = known_usage
                    span.metadata["usage_metadata"] = known_usage
            return result

    async def _complete(self, payload, requestOptions=None):
        if not self.configured:
            raise ApiError(
                503, "MODEL_NOT_CONFIGURED", "管理员尚未配置 AI 服务，请暂时使用表单。"
            )
        signal = (requestOptions or {}).get("signal")
        if signal and signal.is_set():
            raise ApiError(499, "REQUEST_CANCELLED", "请求已取消。")
        requestedTokens = payload.get("maxTokens", 1500)
        if not isinstance(requestedTokens, int) or isinstance(requestedTokens, bool) or requestedTokens < 1:
            raise ValueError("Invalid maxTokens request.")
        # 思考和答案共享上游 max_tokens；复杂模型使用管理员配置的总上限。
        forcedThinking = self.model == "qwen3.7-plus" or self.model.startswith("qwen3.7-plus-")
        maxTokens = self.maxTokens if self.enableThinking is True or forcedThinking else min(requestedTokens, self.maxTokens)
        body = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": payload["prompt"]},
                {"role": "user", "content": js_json(payload["input"])},
            ],
            "max_tokens": maxTokens,
            "stream": self.stream,
        }
        if self.jsonMode:
            body["response_format"] = {"type": "json_object"}
        if payload.get("outputContract") is not None:
            from .route_contract import ROUTE_OUTPUT_CONTRACT, routeResponseFormat
            if payload["outputContract"] != ROUTE_OUTPUT_CONTRACT:
                raise ValueError("Unknown model output contract")
            structured = routeResponseFormat(self.model, self.jsonMode)
            if structured is not None:
                body["response_format"] = structured
        if self.enableThinking is not None:
            body["enable_thinking"] = self.enableThinking
        if self.stream:
            body["stream_options"] = {"include_usage": True}

        async def operation():
            # 每次请求有自己的资源生命周期，取消即关闭响应；不自动重试计费请求。
            owned = self.client is None
            client = self.client or httpx.AsyncClient(
                follow_redirects=False, trust_env=False
            )
            try:
                async with client.stream(
                    "POST",
                    self.endpoint,
                    headers={
                        "Content-Type": "application/json",
                        "Authorization": "Bearer " + self.apiKey,
                    },
                    content=js_json(body).encode("utf-8"),
                    follow_redirects=False,
                ) as response:
                    if not 200 <= response.status_code < 300:
                        raise ApiError(
                            502,
                            "MODEL_UNAVAILABLE",
                            "AI 服务暂时不可用，请稍后重试或使用表单。",
                        )
                    length = response.headers.get("content-length", "0")
                    if length.isdigit() and int(length) > RESPONSE_LIMIT:
                        raise ApiError(
                            502, "MODEL_RESPONSE_TOO_LARGE", "AI 回复过大，已停止解析。"
                        )
                    if self.stream and response.headers.get("content-type", "").split(";", 1)[0].strip().lower() != "text/event-stream":
                        _invalidStream()
                    chunks, size = [], 0
                    parser = _SSEEnvelope() if self.stream else None
                    async for chunk in response.aiter_bytes():
                        size += len(chunk)
                        if size > RESPONSE_LIMIT:
                            raise ApiError(
                                502,
                                "MODEL_RESPONSE_TOO_LARGE",
                                "AI 回复过大，已停止解析。",
                            )
                        if parser is not None:
                            parser.feed(chunk)
                        else:
                            chunks.append(chunk)
                    if parser is not None:
                        result = parser.envelope()
                    else:
                        try:
                            result = strict_json_loads(b"".join(chunks))
                        except (ValueError, UnicodeError, RecursionError):
                            raise ApiError(
                                422, "INVALID_MODEL_RESPONSE", "AI 服务未返回有效 JSON。"
                            ) from None
                    if isinstance(result, dict):
                        result["_transport"] = _metadata(result, self.apiKey)
                        # 原字段也只留下筛选后的标识，避免以后新增调用方误记原始上游字段。
                        if "model" in result:
                            result["model"] = result["_transport"]["providerModel"]
                        if "id" in result:
                            result["id"] = result["_transport"]["providerResponseId"]
                        # 不带内部推理内容到业务层；工具/拒答字段保留供契约拒绝。
                        for choice in result.get("choices", []) if isinstance(result.get("choices"), list) else []:
                            if isinstance(choice, dict) and isinstance(choice.get("message"), dict):
                                choice["message"].pop("reasoning_content", None)
                    return result
            finally:
                if owned:
                    await client.aclose()

        work = asyncio.create_task(operation())
        cancelled = asyncio.create_task(signal.wait()) if signal else None
        try:
            done, _ = await asyncio.wait(
                [work] + ([cancelled] if cancelled else []),
                timeout=self.timeout,
                return_when=asyncio.FIRST_COMPLETED,
            )
            if cancelled and cancelled in done:
                raise ApiError(499, "REQUEST_CANCELLED", "请求已取消。")
            if work not in done:
                raise ApiError(
                    504, "MODEL_TIMEOUT", "AI 解析超时，请稍后重试或使用表单。"
                )
            return await work
        except ApiError:
            raise
        except asyncio.CancelledError:
            raise
        except Exception:
            raise ApiError(
                502, "MODEL_UNAVAILABLE", "无法连接 AI 服务，请稍后重试或使用表单。"
            ) from None
        finally:
            work.cancel()
            if cancelled:
                cancelled.cancel()
            await asyncio.gather(
                work, *([cancelled] if cancelled else []), return_exceptions=True
            )


def createChatTransport(options=None, **kwargs):
    return ChatTransport(options, **kwargs)
