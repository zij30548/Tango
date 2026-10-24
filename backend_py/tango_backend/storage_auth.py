"""兼容 Node scrypt 旧记录与 v2，固定算法成本并做常量时间比较。"""

import asyncio
import hashlib
import hmac
import re
import secrets

_DUMMY_SALT, _DUMMY_HASH = secrets.token_bytes(16), secrets.token_bytes(64)


def sessionTokenHash(token):
    return hashlib.sha256(token.encode()).hexdigest()


async def _derive(password, salt, legacy=False):
    encoded = (
        password.encode("utf-16-le", "surrogatepass")
        .decode("utf-16-le", "replace")
        .encode("utf-8")
    )
    return await asyncio.to_thread(
        hashlib.scrypt,
        encoded,
        salt=salt,
        n=16384 if legacy else 32768,
        r=8,
        p=1 if legacy else 3,
        maxmem=64 * 1024 * 1024,
        dklen=64,
    )


async def passwordRecord(password):
    salt = secrets.token_bytes(16)
    return {
        "salt": salt.hex(),
        "hash": "scrypt-v2$" + (await _derive(password, salt)).hex(),
    }


def passwordNeedsUpgrade(record):
    return bool(
        record
        and re.fullmatch("[a-f0-9]{32}", record.get("password_salt", ""))
        and re.fullmatch("[a-f0-9]{128}", record.get("password_hash", ""))
    )


async def passwordMatches(password, record):
    salt_ok = bool(
        record
        and isinstance(record.get("password_salt"), str)
        and re.fullmatch("[a-f0-9]{32}", record["password_salt"])
    )
    value = record.get("password_hash", "") if record else ""
    legacy = (
        salt_ok
        and isinstance(value, str)
        and bool(re.fullmatch("[a-f0-9]{128}", value))
    )
    current = (
        salt_ok
        and isinstance(value, str)
        and bool(re.fullmatch(r"scrypt-v2\$[a-f0-9]{128}", value))
    )
    valid = legacy or current
    salt = bytes.fromhex(record["password_salt"]) if valid else _DUMMY_SALT
    expected = bytes.fromhex(value[10:] if current else value) if valid else _DUMMY_HASH
    actual = await _derive(password if isinstance(password, str) else "", salt, legacy)
    return hmac.compare_digest(actual, expected) and valid and isinstance(password, str)
