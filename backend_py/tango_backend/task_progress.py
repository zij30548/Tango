"""SSE 进度只公开业务状态，Redis 负责通知，数据库始终是权威来源。"""

import asyncio
import hashlib
import re
from collections import defaultdict
from starlette.responses import StreamingResponse
from .compat import js_json, now_ms, safe_integer
from .errors import ApiError

terminal = {
    "preview",
    "awaiting_apply",
    "adopted",
    "paused",
    "needs_clarification",
    "failed",
    "cancelled",
    "budget_exhausted",
}
labels = {
    "understanding": "正在理解你的音乐要求",
    "researching": "正在规划音乐检索",
    "searching": "正在查询真实曲库",
    "checking": "正在核对候选歌曲",
    "composing": "正在整理歌单",
    "ready": "歌单已生成，可以预览",
    "awaiting_apply": "正在等待本机采用歌单",
    "adopted": "歌单已采用",
    "paused": "任务已暂停，进度已保存",
    "needs_clarification": "需要补充信息后继续",
    "failed": "任务未完成，进度已保存",
    "cancelled": "任务已取消",
    "budget_exhausted": "已达到调用预算，进度已保存",
}
publishScript = "local id=redis.call('XADD',KEYS[1],'MAXLEN','~',128,'*','version',ARGV[1]); redis.call('EXPIRE',KEYS[1],3600); return id"


def taskProgress(task):
    status = task["status"]
    stage = (
        "understanding"
        if status == "planning"
        else ("checking" if task.get("phase") == "observe" else "researching")
        if status == "researching"
        else "searching"
        if status == "waiting_tools"
        else "ready"
        if status == "preview"
        else status
    )
    return {
        "taskId": task["id"],
        "revision": task["revision"],
        "stateVersion": task.get("stateVersion", 0),
        "status": status,
        "stage": stage if stage in labels else "researching",
        "message": labels.get(stage, labels["researching"]),
        "at": task["updatedAt"],
    }


class TaskProgressHub:
    def __init__(self, options=None, **kwargs):
        opts = {**(options or {}), **kwargs}
        self.client = opts.get("client")
        self.namespace = opts.get("namespace", "tango:local")
        self.timeoutMs = opts.get("timeoutMs", 500)
        self.closed = False
        self.pending = set()
        self.listeners = defaultdict(set)
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9:_-]{0,79}", self.namespace):
            raise ValueError("Invalid progress namespace")

    def key(self, userId, taskId):
        return (
            self.namespace
            + ":task_progress:"
            + hashlib.sha256(js_json([userId, taskId]).encode()).hexdigest()
        )

    def subscribe(self, userId, taskId, listener):
        key = self.key(userId, taskId)
        self.listeners[key].add(listener)

        def unsubscribe():
            self.listeners[key].discard(listener)
            if not self.listeners[key]:
                self.listeners.pop(key, None)

        return unsubscribe

    def publish(self, task):
        if self.closed:
            return
        key = self.key(task["userId"], task["id"])
        for listener in tuple(self.listeners.get(key, ())):
            listener()
        if not self.client or len(self.pending) >= 128:
            return

        async def send():
            try:
                await asyncio.wait_for(
                    self.client.eval(
                        publishScript, 1, key, str(task.get("stateVersion", 0))
                    ),
                    self.timeoutMs / 1000,
                )
            except Exception:
                pass

        work = asyncio.create_task(send())
        self.pending.add(work)
        work.add_done_callback(self.pending.discard)

    async def read(self, userId, taskId, cursor="0-0"):
        if not self.client or self.closed:
            return {"cursor": cursor, "changed": False, "delivery": "database"}
        try:
            streams = await asyncio.wait_for(
                self.client.xread({self.key(userId, taskId): cursor}, count=128),
                self.timeoutMs / 1000,
            )
            messages = streams[0][1] if streams else []
            value = messages[-1][0] if messages else cursor
            if isinstance(value, bytes):
                value = value.decode()
            return {"cursor": value, "changed": bool(messages), "delivery": "redis"}
        except Exception:
            return {"cursor": cursor, "changed": False, "delivery": "database"}

    async def close(self):
        self.closed = True
        self.listeners.clear()
        await asyncio.gather(*self.pending, return_exceptions=True)


