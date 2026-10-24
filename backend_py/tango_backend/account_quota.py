"""ai_usage 是唯一用量账本；额度策略不保存重复余额。"""

from datetime import datetime, timedelta, timezone
from .errors import ApiError
from .compat import now_ms, iso
from .storage_postgres import safeCount

DEFAULT_QUOTA_POLICY = {"dailyLimit": 30, "totalLimit": 300, "enabled": True}
ACCOUNT_QUOTA_MAXIMUM = 1_000_000_000
SQLITE_ACCOUNT_QUOTA_SCHEMA = """CREATE TABLE IF NOT EXISTS music_account_quota (
user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
daily_limit INTEGER NOT NULL CHECK(daily_limit BETWEEN 0 AND 1000000000),
total_limit INTEGER NOT NULL CHECK(total_limit BETWEEN 0 AND 1000000000),
enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),updated_at INTEGER NOT NULL) STRICT;"""
POSTGRES_ACCOUNT_QUOTA_SCHEMA = (
    SQLITE_ACCOUNT_QUOTA_SCHEMA.replace("INTEGER", "BIGINT")
    .replace(
        "enabled BIGINT NOT NULL CHECK(enabled IN (0,1))", "enabled BOOLEAN NOT NULL"
    )
    .replace(" STRICT;", ";")
)


def validateQuotaPolicy(policy):
    if (
        not isinstance(policy, dict)
        or set(policy) != {"dailyLimit", "totalLimit", "enabled"}
        or type(policy["enabled"]) is not bool
        or any(
            type(policy[key]) is not int
            or not 0 <= policy[key] <= ACCOUNT_QUOTA_MAXIMUM
            for key in ["dailyLimit", "totalLimit"]
        )
    ):
        raise TypeError("Invalid account quota policy")
    return dict(policy)


def cap(value):
    if type(value) is not int or not 0 <= value <= 9007199254740991:
        raise TypeError("Invalid account daily cap")
    return value


def clock(now):
    if type(now) is not int or not 0 <= now <= 253402214400000:
        raise TypeError("Invalid quota time")
    date = datetime.fromtimestamp(now / 1000, timezone.utc)
    reset = date.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(days=1)
    return {
        "day": date.strftime("%Y-%m-%d"),
        "resetAt": iso(int(reset.timestamp() * 1000)),
    }


