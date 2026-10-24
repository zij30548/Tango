"""保留既有账号表、会话令牌哈希与密码格式的异步账号仓库。"""

import asyncio
import os
import re
import secrets
import sqlite3
import unicodedata
import uuid
import regex as unicode_regex
from pathlib import Path
from .errors import ApiError
from .compat import iso
from .storage_db import Database
from .storage_auth import (
    passwordRecord,
    passwordMatches,
    passwordNeedsUpgrade,
    sessionTokenHash,
)
from .account_quota import SQLITE_ACCOUNT_QUOTA_SCHEMA, POSTGRES_ACCOUNT_QUOTA_SCHEMA
from .invite_store import (
    SQLITE_INVITE_SCHEMA,
    POSTGRES_INVITE_SCHEMA,
    registrationOptions,
    consumeInvite,
)

BASE_SCHEMA = """CREATE TABLE IF NOT EXISTS users(id TEXT PRIMARY KEY,username TEXT NOT NULL,
username_key TEXT NOT NULL UNIQUE,password_salt TEXT NOT NULL,password_hash TEXT NOT NULL,created_at INTEGER NOT NULL) STRICT;
CREATE TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY,user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL) STRICT;
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id,created_at);
CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);
CREATE TABLE IF NOT EXISTS ai_usage(user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
utc_day TEXT NOT NULL,request_count INTEGER NOT NULL,PRIMARY KEY(user_id,utc_day)) STRICT;"""


async def ensureModelLedgerColumns(db, table, dialect):
    """仅追加可空审计字段；旧尝试保留未知，绝不反填猜测的模型。"""
    if table not in {"music_task_calls", "music_conversation_route_attempts"}:
        raise TypeError("Unsupported model ledger table")
    columns = ("model_selection_json", "model_metadata_json")
    if dialect == "postgres":
        for column in columns:
            await db.execute(
                f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {column} JSONB"
            )
        return
    if dialect == "mysql":
        rows = await db.fetchall(
            "SELECT COLUMN_NAME AS column_name FROM information_schema.columns "
            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=?", [table]
        )
        known = {row["column_name"] for row in rows}
        for column in columns:
            if column not in known:
                try:
                    await db.execute(f"ALTER TABLE {table} ADD COLUMN {column} JSON")
                except Exception as failure:
                    # 多实例同时启动可能已完成同一加列；其余 DDL 错误继续阻止就绪。
                    if getattr(failure, "args", (None,))[0] != 1060:
                        raise
        return
    known = {row["name"] for row in await db.fetchall(f"PRAGMA table_info({table})")}
    for column in columns:
        if column not in known:
            await db.execute(f"ALTER TABLE {table} ADD COLUMN {column} TEXT")


async def ensureTaskRequestMetadataColumn(db, dialect):
    """仅追加原操作的可空事实列；历史请求不回填或推测动作。"""
    if dialect == "postgres":
        await db.execute(
            "ALTER TABLE music_task_requests ADD COLUMN IF NOT EXISTS request_metadata_json JSONB"
        )
    elif dialect == "mysql":
        row = await db.fetchone(
            "SELECT COLUMN_NAME FROM information_schema.columns "
            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='music_task_requests' "
            "AND COLUMN_NAME='request_metadata_json'"
        )
        if not row:
            try:
                await db.execute("ALTER TABLE music_task_requests ADD COLUMN request_metadata_json JSON")
            except Exception as failure:
                # 多实例就绪竞争只容许另一实例已加同列，其他错误继续阻止启动。
                if getattr(failure, "args", (None,))[0] != 1060:
                    raise
    elif dialect == "sqlite":
        columns = {row["name"] for row in await db.fetchall("PRAGMA table_info(music_task_requests)")}
        if "request_metadata_json" not in columns:
            await db.execute("ALTER TABLE music_task_requests ADD COLUMN request_metadata_json TEXT")
    else:
        raise TypeError("Unsupported task request metadata dialect")


