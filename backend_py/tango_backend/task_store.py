"""任务检查点与付费尝试账本，复用账号数据库事务。"""

import asyncio
import json
import re
import uuid
from .errors import ApiError
from .compat import digest, clone, now_ms, iso, safe_integer
from .account_quota import AccountPolicyStore
from .storage_postgres import safeCount, callDiagnostic, pgTaskConflict
from .store import ensureModelLedgerColumns, ensureTaskRequestMetadataColumn

ACTIVE = ["planning", "researching", "composing", "waiting_tools"]
WORKING = ["planning", "researching", "composing"]
SQLITE_TASK_SCHEMA = """CREATE TABLE IF NOT EXISTS music_tasks(id TEXT PRIMARY KEY,user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
device_id TEXT NOT NULL,revision INTEGER NOT NULL,status TEXT NOT NULL,snapshot_json TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL) STRICT;
CREATE INDEX IF NOT EXISTS music_tasks_owner ON music_tasks(user_id,device_id,updated_at);
CREATE TABLE IF NOT EXISTS music_task_requests(user_id TEXT NOT NULL,device_id TEXT NOT NULL,request_id TEXT NOT NULL,
payload_hash TEXT NOT NULL,task_id TEXT NOT NULL REFERENCES music_tasks(id) ON DELETE CASCADE,request_metadata_json TEXT,PRIMARY KEY(user_id,device_id,request_id)) STRICT;
CREATE TABLE IF NOT EXISTS music_task_calls(id TEXT PRIMARY KEY,task_id TEXT NOT NULL REFERENCES music_tasks(id) ON DELETE CASCADE,
revision INTEGER NOT NULL,run_id TEXT NOT NULL DEFAULT '',node TEXT NOT NULL,status TEXT NOT NULL,started_at INTEGER NOT NULL,finished_at INTEGER,
input_tokens INTEGER,output_tokens INTEGER,error_code TEXT,diagnostics_json TEXT) STRICT;
CREATE TABLE IF NOT EXISTS music_task_global_usage(utc_day TEXT PRIMARY KEY,request_count INTEGER NOT NULL) STRICT;"""


def snapshot(row):
    if not row:
        return None
    value = row["snapshot_json"]
    return json.loads(value) if isinstance(value, str) else clone(value)


def busy():
    return ApiError(429, "AI_BUSY", "同一账号已有任务或模型请求执行中，请稍后继续。")


def options(value=None, **kwargs):
    return {**(value or {}), **kwargs}


REQUEST_ACTIONS = {
    "turn", "resume", "pause", "cancel", "adopt", "applied", "undo", "lock", "select_artist",
}


def requestMetadataJson(action, body):
    """持久化已验证入口的原操作事实；正文、偏好和曲库上下文不进入回执。"""
    if action is None:
        return None
    if not isinstance(body, dict):
        raise TypeError("Request metadata requires the original operation")
    if action == "create":
        # 创建入口允许额外字段，不能把正文夹带的 action 误认成采用操作。
        facts = {"action": "create"}
    elif (
        isinstance(action, str)
        and action in REQUEST_ACTIONS
        and body.get("action") == action
        and safe_integer(body.get("expectedRevision"))
        and body["expectedRevision"] >= 1
    ):
        facts = {"action": action, "expectedRevision": int(body["expectedRevision"])}
    else:
        raise TypeError("Invalid original request metadata")
    return json.dumps(facts, separators=(",", ":"))


def readRequestFacts(value):
    """旧行和畸形元数据保持未知；不能从任务当前状态倒推历史动作。"""
    if isinstance(value, str):
        if len(value) > 160:
            return {}
        try:
            value = json.loads(value)
        except (ValueError, TypeError):
            return {}
    if not isinstance(value, dict) or not isinstance(value.get("action"), str):
        return {}
    action = value["action"]
    if action == "create" and set(value) == {"action"}:
        return {"action": action}
    if (
        action in REQUEST_ACTIONS
        and set(value) == {"action", "expectedRevision"}
        and type(value.get("expectedRevision")) is int
        and 1 <= value["expectedRevision"] <= 9007199254740991
    ):
        return {"action": action, "expectedRevision": value["expectedRevision"]}
    return {}