class AccountPolicyStore:
    def __init__(
        self, store, options=None, *, defaults=None, dailyCap=9007199254740991
    ):
        options = options or {}
        self.store = store
        self.defaults = validateQuotaPolicy(
            options.get("defaults", defaults or DEFAULT_QUOTA_POLICY)
        )
        self.dailyCap = cap(options.get("dailyCap", dailyCap))
        self.postgres = store.dialect == "postgres"

    async def initialize(self):
        await self.store.initialize()
        return self

    def policy(self, row):
        if not row:
            raise ApiError(404, "ACCOUNT_NOT_FOUND", "探歌账号不存在。")
        return validateQuotaPolicy(
            self.defaults
            if row.get("daily_limit") is None
            else {
                "dailyLimit": safeCount(row["daily_limit"]),
                "totalLimit": safeCount(row["total_limit"]),
                "enabled": bool(row["enabled"]),
            }
        )

    async def read(self, userId, now, client=None):
        await self.initialize()
        return await (client or self.store.db).fetchone(
            """SELECT u.id,q.daily_limit,q.total_limit,q.enabled,
COALESCE(SUM(a.request_count),0) AS total_used,
COALESCE(SUM(CASE WHEN a.utc_day=? THEN a.request_count ELSE 0 END),0) AS daily_used
FROM users u LEFT JOIN music_account_quota q ON q.user_id=u.id
LEFT JOIN ai_usage a ON a.user_id=u.id WHERE u.id=?
GROUP BY u.id,q.daily_limit,q.total_limit,q.enabled""",
            [clock(now)["day"], userId],
        )

    readPostgres = read

    def view(self, row, now, dailyLimit=9007199254740991):
        policy = self.policy(row)
        daily, total = safeCount(row["daily_used"]), safeCount(row["total_used"])
        maximum = min(policy["dailyLimit"], self.dailyCap, cap(dailyLimit))
        return {
            "daily": {
                "limit": maximum,
                "used": daily,
                "remaining": max(0, maximum - daily),
                "resetAt": clock(now)["resetAt"],
            },
            "total": {
                "limit": policy["totalLimit"],
                "used": total,
                "remaining": max(0, policy["totalLimit"] - total),
            },
            "disabled": not policy["enabled"],
        }

    async def snapshot(self, userId, now=None):
        now = now_ms() if now is None else now
        return self.view(await self.read(userId, now), now)

    async def assertEnabled(self, userId):
        if not self.policy(await self.read(userId, now_ms()))["enabled"]:
            raise ApiError(
                403, "AI_ACCOUNT_DISABLED", "此账号的 AI 功能已停用，请联系管理员。"
            )

    def check(self, row, now, dailyLimit):
        usage = self.view(row, now, dailyLimit)
        if usage["disabled"]:
            raise ApiError(
                403, "AI_ACCOUNT_DISABLED", "此账号的 AI 功能已停用，请联系管理员。"
            )
        if not usage["total"]["remaining"]:
            raise ApiError(
                429, "AI_TOTAL_LIMIT", "账号 AI 总额度已用完，请联系管理员。"
            )
        if not usage["daily"]["remaining"]:
            raise ApiError(
                429, "AI_DAILY_LIMIT", "今日 AI 使用次数已用完，请在额度重置后继续。"
            )
        return usage["daily"]["limit"]

    async def reserve(self, userId, now, dailyLimit, client=None):
        database = client or self.store.db
        if not database.isTransaction:
            raise TypeError("Account quota reservation requires a transaction")
        if self.store.dialect == "mysql":
            await database.lock_key("music:user:" + userId)
            await database.fetchone("SELECT id FROM users WHERE id=? FOR UPDATE", [userId])
        maximum = self.check(await self.read(userId, now, database), now, dailyLimit)
        if not await self.store.reserveDailyUsage(userId, now, maximum, database):
            raise ApiError(
                429, "AI_DAILY_LIMIT", "今日 AI 使用次数已用完，请在额度重置后继续。"
            )

    async def adminset(self, userId, policy, options=None, **kwargs):
        value = validateQuotaPolicy(policy)
        now = (options or {}).get("now", kwargs.get("now", now_ms()))
        clock(now)
        await self.initialize()
        async with self.store.db.transaction() as db:
            if self.store.dialect == "mysql":
                await db.lock_key("music:user:" + userId)
            if not await db.fetchone(
                "SELECT id FROM users WHERE id=?"
                + (" FOR UPDATE" if self.store.dialect in {"postgres", "mysql"} else ""),
                [userId],
            ):
                raise ApiError(404, "ACCOUNT_NOT_FOUND", "探歌账号不存在。")
            statement = """INSERT INTO music_account_quota(user_id,daily_limit,total_limit,enabled,updated_at)
VALUES(?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET daily_limit=excluded.daily_limit,
total_limit=excluded.total_limit,enabled=excluded.enabled,updated_at=excluded.updated_at"""
            if self.store.dialect == "mysql":
                statement = """INSERT INTO music_account_quota(user_id,daily_limit,total_limit,enabled,updated_at)
VALUES(?,?,?,?,?) AS incoming ON DUPLICATE KEY UPDATE daily_limit=incoming.daily_limit,
total_limit=incoming.total_limit,enabled=incoming.enabled,updated_at=incoming.updated_at"""
            await db.execute(
                statement,
                [
                    userId,
                    value["dailyLimit"],
                    value["totalLimit"],
                    value["enabled"] if self.postgres else int(value["enabled"]),
                    now,
                ],
            )
        return await self.snapshot(userId, now)
