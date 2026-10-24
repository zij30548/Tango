"""迁移协议工具：保留旧摘要、UTC 时间和 Qt UTF-16 字符长度语义。"""

import copy
import hashlib
import json
import math
import time
from decimal import Decimal
from datetime import datetime, timezone


def now_ms():
    return int(time.time() * 1000)


def iso(value=None):
    value = now_ms() if value is None else value
    return (
        datetime.fromtimestamp(value / 1000, timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def clone(value):
    return copy.deepcopy(value)


def utf16_len(value):
    return len(value.encode("utf-16-le", errors="surrogatepass")) // 2


def utf16_slice(value, maximum):
    # 不产生孤立代理项，便于 JSON 和 Qt 正确接收四字节字符。
    return value.encode("utf-16-le", errors="surrogatepass")[: maximum * 2].decode(
        "utf-16-le", errors="ignore"
    )


def well_formed(value):
    return isinstance(value, str) and not any(0xD800 <= ord(c) <= 0xDFFF for c in value)


def safe_integer(value):
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and abs(value) <= 9007199254740991
        and math.isfinite(value)
        and int(value) == value
    )


def strict_json_loads(value):
    def invalid_constant(_value):
        raise ValueError("JSON 不允许非有限数值")

    return json.loads(value, parse_constant=invalid_constant)


def js_truthy(value):
    # JS 中空对象、空数组也是真值，不能用 Python 的容器真值替代协议检查。
    return (
        value is not None
        and value is not False
        and not (
            isinstance(value, (int, float))
            and (value == 0 or isinstance(value, float) and math.isnan(value))
        )
        and value != ""
    )


def js_json(value, *, sort_keys=False):
    # 旧账本摘要使用JSON.stringify：须保留数值表示、UTF-16排序与整数属性枚举顺序。
    def string(item):
        encoded = json.dumps(item, ensure_ascii=False, separators=(",", ":"))
        return "".join(
            f"\\u{ord(c):04x}" if 0xD800 <= ord(c) <= 0xDFFF else c for c in encoded
        )

    def number(item):
        if isinstance(item, int) and abs(item) <= 9007199254740991:
            return str(item)
        item = float(item)
        if not math.isfinite(item):
            return "null"
        if item == 0:
            return "0"
        raw = repr(item).lower()
        if 1e-6 <= abs(item) < 1e21:
            fixed = format(Decimal(raw), "f")
            return fixed.rstrip("0").rstrip(".") if "." in fixed else fixed
        mantissa, _, exponent = raw.partition("e")
        if not exponent:
            return raw
        return (
            mantissa.removesuffix(".0")
            + "e"
            + ("+" if int(exponent) >= 0 else "-")
            + str(abs(int(exponent)))
        )

    def encode(item):
        if item is None:
            return "null"
        if isinstance(item, bool):
            return "true" if item else "false"
        if isinstance(item, str):
            return string(item)
        if isinstance(item, (int, float)):
            return number(item)
        if isinstance(item, (list, tuple)):
            return "[" + ",".join(encode(v) for v in item) + "]"
        if isinstance(item, dict):
            keys = list(item)
            if not all(isinstance(k, str) for k in keys):
                raise TypeError("JSON对象键必须是字符串")
            if sort_keys:
                keys.sort(key=lambda k: k.encode("utf-16-be", errors="surrogatepass"))
            indices = sorted(
                (
                    k
                    for k in keys
                    if k.isascii()
                    and k.isdigit()
                    and str(int(k)) == k
                    and int(k) < 4294967295
                ),
                key=int,
            )
            index_set = set(indices)
            keys = indices + [k for k in keys if k not in index_set]
            return "{" + ",".join(string(k) + ":" + encode(item[k]) for k in keys) + "}"
        raise TypeError("仅支持JSON数据")

    return encode(value)


def digest(value):
    return hashlib.sha256(js_json(value, sort_keys=True).encode("utf-8")).hexdigest()


async def maybe_await(value):
    import inspect

    return await value if inspect.isawaitable(value) else value
