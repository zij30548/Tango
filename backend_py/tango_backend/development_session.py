"""开发身份由私密令牌、真实本机连接和独立账号映射共同保护。"""

import hashlib
import hmac
import os
import re
import secrets
import uuid
from .errors import ApiError
from .compat import now_ms
from .storage_auth import passwordRecord

IDENTITY_KEY = "tango_local_development_v1"
QUOTA = {"dailyLimit": 10000, "totalLimit": 100000}
SCHEMA = """CREATE TABLE IF NOT EXISTS development_identities (
identity_key TEXT PRIMARY KEY CHECK(identity_key='tango_local_development_v1'),
user_id TEXT NOT NULL UNIQUE REFERENCES users(id),created_at BIGINT NOT NULL);"""


def denied():
    raise ApiError(
        403,
        "DEVELOPMENT_SESSION_FORBIDDEN",
        "开发会话仅允许受信任的本机开发客户端使用。",
    )


def identityError():
    raise ApiError(
        503, "DEVELOPMENT_IDENTITY_INVALID", "开发账户状态不完整，请检查本机开发数据。"
    )


def field(value, key, fallback=None):
    return (
        value.get(key, fallback)
        if isinstance(value, dict)
        else getattr(value, key, fallback)
    )


class DevelopmentSession:
    def __init__(self, options=None, **kwargs):
        value = {**(options or {}), **kwargs}
        self.enabled = value.get("enabled") is True
        self.production = bool(
            value.get("production")
            or os.environ.get("NODE_ENV") == "production"
            or os.environ.get("TANGO_ENV") == "production"
        )
        token = value.get("token")
        if self.enabled and (
            self.production
            or value.get("bindAddress", "127.0.0.1") not in ["127.0.0.1", "::1"]
            or not isinstance(token, str)
            or not re.fullmatch("[a-f0-9]{64}", token)
        ):
            raise TypeError("Invalid development session configuration")
        self.store, self.now = value.get("store"), value.get("now", now_ms)
        self.sessionMs = value.get("sessionMs", 2 * 3600000)
        if (
            type(self.sessionMs) is not int
            or not 60000 <= self.sessionMs <= 4 * 3600000
        ):
            raise TypeError("Invalid development session lifetime")
        quota = value.get("quota")
        if quota and quota.store is not self.store:
            raise TypeError("Development quota store mismatch")
        self.postgres = self.store.dialect == "postgres"
        self.mysql = self.store.dialect == "mysql"
        self._tokenDigest = (
            hashlib.sha256(token.encode("ascii")).digest() if self.enabled else None
        )
        self._initialized = False

    def validate(self, request):
        socket = field(request, "socket")
        scope = field(request, "scope", {})
        client = field(request, "client")
        peer = field(socket, "remoteAddress") if socket else field(client, "host")
        if (
            not self.enabled
            or self.production
            or field(request, "method") != "POST"
            or peer not in ["127.0.0.1", "::1", "::ffff:127.0.0.1"]
        ):
            denied()
        port = (
            field(socket, "localPort")
            if socket
            else (scope.get("server") or [None, None])[1]
        )
        if type(port) is not int or not 1 <= port <= 65535:
            denied()
        raw = scope.get("headers")
        headersObject = field(request, "headers", {})
        if raw is None:
            raw = getattr(headersObject, "raw", None)
        if raw is None:
            rawHeaders = field(request, "rawHeaders")
            raw = (
                list(zip(rawHeaders[::2], rawHeaders[1::2]))
                if rawHeaders
                else list(headersObject.items())
            )
        headers = {}
        for key, value in raw:
            name = (
                key.decode("latin1").lower()
                if isinstance(key, bytes)
                else str(key).lower()
            )
            value = value.decode("latin1") if isinstance(value, bytes) else value
            if (
                not isinstance(value, str)
                or name in headers
                and name in ["host", "x-tango-development-token"]
            ):
                denied()
            if name in ["origin", "forwarded", "x-real-ip"] or name.startswith(
                "x-forwarded-"
            ):
                denied()
            headers[name] = value
        allowedHosts = [f"127.0.0.1:{port}", f"[::1]:{port}"] + (
            ["127.0.0.1", "[::1]"] if port == 80 else []
        )
        if headers.get("host") not in allowedHosts:
            denied()
        for name, value in headers.items():
            if name.startswith("sec-fetch-") and value not in {
                "sec-fetch-site": ["none", "same-origin"],
                "sec-fetch-mode": ["cors", "same-origin"],
                "sec-fetch-dest": ["empty"],
            }.get(name, []):
                denied()
        supplied = headers.get("x-tango-development-token")
        if (
            not isinstance(supplied, str)
            or not re.fullmatch("[a-f0-9]{64}", supplied)
            or not hmac.compare_digest(
                hashlib.sha256(supplied.encode("ascii")).digest(), self._tokenDigest
            )
        ):
            denied()

    async def initialize(self):
        if self._initialized:
            return self
        await self.store.initialize()
        if self.mysql:
            self._initialized = True
            return self
        async with self.store.db.transaction() as db:
            if self.postgres:
                await db.execute("SELECT pg_advisory_xact_lock($1,$2)", [1952546919, 8])
            await db.executescript(SCHEMA)
        self._initialized = True
        return self

    ready = initialize

    def identityRow(self, row):
        if not row:
            return None
        if not row.get("username") or not row.get("quota_user_id"):
            identityError()
        return {"id": row["user_id"], "username": row["username"]}

    def identitySql(self):
        return """SELECT d.user_id,u.username,q.user_id AS quota_user_id FROM development_identities d
LEFT JOIN users u ON u.id=d.user_id LEFT JOIN music_account_quota q ON q.user_id=d.user_id WHERE d.identity_key=?"""

    async def identity(self, now):
        await self.initialize()
        user = self.identityRow(
            await self.store.db.fetchone(self.identitySql(), [IDENTITY_KEY])
        )
        if user:
            return user
        user = {"id": str(uuid.uuid4()), "username": "dev_" + secrets.token_hex(12)}
        record = await passwordRecord(secrets.token_urlsafe(32))
        async with self.store.db.transaction() as db:
            if self.postgres:
                await db.execute("SELECT pg_advisory_xact_lock($1,$2)", [1952546919, 8])
            elif self.mysql:
                await db.lock_key("development:identity")
            existing = self.identityRow(
                await db.fetchone(self.identitySql(), [IDENTITY_KEY])
            )
            if existing:
                return existing
            await db.execute(
                "INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)",
                [
                    user["id"],
                    user["username"],
                    user["username"],
                    record["salt"],
                    record["hash"],
                    now,
                ],
            )
            await db.execute(
                "INSERT INTO music_account_quota(user_id,daily_limit,total_limit,enabled,updated_at) VALUES(?,?,?,?,?)",
                [user["id"], 10000, 100000, True if self.postgres else 1, now],
            )
            await db.execute(
                "INSERT INTO development_identities(identity_key,user_id,created_at) VALUES(?,?,?)",
                [IDENTITY_KEY, user["id"], now],
            )
        return user

    async def create(self, request):
        self.validate(request)
        now = self.now()
        if type(now) is not int or now < 0 or now + self.sessionMs > 8640000000000000:
            raise TypeError("Invalid development session time")
        user = await self.identity(now)
        async with self.store.db.transaction() as db:
            if not await db.fetchone(
                "SELECT user_id FROM development_identities WHERE identity_key=? AND user_id=?"
                + (" FOR UPDATE" if self.postgres or self.mysql else ""),
                [IDENTITY_KEY, user["id"]],
            ):
                identityError()
            session = await self.store.createSession(
                user, now, sessionMs=self.sessionMs
            )
        return {**session, "development": True}
