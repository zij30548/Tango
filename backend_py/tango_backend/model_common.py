"""严格的模型输入输出检查，保留跨语言协议中的字符串长度与整数限制。"""

import json
from importlib.resources import files

from .compat import utf16_len, strict_json_loads
from .errors import ApiError

PROMPTS = json.loads(files("tango_backend").joinpath("prompts.json").read_text("utf-8"))


def exact(value, fields):
    return isinstance(value, dict) and set(value) == set(fields)


def text(value, maximum, required=False):
    return (
        isinstance(value, str)
        and utf16_len(value) <= maximum
        and (not required or bool(value.strip()))
    )


def strings(value, maximumItems, maximumLength, unique=False):
    return (
        isinstance(value, list)
        and len(value) <= maximumItems
        and all(text(v, maximumLength, True) for v in value)
        and (not unique or len({v.strip().lower() for v in value}) == len(value))
    )


def envelope_json(envelope, code, message):
    def invalid():
        return ApiError(422, code, message)

    choices = envelope.get("choices") if isinstance(envelope, dict) else None
    if (
        not isinstance(choices, list)
        or len(choices) != 1
        or not isinstance(choices[0], dict)
    ):
        raise invalid()
    choice = choices[0]
    msg = choice.get("message")
    if (
        choice.get("finish_reason") != "stop"
        or not isinstance(msg, dict)
        or not isinstance(msg.get("content"), str)
        or "tool_calls" in msg
        or "function_call" in msg
        or msg.get("refusal") is not None
    ):
        raise invalid()
    try:
        return strict_json_loads(msg["content"])
    except (ValueError, RecursionError):
        raise invalid() from None
