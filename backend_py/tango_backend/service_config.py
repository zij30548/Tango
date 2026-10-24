import asyncio
import os
import re
import uuid
from urllib.parse import urlparse


def storageConfiguration(env=None):
    env = os.environ if env is None else env
    driver = env.get("QUIET_STORAGE_DRIVER") or "mysql"
    if driver not in ["sqlite", "postgres", "mysql"]:
        raise ValueError("Invalid storage driver.")
    if driver == "sqlite":
        return {"driver": driver}
    database, redis, namespace = (
        env.get("QUIET_DATABASE_URL"),
        env.get("QUIET_REDIS_URL"),
        env.get("QUIET_REDIS_NAMESPACE"),
    )
    worker = env.get("QUIET_WORKER_ID", "")
    if (
        urlparse(database or "").scheme not in (["mysql"] if driver == "mysql" else ["postgres", "postgresql"])
        or urlparse(redis or "").scheme not in ["redis", "rediss"]
        or not isinstance(namespace, str)
        or not re.fullmatch("[A-Za-z0-9][A-Za-z0-9:_-]{0,79}", namespace)
        or worker
        and not re.fullmatch("[A-Za-z0-9_-]{1,48}", worker)
    ):
        raise ValueError(
            "Shared storage requires valid database, Redis and deployment namespace configuration."
        )
    return {
        "driver": driver,
        "databaseUrl": database,
        "redisUrl": redis,
        "namespace": namespace,
        "workerId": (worker or "worker") + "_" + str(uuid.uuid4()),
    }


async def connectRateRedis(configuration):
    from redis.asyncio import Redis

    client = Redis.from_url(
        configuration["redisUrl"],
        socket_connect_timeout=1.5,
        socket_timeout=1.5,
        retry_on_timeout=False,
        max_connections=1000,
        decode_responses=True,
    )
    try:
        await asyncio.wait_for(client.ping(), 4)
        return client
    except Exception:
        await client.aclose()
        raise RuntimeError("Redis connection unavailable.") from None


async def serviceReady(store, redis=None):
    async def check():
        if store.dialect in ("postgres", "mysql"):
            await store.db.execute("SELECT 1")
        if redis:
            await redis.ping()

    try:
        await asyncio.wait_for(check(), 1.5)
        return True
    except Exception:
        return False