async def ensureConversationClearColumn(db):
    """旧 MySQL 仅新增可空墓碑时间；启动竞争只容许相同列已被添加。"""
    row = await db.fetchone("SELECT COLUMN_NAME FROM information_schema.columns WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='music_conversations' AND COLUMN_NAME='cleared_at'")
    if not row:
        try:
            await db.execute("ALTER TABLE music_conversations ADD COLUMN cleared_at BIGINT")
        except Exception as failure:
            if getattr(failure, "args", (None,))[0] != 1060:
                raise


def validateCredentials(body):
    if (
        not isinstance(body, dict)
        or set(body) != {"username", "password"}
        or not isinstance(body["username"], str)
        or not isinstance(body["password"], str)
    ):
        raise ApiError(400, "INVALID_CREDENTIALS_FORMAT", "请输入用户名和密码。")
    username = unicodedata.normalize("NFKC", body["username"]).strip()
    if not unicode_regex.fullmatch(r"[\p{Script=Han}A-Za-z0-9_.-]{2,32}", username):
        raise ApiError(
            400,
            "INVALID_USERNAME",
            "用户名须为 2 至 32 个中文、英文字母、数字或 _ . -。",
        )
    password = body["password"]
    if not 8 <= len(
        password.encode("utf-16-le", "surrogatepass")
    ) // 2 <= 128 or re.search(r"[\x00-\x1f\x7f]", password):
        raise ApiError(
            400, "INVALID_PASSWORD", "密码须为 8 至 128 个字符，且不能包含控制字符。"
        )
    return {"username": username, "usernameKey": username.lower(), "password": password}


