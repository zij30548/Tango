"""显式切换本机存储；迁移失败时绝不悄悄连接到另一份用户库。"""

import json
import os
import stat
from pathlib import Path


def selectedStorageProfile(project):
    project = Path(project)
    mysql = project / ".local/mysql-v1"
    marker = mysql / "active.json"
    if marker.exists() or marker.is_symlink():
        fd = os.open(marker, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != os.getuid()
                or stat.S_IMODE(info.st_mode) != 0o600
                or info.st_nlink != 1
                or info.st_size > 4096
            ):
                raise ValueError("本机 MySQL 启用标记无效。")
            with os.fdopen(fd, "r", encoding="utf-8", closefd=False) as stream:
                value = json.load(stream)
            if not isinstance(value, dict) or type(value.get("version")) is not int or value != {"driver": "mysql", "version": 1} or not (mysql / "storage.env").is_file():
                raise ValueError("本机 MySQL 配置不完整。")
        finally:
            os.close(fd)
        return {"driver": "mysql", "envFile": mysql / "storage.env"}
    legacy = project / ".local/storage-v1/storage.env"
    if legacy.is_file():
        # 仅迁移前识别旧配置；成功切换后 marker 优先，故障不会回写旧库。
        return {"driver": "postgres", "envFile": legacy}
    return {"driver": "mysql", "envFile": mysql / "storage.env"}
