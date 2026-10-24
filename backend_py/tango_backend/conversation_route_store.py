"""每个持久用户轮次只有一个路由尝试；失败和未知结果均不能自动重发。"""

import asyncio
import json
import re
from uuid import uuid4
from .errors import ApiError
from .compat import digest, now_ms, js_json, safe_integer
from .conversation_store import ConversationStore, conversationId, _decoded
from .store import ensureModelLedgerColumns
from .task_store import modelSelectionJson, modelMetadataJson

SCHEMA = """CREATE TABLE IF NOT EXISTS music_conversation_routes (
 id TEXT PRIMARY KEY, conversation_id TEXT NOT NULL REFERENCES music_conversations(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, device_id TEXT NOT NULL, message_id TEXT NOT NULL,
 input_hash TEXT NOT NULL, input_json TEXT NOT NULL, capability_version BIGINT NOT NULL,
 status TEXT NOT NULL, paid INTEGER NOT NULL, attempt_id TEXT NOT NULL UNIQUE,
 owner_id TEXT NOT NULL, owner_token TEXT NOT NULL, owner_epoch BIGINT NOT NULL, lease_until BIGINT NOT NULL,
 result_json TEXT, error_code TEXT, created_at BIGINT NOT NULL, finished_at BIGINT,
 UNIQUE(conversation_id,message_id));
 CREATE TABLE IF NOT EXISTS music_conversation_route_attempts (
 id TEXT PRIMARY KEY, route_id TEXT NOT NULL UNIQUE REFERENCES music_conversation_routes(id) ON DELETE CASCADE,
 status TEXT NOT NULL, paid INTEGER NOT NULL, started_at BIGINT NOT NULL, finished_at BIGINT,
 input_tokens BIGINT, output_tokens BIGINT, error_code TEXT);
 CREATE INDEX IF NOT EXISTS music_conversation_routes_owner ON music_conversation_routes(user_id,device_id);"""


def _stale():
    return ApiError(409, "ROUTE_CLAIM_LOST", "路由执行凭据已失效，请读取已保存的状态。")


def _positive(n):
    return safe_integer(n) and n > 0


def _bounded(value, maximum=96 * 1024):
    text = js_json(value)
    if len(text.encode()) > maximum:
        raise TypeError("Routing data exceeds its bounded JSON contract")
    return text


def _view(row, now):
    if not row:
        return None
    expired = row["status"] == "running" and int(row["lease_until"]) <= now
    return {
        "id": row["id"],
        "conversationId": row["conversation_id"],
        "messageId": row["message_id"],
        "deviceId": row["device_id"],
        "inputHash": row["input_hash"],
        "input": _decoded(row["input_json"]),
        "capabilityVersion": int(row["capability_version"]),
        "status": "uncertain" if expired else row["status"],
        "paid": row["paid"] == 1,
        "attemptId": row["attempt_id"],
        "result": _decoded(row["result_json"])
        if row["result_json"] is not None
        else None,
        "errorCode": "ROUTE_LEASE_EXPIRED" if expired else row["error_code"],
        "createdAt": int(row["created_at"]),
        "finishedAt": int(row["finished_at"])
        if row["finished_at"] is not None
        else None,
    }


