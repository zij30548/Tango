"""后台模型配置与能力声明；不读文件、不发请求，公开描述不含凭据和端点。"""

from dataclasses import dataclass, field
import hashlib
import json
import re
from urllib.parse import urlsplit, urlunsplit


DEFAULT_BASE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1"
DEFAULT_FLASH_MODEL = "qwen3.7-flash-2026-07-15"
MODEL_TIMEOUT_LIMIT_MS = 60000
MODEL_TOKEN_LIMIT = 8192
MODEL_STRATEGIES = frozenset(("flash_only", "task_split", "plus_only"))


def _integer(value, name, default, minimum, maximum):
    if value is None or value == "":
        return default
    if isinstance(value, bool):
        raise ValueError("Invalid " + name + " configuration.")
    try:
        parsed = int(value)
        if str(parsed) != str(value).strip() or not minimum <= parsed <= maximum:
            raise ValueError()
        return parsed
    except (ValueError, TypeError, OverflowError):
        raise ValueError("Invalid " + name + " configuration.") from None


def _boolean(value, name, default):
    if value is None or value == "":
        return default
    if value is True or value == "true":
        return True
    if value is False or value == "false":
        return False
    raise ValueError("Invalid " + name + " configuration; use true or false.")


def _thinking(value, name, default):
    if value is None or value == "":
        return default
    if value == "omit":
        return None
    return _boolean(value, name, default)


def validateModelId(value, *, allowEmpty=False):
    # 型号是标识符，拒绝控制符、空白及把整条 URL 误填到型号的情况。
    if allowEmpty and value == "":
        return value
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,199}", value
    ) or "://" in value:
        raise ValueError("Invalid QWEN_MODEL configuration.")
    return value


def validateApiKey(value):
    if not isinstance(value, str) or (
        value and not re.fullmatch(r"[\x21-\x7e]{1,4096}", value)
    ):
        raise ValueError("Invalid QWEN_API_KEY format.")
    return value


def validateBaseUrl(value):
    try:
        if not isinstance(value, str) or re.search(r"[\s\x00-\x1f\x7f]", value):
            raise ValueError()
        url = urlsplit(value)
        port = url.port
        local = url.hostname in ("127.0.0.1", "localhost", "::1")
        if (
            (url.scheme != "https" and not (url.scheme == "http" and local))
            or not url.hostname
            or url.username
            or url.password
            or url.query
            or url.fragment
            or not re.fullmatch(r"(?:/[A-Za-z0-9_-]+)*/?", url.path)
            or url.path.rstrip("/").endswith("/chat/completions")
        ):
            raise ValueError()
        host = url.hostname.lower()
        if ":" in host:
            host = "[" + host + "]"
        defaultPort = 443 if url.scheme == "https" else 80
        netloc = host + (":" + str(port) if port is not None and port != defaultPort else "")
        return urlunsplit((url.scheme, netloc, url.path.rstrip("/"), "", ""))
    except (ValueError, TypeError, AttributeError):
        raise ValueError(
            "QWEN_BASE_URL must be an HTTPS API base URL; only loopback permits HTTP."
        ) from None


def validatedTransportOptions(options=None):
    """所有适配器共用同一验证；错误信息只包含配置名，不回显输入。"""
    opts = dict(options or {})
    model = validateModelId(opts.get("model", DEFAULT_FLASH_MODEL))
    key = validateApiKey(opts.get("apiKey", ""))
    base = validateBaseUrl(opts.get("baseUrl", "")) if key else None
    forcedThinking = model == "qwen3.7-plus" or model.startswith("qwen3.7-plus-")
    thinking = opts.get("enableThinking", True if forcedThinking else False if model.lower().startswith("qwen") else None)
    if thinking is not None and not isinstance(thinking, bool):
        raise ValueError("Invalid enableThinking configuration.")
    structured = _boolean(opts.get("jsonMode"), "jsonMode", True)
    stream = _boolean(opts.get("stream"), "stream", forcedThinking)
    if forcedThinking and thinking is False:
        raise ValueError("This QWEN_MODEL requires thinking mode.")
    if (thinking is True or forcedThinking) and not stream:
        raise ValueError("Thinking models require stream=true in this adapter.")
    return {
        **opts,
        "model": model,
        "apiKey": key,
        "baseUrl": base,
        "enableThinking": thinking,
        "jsonMode": structured,
        "stream": stream,
        "timeoutMs": _integer(opts.get("timeoutMs"), "timeoutMs", 15000, 1, MODEL_TIMEOUT_LIMIT_MS),
        "maxTokens": _integer(opts.get("maxTokens"), "maxTokens", 4096, 1, MODEL_TOKEN_LIMIT),
    }