MODEL_SELECTION_REASONS = {
    "routing_flash", "flash_baseline", "plus_baseline", "simple_task", "multi_intent",
    "dependent_actions", "pending_continuation", "confirmed_revision", "multiple_constraints",
}


def _modelIdentifier(value):
    return (
        isinstance(value, str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,199}", value) is not None
        and "://" not in value
        and not value.lower().startswith(("sk-", "bearer", "cookie", "token="))
    )


def modelSelectionJson(value):
    """只接受后台模型策略的固定投影，不接收客户端或模型任意附加字段。"""
    if value is None:
        return None
    if (
        not isinstance(value, dict)
        or set(value) != {"profile", "configuredModel", "configFingerprint", "policyVersion", "strategy", "purpose", "reason"}
        or value["profile"] not in {"flash", "plus"}
        or not _modelIdentifier(value["configuredModel"])
        or not isinstance(value["configFingerprint"], str)
        or re.fullmatch(r"[a-f0-9]{64}", value["configFingerprint"]) is None
        or value["policyVersion"] != "qwen-task-routing-v1"
        or value["strategy"] not in {"flash_only", "task_split", "plus_only"}
        or value["purpose"] not in {"route", "plan", "research", "observe", "compose"}
        or value["reason"] not in MODEL_SELECTION_REASONS
    ):
        raise TypeError("Invalid model selection ledger metadata")
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"))


def modelMetadataJson(value):
    """上游只留实际模型和响应标识，禁止保存 URL、凭据或完整响应。"""
    if value is None:
        return None
    if (
        not isinstance(value, dict)
        or set(value) - {"providerModel", "providerResponseId"}
        or any(item is not None and not _modelIdentifier(item) for item in value.values())
    ):
        raise TypeError("Invalid provider model ledger metadata")
    cleaned = {key: item for key, item in value.items() if item is not None}
    return json.dumps(cleaned, separators=(",", ":")) if cleaned else None


def readModelLedger(row):
    """新增列为 NULL 的历史行明确未知，不能以当前配置冒充历史模型。"""
    def decoded(key):
        value = row.get(key)
        return json.loads(value) if isinstance(value, str) else clone(value)

    selection, metadata = decoded("model_selection_json"), decoded("model_metadata_json")
    return {"selectionStatus": "recorded" if selection is not None else "legacy_unknown",
            "modelSelection": selection, "modelMetadata": metadata}


