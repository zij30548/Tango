"""共享限流依赖 Redis 原子 Lua；故障时拒绝写入和模型调用。"""

import asyncio
import hashlib
import re
from .errors import ApiError
from .compat import now_ms

TAKE = """-- tango_rate_take_v1
local limit = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local count = tonumber(redis.call('GET', KEYS[1]) or '0')
if count == nil or count < 0 then return redis.error_reply('invalid rate counter') end
local ttl = redis.call('PTTL', KEYS[1])
if count >= limit then
  if ttl < 1 then redis.call('PEXPIRE', KEYS[1], window); ttl = window end
  return {0, ttl}
end
count = redis.call('INCR', KEYS[1])
if ttl < 1 then redis.call('PEXPIRE', KEYS[1], window); ttl = window end
return {1, ttl}"""
ALLOWS = """-- tango_rate_allows_v1
local count = tonumber(redis.call('GET', KEYS[1]) or '0')
if count == nil or count < 0 then return redis.error_reply('invalid rate counter') end
local ttl = redis.call('PTTL', KEYS[1])
if count > 0 and ttl < 1 then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
if count < tonumber(ARGV[1]) then return 1 else return 0 end"""


def unavailable():
    return ApiError(
        503, "RATE_LIMIT_UNAVAILABLE", "请求保护服务暂时不可用，请稍后重试。"
    )


def policy(scope, value):
    if (
        not isinstance(scope, str)
        or not re.fullmatch("[a-z][a-z0-9_]{0,47}", scope)
        or not isinstance(value, dict)
        or any(
            type(value.get(k)) is not int or value[k] < 1
            for k in ["maximum", "periodMs"]
        )
    ):
        raise ValueError("Invalid rate limiter policy.")
    return value["maximum"], value["periodMs"]


class RedisWindow:
    def __init__(self, owner, scope, maximum, periodMs):
        self.owner, self.scope, self.maximum, self.periodMs = (
            owner,
            scope,
            maximum,
            periodMs,
        )

    def key(self, identity):
        if (
            not isinstance(identity, str)
            or not identity
            or len(identity.encode("utf-16-le")) // 2 > 1024
        ):
            raise unavailable()
        return (
            self.owner.namespace
            + ":rate:"
            + self.scope
            + ":"
            + hashlib.sha256(identity.encode()).hexdigest()
        )

    async def _run(self, action):
        try:
            return await asyncio.wait_for(action(), self.owner.commandTimeoutMs / 1000)
        except Exception:
            raise unavailable() from None

    async def take(self, identity, now=None):
        async def action():
            result = await self.owner.client.eval(
                TAKE, 1, self.key(identity), str(self.maximum), str(self.periodMs)
            )
            if (
                not isinstance(result, (list, tuple))
                or len(result) != 2
                or result[0] not in [0, 1]
                or type(result[1]) is not int
                or result[1] < 1
            ):
                raise unavailable()
            return result[0] == 1

        return await self._run(action)

    async def allows(self, identity, now=None):
        async def action():
            result = await self.owner.client.eval(
                ALLOWS, 1, self.key(identity), str(self.maximum), str(self.periodMs)
            )
            if result not in [0, 1]:
                raise unavailable()
            return result == 1

        return await self._run(action)

    async def clear(self, identity):
        async def action():
            result = await self.owner.client.delete(self.key(identity))
            if result not in [0, 1]:
                raise unavailable()

        return await self._run(action)


class RedisRateLimiter:
    def __init__(self, options=None, **kwargs):
        value = {**(options or {}), **kwargs}
        self.client, self.namespace = value.get("client"), value.get("namespace")
        self.commandTimeoutMs = value.get("commandTimeoutMs", 1500)
        if (
            self.client is None
            or not isinstance(self.namespace, str)
            or not re.fullmatch("[A-Za-z0-9][A-Za-z0-9:_-]{0,79}", self.namespace)
            or type(self.commandTimeoutMs) is not int
            or not 1 <= self.commandTimeoutMs <= 60000
        ):
            raise ValueError(
                "Redis rate limiter requires a client and an explicit valid namespace."
            )
        self.policies = {}

    def window(self, scope, value):
        maximum, period = policy(scope, value)
        signature = (maximum, period)
        if scope in self.policies and self.policies[scope] != signature:
            raise ValueError("Conflicting rate limiter policy.")
        self.policies[scope] = signature
        return RedisWindow(self, scope, maximum, period)


class MemoryWindow:
    def __init__(self, maximum, periodMs):
        self.maximum, self.periodMs, self.entries = maximum, periodMs, {}

    async def allows(self, key, now=None):
        now = now_ms() if now is None else now
        entry = self.entries.get(key)
        if entry and entry["expires"] <= now:
            del self.entries[key]
            entry = None
        if not entry:
            if len(self.entries) >= 10000:
                self.entries = {
                    k: v for k, v in self.entries.items() if v["expires"] > now
                }
                if len(self.entries) >= 10000:
                    return False
            return True
        return entry["count"] < self.maximum

    async def take(self, key, now=None):
        now = now_ms() if now is None else now
        if not await self.allows(key, now):
            return False
        entry = self.entries.setdefault(
            key, {"count": 0, "expires": now + self.periodMs}
        )
        entry["count"] += 1
        return True

    async def clear(self, key):
        self.entries.pop(key, None)


class MemoryRateLimiter:
    def __init__(self):
        self.windows = {}

    def window(self, scope, value):
        maximum, period = policy(scope, value)
        if scope in self.windows:
            current = self.windows[scope]
            if current.maximum != maximum or current.periodMs != period:
                raise ValueError("Conflicting rate limiter policy.")
            return current
        self.windows[scope] = MemoryWindow(maximum, period)
        return self.windows[scope]
