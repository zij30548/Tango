"""维护者只读诊断：关联持久证据，不恢复任务、不触发模型或输出聊天正文。"""

import argparse
import asyncio
from contextlib import asynccontextmanager
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import sys
import time
import unicodedata
from uuid import UUID
from urllib.parse import urlsplit, unquote


class DiagnosticError(Exception):
    """只携带固定错误码和中文说明，避免数据库异常泄露连接配置。"""

    def __init__(self, code, message):
        self.code, self.message = code, message
        super().__init__(message)


def fail(code, message):
    raise DiagnosticError(code, message)


class ReadDatabase:
    """连接和事务在上下文内独占；查询入口再限制为只读语句。"""

    def __init__(self, dialect, connection):
        self.dialect, self._connection = dialect, connection

    async def fetchall(self, sql, params=()):
        if not isinstance(sql, str) or not re.match(r"^SELECT\s", sql, re.I) or re.search(
            r";|/\*|--|\bINTO\b|\bFOR\s+UPDATE\b|\bLOCK\s+IN\s+SHARE\s+MODE\b|\b(?:GET_LOCK|RELEASE_LOCK|SLEEP|BENCHMARK|LOAD_FILE)\s*\(", sql, re.I
        ):
            fail("READ_ONLY", "诊断入口只允许读取，不接受修改语句。")
        try:
            if self.dialect == "sqlite":
                return [dict(row) for row in self._connection.execute(sql, params).fetchall()]
            from asyncmy.cursors import DictCursor
            from .storage_db import parameters

            query, values = parameters(sql, params, "mysql")
            async with self._connection.cursor(DictCursor) as cursor:
                await cursor.execute(query, tuple(values))
                return [dict(row) for row in await cursor.fetchall()]
        except DiagnosticError:
            raise
        except Exception as error:
            if (self.dialect == "sqlite" and isinstance(error, sqlite3.OperationalError)
                and ("no such table" in str(error) or "no such column" in str(error))) or (
                self.dialect == "mysql" and error.args and error.args[0] in (1146, 1054)
            ):
                fail("SCHEMA_INCOMPLETE", "诊断所需表或字段不存在；请核对后台版本，本工具不会初始化或迁移数据库。")
            fail("DATABASE_READ_FAILED", "数据库读取失败，未输出连接配置或原始异常。")


@asynccontextmanager
async def open_database(*, env_file=None, sqlite=None):
    """显式选库并建立一致只读快照，不调用产品初始化或恢复逻辑。"""
    if bool(env_file) == bool(sqlite):
        fail("INVALID_STORAGE", "须明确选择一个已有 SQLite 文件或数据库配置文件。")
    connection = None
    try:
        if sqlite:
            path = Path(sqlite).absolute()
            if path.is_symlink() or not path.is_file():
                fail("INVALID_STORAGE", "指定的 SQLite 文件不存在或不是常规文件；未创建数据库。")
            connection = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, isolation_level=None)
            connection.row_factory = sqlite3.Row
            connection.execute("PRAGMA query_only=ON")
            connection.execute("BEGIN")
            database = ReadDatabase("sqlite", connection)
        else:
            fd = os.open(Path(env_file).absolute(), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            try:
                details = os.fstat(fd)
                if not stat.S_ISREG(details.st_mode) or details.st_size > 65536:
                    fail("INVALID_STORAGE", "数据库配置须为大小受限的常规文件。")
                raw = os.read(fd, 65537).decode("utf-8")
            finally:
                os.close(fd)
            from dotenv import dotenv_values

            config = dotenv_values(stream=io.StringIO(raw), interpolate=False)
            url = urlsplit(config.get("QUIET_DATABASE_URL") or "")
            if config.get("QUIET_STORAGE_DRIVER") != "mysql" or url.scheme not in {"mysql", "mysql+asyncmy"} or not url.hostname or not url.path.strip("/") or url.query or url.fragment:
                fail("INVALID_STORAGE", "诊断配置须显式指定 MySQL 和完整数据库地址。")
            import asyncmy

            connection = await asyncmy.connect(
                host=url.hostname, port=url.port or 3306, user=unquote(url.username or ""),
                password=unquote(url.password or ""), db=unquote(url.path[1:]), charset="utf8mb4",
                autocommit=True, local_infile=False, connect_timeout=5, read_timeout=10,
            )
            async with connection.cursor() as cursor:
                await cursor.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")
                await cursor.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY")
            database = ReadDatabase("mysql", connection)
    except DiagnosticError:
        if connection:
            connection.close()
        raise
    except Exception:
        if connection:
            connection.close()
        fail("DATABASE_OPEN_FAILED", "无法打开指定数据库，请检查服务、配置路径和只读权限；未输出凭据或原始异常。")
    try:
        yield database
    finally:
        if database.dialect == "mysql":
            try:
                await connection.rollback()
            finally:
                connection.close()
        else:
            connection.rollback()
            connection.close()


def safe_id(value):
    """只显示不携带控制字符、地址或已知凭据前缀的标识。"""
    return value if isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,100}", value) and not value.lower().startswith(("sk-", "bearer", "cookie", "token")) else None


