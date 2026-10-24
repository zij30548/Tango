"""只在 runner 显式提供的临时实例中建随机库；每例结束删除自己创建的库。"""

import os
from contextlib import asynccontextmanager
from urllib.parse import urlsplit, urlunsplit, unquote
from uuid import uuid4

import pytest


@asynccontextmanager
async def mysql_database():
    url = os.environ.get("TANGO_MYSQL_TEST_URL")
    if not url:
        pytest.skip("需要独立 MySQL runner；禁止读取当前应用用户库")
    import asyncmy

    parsed = urlsplit(url)
    assert parsed.scheme == "mysql" and parsed.hostname in ("127.0.0.1", "localhost", "::1")
    admin = await asyncmy.connect(
        host=parsed.hostname, port=parsed.port or 3306,
        user=unquote(parsed.username or ""), password=unquote(parsed.password or ""),
        autocommit=True, charset="utf8mb4",
    )
    name = "tango_case_" + uuid4().hex
    try:
        async with admin.cursor() as cursor:
            await cursor.execute("CREATE DATABASE `" + name + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
        yield urlunsplit((parsed.scheme, parsed.netloc, "/" + name, "", ""))
    finally:
        async with admin.cursor() as cursor:
            await cursor.execute("DROP DATABASE IF EXISTS `" + name + "`")
        await admin.ensure_closed()
