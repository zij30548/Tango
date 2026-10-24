"""维护者显式选择数据库；邀请码明文只写入新建的私密文件。

示例：python -m tango_backend.admin invite issue --sqlite /绝对路径/账号.sqlite --output /私密目录/beta.invite-codes.json
      python -m tango_backend.admin account set --env-file /私密配置/后台.env --user-id 账号ID --daily 30 --total 300 --enabled true
"""

import argparse
import asyncio
import contextlib
import io
import json
import os
from pathlib import Path
import stat
import sys
from urllib.parse import urlparse
from .compat import now_ms
from .store import Store, PostgresStore, MySQLStore
from .invite_store import InviteStore
from .account_quota import AccountPolicyStore


class ChineseFormatter(argparse.HelpFormatter):
    def _format_usage(self, usage, actions, groups, prefix):
        return super()._format_usage(
            usage, actions, groups, "用法：" if prefix is None else prefix
        )


class SafeParser(argparse.ArgumentParser):
    def __init__(self, *args, **kwargs):
        kwargs.setdefault("formatter_class", ChineseFormatter)
        kwargs["add_help"] = False
        super().__init__(*args, **kwargs)
        self._positionals.title, self._optionals.title = "位置参数", "选项"
        self.add_argument("-h", "--help", action="help", help="显示帮助信息并退出")

    def error(self, message):
        self.exit(2, "管理参数无效，请使用 --help 查看用法。未打印账号或配置内容。\n")


def integer(value):
    if not value.isascii() or not value.isdigit() or int(value) > 1000000000:
        raise argparse.ArgumentTypeError("须为范围内的非负整数")
    return int(value)


def parser():
    root = SafeParser(
        description="探歌后台管理：必须显式选择已有数据库，不自动加载 .env。"
    )
    groups = root.add_subparsers(
        dest="group", required=True, help="管理内测码或账号额度"
    )
    invites = groups.add_parser("invite", help="生成、查看或禁用内测码").add_subparsers(
        dest="action", required=True
    )
    accounts = groups.add_parser(
        "account", help="查看账号和调整额度，不清空历史用量"
    ).add_subparsers(dest="action", required=True)

    def storage(command):
        group = command.add_mutually_exclusive_group(required=True)
        group.add_argument("--sqlite", help="显式指定已有的常规 SQLite 文件")
        group.add_argument(
            "--env-file", help="显式指定数据库配置文件；不读取其他环境配置"
        )

    issue = invites.add_parser("issue", help="明码仅写新建的 0600 私密文件")
    storage(issue)
    issue.add_argument(
        "--output", required=True, help="不存在的新文件，后缀必须为 .invite-codes.json"
    )
    issue.add_argument(
        "--expires-days", type=integer, default=7, help="有效天数，1 至 365"
    )
    issue.add_argument(
        "--max-uses", type=integer, default=1, help="最大使用次数，1 至 10000"
    )
    issue.add_argument(
        "--daily-limit", type=integer, default=30, help="注册账号每日额度，允许 0"
    )
    issue.add_argument(
        "--total-limit", type=integer, default=300, help="注册账号累计额度，允许 0"
    )
    issue.add_argument("--label", default="", help="维护者备注，不写入密钥")
    storage(invites.add_parser("list", help="只显示元数据，不显示明码和哈希"))
    disable = invites.add_parser("disable", help="禁用指定内测码")
    storage(disable)
    disable.add_argument("--id", required=True, help="内测码记录 ID")
    users = accounts.add_parser("users", help="列出账号 ID、用户名与创建时间")
    storage(users)
    users.add_argument("--limit", type=integer, default=50, help="最多 100 条")
    show = accounts.add_parser("show", help="显示账号额度与已用次数")
    storage(show)
    show.add_argument("--user-id", required=True)
    update = accounts.add_parser("set", help="设置额度或停用 AI；不重置使用记录")
    storage(update)
    update.add_argument("--user-id", required=True)
    update.add_argument("--daily", type=integer, required=True)
    update.add_argument("--total", type=integer, required=True)
    update.add_argument("--enabled", choices=["true", "false"], required=True)
    return root


