import base64
import hashlib
import re
import secrets
import uuid
from .errors import ApiError
from .compat import now_ms, iso
from .account_quota import validateQuotaPolicy, DEFAULT_QUOTA_POLICY

SQLITE_INVITE_SCHEMA = """CREATE TABLE IF NOT EXISTS music_registration_invites (
id TEXT PRIMARY KEY,code_hash TEXT NOT NULL UNIQUE CHECK(length(code_hash)=64),
label TEXT NOT NULL,expires_at INTEGER NOT NULL,max_uses INTEGER NOT NULL CHECK(max_uses BETWEEN 1 AND 10000),
used_count INTEGER NOT NULL DEFAULT 0 CHECK(used_count BETWEEN 0 AND max_uses),
disabled INTEGER NOT NULL DEFAULT 0 CHECK(disabled IN (0,1)),
daily_limit INTEGER NOT NULL CHECK(daily_limit BETWEEN 0 AND 1000000000),
total_limit INTEGER NOT NULL CHECK(total_limit BETWEEN 0 AND 1000000000),created_at INTEGER NOT NULL) STRICT;
CREATE TABLE IF NOT EXISTS music_registration_invite_uses (
user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
invite_id TEXT NOT NULL REFERENCES music_registration_invites(id),created_at INTEGER NOT NULL) STRICT;"""
POSTGRES_INVITE_SCHEMA = SQLITE_INVITE_SCHEMA.replace(" STRICT;", ";").replace(
    "INTEGER", "BIGINT"
)
inviteCodePattern = re.compile(r"TANGO-[A-Za-z0-9_-]{32}")


def invalidInvite():
    raise ApiError(403, "INVITE_INVALID", "内测码无效或已不可使用，请联系应用维护者。")


def inviteCodeHash(code):
    if not isinstance(code, str) or not inviteCodePattern.fullmatch(code.strip()):
        invalidInvite()
    return hashlib.sha256(code.strip().encode()).hexdigest()


def registrationOptions(options=None):
    options = options or {}
    mode = options.get("registrationMode", "open")
    if mode not in ["open", "invite_only", "closed"]:
        raise TypeError("Invalid registration mode")
    if mode == "closed":
        raise ApiError(403, "REGISTRATION_CLOSED", "当前暂不开放注册。")
    return {
        "registrationMode": mode,
        "codeHash": inviteCodeHash(options.get("inviteCode"))
        if mode == "invite_only"
        else None,
    }


def usable(row, now):
    if (
        not row
        or row["disabled"] != 0
        or row["expires_at"] <= now
        or row["used_count"] >= row["max_uses"]
    ):
        invalidInvite()
    return {
        "dailyLimit": int(row["daily_limit"]),
        "totalLimit": int(row["total_limit"]),
        "enabled": True,
    }


async def consumeInvite(db, codeHash, now, defaults=None):
    if not db.isTransaction:
        raise TypeError("Invite consumption requires account transaction")
    if not codeHash:
        return {
            "inviteId": None,
            "policy": validateQuotaPolicy(defaults or DEFAULT_QUOTA_POLICY),
        }
    row = await db.fetchone(
        "SELECT * FROM music_registration_invites WHERE code_hash=?"
        + (" FOR UPDATE" if db.dialect in {"postgres", "mysql"} else ""),
        [codeHash],
    )
    policy = usable(row, now)
    changed = await db.execute(
        "UPDATE music_registration_invites SET used_count=used_count+1 WHERE id=? AND disabled=0 AND expires_at>? AND used_count<max_uses",
        [row["id"], now],
    )
    if changed.rowCount != 1:
        invalidInvite()
    return {"inviteId": row["id"], "policy": policy}


consumeSqliteInvite = consumePostgresInvite = consumeInvite


def metadata(row):
    return {
        "id": row["id"],
        "label": row["label"],
        "expiresAt": iso(int(row["expires_at"])),
        "maxUses": int(row["max_uses"]),
        "usedCount": int(row["used_count"]),
        "disabled": row["disabled"] == 1,
        "dailyLimit": int(row["daily_limit"]),
        "totalLimit": int(row["total_limit"]),
        "createdAt": iso(int(row["created_at"])),
    }


class InviteStore:
    def __init__(self, store):
        self.store, self.postgres = store, store.dialect == "postgres"

    async def assertUsable(self, code, now=None):
        await self.store.initialize()
        usable(
            await self.store.db.fetchone(
                "SELECT * FROM music_registration_invites WHERE code_hash=?",
                [inviteCodeHash(code)],
            ),
            now_ms() if now is None else now,
        )
        return True

    async def issue(self, options=None, now=None):
        options, now = options or {}, now_ms() if now is None else now
        if not isinstance(options, dict) or set(options) - {
            "expiresAt",
            "maxUses",
            "dailyLimit",
            "totalLimit",
            "label",
        }:
            raise TypeError("Invalid invite options")
        expires, uses, label = (
            options.get("expiresAt", now + 7 * 86400000),
            options.get("maxUses", 1),
            options.get("label", ""),
        )
        if (
            type(expires) is not int
            or not now < expires <= 8640000000000000
            or type(uses) is not int
            or not 1 <= uses <= 10000
            or not isinstance(label, str)
            or len(label.encode("utf-16-le")) // 2 > 100
            or re.search(r"[\x00-\x1f\x7f]", label)
        ):
            raise TypeError("Invalid invite expiry, uses or label")
        policy = validateQuotaPolicy(
            {
                "dailyLimit": options.get("dailyLimit", 30),
                "totalLimit": options.get("totalLimit", 300),
                "enabled": True,
            }
        )
        await self.store.initialize()
        code = "TANGO-" + base64.urlsafe_b64encode(
            secrets.token_bytes(24)
        ).decode().rstrip("=")
        row = {
            "id": str(uuid.uuid4()),
            "code_hash": inviteCodeHash(code),
            "label": label.strip(),
            "expires_at": expires,
            "max_uses": uses,
            "used_count": 0,
            "disabled": 0,
            "daily_limit": policy["dailyLimit"],
            "total_limit": policy["totalLimit"],
            "created_at": now,
        }
        await self.store.db.execute(
            "INSERT INTO music_registration_invites(id,code_hash,label,expires_at,max_uses,daily_limit,total_limit,created_at) VALUES(?,?,?,?,?,?,?,?)",
            [
                row[k]
                for k in [
                    "id",
                    "code_hash",
                    "label",
                    "expires_at",
                    "max_uses",
                    "daily_limit",
                    "total_limit",
                    "created_at",
                ]
            ],
        )
        return {"code": code, "invite": metadata(row)}

    async def list(self, options=None, *, limit=100):
        limit = (options or {}).get("limit", limit)
        if type(limit) is not int or not 1 <= limit <= 500:
            raise TypeError("Invalid invite list limit")
        await self.store.initialize()
        return [
            metadata(row)
            for row in await self.store.db.fetchall(
                "SELECT id,label,expires_at,max_uses,used_count,disabled,daily_limit,total_limit,created_at FROM music_registration_invites ORDER BY created_at DESC,id DESC LIMIT ?",
                [limit],
            )
        ]

    async def disable(self, id):
        if not isinstance(id, str) or not re.fullmatch("[A-Za-z0-9_-]{8,100}", id):
            raise TypeError("Invalid invite id")
        await self.store.initialize()
        if (
            await self.store.db.execute(
                "UPDATE music_registration_invites SET disabled=1 WHERE id=?", [id]
            )
        ).rowCount != 1:
            raise ApiError(404, "INVITE_NOT_FOUND", "内测码记录不存在。")
        return {"id": id, "disabled": True}