def number(value):
    return value if type(value) is int and 0 <= value <= 9007199254740991 else None


def choice(value, allowed):
    return value if isinstance(value, str) and value in allowed.split() else "unknown"


def decoded(value):
    try:
        if isinstance(value, str):
            if len(value.encode()) > 16 * 1024 * 1024:
                raise ValueError()
            value = json.loads(value)
        if not isinstance(value, dict):
            raise ValueError()
        return value
    except (ValueError, TypeError):
        fail("INVALID_RECORD", "持久记录格式异常；诊断未执行修复或输出原始内容。")


def items(value):
    return [entry for entry in value if isinstance(entry, dict)] if isinstance(value, list) else []


def code(value):
    return value if isinstance(value, str) and re.fullmatch(r"[A-Z][A-Z0-9_]{0,79}", value) else None


STATUS = "planning researching composing waiting_tools preview awaiting_apply adopted paused needs_clarification failed cancelled budget_exhausted running completed uncertain reserved dispatched"
NODES = "created understand understand_invalid plan research shared_research scene_agent releases_agent compose observe shared_observe dispatch tool_result tool_timeout preview brief_ready clarify clarify_waiting fixed_dispatch shared_planned scene_planned releases_planned scene_observed releases_observed scene_observe releases_observe pause resume cancel undo revise turn select_artist lock adopt adopted applied apply_failed recovered interrupted failed logout observation_program_checked supplement_skipped catalog_cache_hit no_candidates release_candidates_unavailable catalog_supplement_failed observer observer_degraded"
CHECKS = "real_ids locks duration exclude_live excluded_ids excluded_artists required_artist releases style catalog_language ranking adjustment catalog_supplement_failed observer observer_degraded"


def summary(row):
    return {"id": safe_id(row.get("id")), "userId": safe_id(row.get("user_id")),
            "deviceId": safe_id(row.get("device_id")), "createdAt": number(row.get("created_at")),
            "updatedAt": number(row.get("updated_at")), **({"revision": number(row.get("revision")), "status": choice(row.get("status"), STATUS)} if "status" in row else {})}