async def openAdminStore(*, sqlite=None, envFile=None):
    if bool(sqlite) == bool(envFile):
        raise ValueError("Select exactly one explicit storage configuration")
    database, url = Path(sqlite).absolute() if sqlite else None, None
    if envFile:
        path = Path(envFile).absolute()
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            details = os.fstat(fd)
            if not stat.S_ISREG(details.st_mode) or details.st_size > 65536:
                raise ValueError("Invalid explicit admin configuration")
            value = os.read(fd, 65537).decode("utf-8")
        finally:
            os.close(fd)
        from dotenv import dotenv_values

        configuration = dotenv_values(stream=io.StringIO(value), interpolate=False)
        driver = configuration.get("QUIET_STORAGE_DRIVER") or "sqlite"
        if driver in {"postgres", "mysql"}:
            url = configuration.get("QUIET_DATABASE_URL")
            allowed = ["mysql", "mysql+asyncmy"] if driver == "mysql" else ["postgres", "postgresql"]
            if not isinstance(url, str) or urlparse(url).scheme not in allowed:
                raise ValueError("Invalid explicit database configuration")
        elif driver == "sqlite":
            database = (
                path.parent
                / (configuration.get("QUIET_DB_PATH") or "./data/quiet.sqlite")
            ).absolute()
        else:
            raise ValueError("Invalid storage driver")
    if database:
        details = database.lstat()
        if not stat.S_ISREG(details.st_mode) or stat.S_ISLNK(details.st_mode):
            raise ValueError("Select an existing regular SQLite database")
        store = Store(str(database))
    else:
        store = (MySQLStore if urlparse(url).scheme in {"mysql", "mysql+asyncmy"} else PostgresStore)(connectionString=url, maximumConnections=2)
    try:
        await store.initialize()
        return store
    except BaseException:
        await store.close()
        raise


async def runAdmin(argv, *, now=now_ms):
    args = parser().parse_args(argv)
    if (
        args.group == "invite"
        and args.action == "issue"
        and (
            not args.output.endswith(".invite-codes.json")
            or not 1 <= args.expires_days <= 365
        )
    ):
        raise ValueError("Invalid output or expiry")
    store = await openAdminStore(sqlite=args.sqlite, envFile=args.env_file)
    fd, output, issued = None, None, None
    try:
        if args.group == "invite":
            invites = InviteStore(store)
            if args.action == "list":
                return {"ok": True, "invites": await invites.list()}
            if args.action == "disable":
                return {"ok": True, "invite": await invites.disable(args.id)}
            output = Path(args.output).absolute()
            fd = os.open(
                output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600
            )
            os.fchmod(fd, 0o600)
            timestamp = now()
            issued = await invites.issue(
                {
                    "expiresAt": timestamp + args.expires_days * 86400000,
                    "maxUses": args.max_uses,
                    "dailyLimit": args.daily_limit,
                    "totalLimit": args.total_limit,
                    "label": args.label,
                },
                timestamp,
            )
            payload = (
                json.dumps({"version": 1, **issued}, ensure_ascii=False, indent=2)
                + "\n"
            ).encode("utf-8")
            offset = 0
            while offset < len(payload):
                offset += os.write(fd, payload[offset:])
            os.fsync(fd)
            return {"ok": True, "outputFile": str(output), "invite": issued["invite"]}
        if args.action == "users":
            if not 1 <= args.limit <= 100:
                raise ValueError("Invalid list limit")
            return {
                "users": await store.db.fetchall(
                    "SELECT id,username,created_at FROM users ORDER BY created_at DESC,id DESC LIMIT ?",
                    [args.limit],
                )
            }
        import re

        if not re.fullmatch("[A-Za-z0-9_-]{8,100}", args.user_id):
            raise ValueError("Invalid user ID")
        quota = AccountPolicyStore(store)
        usage = (
            await quota.adminset(
                args.user_id,
                {
                    "dailyLimit": args.daily,
                    "totalLimit": args.total,
                    "enabled": args.enabled == "true",
                },
                now=now(),
            )
            if args.action == "set"
            else await quota.snapshot(args.user_id, now())
        )
        return {"userId": args.user_id, **usage, "dailyServerCapApplied": False}
    except BaseException:
        if issued:
            with contextlib.suppress(Exception):
                await InviteStore(store).disable(issued["invite"]["id"])
        if fd is not None:
            with contextlib.suppress(OSError):
                output.unlink()
        raise
    finally:
        if fd is not None:
            os.close(fd)
        await store.close()


def main():
    try:
        result = asyncio.run(runAdmin(sys.argv[1:]))
        sys.stdout.write(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    except (Exception, KeyboardInterrupt):
        sys.stderr.write(
            "管理操作失败。请检查显式数据库选项、账号 ID 和全新的私密输出文件；未打印凭据或配置内容。\n"
        )
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