class CommonTaskStore:
    def __init__(self, store):
        self.accountStore, self.db, self.dialect = store, store.db, store.dialect
        self.quota = store.accountPolicy or AccountPolicyStore(store)

    async def initialize(self):
        await self.accountStore.initialize()
        return self

    async def query(self, sql, values=()):
        await self.initialize()
        return await self.db.execute(sql, values)

    async def transaction(self, operation):
        await self.initialize()
        async with self.db.transaction() as db:
            return await operation(db)

    async def load(self, id, userId):
        await self.initialize()
        return snapshot(
            await self.db.fetchone(
                "SELECT snapshot_json FROM music_tasks WHERE id=? AND user_id=?",
                [id, userId],
            )
        )

    async def latest(self, userId, deviceId):
        await self.initialize()
        return snapshot(
            await self.db.fetchone(
                "SELECT snapshot_json FROM music_tasks WHERE user_id=? AND device_id=? ORDER BY updated_at DESC,created_at DESC LIMIT 1",
                [userId, deviceId],
            )
        )

    async def replay(self, userId, deviceId, requestId, body):
        await self.initialize()
        row = await self.db.fetchone(
            "SELECT * FROM music_task_requests WHERE user_id=? AND device_id=? AND request_id=?",
            [userId, deviceId, requestId],
        )
        if not row:
            return None
        if row["payload_hash"] != digest(body):
            raise ApiError(
                409, "IDEMPOTENCY_CONFLICT", "同一操作标识不能用于不同要求。"
            )
        return await self.load(row["task_id"], userId)

    async def requestReceipt(self, userId, deviceId, requestId):
        """只读取原操作的持久事实；未找到不等于请求从未到达。"""
        await self.initialize()
        row = await self.db.fetchone(
            """SELECT t.snapshot_json FROM music_task_requests r
            JOIN music_tasks t ON t.id=r.task_id AND t.user_id=r.user_id
              AND t.device_id=r.device_id
            WHERE r.user_id=? AND r.device_id=? AND r.request_id=?""",
            [userId, deviceId, requestId],
        )
        return snapshot(row)

    async def requestFacts(self, userId, deviceId, requestId):
        """只读原请求的受限事实，账号与设备必须同时匹配其绑定任务。"""
        await self.initialize()
        row = await self.db.fetchone(
            """SELECT r.request_metadata_json FROM music_task_requests r
            JOIN music_tasks t ON t.id=r.task_id AND t.user_id=r.user_id
              AND t.device_id=r.device_id
            WHERE r.user_id=? AND r.device_id=? AND r.request_id=?""",
            [userId, deviceId, requestId],
        )
        return readRequestFacts(row["request_metadata_json"]) if row else {}

    async def remember(self, task, requestId, body, client=None, requestAction=None):
        db = client or self.db
        if not db.isTransaction:
            raise TypeError("Idempotency records require task transaction")
        metadata = requestMetadataJson(requestAction, body)
        await db.execute(
            "INSERT INTO music_task_requests(user_id,device_id,request_id,payload_hash,task_id,request_metadata_json) VALUES(?,?,?,?,?,?)",
            [task["userId"], task["deviceId"], requestId, digest(body), task["id"], metadata],
        )

    async def _checkReplay(self, db, task, requestId, body):
        row = await db.fetchone(
            "SELECT payload_hash FROM music_task_requests WHERE user_id=? AND device_id=? AND request_id=?",
            [task["userId"], task["deviceId"], requestId],
        )
        if row:
            same = row["payload_hash"] == digest(body)
            raise ApiError(
                409,
                "IDEMPOTENT_REPLAY" if same else "IDEMPOTENCY_CONFLICT",
                "此操作已提交，请获取已保存的任务。"
                if same
                else "同一操作标识不能用于不同要求。",
            )

    async def assertConversationActive(self, db, task, requestId, requestAction):
        """清除账号聊天后，旧消息迟到创建和旧任务采用都不能继续派发。"""
        # applied 仅补记已授权采用的设备回执；运行时仍核验原 adoptionId 与状态，不触发播放。
        if requestAction in ("pause", "cancel", "applied"):
            return
        if self.dialect != "mysql" and not getattr(self.accountStore, "_conversationRoutesReady", False):
            # 独立 TaskStore 可能尚未建聊天表；同库的另一实例建表后仍必须遵守墓碑。
            exists = await db.fetchone("SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='music_conversation_routes'" if self.dialect == "postgres" else "SELECT 1 FROM sqlite_master WHERE type='table' AND name='music_conversation_routes'")
            if not exists:
                return
        row = await db.fetchone("SELECT 1 FROM music_conversation_routes r JOIN music_conversations c ON c.id=r.conversation_id AND c.user_id=r.user_id WHERE r.user_id=? AND r.device_id=? AND c.cleared_at IS NOT NULL AND (r.message_id=? OR EXISTS (SELECT 1 FROM music_task_requests q WHERE q.user_id=r.user_id AND q.device_id=r.device_id AND q.request_id=r.message_id AND q.task_id=?)) LIMIT 1", (task["userId"], task["deviceId"], requestId, task["id"]))
        if row:
            raise ApiError(409, "CONVERSATION_CLEARED", "原对话已清除，请在新对话提出要求。")

    async def getGlobalDailyUsage(self, day):
        await self.initialize()
        row = await self.db.fetchone(
            "SELECT request_count FROM music_task_global_usage WHERE utc_day=?", [day]
        )
        return safeCount(row["request_count"]) if row else 0

    globalUsage = getGlobalDailyUsage

    async def getAccountUsage(self, userId, now=None):
        return await self.quota.snapshot(userId, now_ms() if now is None else now)

    async def setAccountQuota(self, userId, policy, value=None, **kwargs):
        return await self.quota.adminset(userId, policy, options(value, **kwargs))

    async def callCounts(self, taskId, runId, revision, client=None):
        await self.initialize()
        field, key = ("run_id", runId) if runId else ("revision", revision)
        row = await (client or self.db).fetchone(
            f"SELECT count(*) AS total,COALESCE(SUM(CASE WHEN {field}=? THEN 1 ELSE 0 END),0) AS run FROM music_task_calls WHERE task_id=?",
            [key, taskId],
        )
        return {"total": safeCount(row["total"]), "run": safeCount(row["run"])}

    async def activeForUser(self, userId):
        await self.initialize()
        return [
            snapshot(row)
            for row in await self.db.fetchall(
                "SELECT snapshot_json FROM music_tasks WHERE user_id=? AND status IN ('planning','researching','composing','waiting_tools')",
                [userId],
            )
        ]

    async def hasActiveTask(self, userId, exceptTaskId=None):
        return any(
            task["id"] != exceptTaskId for task in await self.activeForUser(userId)
        )

    async def reserveUsage(self, client, userId, now, userLimit, globalLimit):
        if not client.isTransaction:
            raise TypeError("Usage reservation requires a transaction")
        if self.dialect == "mysql":
            # 全站和账户额度在同一事务内预留；账户拒绝会回滚前面的全站计数。
            day = iso(now)[:10]
            await client.lock_key("music:global_usage:" + day)
            row = await client.fetchone(
                "SELECT request_count FROM music_task_global_usage WHERE utc_day=? FOR UPDATE",
                [day],
            )
            count = safeCount(row["request_count"]) if row else 0
            if count >= globalLimit:
                raise ApiError(
                    429, "AI_GLOBAL_DAILY_LIMIT", "全站今日 AI 预算已用完，请稍后继续。"
                )
            if row:
                await client.execute(
                    "UPDATE music_task_global_usage SET request_count=? WHERE utc_day=?",
                    [count + 1, day],
                )
            else:
                await client.execute(
                    "INSERT INTO music_task_global_usage(utc_day,request_count) VALUES(?,?)",
                    [day, count + 1],
                )
            await self.quota.reserve(userId, now, userLimit, client)
            return
        globalUsage = await client.fetchone(
            """INSERT INTO music_task_global_usage(utc_day,request_count) VALUES(?,1)
ON CONFLICT(utc_day) DO UPDATE SET request_count=music_task_global_usage.request_count+1
WHERE music_task_global_usage.request_count<? RETURNING request_count""",
            [iso(now)[:10], globalLimit],
        )
        if not globalUsage:
            raise ApiError(
                429, "AI_GLOBAL_DAILY_LIMIT", "全站今日 AI 预算已用完，请稍后继续。"
            )
        await self.quota.reserve(userId, now, userLimit, client)

    def recovered(self, current, counts, now):
        current["budget"]["totalCalls"] = max(
            current["budget"]["totalCalls"], counts["total"]
        )
        current["budget"]["runCalls"] = max(
            current["budget"]["runCalls"], counts["run"]
        )
        current.update(
            status="paused",
            pendingSteps=[],
            stateVersion=current.get("stateVersion", 0) + 1,
            updatedAt=max(now, current.get("updatedAt", 0) + 1),
            statusText="任务已暂停，可继续",
        )
        current["recoveryReason"] = (
            "后台执行租约已过期；未确认结果的模型调用不会自动重放，继续可能产生新的调用费用。"
            if self.dialect in ("postgres", "mysql")
            else "设备或服务器断开，恢复后将重新验证待执行步骤。"
            if current.get("phase") == "waiting"
            else "服务器重启；未确认结果的模型调用不会自动重放，继续可能产生新的调用费用。"
        )
        current["trace"] = (
            current.get("trace", [])
            + [
                {
                    "node": "recovered",
                    "status": "paused",
                    "revision": current["revision"],
                    "at": current["updatedAt"],
                    "note": "已保存任务，请主动继续。",
                }
            ]
        )[-64:]
        return current