class ConversationRouteStore:
    def __init__(self, options=None, **kwargs):
        o = {**(options or {}), **kwargs}
        self.store, self.taskStore = o.get("store"), o.get("taskStore")
        self.now, self.workerId = (
            o.get("now", now_ms),
            o.get("workerId") or str(uuid4()),
        )
        self.leaseMs, self.dailyLimit = o.get("leaseMs", 30000), o.get("dailyLimit", 30)
        self.globalDailyLimit, self.globalConcurrency = (
            o.get("globalDailyLimit", 1000),
            o.get("globalConcurrency", 8),
        )
        if (
            not self.store
            or not self.taskStore
            or not all(
                _positive(n)
                for n in [
                    self.leaseMs,
                    self.dailyLimit,
                    self.globalDailyLimit,
                    self.globalConcurrency,
                ]
            )
            or not isinstance(self.workerId, str)
            or not self.workerId
        ):
            raise TypeError("Invalid route repository configuration")
        self.conversations = o.get("conversations") or ConversationStore(
            self.store, now=self.now
        )
        self.localHooks, self.localClaims = o.get("localHooks", {}), {}
        self.postgres = self.store.dialect == "postgres"
        self.mysql = self.store.dialect == "mysql"
        self.distributed = self.store.dialect in ("postgres", "mysql")
        self._initialized, self._init_lock = False, asyncio.Lock()

    async def initialize(self):
        async with self._init_lock:
            if not self._initialized:
                await self.store.initialize()
                await self.taskStore.initialize()
                await self.conversations.initialize()
                if not self.mysql:
                    async with self.store.db.transaction():
                        if self.postgres:
                            await self.store.db.execute(
                                "SELECT pg_advisory_xact_lock($1,$2)", (1952546919, 6)
                            )
                        await self.store.db.executescript(SCHEMA)
                        await ensureModelLedgerColumns(
                            self.store.db, "music_conversation_route_attempts", self.store.dialect
                        )
                self._initialized = True
                self.store._conversationRoutesReady = True
        return self

    def validateKey(self, request):
        for key in ["conversationId", "messageId", "deviceId"]:
            conversationId(request.get(key))
        if not isinstance(request.get("userId"), str) or not request["userId"]:
            raise TypeError("Missing route owner")

    def assertJournal(self, row, request):
        if not row or row["user_id"] != request["userId"] or row.get("cleared_at") is not None:
            raise ApiError(404, "CONVERSATION_NOT_FOUND", "会话不存在。")
        if row["device_id"] != request["deviceId"]:
            raise ApiError(409, "CONVERSATION_DEVICE_MISMATCH", "请在原设备继续。")
        state = _decoded(row["journal_json"])
        anchor = next(
            (
                e
                for e in state["messages"]
                if e["id"] == request["messageId"] and e["role"] == "user"
            ),
            None,
        )
        if not anchor or request["messageId"] not in state["receipts"]:
            raise ApiError(404, "MESSAGE_NOT_FOUND", "消息尚未保存。")
        return state, anchor

    async def get(self, request):
        self.validateKey(request)
        await self.initialize()
        db = self.store.db
        journal = await db.fetchone(
            "SELECT * FROM music_conversations WHERE id=? AND user_id=?",
            (request["conversationId"], request["userId"]),
        )
        self.assertJournal(journal, request)
        row = await db.fetchone(
            "SELECT * FROM music_conversation_routes WHERE conversation_id=? AND message_id=? AND user_id=? AND device_id=?",
            (
                request["conversationId"],
                request["messageId"],
                request["userId"],
                request["deviceId"],
            ),
        )
        return _view(row, self.now())

    async def findForTask(self, request):
        conversationId(request["deviceId"])
        conversationId(request["messageId"])
        await self.initialize()
        cleared = await self.store.db.fetchone("SELECT 1 FROM music_conversation_routes r JOIN music_conversations c ON c.id=r.conversation_id AND c.user_id=r.user_id WHERE r.user_id=? AND r.device_id=? AND r.message_id=? AND c.cleared_at IS NOT NULL LIMIT 1", (request["userId"], request["deviceId"], request["messageId"]))
        if cleared:
            raise ApiError(409, "CONVERSATION_CLEARED", "原对话已清除，请在新对话提出要求。")
        rows = await self.store.db.fetchall(
            "SELECT * FROM music_conversation_routes WHERE user_id=? AND device_id=? AND message_id=? AND status='completed'",
            (request["userId"], request["deviceId"], request["messageId"]),
        )
        if len(rows) > 1:
            raise ApiError(
                409,
                "ROUTE_CONTEXT_AMBIGUOUS",
                "此消息关联多个会话，不能推断任务上下文。",
            )
        matches = [
            r
            for r in rows
            if _decoded(r["input_json"]).get("instruction") == request["instruction"]
            and any(
                a.get("capability") == "music.task"
                for a in (_decoded(r["result_json"]) or {}).get("actions", [])
            )
        ]
        if not matches:
            return None
        row = matches[0]
        route = await self.get(
            {
                "conversationId": row["conversation_id"],
                "userId": request["userId"],
                "deviceId": request["deviceId"],
                "messageId": request["messageId"],
            }
        )
        return {"input": route["input"], "result": route["result"]}

    async def begin(self, request, paid=True, modelSelection=None):
        selection = modelSelectionJson(modelSelection)
        if not paid and selection is not None:
            raise TypeError("Free routes cannot record a paid model selection")
        self.validateKey(request)
        if (
            type(paid) is not bool
            or not _positive(request.get("capabilityVersion"))
            or not isinstance(request.get("inputHash"), str)
            or not re.fullmatch(r"[a-f0-9]{64}", request["inputHash"])
            or request["inputHash"] != digest(request["input"])
        ):
            raise TypeError("Invalid frozen route input")
        input_json = _bounded(request["input"])
        await self.initialize()
        now, routeId, attemptId = self.now(), str(uuid4()), str(uuid4())
        claim = {
            "routeId": routeId,
            "attemptId": attemptId,
            "token": str(uuid4()),
            "epoch": 1,
            "workerId": self.workerId,
            "userId": request["userId"],
            "expiresAt": now + self.leaseMs,
            "paid": paid,
        }
        columns = "id,conversation_id,user_id,device_id,message_id,input_hash,input_json,capability_version,status,paid,attempt_id,owner_id,owner_token,owner_epoch,lease_until,created_at"
        values = [
            routeId,
            request["conversationId"],
            request["userId"],
            request["deviceId"],
            request["messageId"],
            request["inputHash"],
            input_json,
            request["capabilityVersion"],
            "running",
            int(paid),
            attemptId,
            self.workerId,
            claim["token"],
            1,
            claim["expiresAt"],
            now,
        ]
        db, locally_reserved = self.store.db, False
        try:
            async with db.transaction():
                if self.distributed:
                    await self.taskStore.lockUser(db, request["userId"])
                journal = await db.fetchone(
                    "SELECT * FROM music_conversations WHERE id=? AND user_id=?"
                    + (" FOR UPDATE" if self.distributed else ""),
                    (request["conversationId"], request["userId"]),
                )
                existing = await db.fetchone(
                    "SELECT * FROM music_conversation_routes WHERE conversation_id=? AND message_id=?",
                    (request["conversationId"], request["messageId"]),
                )
                state, anchor = self.assertJournal(journal, request)
                if existing:
                    if (
                        existing["input_hash"] != request["inputHash"]
                        or int(existing["capability_version"])
                        != request["capabilityVersion"]
                        or existing["paid"] != int(paid)
                    ):
                        raise ApiError(
                            409,
                            "IDEMPOTENCY_CONFLICT",
                            "同一消息不能用于不同路由输入。",
                        )
                    return {
                        "route": _view(existing, now),
                        "acquired": False,
                        "claim": None,
                    }
                users = [e for e in state["messages"] if e["role"] == "user"]
                if not users or users[-1]["id"] != request["messageId"]:
                    raise ApiError(
                        409,
                        "CONVERSATION_STALE",
                        "已有更新的用户消息，请处理最新一轮。",
                    )
                expected = {
                    "instruction": anchor["content"],
                    "messageId": request["messageId"],
                    "conversationId": request["conversationId"],
                    "deviceId": request["deviceId"],
                }
                if any(request["input"].get(k) != v for k, v in expected.items()):
                    raise ApiError(
                        409,
                        "CONVERSATION_ANCHOR_MISMATCH",
                        "路由输入与已保存消息不一致。",
                    )
                if paid and self.distributed:
                    await self.taskStore.reserveSlot(
                        db,
                        {
                            "callId": attemptId,
                            "userId": request["userId"],
                            "claim": claim,
                            "now": now,
                            "globalConcurrency": self.globalConcurrency,
                        },
                    )
                    await self.taskStore.reserveUsage(
                        db,
                        request["userId"],
                        now,
                        self.dailyLimit,
                        self.globalDailyLimit,
                    )
                elif paid:
                    active = await db.fetchone(
                        "SELECT 1 FROM music_conversation_routes WHERE user_id=? AND paid=1 AND status='running' AND lease_until>? LIMIT 1",
                        (request["userId"], now),
                    )
                    count = await db.fetchone(
                        "SELECT count(*) AS n FROM music_conversation_routes WHERE paid=1 AND status='running' AND lease_until>?",
                        (now,),
                    )
                    if active or int(count["n"]) >= self.globalConcurrency:
                        raise ApiError(
                            429, "AI_BUSY", "已有模型请求执行中，请稍后继续。"
                        )
                    if self.localHooks.get("reserve"):
                        reserved = self.localHooks["reserve"](claim)
                        if hasattr(reserved, "__await__"):
                            if hasattr(reserved, "close"):
                                reserved.close()
                            raise TypeError("Route localHooks must be synchronous")
                    locally_reserved = True
                    result = await db.fetchone(
                        """INSERT INTO music_task_global_usage(utc_day,request_count) VALUES(?,1)
                      ON CONFLICT(utc_day) DO UPDATE SET request_count=request_count+1 WHERE request_count<? RETURNING request_count""",
                        (iso_day(now), self.globalDailyLimit),
                    )
                    if not result:
                        raise ApiError(
                            429, "AI_GLOBAL_DAILY_LIMIT", "全站今日 AI 预算已用完。"
                        )
                    await self.taskStore.quota.reserve(
                        request["userId"], now, self.dailyLimit, client=db
                    )
                await db.execute(
                    f"INSERT INTO music_conversation_routes({columns}) VALUES({','.join('?' for _ in values)})",
                    values,
                )
                await db.execute(
                    "INSERT INTO music_conversation_route_attempts(id,route_id,status,paid,started_at,model_selection_json) VALUES(?,?,?,?,?,?)",
                    (attemptId, routeId, "reserved", int(paid), now, selection),
                )
                row = await db.fetchone(
                    "SELECT * FROM music_conversation_routes WHERE id=?", (routeId,)
                )
            if locally_reserved:
                self.localClaims[attemptId] = claim
            return {"route": _view(row, now), "acquired": True, "claim": claim}
        except BaseException:
            if locally_reserved and self.localHooks.get("release"):
                self.localHooks["release"](claim)
            raise

    def validClaim(self, row, claim):
        return bool(
            row
            and claim
            and row["id"] == claim["routeId"]
            and row["attempt_id"] == claim["attemptId"]
            and row["owner_id"] == claim["workerId"]
            and row["owner_token"] == claim["token"]
            and int(row["owner_epoch"]) == claim["epoch"]
            and row["user_id"] == claim["userId"]
        )

    def releaseLocal(self, claim):
        held = self.localClaims.get(claim["attemptId"])
        if held and held["token"] == claim["token"]:
            self.localClaims.pop(claim["attemptId"], None)
            if self.localHooks.get("release"):
                self.localHooks["release"](held)

    async def finish(self, claim, options=None, **kwargs):
        o = {**(options or {}), **kwargs}
        metadata = modelMetadataJson(o.get("modelMetadata"))
        status, result, errorCode, usage = (
            o["status"],
            o.get("result"),
            o.get("errorCode"),
            o.get("usage"),
        )
        if (
            status not in ["completed", "failed", "uncertain"]
            or errorCode is not None
            and (
                not isinstance(errorCode, str)
                or not re.fullmatch(r"[A-Z][A-Z0-9_]{0,79}", errorCode)
            )
        ):
            raise TypeError("Invalid route completion")
        result_json = None if result is None else _bounded(result)

        def tokens(key):
            n = (usage or {}).get(key)
            if n is not None and (not safe_integer(n) or n < 0):
                raise TypeError("Invalid route usage")
            return n

        input_tokens, output_tokens = tokens("inputTokens"), tokens("outputTokens")
        await self.initialize()
        db, now = self.store.db, self.now()
        async with db.transaction():
            # 共享数据库始终按账号、会话、路由顺序加锁，提交时再次校验最新轮次。
            if self.distributed:
                await self.taskStore.lockUser(db, claim["userId"])
            identity = await db.fetchone(
                "SELECT conversation_id FROM music_conversation_routes WHERE id=? AND user_id=?",
                (claim["routeId"], claim["userId"]),
            )
            if not identity:
                raise _stale()
            journal = await db.fetchone(
                "SELECT journal_json,cleared_at FROM music_conversations WHERE id=? AND user_id=?"
                + (" FOR UPDATE" if self.distributed else ""),
                (identity["conversation_id"], claim["userId"]),
            )
            row = await db.fetchone(
                "SELECT * FROM music_conversation_routes WHERE id=?"
                + (" FOR UPDATE" if self.distributed else ""),
                (claim["routeId"],),
            )
            if not self.validClaim(row, claim):
                raise _stale()
            if journal and journal.get("cleared_at") is not None:
                # 清除后只补记原尝试的费用事实，晚到回复与模型提案不能恢复正文。
                await db.execute("UPDATE music_conversation_route_attempts SET status=?,error_code='CONVERSATION_CLEARED',finished_at=?,input_tokens=?,output_tokens=?,model_metadata_json=? WHERE id=? AND status='reserved'", (status, now, input_tokens, output_tokens, metadata, claim["attemptId"]))
                if self.distributed:
                    await db.execute("DELETE FROM music_task_model_slots WHERE call_id=? AND owner_token=? AND owner_epoch=?", (claim["attemptId"], claim["token"], claim["epoch"]))
                outcome = _view(row, now)
                self.releaseLocal(claim)
                return outcome
            if row["status"] == "running":
                expired = int(row["lease_until"]) <= now
                users = (
                    [
                        e
                        for e in _decoded(journal["journal_json"])["messages"]
                        if e["role"] == "user"
                    ]
                    if journal
                    else []
                )
                latest = users[-1]["id"] if users else None
                if (
                    not expired
                    and status == "completed"
                    and latest != row["message_id"]
                ):
                    final_status, final_result, final_code = (
                        "failed",
                        None,
                        "CONVERSATION_STALE",
                    )
                else:
                    final_status, final_result, final_code = (
                        ("uncertain", None, "ROUTE_LEASE_EXPIRED")
                        if expired
                        else (status, result_json, errorCode)
                    )
                await db.execute(
                    "UPDATE music_conversation_routes SET status=?,result_json=?,error_code=?,finished_at=? WHERE id=?",
                    (final_status, final_result, final_code, now, claim["routeId"]),
                )
                await db.execute(
                    "UPDATE music_conversation_route_attempts SET status=?,error_code=?,finished_at=?,input_tokens=?,output_tokens=?,model_metadata_json=? WHERE id=? AND status=?",
                    (
                        final_status,
                        final_code,
                        now,
                        input_tokens,
                        output_tokens,
                        metadata,
                        claim["attemptId"],
                        "reserved",
                    ),
                )
                if final_status == "completed":
                    # 回复和路由结果共用外层事务：晚到结果不留回复，写入中途失败全部回滚。
                    # appendRouteReply 重入已持有的账号/会话锁，并使用同连接 SAVEPOINT。
                    for action in (result.get("actions", []) if isinstance(result, dict) else []):
                        if action.get("capability") == "assistant.reply":
                            await self.conversations.appendRouteReply(
                                row["conversation_id"],
                                claim["userId"],
                                row["device_id"],
                                {
                                    "turnId": row["message_id"],
                                    "replyId": action["id"] + "_reply",
                                    "content": action["parameters"]["text"],
                                },
                            )
            if self.distributed:
                await db.execute(
                    "DELETE FROM music_task_model_slots WHERE call_id=? AND owner_token=? AND owner_epoch=?",
                    (claim["attemptId"], claim["token"], claim["epoch"]),
                )
            outcome = _view(
                await db.fetchone(
                    "SELECT * FROM music_conversation_routes WHERE id=?",
                    (claim["routeId"],),
                ),
                now,
            )
        self.releaseLocal(claim)
        return outcome

    async def heartbeat(self, claim):
        await self.initialize()
        db, now = self.store.db, self.now()
        expires = now + self.leaseMs
        if self.distributed:
            async with db.transaction():
                row = await db.fetchone(
                    "SELECT * FROM music_conversation_routes WHERE id=? FOR UPDATE",
                    (claim["routeId"],),
                )
                if (
                    not self.validClaim(row, claim)
                    or row["status"] != "running"
                    or int(row["lease_until"]) <= now
                ):
                    return False
                await db.execute(
                    "UPDATE music_conversation_routes SET lease_until=? WHERE id=?",
                    (expires, claim["routeId"]),
                )
                if row["paid"] == 1:
                    slot = await db.execute(
                        "UPDATE music_task_model_slots SET lease_until=? WHERE call_id=? AND owner_token=? AND owner_epoch=? AND lease_until>?",
                        (
                            expires,
                            claim["attemptId"],
                            claim["token"],
                            claim["epoch"],
                            now,
                        ),
                    )
                    if not slot.rowCount:
                        raise _stale()
                return True
        result = await db.execute(
            "UPDATE music_conversation_routes SET lease_until=? WHERE id=? AND attempt_id=? AND owner_id=? AND owner_token=? AND owner_epoch=? AND user_id=? AND status='running' AND lease_until>?",
            (
                expires,
                claim["routeId"],
                claim["attemptId"],
                claim["workerId"],
                claim["token"],
                claim["epoch"],
                claim["userId"],
                now,
            ),
        )
        if not result.rowCount:
            self.releaseLocal(claim)
        return bool(result.rowCount)


def iso_day(value):
    from .compat import iso

    return iso(value)[:10]