def attempt(row):
    result = {"id": safe_id(row.get("id")), "status": choice(row.get("status"), STATUS),
              "startedAt": number(row.get("started_at")), "finishedAt": number(row.get("finished_at")),
              "inputTokens": number(row.get("input_tokens")), "outputTokens": number(row.get("output_tokens")),
              "errorCode": code(row.get("error_code"))}
    if "paid" in row:
        result["paid"] = row["paid"] == 1
    for column, key in (("model_selection_json", "modelSelection"), ("model_metadata_json", "modelMetadata")):
        value = decoded(row[column]) if row.get(column) is not None else {}
        result[key] = {k: choice(value.get(k), allowed) for k, allowed in {
            "profile": "flash plus", "strategy": "flash_only task_split plus_only", "purpose": "route plan research observe compose",
        }.items() if k in value} if key == "modelSelection" else {}
        for source in (("configuredModel",) if key == "modelSelection" else ("providerModel",)):
            model = value.get(source)
            if isinstance(model, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,199}", model) and "://" not in model and not model.lower().startswith(("sk-", "bearer", "cookie", "token")):
                result[key][source] = model
    result["modelSelectionStatus"] = "recorded" if row.get("model_selection_json") is not None else "legacy_unknown"
    if row.get("diagnostics_json") is not None:
        diagnostic = decoded(row["diagnostics_json"])
        reasons = diagnostic.get("reasonCounts") or {}
        result["diagnostics"] = {
            "stage": choice(diagnostic.get("stage"), "compose model_result"),
            "outcome": choice(diagnostic.get("outcome"), "accepted degraded completed failed uncertain"),
            "reasonCounts": {k: number(v) for k, v in reasons.items() if k in "invalidEnvelope malformedJson invalidModelRanking invalidShape emptyOrder excessIndices invalidIndices stringIndices outOfRangeIndices duplicateIndices".split()},
        }
    return result


def query_text(value):
    if not isinstance(value, str):
        return None
    value = "".join(c for c in value if not unicodedata.category(c).startswith("C"))[:160]
    return re.sub(r"(?i)(?:https?://\S+|sk-[A-Za-z0-9_-]+|(?:bearer|cookie|token|api[_-]?key)\s*[:= ]\s*\S+)", "[已隐藏]", value)


def task_detail(row, state):
    if state.get("userId") != row["user_id"] or state.get("deviceId") != row["device_id"] or state.get("id") != row["id"]:
        fail("INVALID_RECORD", "任务快照与记录归属不一致；未关联其他账号或设备。")
    preview, brief = state.get("preview") or {}, state.get("brief") or {}
    trace_id = None
    try:
        value = (state.get("_observation") or {}).get("trace_id")
        if isinstance(value, str) and len(value) == 36:
            trace_id = str(UUID(value))
    except (ValueError, AttributeError, TypeError):
        pass
    return {**summary(row), "phase": choice(state.get("phase"), "plan research dispatch waiting observe compose done"),
            "observation": {"traceId": trace_id, "uploadStatus": "unknown"},
            "stateVersion": number(state.get("stateVersion")), "runId": safe_id(state.get("runId")),
            "errorCode": code((state.get("error") or {}).get("code")), "leaseUntil": number(row.get("lease_until")),
            "targetMinutes": number(brief.get("targetMinutes")), "excludeLive": brief.get("excludeLive") is True,
            "requireReleases": brief.get("requireReleases") is True, "lockedCount": len(state.get("lockedIds", [])),
            "preview": {"version": number(preview.get("version")), "trackCount": len(items(preview.get("tracks"))), "totalDurationMs": number(preview.get("totalDurationMs"))},
            "budget": {k: number(v) for k, v in (state.get("budget") or {}).items() if k in "runCalls maxRunCalls totalCalls maxTaskCalls toolCalls maxToolCalls inputTokens outputTokens cacheHits".split()},
            "pendingSteps": [{"id": safe_id(s.get("id")), "skill": choice(s.get("skill"), "scene.research releases.research"), "revision": number(s.get("revision")), "expiresAt": number(s.get("expiresAt"))} for s in items(state.get("pendingSteps"))[:4]]}


