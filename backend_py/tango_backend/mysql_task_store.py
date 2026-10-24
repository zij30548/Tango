"""MySQL 跨实例任务 CAS、租约与调用账本，使用显式事务及持久锁行。"""

import json
import uuid
from .errors import ApiError
from .compat import now_ms
from .storage_postgres import (
    pgTaskConflict as taskConflict,
    pgLeaseConflict as leaseConflict,
    positiveInteger,
    safeCount,
    callDiagnostic,
)
from .task_store import (CommonTaskStore, ACTIVE, WORKING, snapshot, busy, options,
                         modelSelectionJson, modelMetadataJson)


def leaseCurrent(row, claim, now):
    return bool(
        row
        and claim
        and row.get("owner_id") == claim.get("workerId")
        and row.get("owner_token") == claim.get("token")
        and safeCount(row.get("owner_epoch")) == claim.get("epoch")
        and int(row.get("lease_until") or 0) > now
    )


class MysqlTaskStore(CommonTaskStore):
    async def lockUser(self, client, userId):
        # 空行也必须被串行化，避免首次插入账户租约时出现竞争。
        await client.lock_key("music:user:" + userId)
        if not await client.fetchone(
            "SELECT id FROM users WHERE id=? FOR UPDATE", [userId]
        ):
            raise ApiError(404, "TASK_NOT_FOUND", "音乐任务不存在。")

    async def writeAccountLease(self, client, userId, taskId, claim, exists):
        # 调用方已持有账户锁，插入和更新都无需依赖数据库隐式冲突策略。
        if exists:
            await client.execute(
                "UPDATE music_task_account_leases SET task_id=?,owner_id=?,owner_token=?,owner_epoch=?,lease_until=? WHERE user_id=?",
                [taskId, claim["workerId"], claim["token"], claim["epoch"], claim["expiresAt"], userId],
            )
        else:
            await client.execute(
                "INSERT INTO music_task_account_leases(user_id,task_id,owner_id,owner_token,owner_epoch,lease_until) VALUES(?,?,?,?,?,?)",
                [userId, taskId, claim["workerId"], claim["token"], claim["epoch"], claim["expiresAt"]],
            )

    async def writeTask(self, client, task, value=None, **kwargs):
        value = options(value, **kwargs)
        expected, now = value.get("expectedStateVersion"), value.get("now", now_ms())
        if (
            type(expected) is not int
            or expected < 0
            or type(task.get("stateVersion")) is not int
            or task["stateVersion"] != expected + 1
        ):
            raise TypeError("MySQL task writes require the preceding stateVersion")
        row = await client.fetchone(
            "SELECT * FROM music_tasks WHERE id=? FOR UPDATE", [task["id"]]
        )
        if task["status"] in ACTIVE:
            if await client.fetchone(
                "SELECT 1 FROM music_tasks WHERE user_id=? AND id<>? AND status IN ('planning','researching','composing','waiting_tools') LIMIT 1",
                [task["userId"], task["id"]],
            ):
                raise busy()
            if await client.fetchone(
                "SELECT 1 FROM music_task_account_leases WHERE user_id=? AND NOT(task_id <=> ?) AND lease_until>? LIMIT 1",
                [task["userId"], task["id"], now],
            ):
                raise busy()
        if row:
            if (
                row["user_id"] != task["userId"]
                or safeCount(row["state_version"]) != expected
            ):
                raise taskConflict()
            if value.get("claim") and not leaseCurrent(row, value["claim"], now):
                raise leaseConflict()
            invalid = bool(value.get("invalidateLease"))
            result = await client.execute(
                """UPDATE music_tasks SET revision=$3,state_version=$4,status=$5,snapshot_json=$6,
updated_at=$7,owner_id=CASE WHEN $8 THEN NULL ELSE owner_id END,
owner_token=CASE WHEN $8 THEN NULL ELSE owner_token END,lease_until=CASE WHEN $8 THEN NULL ELSE lease_until END,
owner_epoch=owner_epoch+CASE WHEN $8 THEN 1 ELSE 0 END WHERE id=$1 AND state_version=$2""",
                [
                    task["id"],
                    expected,
                    task["revision"],
                    task["stateVersion"],
                    task["status"],
                    json.dumps(task, ensure_ascii=False),
                    task["updatedAt"],
                    invalid,
                ],
            )
            if not result.rowCount:
                raise taskConflict()
        else:
            if expected != 0 or value.get("claim"):
                raise taskConflict()
            await client.execute(
                "INSERT INTO music_tasks(id,user_id,device_id,revision,state_version,status,snapshot_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)",
                [
                    task["id"],
                    task["userId"],
                    task["deviceId"],
                    task["revision"],
                    task["stateVersion"],
                    task["status"],
                    json.dumps(task, ensure_ascii=False),
                    task["createdAt"],
                    task["updatedAt"],
                ],
            )
        if value.get("invalidateLease"):
            await client.execute(
                "DELETE FROM music_task_account_leases WHERE user_id=? AND task_id=?",
                [task["userId"], task["id"]],
            )

    async def save(self, task, value=None, **kwargs):
        await self.initialize()
        async with self.db.transaction() as db:
            await self.lockUser(db, task["userId"])
            await self.writeTask(db, task, options(value, **kwargs))

    async def commit(self, task, requestId, body, value=None, **kwargs):
        await self.initialize()
        value = options(value, **kwargs)
        async with self.db.transaction() as db:
            await self.lockUser(db, task["userId"])
            await self.assertConversationActive(db, task, requestId, value.get("requestAction"))
            await self._checkReplay(db, task, requestId, body)
            await self.writeTask(db, task, value)
            await self.remember(task, requestId, body, db, value.get("requestAction"))

    async def interrupted(self, value=None, **kwargs):
        value = options(value, **kwargs)
        now, grace = value.get("now", now_ms()), value.get("graceMs", 30000)
        await self.initialize()
        rows = await self.db.fetchall(
            """SELECT snapshot_json FROM music_tasks WHERE
(status IN ('planning','researching','composing') OR (status='waiting_tools' AND
JSON_LENGTH(CASE WHEN JSON_TYPE(JSON_EXTRACT(snapshot_json,'$.pendingSteps')) COLLATE utf8mb4_bin='ARRAY'
THEN JSON_EXTRACT(snapshot_json,'$.pendingSteps') ELSE JSON_ARRAY() END)=0)) AND
((owner_id IS NOT NULL AND lease_until<=?) OR (owner_id IS NULL AND updated_at<=?))""",
            [now, now - grace],
        )
        return [snapshot(row) for row in rows]

    async def recoverInterrupted(self, task, value=None, **kwargs):
        value = options(value, **kwargs)
        now, grace = value.get("now", now_ms()), value.get("graceMs", 30000)
        await self.initialize()
        async with self.db.transaction() as db:
            await self.lockUser(db, task["userId"])
            row = await db.fetchone(
                "SELECT * FROM music_tasks WHERE id=? AND user_id=? FOR UPDATE",
                [task["id"], task["userId"]],
            )
            current = snapshot(row)
            stranded = bool(
                current
                and current["status"] == "waiting_tools"
                and not current.get("pendingSteps")
            )
            expected = task.get("stateVersion", 0)
            if (
                not row
                or safeCount(row["state_version"]) != expected
                or row["status"] not in WORKING
                and not stranded
                or (
                    int(row["lease_until"] or 0) > now
                    if row["owner_id"]
                    else int(row["updated_at"]) > now - grace
                )
            ):
                return None
            counts = await self.callCounts(
                task["id"], current.get("runId"), current["revision"], db
            )
            await db.execute(
                "UPDATE music_task_calls SET status='uncertain',finished_at=?,error_code='SERVER_RESTART' WHERE task_id=? AND status='reserved'",
                [now, task["id"]],
            )
            self.recovered(current, counts, now)
            await self.writeTask(
                db,
                current,
                {"expectedStateVersion": expected, "invalidateLease": True, "now": now},
            )
            return current

    async def claimTask(self, taskId, userId, value=None, **kwargs):
        value = options(value, **kwargs)
        worker, now, leaseMs = (
            value.get("workerId"),
            value.get("now", now_ms()),
            value.get("leaseMs", 30000),
        )
        if not isinstance(worker, str) or not worker:
            raise TypeError("workerId is required")
        positiveInteger(leaseMs, "leaseMs")
        await self.initialize()
        async with self.db.transaction() as db:
            await self.lockUser(db, userId)
            row = await db.fetchone(
                "SELECT * FROM music_tasks WHERE id=? AND user_id=? FOR UPDATE",
                [taskId, userId],
            )
            if not row:
                raise ApiError(404, "TASK_NOT_FOUND", "音乐任务不存在。")
            if (
                row["status"] not in ACTIVE
                or row["owner_id"]
                and int(row["lease_until"]) > now
                and row["owner_id"] != worker
            ):
                raise leaseConflict()
            account = await db.fetchone(
                "SELECT * FROM music_task_account_leases WHERE user_id=? FOR UPDATE",
                [userId],
            )
            if (
                account
                and int(account["lease_until"]) > now
                and (account["task_id"] != taskId or account["owner_id"] != worker)
            ):
                raise busy()
            existing = row["owner_id"] == worker and int(row["lease_until"] or 0) > now
            claim = {
                "workerId": worker,
                "token": row["owner_token"] if existing else str(uuid.uuid4()),
                "epoch": safeCount(row["owner_epoch"]) + (0 if existing else 1),
                "expiresAt": now + leaseMs,
                "userId": userId,
                "taskId": taskId,
            }
            await db.execute(
                "UPDATE music_tasks SET owner_id=?,owner_token=?,owner_epoch=?,lease_until=? WHERE id=?",
                [worker, claim["token"], claim["epoch"], claim["expiresAt"], taskId],
            )
            await self.writeAccountLease(db, userId, taskId, claim, account is not None)
            return claim

    async def isTaskClaimCurrent(self, taskId, claim, now=None):
        await self.initialize()
        return leaseCurrent(
            await self.db.fetchone(
                "SELECT owner_id,owner_token,owner_epoch,lease_until FROM music_tasks WHERE id=?",
                [taskId],
            ),
            claim,
            now_ms() if now is None else now,
        )

    async def heartbeatTask(self, taskId, claim, value=None, **kwargs):
        value = options(value, **kwargs)
        now, leaseMs = value.get("now", now_ms()), value.get("leaseMs", 30000)
        positiveInteger(leaseMs, "leaseMs")
        if not claim:
            return False
        async with self.db.transaction() as db:
            await self.lockUser(db, claim["userId"])
            row = await db.fetchone(
                "SELECT * FROM music_tasks WHERE id=? FOR UPDATE", [taskId]
            )
            if not leaseCurrent(row, claim, now):
                return False
            expires = now + leaseMs
            await db.execute(
                "UPDATE music_tasks SET lease_until=? WHERE id=?", [expires, taskId]
            )
            await db.execute(
                "UPDATE music_task_account_leases SET lease_until=? WHERE user_id=? AND owner_token=?",
                [expires, row["user_id"], claim["token"]],
            )
            await db.execute(
                "UPDATE music_task_model_slots SET lease_until=? WHERE task_id=? AND owner_token=?",
                [expires, taskId, claim["token"]],
            )
            return True

    async def releaseTask(self, taskId, claim):
        if not claim:
            return False
        async with self.db.transaction() as db:
            await self.lockUser(db, claim["userId"])
            # MySQL 不支持 UPDATE RETURNING；先锁定并验证当前 fence，再更新。
            row = await db.fetchone(
                "SELECT user_id FROM music_tasks WHERE id=? AND owner_id=? AND owner_token=? AND owner_epoch=? FOR UPDATE",
                [taskId, claim["workerId"], claim["token"], claim["epoch"]],
            )
            if not row:
                return False
            await db.execute(
                "UPDATE music_tasks SET owner_id=NULL,owner_token=NULL,lease_until=NULL WHERE id=? AND owner_id=? AND owner_token=? AND owner_epoch=?",
                [taskId, claim["workerId"], claim["token"], claim["epoch"]],
            )
            await db.execute(
                "DELETE FROM music_task_account_leases WHERE user_id=? AND owner_token=?",
                [row["user_id"], claim["token"]],
            )
            return True

    async def reserveSlot(self, client, value=None, **kwargs):
        value = options(value, **kwargs)
        callId, userId, claim, now = (
            value["callId"],
            value["userId"],
            value["claim"],
            value["now"],
        )
        maximum = positiveInteger(
            value.get("globalConcurrency", 8), "globalConcurrency"
        )
        await client.lock_key("music:model_slots")
        await client.execute(
            "DELETE FROM music_task_model_slots WHERE lease_until<=?", [now]
        )
        if safeCount(
            (await client.fetchone("SELECT count(*) AS n FROM music_task_model_slots"))[
                "n"
            ]
        ) >= maximum or await client.fetchone(
            "SELECT 1 FROM music_task_model_slots WHERE user_id=?", [userId]
        ):
            raise busy()
        await client.execute(
            "INSERT INTO music_task_model_slots(call_id,user_id,task_id,owner_id,owner_token,owner_epoch,lease_until) VALUES(?,?,?,?,?,?,?)",
            [
                callId,
                userId,
                value.get("taskId"),
                claim["workerId"],
                claim["token"],
                claim["epoch"],
                claim["expiresAt"],
            ],
        )

    async def reserveUsage(self, client, userId, now, userLimit, globalLimit):
        positiveInteger(userLimit, "daily limit")
        positiveInteger(globalLimit, "global daily limit")
        await super().reserveUsage(client, userId, now, userLimit, globalLimit)

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
        value = options(value, **kwargs)
        selection = modelSelectionJson(value.get("modelSelection"))
        claim = value.get("claim")
        async with self.db.transaction() as db:
            await self.lockUser(db, task["userId"])
            row = await db.fetchone(
                "SELECT * FROM music_tasks WHERE id=? AND user_id=? FOR UPDATE",
                [task["id"], task["userId"]],
            )
            if (
                not leaseCurrent(row, claim, now)
                or row["revision"] != task["revision"]
                or row["status"] not in ACTIVE
            ):
                raise leaseConflict()
            if safeCount(row["state_version"]) != task.get("stateVersion", 0):
                raise taskConflict()
            id = str(uuid.uuid4())
            await self.reserveSlot(
                db,
                {
                    "callId": id,
                    "userId": task["userId"],
                    "taskId": task["id"],
                    "claim": {**claim, "expiresAt": int(row["lease_until"])},
                    "now": now,
                    "globalConcurrency": value.get("globalConcurrency", 8),
                },
            )
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
            return id

    async def finishCall(
        self, id, status, now, usage=None, errorCode=None, diagnostics=None, modelMetadata=None
    ):
        metadata = modelMetadataJson(modelMetadata)
        diagnostic = callDiagnostic(diagnostics, status)
        async with self.db.transaction() as db:
            await db.execute(
                """UPDATE music_task_calls SET status=?,finished_at=?,input_tokens=?,output_tokens=?,error_code=?,diagnostics_json=?,model_metadata_json=?
WHERE id=? AND status='reserved' """,
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
            await db.execute("DELETE FROM music_task_model_slots WHERE call_id=?", [id])

    async def acquireLegacy(self, userId, value=None, **kwargs):
        value = options(value, **kwargs)
        worker, now, leaseMs = (
            value.get("workerId"),
            value.get("now", now_ms()),
            value.get("leaseMs", 30000),
        )
        if not isinstance(worker, str) or not worker:
            raise TypeError("workerId is required")
        positiveInteger(leaseMs, "leaseMs")
        async with self.db.transaction() as db:
            await self.lockUser(db, userId)
            if await db.fetchone(
                "SELECT 1 FROM music_tasks WHERE user_id=? AND status IN ('planning','researching','composing','waiting_tools') LIMIT 1",
                [userId],
            ):
                raise busy()
            row = await db.fetchone(
                "SELECT * FROM music_task_account_leases WHERE user_id=? FOR UPDATE",
                [userId],
            )
            if row and int(row["lease_until"]) > now:
                raise busy()
            claim = {
                "userId": userId,
                "workerId": worker,
                "token": str(uuid.uuid4()),
                "epoch": safeCount(row["owner_epoch"] if row else 0) + 1,
                "expiresAt": now + leaseMs,
                "callId": str(uuid.uuid4()),
            }
            await self.writeAccountLease(db, userId, None, claim, row is not None)
            await self.reserveSlot(
                db,
                {
                    "callId": claim["callId"],
                    "userId": userId,
                    "claim": claim,
                    "now": now,
                    "globalConcurrency": value.get("globalConcurrency", 8),
                },
            )
            return claim

    async def heartbeatLegacy(self, claim, value=None, **kwargs):
        value = options(value, **kwargs)
        now, leaseMs = value.get("now", now_ms()), value.get("leaseMs", 30000)
        positiveInteger(leaseMs, "leaseMs")
        async with self.db.transaction() as db:
            await self.lockUser(db, claim["userId"])
            # 即便时钟未前进、UPDATE 值不变，也应承认仍有效的相同租约。
            row = await db.fetchone(
                "SELECT * FROM music_task_account_leases WHERE user_id=? FOR UPDATE",
                [claim["userId"]],
            )
            if not row or row["task_id"] is not None or not leaseCurrent(row, claim, now):
                return False
            await db.execute(
                "UPDATE music_task_account_leases SET lease_until=? WHERE user_id=? AND owner_token=?",
                [now + leaseMs, claim["userId"], claim["token"]],
            )
            await db.execute(
                "UPDATE music_task_model_slots SET lease_until=? WHERE call_id=? AND owner_token=?",
                [now + leaseMs, claim["callId"], claim["token"]],
            )
            return True

    async def releaseLegacy(self, claim):
        if not claim:
            return False
        async with self.db.transaction() as db:
            await self.lockUser(db, claim["userId"])
            await db.execute(
                "DELETE FROM music_task_model_slots WHERE call_id=? AND owner_token=?",
                [claim["callId"], claim["token"]],
            )
            return bool(
                (
                    await db.execute(
                        "DELETE FROM music_task_account_leases WHERE user_id=? AND owner_token=? AND task_id IS NULL",
                        [claim["userId"], claim["token"]],
                    )
                ).rowCount
            )

    async def reserveLegacyUsage(
        self, userId, now, accountStore, userLimit, globalLimit, value=None, **kwargs
    ):
        claim = options(value, **kwargs).get("claim")
        async with self.db.transaction() as db:
            await self.lockUser(db, userId)
            row = await db.fetchone(
                "SELECT * FROM music_task_account_leases WHERE user_id=? FOR UPDATE",
                [userId],
            )
            if (
                not row
                or row["task_id"] is not None
                or not leaseCurrent(row, claim, now)
            ):
                raise leaseConflict()
            await self.reserveUsage(db, userId, now, userLimit, globalLimit)

    async def markUncertain(self, *args, **kwargs):
        raise TypeError("Use recoverInterrupted for atomic lease-aware recovery")
