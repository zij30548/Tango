"""Python 后台启动入口：只有本进程加载模型密钥与存储配置。"""

import argparse
import asyncio
import os
from pathlib import Path

import uvicorn
from dotenv import dotenv_values

from .app import createApp
from .store import Store, PostgresStore
from .mysql_store import MysqlStore
from .storage_profile import selectedStorageProfile
from .service_config import storageConfiguration, connectRateRedis
from .security_config import securityConfiguration
from .development_config import developmentConfiguration
from .redis_limiter import RedisRateLimiter
from .task_progress import TaskProgressHub
from .task_tool_cache import createTaskToolCache
from .model import createModel
from .model_profiles import modelProfilesConfiguration
from .radio_model import createRadioModel
from .assistant_model import createAssistantModel
from .task_model import createTaskModel
from .intent_routing import createRoutingModel
from .http_protocol import BoundedH11Protocol
from .agent_tracing import AgentTracing

PROJECT = Path(__file__).resolve().parents[2]


def integer(name, fallback, minimum, maximum):
    raw = os.environ.get(name)
    if not raw:
        return fallback
    try:
        value = int(raw)
        if str(value) != raw.strip() or not minimum <= value <= maximum:
            raise ValueError()
        return value
    except ValueError:
        raise ValueError("Invalid " + name + " configuration.") from None


def load_environment(paths):
    # 当前shell显式配置优先，存储profile覆盖普通后台env；绝不打印键值。
    values = {}
    for path in paths:
        if Path(path).is_file():
            values.update(
                {k: v for k, v in dotenv_values(path).items() if v is not None}
            )
    for k, v in values.items():
        os.environ.setdefault(k, v)


async def configured_app():
    root = PROJECT / "backend_py"
    host = os.environ.get("QUIET_BACKEND_HOST", "127.0.0.1")
    development = await developmentConfiguration(os.environ, {"host": host})
    security = securityConfiguration(os.environ)
    storage = storageConfiguration(os.environ)
    # 解析角色配置只读当前后台环境；默认仍为 Flash，不根据客户端指定模型。
    modelProfiles = modelProfilesConfiguration(os.environ)
    options = modelProfiles["profiles"]["flash"].transportOptions()
    taskOptions = {**options, "modelProfiles": modelProfiles}
    sessionMs = integer("QUIET_SESSION_DAYS", 7, 1, 30) * 86400000
    store = (
        (MysqlStore if storage["driver"] == "mysql" else PostgresStore)(
            connectionString=storage["databaseUrl"],
            sessionMs=sessionMs,
            maximumConnections=integer("QUIET_DATABASE_POOL_SIZE", 10, 2, 100),
        )
        if storage["driver"] in ("postgres", "mysql")
        else Store(
            str(
                (
                    root / os.environ.get("QUIET_DB_PATH", "./.data/quiet.sqlite")
                ).resolve()
            ),
            sessionMs=sessionMs,
        )
    )
    redis = None
    tracing = AgentTracing.from_env(os.environ)
    try:
        await store.initialize()
        if storage["driver"] in ("postgres", "mysql"):
            redis = await connectRateRedis(storage)
        if development["enabled"]:
            security.update(
                requestPerMinute=max(security["requestPerMinute"], 6000),
                userRequestPerMinute=max(security["userRequestPerMinute"], 4000),
                conversationCreatesPerDay=max(
                    security["conversationCreatesPerDay"], 1000
                ),
            )
        app = createApp(
            store=store,
            tracing=tracing,
            model=createModel(options),
            radioModel=createRadioModel(options),
            assistantModel=createAssistantModel(options),
            taskModel=createTaskModel(taskOptions),
            routingModel=createRoutingModel(taskOptions),
            modelRouting={
                "version": 1,
                "strategy": modelProfiles["strategy"],
                "plusEnabled": modelProfiles["profiles"]["plus"].configured,
                "selection": "server_policy",
            },
            **security,
            developmentSession=development,
            redis=redis,
            **(
                {
                    "rateLimiter": RedisRateLimiter(
                        client=redis, namespace=storage["namespace"]
                    )
                }
                if redis
                else {}
            ),
            progressHub=TaskProgressHub(
                client=redis, namespace=storage.get("namespace", "tango:local")
            ),
            toolCache=createTaskToolCache(
                client=redis, namespace=storage.get("namespace", "tango:local")
            ),
            workerId=storage.get("workerId"),
            leaseMs=integer("QUIET_TASK_LEASE_MS", 30000, 500, 120000),
            authPerTenMinutes=integer("QUIET_AUTH_PER_TEN_MINUTES", 20, 1, 1000),
            authFailuresPerTenMinutes=integer(
                "QUIET_AUTH_FAILURES_PER_TEN_MINUTES", 10, 1, 1000
            ),
            aiPerMinute=max(
                integer("QUIET_AI_PER_MINUTE", 5, 1, 1000),
                1000 if development["enabled"] else 0,
            ),
            aiDailyLimit=max(
                integer("QUIET_AI_DAILY_LIMIT", 30, 1, 100000),
                10000 if development["enabled"] else 0,
            ),
            aiGlobalDailyLimit=integer("QUIET_AI_GLOBAL_DAILY_LIMIT", 500, 1, 1000000),
            aiGlobalConcurrency=integer("QUIET_AI_GLOBAL_CONCURRENCY", 8, 1, 100),
            taskStrategy=os.environ.get("QUIET_TASK_STRATEGY", "single_agent"),
            trustLoopbackProxy=os.environ.get("QUIET_TRUST_LOOPBACK_PROXY") == "1",
        )
        return app, store, redis
    except BaseException:
        await asyncio.to_thread(tracing.close, timeout=3)
        if redis:
            await redis.aclose()
        await store.close()
        raise


async def serve():
    store = redis = app = None
    try:
        app, store, redis = await configured_app()
        config = uvicorn.Config(
            app,
            host=os.environ.get("QUIET_BACKEND_HOST", "127.0.0.1"),
            port=integer("QUIET_BACKEND_PORT", 16382, 1, 65535),
            http=BoundedH11Protocol,
            access_log=False,
            log_level="warning",
            timeout_keep_alive=5,
            timeout_graceful_shutdown=5,
            limit_concurrency=integer("QUIET_HTTP_MAX_CONNECTIONS", 1000, 10, 10000),
            h11_max_incomplete_event_size=16384,
            proxy_headers=False,
        )
        server = uvicorn.Server(config)
        await server.serve()
        if not server.started:
            return 1
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    except Exception:
        # 配置/连接错误只报告固定文案，禁止打印连接串和上游异常。
        print(
            "探歌 Python 后台启动失败，请检查后台配置及数据库/Redis。", flush=True
        )
        return 1
    finally:
        if app:
            await app.close()
        if redis:
            await redis.aclose()
        if store:
            await store.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description="探歌 Python 后台")
    parser.add_argument("--env-file", action="append", default=None)
    parser.add_argument(
        "--no-env", action="store_true", help="隔离测试时不加载本地配置"
    )
    args = parser.parse_args()
    os.umask(0o077)
    if not args.no_env:
        load_environment(
            args.env_file
            or [PROJECT / "backend_py/.env", selectedStorageProfile(PROJECT)["envFile"]]
        )
    raise SystemExit(asyncio.run(serve()))


if __name__ == "__main__":
    main()
