"""安全启动本机 Python 后台；拒绝把旧 Node 进程误认为新实现。"""

import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlsplit
import httpx
from .compat import strict_json_loads

PROJECT = Path(__file__).resolve().parents[2]
PROBE_LIMIT = 4096
PROBE_TIMEOUT = 1.8


async def _probe(client, address):
    # 健康检查只接收小型 JSON；限制整个响应而非仅单次网络读取，避免慢速持续发送。
    async with asyncio.timeout(PROBE_TIMEOUT):
        async with client.stream(
            "GET",
            address,
            follow_redirects=False,
            headers={"Accept-Encoding": "identity"},
        ) as response:
            if response.status_code != 200:
                raise ValueError("后台探测响应无效。")
            # 小型本机探测不接受压缩，避免先解压巨大内容再检查字节上限。
            if response.headers.get("content-encoding", "").strip().lower() not in [
                "",
                "identity",
            ]:
                raise ValueError("后台探测响应无效。")
            length = response.headers.get("content-length", "")
            if length.isdigit() and int(length) > PROBE_LIMIT:
                raise ValueError("后台探测响应过大。")
            chunks = []
            size = 0
            async for chunk in response.aiter_bytes():
                size += len(chunk)
                if size > PROBE_LIMIT:
                    raise ValueError("后台探测响应过大。")
                chunks.append(chunk)
            body = strict_json_loads(b"".join(chunks))
            if not isinstance(body, dict):
                raise ValueError("后台探测响应无效。")
            return body


async def launch(address):
    from .development_config import (
        ensureDevelopmentToken,
        readDevelopmentToken,
        developmentTokenFingerprint,
    )

    url = urlsplit(address)
    if (
        url.scheme != "http"
        or url.hostname not in ["127.0.0.1", "localhost"]
        or url.path not in ["", "/"]
        or url.query
        or url.fragment
        or url.username
        or url.password
    ):
        raise ValueError("本机后台启动地址无效；远程后台请独立部署。")
    flag = os.environ.get("MUSICPLAYER_DEVELOPMENT_MODE", "0")
    if flag not in ["0", "1"]:
        raise ValueError("开发模式配置无效。")
    development = flag == "1"
    if development and (
        url.hostname != "127.0.0.1"
        or os.environ.get("NODE_ENV") == "production"
        or os.environ.get("TANGO_ENV") == "production"
    ):
        raise ValueError("开发免登录仅允许本机开发环境。")
    tokenFile = os.environ.get(
        "MUSICPLAYER_DEVELOPMENT_TOKEN_FILE",
        str(PROJECT / ".local/development/session.token"),
    )
    fingerprint = None
    if development:
        await ensureDevelopmentToken(tokenFile)
        token = await readDevelopmentToken(tokenFile)
        fingerprint = developmentTokenFingerprint(token)
    from .storage_profile import selectedStorageProfile

    expectedStorage = os.environ.get("QUIET_STORAGE_DRIVER") or selectedStorageProfile(PROJECT)["driver"]
    origin = f"http://{url.netloc}"
    async with httpx.AsyncClient(
        timeout=PROBE_TIMEOUT, trust_env=False, follow_redirects=False
    ) as client:

        async def health():
            try:
                body = await _probe(client, origin + "/health")
            except httpx.ConnectError:
                return "absent"
            except Exception:
                return "occupied"
            if body.get("app") != "quiet-app-backend":
                return "occupied"
            if (
                body.get("runtimeLanguage") != "python"
                or body.get("runtimeVersion") != 2
            ):
                return "outdated"
            if (
                body.get("storage") != expectedStorage
                or body.get("storageVersion") != 1
                or (
                    expectedStorage in ("postgres", "mysql") and body.get("rateLimiter") != "redis"
                )
            ):
                return "mismatch"
            if (
                body.get("agentFeaturesVersion") != 2
                or body.get("accountFeaturesVersion") != 1
                or body.get("routingFeaturesVersion") != 1
                or body.get("modelRoutingVersion") != 1
                or body.get("lookupContextVersion") != 1
                or type(body.get("conversationClearVersion")) is not int
                or body.get("conversationClearVersion") != 1
                or type(body.get("musicIntentVersion")) is not int
                or body.get("musicIntentVersion") != 1
            ):
                return "outdated"
            if (
                bool(body.get("developmentMode")) != development
                or development
                and (
                    body.get("developmentSessionVersion") != 1
                    or body.get("developmentTokenFingerprint") != fingerprint
                )
            ):
                return "development-mismatch"
            try:
                ready = await _probe(client, origin + "/ready")
                return "ready" if ready.get("ready") is True else "unavailable"
            except Exception:
                return "unavailable"

        status = await health()
        if status == "ready":
            print("探歌 Python 本机后台已运行。")
            return
        messages = {
            "occupied": "后台地址被其它服务占用，未启动新的后台进程。",
            "mismatch": "现有后台存储模式不匹配，请先正常关闭本项目旧后台后重启。",
            "outdated": "该端口仍是旧后台，请正常关闭本项目旧后台，再启动 Python 版本。",
            "development-mismatch": "现有后台开发模式不匹配，请正常关闭该端口的旧后台后重启。",
            "unavailable": "后台依赖尚未就绪，请检查本项目数据库和 Redis。",
        }
        if status in messages:
            raise ValueError(messages[status])
        directory = PROJECT / "backend_py/.data"
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        logPath = directory / "backend.log"
        fd = os.open(logPath, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        os.chmod(logPath, 0o600)
        env = {
            **os.environ,
            "QUIET_BACKEND_HOST": url.hostname,
            "QUIET_BACKEND_PORT": str(url.port or 80),
            "QUIET_DEVELOPMENT_MODE": "1" if development else "0",
        }
        if development:
            env["QUIET_DEVELOPMENT_TOKEN_FILE"] = tokenFile
        try:
            child = subprocess.Popen(
                [sys.executable, "-m", "tango_backend.server"],
                cwd=PROJECT / "backend_py",
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=fd,
                stderr=fd,
                start_new_session=True,
            )
        finally:
            os.close(fd)
        for _ in range(80):
            await asyncio.sleep(0.1)
            if child.poll() is not None:
                break
            if await health() == "ready":
                print("探歌 Python 本机后台已启动；模型配置由后台管理。")
                return
        raise ValueError(
            "Python 后台启动失败，请检查后台配置与 backend_py/.data/backend.log。"
        )


def main():
    os.umask(0o077)
    try:
        asyncio.run(
            launch(sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:16382")
        )
    except Exception as error:
        # 固定的配置/健康检查错误不含秘密；其他异常不直接回显。
        print(
            str(error)
            if isinstance(error, ValueError)
            else "Python 后台启动失败，请检查本机配置。",
            file=sys.stderr,
        )
        raise SystemExit(1)


if __name__ == "__main__":
    main()