async def report(db, scope, identifier=None, limit=10, now_ms=None, show_queries=False):
    """固定范围读取，状态仍按证据分层呈现，不把计划成功认作业务成功。"""
    if scope not in {"recent", "conversation", "task"} or type(limit) is not int or not 1 <= limit <= 30:
        fail("INVALID_ARGUMENT", "诊断范围或数量无效，数量须为 1 至 30。")
    if scope != "recent" and (not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,100}", identifier)):
        fail("INVALID_ARGUMENT", "请提供有效会话或任务 ID。")
    now = int(time.time() * 1000) if now_ms is None else now_ms
    if number(now) is None:
        fail("INVALID_ARGUMENT", "诊断时间须为有效的毫秒时间戳。")
    result = {"schemaVersion": 1, "scope": scope, "readAt": now, "storage": db.dialect, "readOnly": True,
              "warnings": ["一致只读快照；读取不恢复任务、不调用模型。", "completed仅表示对应路由或调用完成；不等于任务、听感或完整播放成功。"]}
    async def rows(sql, params=()):
        return await db.fetchall(sql, params)

    if scope == "recent":
        result["conversations"] = [summary(r) for r in await rows("SELECT id,user_id,device_id,created_at,updated_at FROM music_conversations ORDER BY updated_at DESC,id DESC LIMIT ?", (limit,))]
        result["tasks"] = [summary(r) for r in await rows("SELECT id,user_id,device_id,revision,status,created_at,updated_at FROM music_tasks ORDER BY updated_at DESC,id DESC LIMIT ?", (limit,))]
        return result
    table = "music_conversations" if scope == "conversation" else "music_tasks"
    found = await rows("SELECT * FROM " + table + " WHERE id=?", (identifier,))
    if not found:
        fail("NOT_FOUND", "未找到指定记录；未按时间猜测或自动创建。")
    row = found[0]
    required = {"id", "user_id", "device_id", "created_at", "updated_at", "journal_json" if scope == "conversation" else "snapshot_json"}
    if not required <= set(row):
        fail("SCHEMA_INCOMPLETE", "诊断所需表或字段不存在；请核对后台版本，本工具不会初始化或迁移数据库。")
    if scope == "conversation":
        journal = decoded(row["journal_json"])
        result["conversation"] = {**summary(row), "version": number(journal.get("version")), "messageCount": len(items(journal.get("messages")))}
        result["messages"] = [{k: (number(m.get(k)) if k in {"sequence", "taskRevision"} else choice(m.get(k), "user assistant") if k == "role" else choice(m.get(k), "route task device_report") if k == "source" else safe_id(m.get(k))) for k in ("id", "role", "sequence", "turnId", "source", "taskId", "taskRevision")} for m in items(journal.get("messages"))[-limit:]]
        routes = await rows("SELECT id,message_id,status,paid,attempt_id,capability_version,error_code,created_at,finished_at,lease_until,result_json FROM music_conversation_routes WHERE conversation_id=? AND user_id=? AND device_id=? ORDER BY created_at DESC,id DESC LIMIT ?", (identifier, row["user_id"], row["device_id"], limit))
        attempts = await rows("SELECT a.* FROM music_conversation_route_attempts a JOIN music_conversation_routes r ON r.id=a.route_id WHERE r.conversation_id=? AND r.user_id=? AND r.device_id=? ORDER BY a.started_at DESC,a.id DESC LIMIT ?", (identifier, row["user_id"], row["device_id"], limit))
        attempt_map = {a["id"]: attempt(a) for a in attempts}
        result["routes"] = []
        for route in reversed(routes):
            plan = decoded(route["result_json"]) if route.get("result_json") is not None else {}
            entry = {"id": safe_id(route["id"]), "messageId": safe_id(route["message_id"]), "status": choice(route["status"], STATUS), "effectiveStatus": "uncertain" if route["status"] == "running" and (route.get("lease_until") or 0) <= now else choice(route["status"], STATUS), "paid": route["paid"] == 1, "errorCode": code(route.get("error_code")), "planner": choice(plan.get("planner"), "deterministic model"), "intents": [choice(i, "scene_research release_research song_lookup artist_catalog playlist_search conversation_history explain_result preference_proposal preference_deletion task_control clarification help unsupported") for i in plan.get("intents", [])[:12]], "targetMinutes": number((plan.get("slots") or {}).get("targetMinutes")), "attempt": attempt_map.get(route["attempt_id"])}
            entry.update(createdAt=number(route.get("created_at")), finishedAt=number(route.get("finished_at")), capabilityVersion=number(route.get("capability_version")))
            entry["actions"] = [{"id": safe_id(a.get("id")), "capability": choice(a.get("capability"), "music.task music.lookup music.refine music.artist_catalog music.playlist_search memory.propose task.control assistant.reply"), "dependsOn": [safe_id(v) for v in a.get("dependsOn", [])[:4]], "mode": choice((a.get("parameters") or {}).get("mode"), "new continue"), "operation": choice((a.get("parameters") or {}).get("operation"), "set clear select version exclude_version artist other_artist"), "action": choice((a.get("parameters") or {}).get("action"), "pause cancel resume undo")} for a in items(plan.get("actions"))[:4]]
            result["routes"].append(entry)
        links = journal.get("taskLinks") or {}
        result["tasks"] = []
        # MySQL JSON不保留对象插入顺序；以已保存用户轮次排序，并优先当前关联任务。
        sequences = {m.get("id"): number(m.get("sequence")) or 0 for m in items(journal.get("messages"))}
        keys = sorted(links, key=lambda task_id: (task_id == journal.get("latestTaskId"), sequences.get((links[task_id] or {}).get("turnId"), 0)), reverse=True)[:30]
        if len(links) > len(keys):
            result["warnings"].append("任务关联超过30条，优先当前任务并按已保存轮次展开30条；截取不代表其他关联不存在。")
        if keys:
            tasks = await rows("SELECT * FROM music_tasks WHERE id IN (" + ",".join("?" for _ in keys) + ") AND user_id=? AND device_id=?", (*keys, row["user_id"], row["device_id"]))
            for task in tasks:
                result["tasks"].append({**task_detail(task, decoded(task["snapshot_json"])), "link": {"turnId": safe_id((links[task["id"]] or {}).get("turnId")), "revision": number((links[task["id"]] or {}).get("revision"))}})
            if len(tasks) != len(keys):
                result["warnings"].append("部分任务关联缺失或归属不符，已拒绝关联；不猜测最近任务。")
        result["warnings"].append("未出现路由或任务可能是本机命令、单曲查找或未同步，不能仅据缺记录判断失败。")
        return result
    state = decoded(row["snapshot_json"])
    result["task"] = task_detail(row, state)
    result["trace"] = [{"node": choice(e.get("node"), NODES), "status": choice(e.get("status"), STATUS), "revision": number(e.get("revision")), "at": number(e.get("at"))} for e in items(state.get("trace"))[-64:]]
    result["checks"] = [{"key": choice(c.get("key"), CHECKS), "status": choice(c.get("status"), "satisfied unmet unverified not_requested")} for c in items((state.get("preview") or {}).get("checks"))[:30]]
    history = items(state.get("toolHistory"))
    result["toolHistoryCount"], result["toolHistory"] = len(history), []
    for entry in history[-limit:]:
        data, inputs = entry.get("result") or {}, entry.get("input") or {}
        item = {"stepId": safe_id(entry.get("stepId")), "skill": choice(entry.get("skill"), "scene.research releases.research"), "status": choice(entry.get("status"), STATUS), "revision": number(entry.get("revision")), "attempt": number(entry.get("attempt")), "dispatchedAt": number(entry.get("dispatchedAt")), "expiresAt": number(entry.get("expiresAt")), "completedAt": number(entry.get("completedAt")), "trackCount": len(items(data.get("tracks"))), "sourceCount": len(items(data.get("sources"))), "errorCode": code((data.get("error") or {}).get("code")), "cacheHit": (entry.get("cache") or {}).get("hit") is True}
        item.update(runId=safe_id(entry.get("runId")), resultReceived=isinstance(entry.get("result"), dict))
        item["queryCount"] = len(inputs["queries"]) if isinstance(inputs.get("queries"), list) else None
        if not item["resultReceived"]:
            item.update(trackCount=None, sourceCount=None)
        if show_queries:
            item["queries"] = [query_text(q) for q in inputs.get("queries", [])[:8]]
        result["toolHistory"].append(item)
    calls = await rows("SELECT * FROM music_task_calls WHERE task_id=? ORDER BY started_at DESC,id DESC LIMIT ?", (identifier, limit))
    result["calls"] = [{**attempt(c), "node": choice(c.get("node"), NODES), "revision": number(c.get("revision")), "runId": safe_id(c.get("run_id"))} for c in reversed(calls)]
    candidates = await rows("SELECT id,user_id,device_id,created_at,updated_at,journal_json FROM music_conversations WHERE user_id=? AND device_id=? ORDER BY updated_at DESC,id DESC LIMIT 31", (row["user_id"], row["device_id"]))
    result["conversations"] = [summary(c) for c in candidates[:30] if identifier in (decoded(c["journal_json"]).get("taskLinks") or {})]
    result["warnings"].extend(["trace最多保留64条；工具证据来自设备回执或缓存，未独立认证全部上游事实。", "调用列表按数量截取；reserved为预留，未知Token保持null；账号总用量请查账号管理。", "只检查最近30个同账号同设备会话的任务关联，未找到不代表从未关联。"])
    if show_queries:
        result["warnings"].append("已显式展开本机曲库检索文本；仍未输出聊天、参数全量和工具原始结果。")
    return result