class TaskStore(CommonTaskStore):
    def __new__(cls, store):
        if cls is TaskStore and store.dialect == "mysql":
            from .mysql_task_store import MysqlTaskStore

            return MysqlTaskStore(store)
        if cls is TaskStore and store.dialect == "postgres":
            from .postgres_task_store import PostgresTaskStore

            return PostgresTaskStore(store)
        return super().__new__(cls)

    def __init__(self, store):
        super().__init__(store)
        self._initialized, self._initialize_lock = False, asyncio.Lock()

    async def initialize(self):
        await self.accountStore.initialize()
        async with self._initialize_lock:
            if not self._initialized:
                async with self.db.transaction():
                    await self.db.executescript(SQLITE_TASK_SCHEMA)
                    columns = {
                        row["name"]
                        for row in await self.db.fetchall(
                            "PRAGMA table_info(music_task_calls)"
                        )
                    }
                    if "run_id" not in columns:
                        await self.db.execute(
                            "ALTER TABLE music_task_calls ADD COLUMN run_id TEXT NOT NULL DEFAULT ''"
                        )
                    if "diagnostics_json" not in columns:
                        await self.db.execute(
                            "ALTER TABLE music_task_calls ADD COLUMN diagnostics_json TEXT"
                        )
                    await ensureModelLedgerColumns(self.db, "music_task_calls", self.dialect)
                    await ensureTaskRequestMetadataColumn(self.db, self.dialect)
                self._initialized = True
        return self

    async def writeTask(self, task, value=None, **kwargs):
        value = options(value, **kwargs)
        expected = value.get("expectedStateVersion")
        if expected is not None:
            if (
                type(expected) is not int
                or expected < 0
                or task.get("stateVersion") != expected + 1
            ):
                raise TypeError("Task writes require the preceding stateVersion")
            row = await self.db.fetchone(
                "SELECT user_id,snapshot_json FROM music_tasks WHERE id=?", [task["id"]]
            )
            if (
                row
                and (
                    row["user_id"] != task["userId"]
                    or snapshot(row).get("stateVersion", 0) != expected
                )
            ) or (not row and expected != 0):
                raise pgTaskConflict()
            if task["status"] in ACTIVE and await self.hasActiveTask(
                task["userId"], task["id"]
            ):
                raise busy()
        await self.db.execute(
            """INSERT INTO music_tasks(id,user_id,device_id,revision,status,snapshot_json,created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET revision=excluded.revision,status=excluded.status,snapshot_json=excluded.snapshot_json,updated_at=excluded.updated_at""",
            [
                task["id"],
                task["userId"],
                task["deviceId"],
                task["revision"],
                task["status"],
                json.dumps(task, ensure_ascii=False, separators=(",", ":")),
                task["createdAt"],
                task["updatedAt"],
            ],
        )

    async def save(self, task, value=None, **kwargs):
        await self.initialize()
        async with self.db.transaction():
            await self.writeTask(task, options(value, **kwargs))

    async def commit(self, task, requestId, body, value=None, **kwargs):
        await self.initialize()
        value = options(value, **kwargs)
        async with self.db.transaction() as db:
            await self.assertConversationActive(db, task, requestId, value.get("requestAction"))
            await self._checkReplay(db, task, requestId, body)
            await self.writeTask(task, value)
            await self.remember(task, requestId, body, db, value.get("requestAction"))

    async def interrupted(self, value=None, **kwargs):
        await self.initialize()
        return [
            snapshot(row)
            for row in await self.db.fetchall(
                "SELECT snapshot_json FROM music_tasks WHERE status IN ('planning','researching','composing','waiting_tools')"
            )
        ]

    async def reserveCall(
        self,
        task,
        node,
        now,
        accountStore,
        userLimit,
        globalLimit,
        value=None,
        **kwargs,
    ):
        selection = modelSelectionJson(options(value, **kwargs).get("modelSelection"))
        await self.initialize()
        id = str(uuid.uuid4())
        async with self.db.transaction() as db:
            await self.reserveUsage(db, task["userId"], now, userLimit, globalLimit)
            await db.execute(
                "INSERT INTO music_task_calls(id,task_id,revision,run_id,node,status,started_at,model_selection_json) VALUES(?,?,?,?,?,?,?,?)",
                [
                    id,
                    task["id"],
                    task["revision"],
                    task.get("runId", ""),
                    node,
                    "reserved",
                    now,
                    selection,
                ],
            )
            await self.writeTask(task)
        return id

    async def reserveLegacyUsage(
        self, userId, now, accountStore, userLimit, globalLimit, value=None, **kwargs
    ):
        await self.initialize()
        async with self.db.transaction() as db:
            await self.reserveUsage(db, userId, now, userLimit, globalLimit)

    async def finishCall(
        self, id, status, now, usage=None, errorCode=None, diagnostics=None, modelMetadata=None
    ):
        metadata = modelMetadataJson(modelMetadata)
        await self.initialize()
        diagnostic = callDiagnostic(diagnostics, status)
        await self.db.execute(
            "UPDATE music_task_calls SET status=?,finished_at=?,input_tokens=?,output_tokens=?,error_code=?,diagnostics_json=?,model_metadata_json=? WHERE id=? AND status='reserved'",
            [
                status,
                now,
                (usage or {}).get("inputTokens"),
                (usage or {}).get("outputTokens"),
                errorCode,
                json.dumps(diagnostic) if diagnostic else None,
                metadata,
                id,
            ],
        )

    async def markUncertain(self, taskId, now):
        await self.db.execute(
            "UPDATE music_task_calls SET status='uncertain',finished_at=?,error_code='SERVER_RESTART' WHERE task_id=? AND status='reserved'",
            [now, taskId],
        )

    async def recoverInterrupted(self, task, value=None, **kwargs):
        await self.initialize()
        now = options(value, **kwargs).get("now", now_ms())
        async with self.db.transaction():
            current = await self.load(task["id"], task["userId"])
            if (
                not current
                or current.get("stateVersion", 0) != task.get("stateVersion", 0)
                or current["status"] not in ACTIVE
            ):
                return None
            await self.markUncertain(task["id"], now)
            self.recovered(
                current,
                await self.callCounts(
                    task["id"], current.get("runId"), current["revision"]
                ),
                now,
            )
            await self.writeTask(current)
            return current

    async def lockUser(self, client, userId):
        if not await client.fetchone("SELECT id FROM users WHERE id=?", [userId]):
            raise ApiError(404, "TASK_NOT_FOUND", "音乐任务不存在。")

    async def claimTask(self, *args, **kwargs):
        return None

    async def heartbeatTask(self, *args, **kwargs):
        return True

    releaseTask = isTaskClaimCurrent = heartbeatTask

    async def acquireLegacy(self, *args, **kwargs):
        return None

    heartbeatLegacy = releaseLegacy = heartbeatTask
