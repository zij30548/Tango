"""本机启动前确保已选择的基础设施就绪；不会自动把旧数据换成空库。"""

import json
import os
from pathlib import Path

from .mysql_infra import start, private_write
from .storage_profile import selectedStorageProfile

PROJECT = Path(__file__).resolve().parents[2]


def ensure(project=PROJECT):
    project = Path(project)
    # 显式外部部署配置由维护者管理，不启动另一套本机数据库。
    if os.environ.get("QUIET_STORAGE_DRIVER"):
        return
    selected = selectedStorageProfile(project)
    if selected["driver"] == "postgres":
        raise ValueError("发现旧 PostgreSQL 配置，请先显式迁移后启用 MySQL。")
    directory = project / ".local/mysql-v1"
    marker = directory / "active.json"
    if not marker.exists():
        for old in [project / "backend/data/quiet.sqlite", project / "backend_py/.data/quiet.sqlite"]:
            if old.exists() and old.stat().st_size:
                raise ValueError("发现旧本机账号库，请先显式迁移数据后启用 MySQL。")
    start(directory)
    if not marker.exists():
        private_write(marker, json.dumps({"driver": "mysql", "version": 1}) + "\n")


def main():
    try:
        ensure()
    except Exception:
        print("数据库启动失败；请检查私有存储配置，旧数据不会被自动替换。")
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