class Parser(argparse.ArgumentParser):
    """错误只输出固定提示，不把维护者误粘贴的密钥作为参数回显。"""

    def error(self, message):
        self.exit(2, "参数无效，请用 --help 查看用法；未回显输入。\n")


def parser():
    root = Parser(description="探歌只读诊断：查看持久证据，不执行恢复或模型请求。")
    commands = root.add_subparsers(dest="scope", required=True)
    for name, title in (("recent", "最近记录"), ("conversation", "会话与路由"), ("task", "任务、工具、约束和调用")):
        command = commands.add_parser(name, help=title)
        storage = command.add_mutually_exclusive_group(required=True)
        storage.add_argument("--env-file", help="明确指定MySQL配置文件")
        storage.add_argument("--sqlite", help="明确指定已有SQLite文件")
        if name != "recent":
            command.add_argument("--id", required=True, help="会话或任务ID")
        command.add_argument("--limit", type=int, default=10, help="列表数量1至30，默认10")
        command.add_argument("--json", action="store_true", help="输出JSON格式")
        command.add_argument("--show-queries", action="store_true", help="显式展开有界本机曲库检索词，不展开聊天或完整工具参数")
    return root


async def run_diagnostics(argv):
    """可测试入口只返回脱敏报告；帮助在建立连接之前退出。"""
    options = parser().parse_args(argv)
    if not 1 <= options.limit <= 30 or (options.scope != "recent" and not re.fullmatch(r"[A-Za-z0-9_-]{1,100}", options.id)):
        fail("INVALID_ARGUMENT", "诊断数量或标识无效；数量须为1至30，ID须来自持久记录。")
    async with open_database(env_file=options.env_file, sqlite=options.sqlite) as db:
        return await report(db, options.scope, getattr(options, "id", None), options.limit, show_queries=options.show_queries)


