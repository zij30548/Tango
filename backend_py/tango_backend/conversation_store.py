"""账号与设备隔离的持久会话日志，读取不会派发业务或触发模型。"""

import asyncio
import json
import math
import re
from uuid import uuid4
from .errors import ApiError
from .compat import (
    clone,
    digest,
    iso,
    now_ms,
    utf16_len,
    utf16_slice,
    well_formed,
    safe_integer,
    js_json,
)

CONVERSATION_WINDOW = 40
CONVERSATION_LIMIT = 500
MAX_EVENTS = 2000
SCHEMA = """CREATE TABLE IF NOT EXISTS music_conversations (
 id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 device_id TEXT NOT NULL, request_id TEXT NOT NULL, payload_hash TEXT NOT NULL,
 journal_json TEXT NOT NULL, created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL, cleared_at BIGINT,
 UNIQUE(user_id,device_id,request_id));
 CREATE INDEX IF NOT EXISTS music_conversations_latest ON music_conversations(user_id,device_id,updated_at);
 CREATE TABLE IF NOT EXISTS music_conversation_clears (
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, request_id TEXT NOT NULL,
 receipt_json TEXT NOT NULL, created_at BIGINT NOT NULL, PRIMARY KEY(user_id,request_id));"""


def _identifier(value):
    return (
        isinstance(value, str)
        and re.fullmatch(r"[A-Za-z0-9_-]{8,100}", value) is not None
    )


def conversationId(value):
    if not _identifier(value):
        raise ApiError(400, "INVALID_CONVERSATION_INPUT", "会话标识无效。")
    return value


def _object(body, keys, required=None):
    if (
        type(body) is not dict
        or set(body) - set(keys)
        or any(key not in body for key in (keys if required is None else required))
    ):
        raise ApiError(400, "INVALID_CONVERSATION_INPUT", "会话请求格式无效。")


def _content(value, maximum):
    if (
        not isinstance(value, str)
        or not value.strip()
        or utf16_len(value) > maximum
        or not well_formed(value)
        or re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", value)
    ):
        raise ApiError(
            400, "INVALID_CONVERSATION_CONTENT", f"消息须为 1 至 {maximum} 个字符。"
        )
    return value


def _conflict():
    raise ApiError(409, "IDEMPOTENCY_CONFLICT", "同一操作标识不能用于不同内容。")


def _full():
    raise ApiError(
        409, "CONVERSATION_FULL", "当前会话已达保存上限，请新建会话。历史消息仍已保存。"
    )


def _decoded(value):
    return json.loads(value) if isinstance(value, str) else clone(value)


def _owned(row, userId, deviceId=None):
    if not row or row["user_id"] != userId or row.get("cleared_at") is not None:
        raise ApiError(404, "CONVERSATION_NOT_FOUND", "会话不存在。")
    if deviceId and row["device_id"] != deviceId:
        raise ApiError(
            409, "CONVERSATION_DEVICE_MISMATCH", "请在创建此会话的设备继续。"
        )
    return _decoded(row["journal_json"])


def _page(state, before=math.inf):
    eligible = [
        message for message in state["messages"] if message["sequence"] < before
    ]
    messages = eligible[-CONVERSATION_WINDOW:]
    while len(js_json(messages).encode()) > 230 * 1024:
        messages.pop(0)
    linked = state.get("taskLinks", {}).get(state.get("latestTaskId"))
    result = {
        "id": state["id"],
        "deviceId": state["deviceId"],
        "version": state["version"],
        "messages": clone(messages),
        "hasEarlier": len(eligible) > len(messages),
        "earliestSequence": messages[0]["sequence"] if messages else 0,
    }
    if state.get("latestTaskId"):
        result["latestTaskId"] = state["latestTaskId"]
    if linked and any(
        e["id"] == linked["turnId"] and e["role"] == "user" for e in state["messages"]
    ):
        result["latestTaskLink"] = {
            "taskId": state["latestTaskId"],
            "turnId": linked["turnId"],
            "revision": linked["revision"],
        }
    return result


