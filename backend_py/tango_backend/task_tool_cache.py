"""只缓存经校验的场景曲库元数据；Redis 失败不影响权威任务状态。"""

import asyncio
import hashlib
import json
import re
from datetime import datetime
from .compat import now_ms, safe_integer, js_json, utf16_len

CACHE_VERSION = 1
MAX_AGE_MS = 5 * 60000
MAX_SCENE_TTL_MS = 120000


def canonical(value, ancestors=None, depth=0, budget=None):
    ancestors = set() if ancestors is None else ancestors
    budget = [0] if budget is None else budget
    budget[0] += 1
    if budget[0] > 10000 or depth > 24:
        raise ValueError("Invalid cache input.")
    if value is None or isinstance(value, (str, bool)):
        return js_json(value)
    if isinstance(value, (int, float)):
        import math

        if math.isfinite(value):
            return js_json(value)
        raise ValueError("Invalid cache input.")
    if type(value) not in (dict, list) or id(value) in ancestors:
        raise ValueError("Invalid cache input.")
    ancestors.add(id(value))
    try:
        if isinstance(value, list):
            return (
                "["
                + ",".join(canonical(v, ancestors, depth + 1, budget) for v in value)
                + "]"
            )
        return (
            "{"
            + ",".join(
                js_json(k) + ":" + canonical(value[k], ancestors, depth + 1, budget)
                for k in sorted(
                    value, key=lambda k: k.encode("utf-16-be", errors="surrogatepass")
                )
            )
            + "}"
        )
    finally:
        ancestors.remove(id(value))


def scopeDigest(scope):
    if (
        not isinstance(scope, dict)
        or scope.get("skill") != "scene.research"
        or type(scope.get("skillVersion")) is not int
        or scope.get("skillVersion") != 1
        or not isinstance(scope.get("input"), dict)
    ):
        return None
    if not all(
        isinstance(scope.get(k), str) and 0 < utf16_len(scope[k]) <= 200
        for k in ["userId", "deviceId", "catalogContext"]
    ):
        return None
    data = canonical(
        {
            k: scope[k]
            for k in [
                "userId",
                "deviceId",
                "catalogContext",
                "skill",
                "skillVersion",
                "input",
            ]
        }
    ).encode()
    return hashlib.sha256(data).hexdigest() if len(data) <= 128 * 1024 else None


def projectResult(result, now):
    from .task_schema import toolResult

    if not isinstance(result, dict) or "error" in result or result.get("question"):
        return None
    parsed = toolResult(
        {
            "deviceId": "cache_device_v1",
            "leaseId": "cache_lease_v1",
            "revision": 1,
            "result": result,
        },
        now,
    )
    if not parsed["tracks"] or parsed.get("question"):
        return None
    stamps = [
        int(datetime.fromisoformat(v.replace("Z", "+00:00")).timestamp() * 1000)
        for v in [parsed["checkedAt"], *(s["checkedAt"] for s in parsed["sources"])]
    ]
    if any(s > now for s in stamps):
        return None
    return {"result": parsed, "earliestExpiry": min(stamps) + MAX_AGE_MS}


class DisabledTaskToolCache:
    async def get(self, scope):
        return None

    async def put(self, scope):
        return {"stored": False}


class RedisTaskToolCache:
    def __init__(self, options=None, **kwargs):
        opts = {**(options or {}), **kwargs}
        self.client = opts.get("client")
        self.namespace = opts.get("namespace")
        self.now = opts.get("now", now_ms)
        self.commandTimeoutMs = opts.get("commandTimeoutMs", 200)
        self.sceneTtlMs = opts.get("sceneTtlMs", MAX_SCENE_TTL_MS)
        self.maxBytes = opts.get("maxBytes", 192 * 1024)
        if (
            not self.client
            or not isinstance(self.namespace, str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9:_-]{0,79}", self.namespace)
            or not callable(self.now)
            or not safe_integer(self.commandTimeoutMs)
            or not 1 <= self.commandTimeoutMs <= 5000
            or not safe_integer(self.sceneTtlMs)
            or not 1 <= self.sceneTtlMs <= MAX_SCENE_TTL_MS
            or not safe_integer(self.maxBytes)
            or not 128 <= self.maxBytes <= 1024 * 1024
        ):
            raise ValueError("Task tool cache requires valid bounded policy.")

    async def command(self, operation):
        try:
            return await asyncio.wait_for(operation(), self.commandTimeoutMs / 1000)
        except Exception:
            return None

    async def get(self, scope):
        try:
            digest = scopeDigest(scope)
            if not digest:
                return None
            raw = await self.command(
                lambda: self.client.get(f"{self.namespace}:toolcache:v1:{digest}")
            )
            if isinstance(raw, bytes):
                raw = raw.decode()
            if not isinstance(raw, str) or len(raw.encode()) > self.maxBytes:
                return None
            e = json.loads(raw)
            now = self.now()
            if (
                not safe_integer(now)
                or not isinstance(e, dict)
                or type(e.get("version")) is not int
                or e.get("version") != 1
                or e.get("digest") != digest
                or not all(safe_integer(e.get(k)) for k in ["cachedAt", "expiresAt"])
                or e["cachedAt"] > now
                or e["expiresAt"] <= now
                or e["cachedAt"] < 1
                or not 0 < e["expiresAt"] - e["cachedAt"] <= self.sceneTtlMs
            ):
                return None
            projected = projectResult(e.get("result"), now)
            if not projected or e["expiresAt"] > projected["earliestExpiry"]:
                return None
            return {
                "result": projected["result"],
                "cache": {
                    "hit": True,
                    "cachedAt": e["cachedAt"],
                    "expiresAt": e["expiresAt"],
                },
            }
        except Exception:
            return None

    async def put(self, scope):
        try:
            digest = scopeDigest(scope)
            now = self.now()
            if not digest or not safe_integer(now) or now < 1:
                return {"stored": False}
            projected = projectResult(scope.get("result"), now)
            if not projected:
                return {"stored": False}
            expiresAt = min(now + self.sceneTtlMs, projected["earliestExpiry"])
            ttl = expiresAt - now
            if ttl < 1:
                return {"stored": False}
            raw = js_json(
                {
                    "version": 1,
                    "digest": digest,
                    "cachedAt": now,
                    "expiresAt": expiresAt,
                    "result": projected["result"],
                }
            )
            if len(raw.encode()) > self.maxBytes:
                return {"stored": False}
            accepted = await self.command(
                lambda: self.client.set(
                    f"{self.namespace}:toolcache:v1:{digest}", raw, px=int(ttl)
                )
            )
            return {"stored": bool(accepted)}
        except Exception:
            return {"stored": False}


def createTaskToolCache(options=None, **kwargs):
    opts = {**(options or {}), **kwargs}
    return RedisTaskToolCache(opts) if opts.get("client") else DisabledTaskToolCache()
