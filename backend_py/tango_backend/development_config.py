import asyncio
import hashlib
import os
import re
import secrets
import stat
from pathlib import Path


def developmentTokenFingerprint(token):
    if not isinstance(token, str) or not re.fullmatch("[a-f0-9]{64}", token):
        raise ValueError("Invalid development token")
    return hashlib.sha256(token.encode("ascii")).hexdigest()


def _read(path):
    if not isinstance(path, str) or not os.path.isabs(path):
        raise ValueError("Development token requires an absolute private path")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        details = os.fstat(fd)
        if (
            not stat.S_ISREG(details.st_mode)
            or details.st_nlink != 1
            or details.st_uid != os.getuid()
            or details.st_mode & 0o077
            or not 64 <= details.st_size <= 65
        ):
            raise ValueError("Invalid development token file")
        value = os.read(fd, 66).decode("ascii")
        token = value[:-1] if value.endswith("\n") else value
        developmentTokenFingerprint(token)
        return token
    finally:
        os.close(fd)


async def readDevelopmentToken(path):
    # 非阻塞打开后再校验文件类型，FIFO 不会挂住后台。
    return await asyncio.to_thread(_read, path)


async def ensureDevelopmentToken(path):
    if not isinstance(path, str) or not os.path.isabs(path):
        raise ValueError("Development token requires an absolute private path")

    def create():
        Path(path).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        try:
            fd = os.open(
                path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600
            )
        except FileExistsError:
            return
        try:
            value = (secrets.token_hex(32) + "\n").encode("ascii")
            offset = 0
            while offset < len(value):
                offset += os.write(fd, value[offset:])
            os.fsync(fd)
        finally:
            os.close(fd)

    await asyncio.to_thread(create)
    await readDevelopmentToken(path)
    return path


async def developmentConfiguration(env=None, options=None, *, host=None):
    env = os.environ if env is None else env
    host = (options or {}).get("host", host)
    mode = env.get("QUIET_DEVELOPMENT_MODE", "0")
    if mode not in ["0", "1"]:
        raise ValueError("Invalid development mode")
    if mode == "0":
        return {"enabled": False}
    if (
        env.get("NODE_ENV") == "production"
        or env.get("TANGO_ENV") == "production"
        or host not in ["127.0.0.1", "::1"]
    ):
        raise ValueError("Development access requires a loopback development server")
    token = await readDevelopmentToken(env.get("QUIET_DEVELOPMENT_TOKEN_FILE"))
    return {
        "enabled": True,
        "production": False,
        "bindAddress": host,
        "token": token,
        "tokenFingerprint": developmentTokenFingerprint(token),
    }