def _append(state, message, now):
    if len(state["messages"]) >= CONVERSATION_LIMIT:
        _full()
    if any(entry["id"] == message["id"] for entry in state["messages"]):
        _conflict()
    entry = {**message, "sequence": len(state["messages"]) + 1, "createdAt": iso(now)}
    state["messages"].append(entry)
    return entry


def _selected_artist_derivation(task, anchor, linked):
    if not linked or linked["turnId"] != anchor["id"]:
        return None
    revision = task["revision"]
    same = (
        linked["revision"] == revision
        and linked.get("derivation") == "select_artist"
        and linked.get("derivedFromRevision") == revision - 1
    )
    if not same and linked["revision"] != revision - 1:
        return None
    artist, artistId = (
        (task.get("brief") or {}).get("artist"),
        (task.get("brief") or {}).get("artistId"),
    )
    if (
        not isinstance(artist, str)
        or not artist.strip()
        or utf16_len(artist) > 120
        or not safe_integer(artistId)
        or artistId < 1
        or (task.get("verifiedArtist") or {}).get("id") != artistId
    ):
        return None
    users = [
        e
        for e in task.get("messages", [])
        if e.get("role") == "user" and e.get("revision") == revision
    ]
    if (
        len(users) != 1
        or users[0]["content"] != f"选择曲库歌手：{artist}。"
        or not any(
            e.get("node") == "select_artist" and e.get("revision") == revision
            for e in task.get("trace", [])
        )
    ):
        return None
    return {"derivation": "select_artist", "derivedFromRevision": revision - 1}


def _payload_variants(base, optional):
    variants = [base]
    for key, value in optional.items():
        variants += [{**entry, key: value} for entry in variants]
    return variants


def requestProvesTaskTurn(message, row, state):
    task = _decoded(row["snapshot_json"])
    if task.get("deviceId") != state["deviceId"] or not isinstance(
        task.get("messages"), list
    ):
        return None
    users = [
        e
        for e in task["messages"]
        if e.get("role") == "user"
        and e.get("content") == message["content"]
        and safe_integer(e.get("revision"))
        and e["revision"] > 0
    ]
    base = {
        "deviceId": state["deviceId"],
        "requestId": message["id"],
        "instruction": message["content"],
    }
    for user in users:
        payload = (
            base
            if user["revision"] == 1
            else {**base, "action": "turn", "expectedRevision": user["revision"] - 1}
        )
        optional = {}
        if isinstance(task.get("preference"), str):
            optional["preference"] = task["preference"]
        if _identifier(task.get("catalogContext")):
            optional["catalogContext"] = task["catalogContext"]
        if user["revision"] == 1 and isinstance(task.get("excludedIds"), list):
            optional["excludedIds"] = task["excludedIds"]
        if any(
            digest(candidate) == row["payload_hash"]
            for candidate in _payload_variants(payload, optional)
        ):
            return user["revision"]
    return None