@dataclass(frozen=True)
class ModelProfile:
    profile: str
    model: str
    enabled: bool
    apiKey: str = field(repr=False)
    baseUrl: str = field(repr=False)
    enableThinking: bool | None = False
    jsonMode: bool = True
    stream: bool = False
    timeoutMs: int = 15000
    maxTokens: int = 4096

    @property
    def configured(self):
        return bool(self.enabled and self.model and self.apiKey and self.baseUrl)

    def publicDescriptor(self):
        # 指纹只描述运行配置，轮换 Key 不改变任务策略；Key 从不参与可公开摘要。
        endpointId = hashlib.sha256(self.baseUrl.encode("utf-8")).hexdigest()
        value = {
            "profile": self.profile,
            "model": self.model,
            "enabled": self.enabled,
            "configured": self.configured,
            "endpointId": endpointId,
            "enableThinking": self.enableThinking,
            "jsonMode": self.jsonMode,
            "stream": self.stream,
            "timeoutMs": self.timeoutMs,
            "maxTokens": self.maxTokens,
        }
        value["fingerprint"] = hashlib.sha256(
            json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
        ).hexdigest()
        return value

    def transportOptions(self):
        return {
            "model": self.model,
            "apiKey": self.apiKey if self.enabled else "",
            "baseUrl": self.baseUrl,
            "enableThinking": self.enableThinking,
            "jsonMode": self.jsonMode,
            "stream": self.stream,
            "timeoutMs": self.timeoutMs,
            "maxTokens": self.maxTokens,
        }


def modelProfilesConfiguration(env):
    """仅由后台注入环境；配置 Plus 与启用 Plus 是两个独立动作。"""
    strategy = env.get("QWEN_MODEL_STRATEGY", "flash_only")
    if strategy not in MODEL_STRATEGIES:
        raise ValueError("Invalid QWEN_MODEL_STRATEGY configuration.")
    profiles = {}
    for name, prefix in (("flash", "QWEN_"), ("plus", "QWEN_PLUS_")):
        plus = name == "plus"
        enabled = _boolean(env.get(prefix + "ENABLED"), prefix + "ENABLED", not plus)
        model = validateModelId(
            env.get(prefix + "MODEL", "" if plus else DEFAULT_FLASH_MODEL),
            allowEmpty=not enabled,
        )
        rawBase = env.get(prefix + "BASE_URL")
        base = validateBaseUrl(
            profiles["flash"].baseUrl if plus and rawBase in (None, "")
            else DEFAULT_BASE_URL if rawBase is None else rawBase
        )
        rawKey = env.get(prefix + "API_KEY")
        if plus and rawKey in (None, ""):
            sameEndpoint = base == profiles["flash"].baseUrl
            # 另一平台/地域必须显式指定凭据，不能把原 Key 隐式带到新端点。
            key = profiles["flash"].apiKey if sameEndpoint else ""
            if enabled and not sameEndpoint:
                raise ValueError("A different QWEN_PLUS_BASE_URL requires an explicit QWEN_PLUS_API_KEY.")
        else:
            key = validateApiKey("" if rawKey is None else rawKey)
        forcedThinking = model == "qwen3.7-plus" or model.startswith("qwen3.7-plus-")
        thinking = _thinking(env.get(prefix + "ENABLE_THINKING"), prefix + "ENABLE_THINKING", True if forcedThinking else None if plus else False)
        stream = _boolean(env.get(prefix + "STREAM"), prefix + "STREAM", plus)
        if forcedThinking and thinking is False:
            raise ValueError("This QWEN_MODEL requires thinking mode.")
        if (thinking is True or forcedThinking) and not stream:
            raise ValueError("Thinking models require stream=true in this adapter.")
        profile = ModelProfile(
            profile=name, model=model, enabled=enabled, apiKey=key, baseUrl=base,
            enableThinking=thinking,
            jsonMode=_boolean(env.get(prefix + "JSON_MODE"), prefix + "JSON_MODE", True),
            stream=stream,
            timeoutMs=_integer(env.get(prefix + "TIMEOUT_MS"), prefix + "TIMEOUT_MS", 15000, 1, MODEL_TIMEOUT_LIMIT_MS),
            maxTokens=_integer(env.get(prefix + "MAX_TOKENS"), prefix + "MAX_TOKENS", 8192 if plus else 4096, 1, MODEL_TOKEN_LIMIT),
        )
        if enabled and plus and not profile.configured:
            raise ValueError("Enabled QWEN_PLUS requires model, API key and base URL.")
        profiles[name] = profile
    if strategy != "flash_only" and not profiles["plus"].configured:
        raise ValueError("Selected model strategy requires an enabled Plus profile.")
    if strategy != "flash_only" and not profiles["flash"].configured:
        raise ValueError("Selected model strategy requires a configured Flash profile.")
    return {"profiles": profiles, "strategy": strategy}