class Store:
    def __init__(self, path=":memory:", options=None, **kwargs):
        options = {**(options or {}), **kwargs}
        self.dialect = "sqlite"
        self.path = str(path)
        self.sessionMs = options.get("sessionMs", 7 * 86400000)
        self.db = Database(path=self.path)
        self.accountPolicy = None
        self._initialized = False
        self._initialize_lock = asyncio.Lock()

    async def initialize(self):
        async with self._initialize_lock:
            if self._initialized:
                return self
            if self.dialect == "sqlite" and self.path != ":memory:":
                path = Path(self.path)
                path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                fd = os.open(path, os.O_CREAT | os.O_APPEND | os.O_WRONLY, 0o600)
                os.close(fd)
                os.chmod(path, 0o600)
            await self.db.initialize()
            if self.dialect == "mysql":
                version = await self.db.fetchone("SELECT VERSION() AS version")
                if not re.match(r"^8\.4\.", version["version"]):
                    raise RuntimeError("MySQL 8.4 is required")
                # MySQL DDL 隐式提交；只在就绪前逐条建表，失败重试不冒充业务回滚。
                schema = (Path(__file__).parent / "migrations" / "001_initial_mysql.sql").read_text()
                await self.db.executescript(schema)
                for table in ("music_task_calls", "music_conversation_route_attempts"):
                    await ensureModelLedgerColumns(self.db, table, self.dialect)
                await ensureTaskRequestMetadataColumn(self.db, self.dialect)
                await ensureConversationClearColumn(self.db)
                tables = await self.db.fetchall("SELECT TABLE_NAME AS table_name,ENGINE AS engine,TABLE_COLLATION AS collation_name FROM information_schema.tables WHERE TABLE_SCHEMA=DATABASE()")
                required = set(re.findall(r"CREATE TABLE IF NOT EXISTS\s+(\w+)", schema))
                known = {row["table_name"]: row for row in tables}
                if any(name not in known or known[name]["engine"] != "InnoDB" or known[name]["collation_name"] != "utf8mb4_0900_bin" for name in required):
                    raise RuntimeError("MySQL tables require InnoDB and exact binary collation")
                self._initialized = True
                return self
            async with self.db.transaction() as db:
                if self.dialect == "postgres":
                    await db.execute(
                        "SELECT pg_advisory_xact_lock($1,$2)", [1952546919, 0]
                    )
                    schema = (
                        Path(__file__).parent
                        / "migrations"
                        / "001_initial_postgres.sql"
                    ).read_text()
                    await db.executescript(schema)
                    await ensureModelLedgerColumns(db, "music_task_calls", self.dialect)
                    await ensureTaskRequestMetadataColumn(db, self.dialect)
                else:
                    await db.executescript(BASE_SCHEMA)
                await db.executescript(
                    POSTGRES_ACCOUNT_QUOTA_SCHEMA
                    if self.dialect == "postgres"
                    else SQLITE_ACCOUNT_QUOTA_SCHEMA
                )
                await db.executescript(
                    POSTGRES_INVITE_SCHEMA
                    if self.dialect == "postgres"
                    else SQLITE_INVITE_SCHEMA
                )
            self._initialized = True
        return self

    async def transaction(self, operation):
        await self.initialize()
        async with self.db.transaction() as db:
            return await operation(db)

    async def register(self, credentials, now, options=None):
        registration = registrationOptions(options)
        await self.initialize()
        record = await passwordRecord(credentials["password"])
        user = {"id": str(uuid.uuid4()), "username": credentials["username"]}
        try:
            async with self.db.transaction() as db:
                invitation = await consumeInvite(
                    db,
                    registration["codeHash"],
                    now,
                    self.accountPolicy.defaults if self.accountPolicy else None,
                )
                await db.execute(
                    "INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)",
                    [
                        user["id"],
                        user["username"],
                        credentials["usernameKey"],
                        record["salt"],
                        record["hash"],
                        now,
                    ],
                )
                policy = invitation["policy"]
                await db.execute(
                    "INSERT INTO music_account_quota(user_id,daily_limit,total_limit,enabled,updated_at) VALUES(?,?,?,?,?)",
                    [
                        user["id"],
                        policy["dailyLimit"],
                        policy["totalLimit"],
                        policy["enabled"]
                        if self.dialect == "postgres"
                        else int(policy["enabled"]),
                        now,
                    ],
                )
                if invitation["inviteId"]:
                    await db.execute(
                        "INSERT INTO music_registration_invite_uses(user_id,invite_id,created_at) VALUES(?,?,?)",
                        [user["id"], invitation["inviteId"], now],
                    )
        except Exception as failure:
            if (
                isinstance(failure, sqlite3.IntegrityError)
                and "users.username_key" in str(failure)
                or getattr(failure, "sqlstate", None) == "23505"
                or self.dialect == "mysql" and getattr(failure, "args", (None,))[0] == 1062
            ):
                raise ApiError(409, "USERNAME_TAKEN", "该用户名已被使用。") from failure
            raise
        return user

    async def authenticate(self, credentials):
        await self.initialize()
        row = await self.db.fetchone(
            "SELECT * FROM users WHERE username_key=?", [credentials["usernameKey"]]
        )
        if not await passwordMatches(credentials["password"], row):
            raise ApiError(401, "INVALID_CREDENTIALS", "用户名或密码不正确。")
        if passwordNeedsUpgrade(row):
            record = await passwordRecord(credentials["password"])
            await self.db.execute(
                "UPDATE users SET password_salt=?,password_hash=? WHERE id=? AND password_salt=? AND password_hash=?",
                [
                    record["salt"],
                    record["hash"],
                    row["id"],
                    row["password_salt"],
                    row["password_hash"],
                ],
            )
        return {"id": row["id"], "username": row["username"]}

    async def createSession(self, user, now, sessionMs=None):
        await self.initialize()
        token, expires = (
            secrets.token_urlsafe(32),
            now + (self.sessionMs if sessionMs is None else sessionMs),
        )
        async with self.db.transaction() as db:
            if self.dialect in {"postgres", "mysql"}:
                if self.dialect == "mysql":
                    await db.lock_key("music:user:" + user["id"])
                await db.execute(
                    "SELECT id FROM users WHERE id=? FOR UPDATE", [user["id"]]
                )
            await db.execute("DELETE FROM sessions WHERE expires_at<=?", [now])
            if self.dialect == "mysql":
                await db.execute("DELETE FROM sessions WHERE token_hash IN (SELECT token_hash FROM (SELECT token_hash FROM sessions WHERE user_id=? ORDER BY created_at DESC,token_hash LIMIT 18446744073709551615 OFFSET 9) AS stale_sessions)", [user["id"]])
            else:
                await db.execute(
                    "DELETE FROM sessions WHERE token_hash IN (SELECT token_hash FROM sessions WHERE user_id=? ORDER BY created_at DESC,token_hash "
                    + ("OFFSET 9)" if self.dialect == "postgres" else "LIMIT -1 OFFSET 9)"),
                    [user["id"]],
                )
            await db.execute(
                "INSERT INTO sessions(token_hash,user_id,created_at,expires_at) VALUES(?,?,?,?)",
                [sessionTokenHash(token), user["id"], now, expires],
            )
        return {"token": token, "user": user, "expiresAt": iso(expires)}

    async def session(self, token, now):
        if not isinstance(token, str) or not re.fullmatch("[A-Za-z0-9_-]{43}", token):
            return None
        await self.initialize()
        row = await self.db.fetchone(
            "SELECT users.id,users.username,sessions.expires_at FROM sessions JOIN users ON users.id=sessions.user_id WHERE sessions.token_hash=? AND sessions.expires_at>?",
            [sessionTokenHash(token), now],
        )
        return (
            {
                "user": {"id": row["id"], "username": row["username"]},
                "expiresAt": iso(int(row["expires_at"])),
            }
            if row
            else None
        )

    async def logout(self, token):
        await self.initialize()
        await self.db.execute(
            "DELETE FROM sessions WHERE token_hash=?", [sessionTokenHash(token)]
        )

    async def reserveDailyUsage(self, userId, now, maximum, client=None):
        await self.initialize()
        if self.dialect == "mysql":
            if type(maximum) is not int or maximum < 1:
                return False
            db = client or self.db
            async def reserve():
                await db.lock_key("music:user:" + userId)
                row = await db.fetchone("SELECT request_count FROM ai_usage WHERE user_id=? AND utc_day=? FOR UPDATE", [userId, iso(now)[:10]])
                if row:
                    if int(row["request_count"]) >= maximum:
                        return False
                    return (await db.execute("UPDATE ai_usage SET request_count=request_count+1 WHERE user_id=? AND utc_day=? AND request_count<?", [userId, iso(now)[:10], maximum])).rowCount == 1
                await db.execute("INSERT INTO ai_usage(user_id,utc_day,request_count) VALUES(?,?,1)", [userId, iso(now)[:10]])
                return True
            if db.isTransaction:
                return await reserve()
            async with db.transaction():
                return await reserve()
        return bool(
            await (client or self.db).fetchone(
                """INSERT INTO ai_usage(user_id,utc_day,request_count) VALUES(?,?,1)
ON CONFLICT(user_id,utc_day) DO UPDATE SET request_count=ai_usage.request_count+1
WHERE ai_usage.request_count<? RETURNING request_count""",
                [userId, iso(now)[:10], maximum],
            )
        )

    async def getDailyUsage(self, userId, day):
        await self.initialize()
        row = await self.db.fetchone(
            "SELECT request_count FROM ai_usage WHERE user_id=? AND utc_day=?",
            [userId, day],
        )
        return int(row["request_count"]) if row else 0

    async def close(self):
        await self.db.close()


class PostgresStore(Store):
    def __init__(self, connectionString=None, options=None, **kwargs):
        if isinstance(connectionString, dict):
            options = {**connectionString, **(options or {})}
            connectionString = options.get("connectionString")
        options = {**(options or {}), **kwargs}
        super().__init__(":memory:", options)
        self.dialect = "postgres"
        self.db = Database(
            connectionString=connectionString or options.get("connectionString"),
            **{k: v for k, v in options.items() if k != "connectionString"},
        )
        self.pool = self.db


class MySQLStore(Store):
    def __init__(self, connectionString=None, options=None, **kwargs):
        if isinstance(connectionString, dict):
            options = {**connectionString, **(options or {})}
            connectionString = options.get("connectionString")
        options = {**(options or {}), **kwargs}
        super().__init__(":memory:", options)
        self.dialect = "mysql"
        self.db = Database(connectionString=connectionString or options.get("connectionString"), dialect="mysql",
            **{key: value for key, value in options.items() if key != "connectionString"})
        self.pool = self.db


MysqlStore = MySQLStore