class TaskProgressConnections:
    def __init__(self, options=None, **kwargs):
        opts = {**(options or {}), **kwargs}
        for k, v in {
            "pollMs": 500,
            "fallbackMs": 2000,
            "safetyMs": 10000,
            "sessionMs": 2000,
            "heartbeatMs": 15000,
            "lifetimeMs": 300000,
            "backpressureMs": 5000,
            "maxConnections": 100,
            "maxPerUser": 2,
            "now": now_ms,
        }.items():
            setattr(self, k, opts.get(k, v))
        self.hub = opts["hub"]
        self.tasks = opts["tasks"]
        self.store = opts["store"]
        self.connections = set()
        self.users = defaultdict(int)
        self.closed = False

    async def open(self, request, *, userId, token, taskId):
        last = request.headers.get("last-event-id")
        if last is not None and (
            not re.fullmatch(r"(0|[1-9]\d{0,15})", last) or not safe_integer(int(last))
        ):
            raise ApiError(400, "INVALID_EVENT_ID", "进度游标无效，请重新连接。")
        initial = await self.tasks.progress(taskId, userId)
        if last is not None and int(last) > initial["stateVersion"]:
            raise ApiError(
                409, "EVENT_CURSOR_AHEAD", "进度游标超出当前版本，请刷新任务后重连。"
            )
        if self.closed:
            raise ApiError(503, "PROGRESS_UNAVAILABLE", "任务进度服务正在关闭。")
        if (
            len(self.connections) >= self.maxConnections
            or self.users[userId] >= self.maxPerUser
        ):
            raise ApiError(
                429, "PROGRESS_CONNECTION_LIMIT", "进度连接过多，请关闭其他连接后重试。"
            )
        entry = object()
        self.connections.add(entry)
        self.users[userId] += 1
        dirty = asyncio.Event()
        dirty.set()
        unsubscribe = self.hub.subscribe(userId, taskId, dirty.set)
        cleaned = False

        def cleanup():
            nonlocal cleaned
            if cleaned:
                return
            cleaned = True
            unsubscribe()
            self.connections.discard(entry)
            self.users[userId] -= 1
            if not self.users[userId]:
                self.users.pop(userId, None)

        def frame(data, event="progress"):
            return (
                f"id: {data['stateVersion']}\n" if event == "progress" else ""
            ) + f"event: {event}\ndata: {js_json(data)}\n\n"

        async def stream():
            loop = asyncio.get_running_loop()
            started = loop.time()
            lastDb = 0
            lastSession = started
            lastHeartbeat = started
            cursor = "0-0"
            version = -1
            mode = "redis" if self.hub.client else "database"

            def delivery(progress):
                return {
                    **progress,
                    "delivery": mode,
                    **(
                        {
                            "transportNotice": "实时通知暂不可用，正在通过数据库同步进度。"
                            if self.hub.client
                            else "正在通过数据库同步进度。"
                        }
                        if mode == "database"
                        else {}
                    ),
                }

            try:
                # StreamingResponse的await send自带流控；应用级有界等待由外层响应实现。
                version = initial["stateVersion"]
                yield frame(delivery(initial))
                lastDb = loop.time()
                if initial["status"] in terminal:
                    return
                while (
                    not self.closed and (loop.time() - started) * 1000 < self.lifetimeMs
                ):
                    await asyncio.sleep(self.pollMs / 1000)
                    if await request.is_disconnected():
                        return
                    time = loop.time()
                    checked = False

                    async def check():
                        nonlocal lastSession, checked
                        lastSession = loop.time()
                        checked = True
                        session = await asyncio.wait_for(
                            self.store.session(token, self.now()), 1.5
                        )
                        return bool(session and session["user"]["id"] == userId)

                    if (
                        time - lastSession
                    ) * 1000 >= self.sessionMs and not await check():
                        yield frame({"code": "AUTH_REQUIRED"}, "auth_expired")
                        return
                    note = await self.hub.read(userId, taskId, cursor)
                    cursor = note["cursor"]
                    previous = mode
                    mode = note["delivery"]
                    if (
                        dirty.is_set()
                        or note["changed"]
                        or (time - lastDb) * 1000
                        >= (self.safetyMs if mode == "redis" else self.fallbackMs)
                        or previous != mode
                    ):
                        if not checked and not await check():
                            yield frame({"code": "AUTH_REQUIRED"}, "auth_expired")
                            return
                        dirty.clear()
                        lastDb = time
                        progress = await asyncio.wait_for(
                            self.tasks.progress(taskId, userId), 1.5
                        )
                        if progress["stateVersion"] >= version and (
                            progress["stateVersion"] > version or previous != mode
                        ):
                            version = progress["stateVersion"]
                            yield frame(delivery(progress))
                            if progress["status"] in terminal:
                                return
                    if (time - lastHeartbeat) * 1000 >= self.heartbeatMs:
                        lastHeartbeat = time
                        yield ": keepalive\n\n"
            except asyncio.CancelledError:
                raise
            except Exception:
                yield frame({"code": "PROGRESS_UNAVAILABLE"}, "unavailable")
            finally:
                cleanup()

        return BoundedStreamingResponse(
            stream(),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache, no-store",
                "X-Content-Type-Options": "nosniff",
                "X-Accel-Buffering": "no",
            },
            sendTimeout=self.backpressureMs / 1000,
            onClose=cleanup,
        )

    def close(self):
        self.closed = True


class BoundedStreamingResponse(StreamingResponse):
    def __init__(self, *args, sendTimeout=5, onClose=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.sendTimeout = sendTimeout
        self.onClose = onClose

    async def __call__(self, scope, receive, send):
        async def bounded(message):
            await asyncio.wait_for(send(message), self.sendTimeout)

        try:
            await super().__call__(scope, receive, bounded)
        except TimeoutError:
            pass
        finally:
            # 响应头发送失败时生成器尚未启动，独立归还连接名额与订阅。
            if hasattr(self.body_iterator, "aclose"):
                await self.body_iterator.aclose()
            if self.onClose:
                self.onClose()
