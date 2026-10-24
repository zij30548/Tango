"""异步数据库边界：事务绑定当前协程，兼容现有 SQL 参数写法。"""

import asyncio
import contextvars
import hashlib
import re
import sqlite3
from urllib.parse import urlsplit, unquote
from contextlib import asynccontextmanager
from dataclasses import dataclass


@dataclass
class Result:
    rows: list
    rowCount: int

    def fetchone(self):
        return self.rows[0] if self.rows else None

    def fetchall(self):
        return self.rows


def parameters(sql, values, postgres):
    """只改 SQL 词法层占位符，跳过字符串、标识符和注释。"""
    dialect = "postgres" if postgres is True else "sqlite" if postgres is False else postgres
    if dialect == "mysql":
        # asyncmy 使用 pyformat 参数；仅转义原 SQL 的百分号，不修改业务语法。
        sql = sql.replace("%", "%%")
    tokens = re.compile(
        r"('(?:''|[^'])*'|\"(?:\"\"|[^\"])*\"|`(?:``|[^`])*`|--[^\n]*|/\*[\s\S]*?\*/|\$[A-Za-z_]*\$[\s\S]*?\$[A-Za-z_]*\$)|(\$\d+|\?)"
    )
    ordered, index = [], 0

    def replace(match):
        nonlocal index
        if match.group(1):
            return match.group(0)
        token = match.group(2)
        position = int(token[1:]) - 1 if token.startswith("$") else index
        if token == "?":
            index += 1
        if position >= len(values):
            raise ValueError("Missing SQL parameter")
        ordered.append(values[position])
        return "$" + str(len(ordered)) if dialect == "postgres" else "%s" if dialect == "mysql" else "?"

    translated = tokens.sub(replace, sql)
    return translated, ordered