def render(value):
    """按证据分组展示，保持空值未知，不添加成功率结论。"""
    names = {"conversations": "关联会话", "conversation": "会话", "messages": "消息定位", "routes": "路由记录", "tasks": "关联任务", "task": "任务进度", "trace": "执行轨迹", "toolHistory": "工具历史", "checks": "约束检查", "calls": "调用账本", "warnings": "阅读说明"}
    lines = ["探歌只读诊断｜" + value["scope"] + "｜" + value["storage"], "快照时间：" + datetime.fromtimestamp(value["readAt"] / 1000, timezone.utc).isoformat()]
    for key, title in names.items():
        if key in value:
            lines.append("\n" + title + "：")
            entries = value[key] if isinstance(value[key], list) else [value[key]]
            lines.extend("  " + (e if isinstance(e, str) else json.dumps(e, ensure_ascii=False, separators=(",", ":"))) for e in entries)
            if not entries:
                lines.append("  无记录／未知；请结合阅读说明判断。")
    return "\n".join(lines)


def main():
    try:
        value = asyncio.run(run_diagnostics(sys.argv[1:]))
        print(json.dumps(value, ensure_ascii=False, indent=2) if "--json" in sys.argv[1:] else render(value))
        return 0
    except DiagnosticError as error:
        print(error.code + "：" + error.message, file=sys.stderr)
        return 1
    except Exception:
        print("DIAGNOSTIC_FAILED：诊断未完成，未输出配置、正文或原始异常。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