class ConversationStore:
    def __init__(self, store, options=None, *, now=None):
        self.accountStore, self.now = store, now or (options or {}).get("now", now_ms)
        self.postgres = store.dialect == "postgres"
        self.mysql = store.dialect == "mysql"
        self.distributed = store.dialect in ("postgres", "mysql")
        self._initialized = False
        self._init_lock = asyncio.Lock()

    async def initialize(self):
        async with self._init_lock:
            if not self._initialized:
                await self.accountStore.initialize()
                # MySQL 的 DDL 会隐式提交，统一在 Store 初始化阶段建表。
                if not self.mysql:
                    async with self.accountStore.db.transaction():
                        if self.postgres:
                            await self.accountStore.db.execute(
                                "SELECT pg_advisory_xact_lock($1,$2)", (1952546919, 4)
                            )
                        await self.accountStore.db.executescript(SCHEMA)
                        if self.postgres:
                            await self.accountStore.db.execute("ALTER TABLE music_conversations ADD COLUMN IF NOT EXISTS cleared_at BIGINT")
                        else:
                            columns = {row["name"] for row in await self.accountStore.db.fetchall("PRAGMA table_info(music_conversations)")}
                            if "cleared_at" not in columns:
                                await self.accountStore.db.execute("ALTER TABLE music_conversations ADD COLUMN cleared_at BIGINT")
                self.accountStore._conversationSchemaReady = True
                self._initialized = True
        return self

    async def row(self, id, userId):
        await self.initialize()
        return await self.accountStore.db.fetchone(
            "SELECT * FROM music_conversations WHERE id=? AND user_id=?", (id, userId)
        )

    async def get(self, id, userId, before=math.inf):
        conversationId(id)
        if before != math.inf and (not safe_integer(before) or before < 1):
            raise ApiError(400, "INVALID_CONVERSATION_INPUT", "历史消息游标无效。")
        return await self.visible(
            _owned(await self.row(id, userId), userId), userId, before
        )

    async def visible(self, state, userId, before=math.inf):
        result = _page(state, before)
        current = state.get("taskLinks", {}).get(state.get("latestTaskId"))
        current_sequence = next(
            (
                e["sequence"]
                for e in state["messages"]
                if current and e["id"] == current["turnId"]
            ),
            0,
        )
        users = [
            e
            for e in state["messages"]
            if e["role"] == "user" and e["sequence"] >= current_sequence
        ]
        if not users:
            return result
        ids = [e["id"] for e in users]
        rows = await self.accountStore.db.fetchall(
            """SELECT r.request_id,r.payload_hash,r.task_id,t.snapshot_json FROM music_task_requests r
          JOIN music_tasks t ON t.id=r.task_id AND t.user_id=r.user_id AND t.device_id=r.device_id
          WHERE r.user_id=? AND r.device_id=? AND r.request_id IN ("""
            + ",".join("?" for _ in ids)
            + ")",
            (userId, state["deviceId"], *ids),
        )
        requests = {r["request_id"]: r for r in rows}
        for message in reversed(users):
            row = requests.get(message["id"])
            proven = requestProvesTaskTurn(message, row, state) if row else None
            if proven:
                prior = (
                    current["revision"]
                    if current
                    and row["task_id"] == state.get("latestTaskId")
                    and message["id"] == current["turnId"]
                    else 0
                )
                result["latestTaskId"] = row["task_id"]
                result["latestTaskLink"] = {
                    "taskId": row["task_id"],
                    "turnId": message["id"],
                    "revision": max(prior, proven),
                }
                break
        return result

    async def latest(self, userId, deviceId):
        conversationId(deviceId)
        await self.initialize()
        row = await self.accountStore.db.fetchone(
            "SELECT * FROM music_conversations WHERE user_id=? AND device_id=? AND cleared_at IS NULL ORDER BY updated_at DESC,created_at DESC,id DESC LIMIT 1",
            (userId, deviceId),
        )
        return (
            await self.visible(_owned(row, userId, deviceId), userId) if row else None
        )

    async def create(self, userId, body):
        _object(body, ["deviceId", "requestId"])
        conversationId(body["deviceId"])
        conversationId(body["requestId"])
        await self.initialize()
        now, hash = self.now(), digest(body)
        state = {
            "id": str(uuid4()),
            "deviceId": body["deviceId"],
            "version": 1,
            "messages": [],
            "receipts": {},
            "eventReceipts": {},
            "taskLinks": {},
        }
        db = self.accountStore.db
        async with db.transaction():
            if self.distributed:
                # 与任务、路由共享账号锁，按账号→会话→任务的顺序提交。
                if self.mysql:
                    await db.lock_key("music:user:" + userId)
                await db.fetchone("SELECT id FROM users WHERE id=? FOR UPDATE", (userId,))
            await db.execute(
                """INSERT INTO music_conversations(id,user_id,device_id,request_id,payload_hash,journal_json,created_at,updated_at)
              VALUES(?,?,?,?,?,?,?,?) """ + (
                    "ON DUPLICATE KEY UPDATE id=id" if self.mysql else
                    "ON CONFLICT(user_id,device_id,request_id) DO NOTHING"
                ),
                (
                    state["id"],
                    userId,
                    body["deviceId"],
                    body["requestId"],
                    hash,
                    js_json(state),
                    now,
                    now,
                ),
            )
            row = await db.fetchone(
                "SELECT * FROM music_conversations WHERE user_id=? AND device_id=? AND request_id=?",
                (userId, body["deviceId"], body["requestId"]),
            )
            if row["payload_hash"] != hash:
                _conflict()
            return _page(_owned(row, userId, body["deviceId"]))

    async def mutate(
        self, id, userId, deviceId, change, taskId=None, taskAnchorId=None
    ):
        conversationId(id)
        conversationId(deviceId)
        await self.initialize()
        db = self.accountStore.db
        async with db.transaction():
            if self.distributed:
                if self.mysql:
                    await db.lock_key("music:user:" + userId)
                await db.fetchone("SELECT id FROM users WHERE id=? FOR UPDATE", (userId,))
            row = await db.fetchone(
                "SELECT * FROM music_conversations WHERE id=? AND user_id=?"
                + (" FOR UPDATE" if self.distributed else ""),
                (id, userId),
            )
            state = _owned(row, userId, deviceId)
            task_row = (
                await db.fetchone(
                    "SELECT snapshot_json FROM music_tasks WHERE id=? AND user_id=?"
                    + (" FOR SHARE" if self.distributed else ""),
                    (taskId, userId),
                )
                if taskId
                else None
            )
            task = _decoded(task_row["snapshot_json"]) if task_row else None
            linked = state["taskLinks"].get(taskId) if taskId else None
            request = None
            if (
                task
                and taskAnchorId
                and (
                    not linked
                    or linked["turnId"] != taskAnchorId
                    or linked["revision"] < task["revision"] - 1
                )
            ):
                request = await db.fetchone(
                    "SELECT payload_hash FROM music_task_requests WHERE user_id=? AND device_id=? AND request_id=? AND task_id=?",
                    (userId, deviceId, taskAnchorId, taskId),
                )
            result = change(state, task, request)
            if result["changed"]:
                await db.execute(
                    "UPDATE music_conversations SET journal_json=?,updated_at=? WHERE id=?",
                    (js_json(state), self.now(), id),
                )
            return {"conversation": _page(state), "receipt": clone(result["receipt"])}

    async def clearAll(self, userId, body, tasks=None):
        """在账号锁内清空聊天并保留墓碑与幂等回执，付费和任务审计不删除。"""
        _object(body, ["requestId"])
        conversationId(body["requestId"])
        await self.initialize()
        db, now = self.accountStore.db, self.now()
        routes_ready = self.mysql or getattr(self.accountStore, "_conversationRoutesReady", False)
        if not routes_ready:
            routes_ready = bool(await db.fetchone("SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='music_conversation_routes'" if self.postgres else "SELECT 1 FROM sqlite_master WHERE type='table' AND name='music_conversation_routes'"))
        async with db.transaction():
            if self.distributed:
                if self.mysql:
                    await db.lock_key("music:user:" + userId)
                await db.fetchone("SELECT id FROM users WHERE id=? FOR UPDATE", (userId,))
            prior = await db.fetchone("SELECT receipt_json FROM music_conversation_clears WHERE user_id=? AND request_id=?", (userId, body["requestId"]))
            if prior:
                return _decoded(prior["receipt_json"])
            rows = await db.fetchall("SELECT * FROM music_conversations WHERE user_id=? AND cleared_at IS NULL ORDER BY id" + (" FOR UPDATE" if self.distributed else ""), (userId,))
            task_ids = set()
            for row in rows:
                state = _decoded(row["journal_json"])
                task_ids.update(state.get("taskLinks", {}))
                # 尚未写回 taskLinks 的任务也通过原消息幂等记录关联，避免清除后继续补查。
                linked = await db.fetchall("SELECT DISTINCT q.task_id FROM music_task_requests q JOIN music_conversation_routes r ON r.user_id=q.user_id AND r.device_id=q.device_id AND r.message_id=q.request_id WHERE r.conversation_id=? AND q.user_id=?", (row["id"], userId)) if routes_ready else []
                task_ids.update(item["task_id"] for item in linked)
                await db.execute("UPDATE music_conversations SET journal_json=?,cleared_at=?,updated_at=? WHERE id=? AND user_id=?", (js_json({"id": row["id"], "deviceId": row["device_id"], "version": state.get("version", 1) + 1, "messages": [], "receipts": {}, "eventReceipts": {}, "taskLinks": {}, "cleared": True}), now, now, row["id"], userId))
                # 路由和尝试行保留计数事实；正文及可执行动作不可从恢复入口重新取回。
                if routes_ready:
                    await db.execute("UPDATE music_conversation_routes SET input_json=?,result_json=NULL,error_code='CONVERSATION_CLEARED',status=CASE WHEN status='running' THEN 'failed' ELSE status END,finished_at=COALESCE(finished_at,?) WHERE conversation_id=? AND user_id=?", ("{}", now, row["id"], userId))
            cancelled = 0
            if tasks:
                for task_id in sorted(task_ids):
                    task = await tasks.store.load(task_id, userId)
                    if task and task["status"] not in ("adopted", "awaiting_apply", "cancelled"):
                        await tasks.action(task_id, userId, {"deviceId": task["deviceId"], "requestId": str(uuid4()), "action": "cancel", "expectedRevision": task["revision"]})
                        cancelled += 1
            receipt = {"cleared": True, "requestId": body["requestId"], "conversationCount": len(rows), "clearedAt": iso(now), "cancelledTaskCount": cancelled}
            await db.execute("INSERT INTO music_conversation_clears(user_id,request_id,receipt_json,created_at) VALUES(?,?,?,?)", (userId, body["requestId"], js_json(receipt), now))
            return receipt

    async def message(self, id, userId, body, options=None, *, dispatchHint="task"):
        dispatchHint = (options or {}).get("dispatchHint", dispatchHint)
        if dispatchHint not in ["task", "legacy_assistant", "router"]:
            raise TypeError("Invalid dispatch hint")
        _object(body, ["deviceId", "messageId", "content"])
        conversationId(body["deviceId"])
        conversationId(body["messageId"])
        _content(body["content"], 1000)

        def change(state, _task, _request):
            hash, prior = digest(body), state["receipts"].get(body["messageId"])
            if prior:
                if prior["hash"] != hash:
                    _conflict()
                return {"changed": False, "receipt": prior["receipt"]}
            entry = _append(
                state,
                {
                    "id": body["messageId"],
                    "role": "user",
                    "content": body["content"],
                    "turnId": body["messageId"],
                    "source": "user",
                },
                self.now(),
            )
            state["version"] += 1
            receipt = {
                "messageId": body["messageId"],
                "turnId": body["messageId"],
                "status": "accepted",
                "sequence": entry["sequence"],
                "conversationVersion": state["version"],
                "acceptedAt": entry["createdAt"],
                "dispatchHint": dispatchHint,
            }
            state["receipts"][body["messageId"]] = {"hash": hash, "receipt": receipt}
            return {"changed": True, "receipt": receipt}

        return await self.mutate(id, userId, body["deviceId"], change)

    async def receipt(self, id, userId, messageId):
        conversationId(id)
        conversationId(messageId)
        state = _owned(await self.row(id, userId), userId)
        if messageId not in state["receipts"]:
            raise ApiError(
                404, "MESSAGE_NOT_FOUND", "消息尚无接收回执；此查询不会重复发送。"
            )
        return {
            "conversation": _page(state),
            "receipt": clone(state["receipts"][messageId]["receipt"]),
        }

    async def routeContext(self, id, userId, deviceId, messageId):
        conversationId(id)
        conversationId(deviceId)
        conversationId(messageId)
        state = _owned(await self.row(id, userId), userId, deviceId)
        anchor = next(
            (
                e
                for e in state["messages"]
                if e["id"] == messageId and e["role"] == "user"
            ),
            None,
        )
        if not anchor or messageId not in state["receipts"]:
            raise ApiError(404, "MESSAGE_NOT_FOUND", "请先保存用户消息，再请求路由。")
        visible = await self.visible(state, userId)
        task = None
        if visible.get("latestTaskId"):
            row = await self.accountStore.db.fetchone(
                "SELECT snapshot_json FROM music_tasks WHERE id=? AND user_id=? AND device_id=?",
                (visible["latestTaskId"], userId, deviceId),
            )
            if row:
                current = _decoded(row["snapshot_json"])

                def text(v, maximum):
                    return utf16_slice(v, maximum) if isinstance(v, str) else ""

                keys = [
                    "kind",
                    "title",
                    "scene",
                    "styles",
                    "queries",
                    "artist",
                    "artistId",
                    "requiredArtist",
                    "artistOnly",
                    "since",
                    "until",
                    "targetMinutes",
                    "excludeLive",
                    "avoidPrevious",
                    "excludedArtists",
                    "requireReleases",
                    "retainFirst",
                    "removeIds",
                    "question",
                    "adjustment",
                ]
                brief = current.get("brief")
                brief = (
                    {key: clone(brief[key]) for key in keys if key in brief}
                    if brief
                    else None
                )
                sources = current.get("sources")
                if not isinstance(sources, list):
                    sources = [
                        s
                        for b in current.get("branches", {}).values()
                        if b
                        for s in ((b.get("result") or {}).get("sources") or [])
                    ]
                seen, selected = set(), []
                for source in sources:
                    key = js_json(
                        [source.get("id"), source.get("name"), source.get("checkedAt")]
                    )
                    if key not in seen:
                        selected.append(source)
                        seen.add(key)
                task = {
                    "id": current["id"],
                    "revision": current["revision"],
                    "stateVersion": current.get("stateVersion", 0),
                    "status": current["status"],
                    "brief": brief,
                    "question": text(current.get("question"), 1000),
                    "preview": None,
                    "sources": [
                        {
                            "name": text(s.get("name"), 200),
                            **(
                                {"checkedAt": s["checkedAt"]}
                                if "checkedAt" in s
                                else {}
                            ),
                        }
                        for s in selected[:6]
                    ],
                    "lockedIds": current.get("lockedIds", [])[:30],
                }
                if current.get("preview"):
                    preview = current["preview"]
                    task["preview"] = {
                        "title": text(preview.get("title"), 160),
                        "note": text(preview.get("note"), 1600),
                        "tracks": [
                            {
                                "id": track["id"],
                                "title": text(track.get("title"), 200),
                                "artist": text(track.get("artist"), 200),
                                **(
                                    {"reason": text(track["reason"], 500)}
                                    if isinstance(track.get("reason"), str)
                                    else {}
                                ),
                            }
                            for track in preview.get("tracks", [])[:30]
                        ],
                    }
        users = [e for e in state["messages"] if e["role"] == "user"]
        return {
            "conversationId": id,
            "messageId": messageId,
            "deviceId": deviceId,
            "instruction": anchor["content"],
            "history": [
                {"role": "user", "content": e["content"]}
                for e in users
                if e["sequence"] < anchor["sequence"]
            ][-6:],
            "task": task,
            "isLatest": bool(users and users[-1]["id"] == messageId),
            "conversationVersion": state["version"],
        }

    async def appendRouteReply(self, id, userId, deviceId, body):
        turnId, replyId, reply = body["turnId"], body["replyId"], body["content"]
        conversationId(turnId)
        conversationId(replyId)
        _content(reply, 1600)

        def change(state, _task, _request):
            if (
                not any(
                    e["id"] == turnId and e["role"] == "user" for e in state["messages"]
                )
                or turnId not in state["receipts"]
            ):
                raise ApiError(
                    409, "CONVERSATION_ANCHOR_MISSING", "请先保存原始用户消息。"
                )
            existing = next((e for e in state["messages"] if e["id"] == replyId), None)
            if existing:
                if any(
                    existing.get(k) != v
                    for k, v in {
                        "source": "route",
                        "role": "assistant",
                        "turnId": turnId,
                        "content": reply,
                    }.items()
                ):
                    _conflict()
                return {
                    "changed": False,
                    "receipt": {
                        "messageId": replyId,
                        "turnId": turnId,
                        "status": "recorded",
                        "source": "route",
                    },
                }
            _append(
                state,
                {
                    "id": replyId,
                    "role": "assistant",
                    "content": reply,
                    "turnId": turnId,
                    "source": "route",
                },
                self.now(),
            )
            state["version"] += 1
            return {
                "changed": True,
                "receipt": {
                    "messageId": replyId,
                    "turnId": turnId,
                    "status": "recorded",
                    "source": "route",
                },
            }

        return await self.mutate(id, userId, deviceId, change)

    async def event(self, id, userId, body, tasks):
        _object(
            body,
            [
                "deviceId",
                "eventId",
                "turnId",
                "kind",
                "content",
                "taskId",
                "taskRevision",
                "sourceMessageId",
            ],
            ["deviceId", "eventId", "turnId", "kind"],
        )
        for key in ["deviceId", "eventId", "turnId"]:
            conversationId(body[key])
        if (
            "sourceMessageId" in body
            and conversationId(body["sourceMessageId"]) != body["turnId"]
        ):
            raise ApiError(
                409, "CONVERSATION_ANCHOR_MISMATCH", "回执与原始消息不一致。"
            )
        if body["kind"] not in ["local_reply", "task_sync"]:
            raise ApiError(400, "INVALID_CONVERSATION_EVENT", "会话事件类型无效。")
        previous = _owned(await self.row(id, userId), userId, body["deviceId"])
        if body["eventId"] in previous["eventReceipts"]:
            saved = previous["eventReceipts"][body["eventId"]]
            if saved["hash"] != digest(body):
                _conflict()
            return {"conversation": _page(previous), "receipt": clone(saved["receipt"])}
        if body["kind"] == "task_sync":
            conversationId(body.get("taskId"))
            if not safe_integer(body.get("taskRevision")) or body["taskRevision"] < 1:
                raise ApiError(400, "INVALID_CONVERSATION_EVENT", "任务版本无效。")
            task = await tasks.owned(body["taskId"], userId, body["deviceId"])
            if task["revision"] != body["taskRevision"]:
                raise ApiError(
                    409, "TASK_REVISION_CONFLICT", "任务版本已变更，请刷新后同步。"
                )
        else:
            _content(body.get("content"), 1600)
            if "taskId" in body or "taskRevision" in body:
                raise ApiError(
                    400, "INVALID_CONVERSATION_EVENT", "设备回执不能冒充任务结果。"
                )

        def change(state, task, anchorRequest):
            hash, prior = digest(body), state["eventReceipts"].get(body["eventId"])
            if prior:
                if prior["hash"] != hash:
                    _conflict()
                return {"changed": False, "receipt": prior["receipt"]}
            if len(state["eventReceipts"]) >= MAX_EVENTS:
                _full()
            anchor = next(
                (
                    e
                    for e in state["messages"]
                    if e["id"] == body["turnId"] and e["role"] == "user"
                ),
                None,
            )
            if not anchor:
                raise ApiError(
                    409,
                    "CONVERSATION_ANCHOR_MISSING",
                    "请先保存原始用户消息，再同步执行结果。",
                )
            imported = []
            if body["kind"] == "local_reply":
                imported.append(
                    _append(
                        state,
                        {
                            "id": body["eventId"],
                            "role": "assistant",
                            "content": body["content"],
                            "turnId": body["turnId"],
                            "source": "device_report",
                        },
                        self.now(),
                    )["id"]
                )
            else:
                if (
                    not task
                    or task.get("deviceId") != body["deviceId"]
                    or task["revision"] != body["taskRevision"]
                ):
                    raise ApiError(
                        409, "TASK_REVISION_CONFLICT", "任务版本已变更，请刷新后同步。"
                    )
                linked = state["taskLinks"].get(body["taskId"])
                if anchorRequest:
                    proven = requestProvesTaskTurn(
                        anchor, {**anchorRequest, "snapshot_json": task}, state
                    )
                    if proven == task["revision"] - 1 and (
                        not linked or linked["revision"] < proven
                    ):
                        linked = {"turnId": body["turnId"], "revision": proven}
                if linked and linked["revision"] > body["taskRevision"]:
                    raise ApiError(
                        409, "TASK_REVISION_CONFLICT", "旧任务版本不能覆盖已同步结果。"
                    )
                if (
                    linked
                    and linked["revision"] == body["taskRevision"]
                    and linked["turnId"] != body["turnId"]
                ):
                    raise ApiError(
                        409,
                        "CONVERSATION_ANCHOR_MISMATCH",
                        "此任务版本已关联另一轮用户消息。",
                    )
                direct = any(
                    e.get("role") == "user"
                    and e.get("revision") == body["taskRevision"]
                    and e.get("content") == anchor["content"]
                    for e in task.get("messages", [])
                )
                derivation = (
                    None
                    if direct
                    else _selected_artist_derivation(task, anchor, linked)
                )
                if not direct and not derivation:
                    raise ApiError(
                        409,
                        "CONVERSATION_ANCHOR_MISMATCH",
                        "任务要求与此轮用户消息不一致。",
                    )
                for entry in task.get("messages", []):
                    if (
                        entry.get("role") != "assistant"
                        or entry.get("revision") != body["taskRevision"]
                    ):
                        continue
                    conversationId(entry.get("messageId"))
                    _content(entry.get("content"), 1600)
                    messageId = "task_" + digest(
                        {"taskId": task["id"], "messageId": entry["messageId"]}
                    )
                    existing = next(
                        (m for m in state["messages"] if m["id"] == messageId), None
                    )
                    if existing:
                        if (
                            existing["content"] != entry["content"]
                            or existing["turnId"] != body["turnId"]
                        ):
                            _conflict()
                        continue
                    imported.append(
                        _append(
                            state,
                            {
                                "id": messageId,
                                "role": "assistant",
                                "content": entry["content"],
                                "turnId": body["turnId"],
                                "source": "task",
                                "taskId": body["taskId"],
                                "taskRevision": body["taskRevision"],
                            },
                            self.now(),
                        )["id"]
                    )
                state["taskLinks"][body["taskId"]] = {
                    "turnId": body["turnId"],
                    "revision": body["taskRevision"],
                    **(derivation or {}),
                }
                currentLink = state["taskLinks"].get(state.get("latestTaskId"))
                currentSequence = next(
                    (
                        e["sequence"]
                        for e in state["messages"]
                        if currentLink and e["id"] == currentLink["turnId"]
                    ),
                    0,
                )
                if anchor["sequence"] >= currentSequence:
                    state["latestTaskId"] = body["taskId"]
            state["version"] += 1
            receipt = {
                "eventId": body["eventId"],
                "turnId": body["turnId"],
                "status": "recorded",
                "kind": body["kind"],
                "source": "device_report" if body["kind"] == "local_reply" else "task",
                "messageIds": imported,
                "conversationVersion": state["version"],
            }
            state["eventReceipts"][body["eventId"]] = {"hash": hash, "receipt": receipt}
            return {"changed": True, "receipt": receipt}

        return await self.mutate(
            id,
            userId,
            body["deviceId"],
            change,
            body.get("taskId") if body["kind"] == "task_sync" else None,
            body["turnId"] if body["kind"] == "task_sync" else None,
        )