class Database:
    def __init__(self, *, path=None, connectionString=None, dialect=None, **options):
        self.dialect = dialect or (
            "postgres"
            if connectionString is not None or options.get("pool") is not None
            else "sqlite"
        )
        self.path, self.connectionString, self.options = path, connectionString, options
        self._lock = asyncio.Lock()
        self._state = contextvars.ContextVar("tango_database_transaction", default=None)
        self._connection, self.pool = None, options.get("pool")
        self._owns_pool = self.pool is None
        self._init_lock = asyncio.Lock()

    async def initialize(self):
        async with self._init_lock:
            if self._connection is not None or self.pool is not None:
                return self
            if self.dialect == "postgres":
                import asyncpg

                self.pool = await asyncpg.create_pool(
                    self.connectionString,
                    min_size=1,
                    max_size=self.options.get("maximumConnections", 10),
                    command_timeout=self.options.get("queryTimeoutMs", 20000) / 1000,
                    server_settings=self.options.get("serverSettings", {}),
                )
            elif self.dialect == "mysql":
                import asyncmy
                from asyncmy.constants import CLIENT
                url = urlsplit(self.connectionString or "")
                if url.scheme not in ("mysql", "mysql+asyncmy") or not url.hostname or not url.path.strip("/") or url.query or url.fragment:
                    raise ValueError("Invalid explicit MySQL connection configuration")
                self.pool = await asyncmy.create_pool(
                    host=url.hostname, port=url.port or 3306, user=unquote(url.username or ""),
                    password=unquote(url.password or ""), db=unquote(url.path[1:]), charset="utf8mb4",
                    minsize=1, maxsize=self.options.get("maximumConnections", 10), autocommit=True,
                    connect_timeout=self.options.get("connectionTimeoutMs", 5000) / 1000,
                    read_timeout=self.options.get("queryTimeoutMs", 20000) / 1000,
                    client_flag=CLIENT.FOUND_ROWS, local_infile=False,
                    init_command="SET SESSION transaction_isolation='READ-COMMITTED',time_zone='+00:00',sql_mode='STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION',collation_connection='utf8mb4_0900_bin'",
                    **({"ssl": self.options["ssl"]} if self.options.get("ssl") is not None else {}),
                )
            else:
                self._connection = await asyncio.to_thread(
                    sqlite3.connect,
                    self.path,
                    timeout=5,
                    isolation_level=None,
                    check_same_thread=False,
                )
                self._connection.row_factory = sqlite3.Row
                await asyncio.to_thread(
                    self._connection.execute, "PRAGMA foreign_keys=ON"
                )
                await asyncio.to_thread(
                    self._connection.execute, "PRAGMA journal_mode=WAL"
                )
                await asyncio.to_thread(
                    self._connection.execute, "PRAGMA synchronous=NORMAL"
                )
        return self

    def _current(self):
        state = self._state.get()
        return state if state and state[0] is asyncio.current_task() else None

    @property
    def isTransaction(self):
        return self._current() is not None

    async def _run(self, connection, sql, values):
        sql, values = parameters(sql, values, self.dialect)
        if self.dialect == "postgres":
            statement = await connection.prepare(sql)
            rows = [dict(row) for row in await statement.fetch(*values)]
            status = statement.get_statusmsg() or ""
            count = (
                int(status.split()[-1])
                if status.split() and status.split()[-1].isdigit()
                else len(rows)
            )
            return Result(rows, count)
        if self.dialect == "mysql":
            from asyncmy.cursors import DictCursor
            async with connection.cursor(DictCursor) as cursor:
                await cursor.execute(sql, tuple(values))
                rows = [dict(row) for row in await cursor.fetchall()] if cursor.description else []
                return Result(rows, max(cursor.rowcount, 0))

        def run():
            cursor = connection.execute(sql, values)
            rows = (
                [dict(row) for row in cursor.fetchall()] if cursor.description else []
            )
            return Result(rows, max(cursor.rowcount, len(rows), 0))

        work = asyncio.create_task(asyncio.to_thread(run))
        try:
            return await asyncio.shield(work)
        except asyncio.CancelledError:
            # 等线程中的语句结束后才允许回滚或释放连接，避免取消产生并发事务。
            await work
            raise

    async def execute(self, sql, params=()):
        await self.initialize()
        state = self._current()
        if self.dialect == "mysql" and state:
            head = re.sub(r"/\*[\s\S]*?\*/|--[^\n]*", "", sql).lstrip().split(None, 1)[0].upper()
            if head in {"CREATE", "ALTER", "DROP", "TRUNCATE", "RENAME", "GRANT", "REVOKE", "LOCK", "UNLOCK", "SET"}:
                raise TypeError("MySQL implicit-commit SQL is forbidden inside a business transaction")
        if state:
            return await self._run(state[1], sql, params)
        if self.dialect in {"postgres", "mysql"}:
            async with self.pool.acquire() as connection:
                return await self._run(connection, sql, params)
        async with self._lock:
            return await self._run(self._connection, sql, params)

    query = execute

    async def fetchone(self, sql, params=()):
        return (await self.execute(sql, params)).fetchone()

    async def fetchall(self, sql, params=()):
        return (await self.execute(sql, params)).rows

    async def executescript(self, sql):
        await self.initialize()
        if self.dialect == "mysql" and self.isTransaction:
            raise TypeError("MySQL schema initialization must run outside business transactions")
        if self.dialect == "postgres":
            state = self._current()
            if state:
                await state[1].execute(sql)
            else:
                async with self.pool.acquire() as connection:
                    await connection.execute(sql)
            return
        # sqlite executescript 会隐式提交；逐条执行才能保留外层事务。
        statement = ""
        for line in sql.splitlines(keepends=True):
            for char in line:
                statement += char
                if char == ";" and sqlite3.complete_statement(statement):
                    await self.execute(statement)
                    statement = ""
        if statement.strip():
            await self.execute(statement)

    @asynccontextmanager
    async def transaction(self):
        await self.initialize()
        state = self._current()
        if state:
            name = "tango_nested_" + str(state[2] + 1)
            token = self._state.set((state[0], state[1], state[2] + 1))
            await self._run(state[1], "SAVEPOINT " + name, ())
            try:
                yield self
                await self._run(state[1], "RELEASE SAVEPOINT " + name, ())
            except BaseException:
                await self._run(state[1], "ROLLBACK TO SAVEPOINT " + name, ())
                await self._run(state[1], "RELEASE SAVEPOINT " + name, ())
                raise
            finally:
                self._state.reset(token)
            return
        connection = (
            await self.pool.acquire()
            if self.dialect in {"postgres", "mysql"}
            else self._connection
        )
        if self.dialect == "sqlite":
            await self._lock.acquire()
        token = self._state.set((asyncio.current_task(), connection, 0))
        try:
            if self.dialect == "mysql":
                await self._run(connection, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED", ())
            await self._run(
                connection,
                "BEGIN" if self.dialect in {"postgres", "mysql"} else "BEGIN IMMEDIATE",
                (),
            )
            try:
                yield self
                await self._run(connection, "COMMIT", ())
            except BaseException:
                try:
                    await self._run(connection, "ROLLBACK", ())
                except BaseException:
                    pass
                raise
        finally:
            self._state.reset(token)
            if self.dialect in {"postgres", "mysql"}:
                await self.pool.release(connection)
            else:
                self._lock.release()

    async def close(self):
        if self.pool and self._owns_pool:
            if self.dialect == "mysql":
                self.pool.close()
                await self.pool.wait_closed()
            else:
                await self.pool.close()
            self.pool = None
        if self._connection:
            await asyncio.to_thread(self._connection.close)
            self._connection = None

    async def lock_key(self, key):
        """事务级互斥，MySQL 使用 InnoDB 行锁，不占用连接级 GET_LOCK。"""
        if not self.isTransaction or not isinstance(key, str) or not key or len(key) > 191 or "\x00" in key:
            raise TypeError("A bounded storage lock key requires an active transaction")
        if self.dialect == "mysql":
            await self.execute("INSERT INTO music_storage_locks(lock_key) VALUES(?) ON DUPLICATE KEY UPDATE lock_key=lock_key", [key])
            await self.fetchone("SELECT lock_key FROM music_storage_locks WHERE lock_key=? FOR UPDATE", [key])
        elif self.dialect == "postgres":
            value = int.from_bytes(hashlib.sha256(key.encode()).digest()[:8], "big", signed=True)
            await self.execute("SELECT pg_advisory_xact_lock($1)", [value])
