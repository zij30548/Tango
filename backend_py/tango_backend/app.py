"""兼容 Qt 的 FastAPI 入口；认证、收消息、路由与任务执行分别处理。"""

import asyncio
import contextlib
import ipaddress
import json
import os
import re
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from starlette.responses import JSONResponse

from .compat import now_ms, maybe_await
from .errors import ApiError
from .store import validateCredentials
from .model import validateInput
from .radio_model import validateRadioPlanInput, validateRadioSelectInput
from .assistant_model import validateAssistantInput
from .task_schema import TASK_SKILLS
from .music_skills import musicSkills
from .task_runtime import MusicTaskRuntime
from .redis_limiter import MemoryRateLimiter
from .task_progress import TaskProgressHub, TaskProgressConnections
from .conversation_store import ConversationStore
from .conversation_router import ConversationRouter
from .registration_service import RegistrationService
from .account_quota import AccountPolicyStore, DEFAULT_QUOTA_POLICY
from .development_session import DevelopmentSession
from .capability_registry import CAPABILITY_REGISTRY_VERSION, capabilities
from .agent_tracing import AgentTracing, disable_automatic_tracing

BODY_LIMIT = 16 * 1024
ID = r"[A-Za-z0-9_-]{8,100}"


async def readBody(request, limit=BODY_LIMIT):
    if not re.match(
        r"^application/json(?:\s*;|$)", request.headers.get("content-type", ""), re.I
    ):
        raise ApiError(415, "JSON_REQUIRED", "请求必须使用 JSON。")
    length = request.headers.get("content-length", "0")
    if length.isdigit() and int(length) > limit:
        raise ApiError(413, "BODY_TOO_LARGE", "请求内容过大。")
    chunks = []
    size = 0
    try:
        async with asyncio.timeout(20):
            async for chunk in request.stream():
                size += len(chunk)
                if size > limit:
                    raise ApiError(413, "BODY_TOO_LARGE", "请求内容过大。")
                chunks.append(chunk)
    except TimeoutError:
        raise ApiError(408, "REQUEST_TIMEOUT", "请求接收超时。") from None
    try:

        def invalid_constant(value):
            raise ValueError("invalid constant")

        return json.loads(b"".join(chunks), parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise ApiError(400, "INVALID_JSON", "请求不是有效 JSON。") from None


def bearer(request):
    match = re.fullmatch(
        r"Bearer ([A-Za-z0-9_-]{43})", request.headers.get("authorization", "")
    )
    return match[1] if match else None


def ipAddress(request, trustLoopbackProxy):
    peer = request.client.host if request.client else "unknown"
    if trustLoopbackProxy and peer in ["127.0.0.1", "::1", "::ffff:127.0.0.1"]:
        try:
            return str(ipaddress.ip_address(request.headers.get("x-real-ip", "")))
        except ValueError:
            pass
    return peer


async def takeRequestLimit(window, identity, now, allowUnavailableRead=False):
    try:
        return await window.take(identity, now)
    except ApiError as error:
        if allowUnavailableRead and error.code == "RATE_LIMIT_UNAVAILABLE":
            return True
        raise


class Services:
    def __init__(self, **options):
        self.options = options
        self.tracing = options.get("tracing") or AgentTracing()
        self.store = options["store"]
        self.now = options.get("now", now_ms)
        for name in [
            "model",
            "radioModel",
            "assistantModel",
            "taskModel",
            "routingModel",
        ]:
            setattr(self, name, options.get(name))
        self.limiter = options.get("rateLimiter") or MemoryRateLimiter()

        def window(name, maximum, periodMs):
            return self.limiter.window(name, {"maximum": maximum, "periodMs": periodMs})

        self.authLimits = window(
            "auth_ip", options.get("authPerTenMinutes", 20), 10 * 60000
        )
        self.accountFailures = window(
            "auth_failures", options.get("authFailuresPerTenMinutes", 10), 10 * 60000
        )
        self.aiLimits = window("ai_user", options.get("aiPerMinute", 10), 60000)
        self.requestLimits = window(
            "request_ip", options.get("requestPerMinute", 240), 60000
        )
        self.userLimits = window(
            "request_user", options.get("userRequestPerMinute", 180), 60000
        )
        self.conversationLimits = window(
            "conversation_create",
            options.get("conversationCreatesPerDay", 20),
            86400000,
        )
        self.registration = RegistrationService(
            store=self.store,
            registrationMode=options.get("registrationMode", "invite_only"),
            passwordMin=options.get("registrationPasswordMin", 15),
            now=self.now,
        )
        self.accountPolicy = AccountPolicyStore(
            self.store,
            defaults=options.get("defaultQuotaPolicy", DEFAULT_QUOTA_POLICY),
            dailyCap=options.get("aiDailyLimit", 100),
        )
        self.store.accountPolicy = self.accountPolicy
        self.devConfig = options.get("developmentSession") or {"enabled": False}
        self.devSessions = (
            DevelopmentSession(
                {
                    **self.devConfig,
                    "store": self.store,
                    "quota": self.accountPolicy,
                    "now": self.now,
                }
            )
            if self.devConfig.get("enabled")
            else None
        )
        self.activeUsers = set()
        self.authenticating = 0
        self.progressHub = options.get("progressHub") or TaskProgressHub()
        self.tasks = MusicTaskRuntime(
            store=self.store,
            model=self.taskModel,
            now=self.now,
            dailyLimit=options.get("aiDailyLimit", 100),
            globalDailyLimit=options.get("aiGlobalDailyLimit", 2000),
            globalConcurrency=options.get("aiGlobalConcurrency", 8),
            strategy=options.get("taskStrategy", "single_agent"),
            workerId=options.get("workerId"),
            leaseMs=options.get("leaseMs", 30000),
            progressHub=self.progressHub,
            toolCache=options.get("toolCache"),
            tracing=self.tracing,
            legacyBusy=lambda userId: userId in self.activeUsers,
            legacyCount=lambda: len(self.activeUsers),
            allowCall=lambda userId: self.aiLimits.take(userId, self.now()),
        )
        self.progressConnections = TaskProgressConnections(
            **{
                **options.get("progressOptions", {}),
                "hub": self.progressHub,
                "tasks": self.tasks,
                "store": self.store,
                "now": self.now,
            }
        )
        self.conversations = ConversationStore(self.store, now=self.now)
        self.router = (
            ConversationRouter(
                store=self.store,
                conversations=self.conversations,
                tasks=self.tasks,
                model=self.routingModel,
                now=self.now,
                workerId=options.get("workerId"),
                leaseMs=options.get("leaseMs", 30000),
                dailyLimit=options.get("aiDailyLimit", 100),
                globalDailyLimit=options.get("aiGlobalDailyLimit", 2000),
                globalConcurrency=options.get("aiGlobalConcurrency", 8),
                allowCall=lambda userId: self.aiLimits.take(userId, self.now()),
                tracing=self.tracing,
            )
            if self.routingModel
            else None
        )
        if self.router:
            self.tasks.conversationContextProvider = self.router.taskContext
            self.tasks.beforeModelCall = self.router.waitForModelSlot

    async def initialize(self):
        await self.store.initialize()
        await self.accountPolicy.initialize()
        await self.tasks.initialize()
        await self.conversations.initialize()
        if self.router:
            await self.router.initialize()

    async def close(self):
        if self.router:
            await self.router.close()
        self.progressConnections.close()
        await self.tasks.close()
        await self.progressHub.close()
        await asyncio.to_thread(self.tracing.close, timeout=3)

    def health(self):
        return {
            "app": "quiet-app-backend",
            "version": 1,
            "pid": os.getpid(),
            "runtimeLanguage": "python",
            "runtimeVersion": 2,
            "storage": self.store.dialect,
            "rateLimiter": "redis" if self.options.get("redis") else "memory",
            "storageVersion": 1,
            "agentFeaturesVersion": 2,
            "accountFeaturesVersion": 1,
            "developmentMode": bool(self.devSessions),
            "developmentSessionVersion": 1,
            "routingFeaturesVersion": 1 if self.router else 0,
            "lookupContextVersion": 1 if self.router else 0,
            "conversationClearVersion": 1,
            "musicIntentVersion": 1,
            "modelRoutingVersion": 1,
            "tracingVersion": 1,
            "tracing": self.tracing.health(),
            **(
                {"developmentTokenFingerprint": self.devConfig["tokenFingerprint"]}
                if self.devSessions
                else {}
            ),
        }

    async def conversationApi(self, request, path, userId, token):
        if not path.startswith("/v1/conversations"):
            return None
        parts = path.split("/")[3:]
        method = request.method
        c = self.conversations
        if path == "/v1/conversations" and method == "DELETE":
            return 200, await c.clearAll(userId, await readBody(request), self.tasks)
        if (
            len(parts) == 4
            and parts[1] == "messages"
            and parts[3] == "route"
            and method in ["GET", "POST"]
            and self.router
        ):
            if method == "GET":
                return 200, await self.router.get(parts[0], parts[2], userId)
            body = await readBody(request)
            return 200, await disconnectable(
                request,
                lambda signal: self.router.route(
                    parts[0],
                    parts[2],
                    userId,
                    body,
                    {
                        "signal": signal,
                        "authorize": lambda: self.store.session(token, self.now()),
                    },
                ),
            )
        if path == "/v1/conversations" and method == "POST":
            return 201, {
                "conversation": await c.create(userId, await readBody(request))
            }
        if len(parts) == 2 and parts[0] == "latest" and method == "GET":
            return 200, {"conversation": await c.latest(userId, parts[1])}
        if len(parts) == 1 and parts[0] and method == "GET":
            return 200, {"conversation": await c.get(parts[0], userId)}
        if len(parts) == 3 and parts[1] == "before" and method == "GET":
            return 200, {
                "conversation": await c.get(
                    parts[0],
                    userId,
                    int(parts[2]) if parts[2].isdigit() else float("nan"),
                )
            }
        if len(parts) == 3 and parts[1] == "messages" and method == "GET":
            return 200, await c.receipt(parts[0], userId, parts[2])
        if len(parts) == 2 and parts[1] == "messages" and method == "POST":
            body = await readBody(request)
            hint = (
                "router"
                if self.router
                else "legacy_assistant"
                if isinstance(body, dict)
                and re.search(
                    r"记住|记忆|(?:清空|删除|清除|忘掉|忘记).*(?:偏好|口味)|(?:长期|永久)保存|(?:以后|今后).*(?:优先|不要|少推|多推)",
                    str(body.get("content", "")),
                )
                else "task"
            )
            return 200, await c.message(parts[0], userId, body, {"dispatchHint": hint})
        if len(parts) == 2 and parts[1] == "events" and method == "POST":
            return 200, await c.event(
                parts[0], userId, await readBody(request), self.tasks
            )
        return None

    async def handle(self, request):
        if "origin" in request.headers:
            raise ApiError(403, "ORIGIN_NOT_ALLOWED", "此接口仅供 App 使用。")
        if request.url.query:
            raise ApiError(400, "INVALID_ROUTE", "接口不接受查询参数。")
        path = request.url.path
        method = request.method
        if method == "GET" and path == "/health":
            return response(200, self.health())
        if method == "GET" and path == "/ready":
            from .service_config import serviceReady

            ready = await serviceReady(self.store, self.options.get("redis"))
            return response(
                200 if ready else 503,
                {
                    "ready": ready,
                    "storage": self.store.dialect,
                    "rateLimiter": "redis" if self.options.get("redis") else "memory",
                },
            )
        recoveryRead = (
            method == "GET"
            and bool(bearer(request))
            and bool(
                re.match(r"^/v1/(?:tasks|conversations)(?:/|$)", path)
                or path in ["/v1/auth/me", "/v1/account/usage", "/v1/agent/status"]
            )
        )
        if not await takeRequestLimit(
            self.requestLimits,
            ipAddress(request, self.options.get("trustLoopbackProxy", False)),
            self.now(),
            recoveryRead,
        ):
            raise ApiError(429, "REQUEST_RATE_LIMITED", "请求过于频繁，请稍后再试。")
        if path == "/v1/dev/session":
            if not self.devSessions or method != "POST":
                raise ApiError(404, "NOT_FOUND", "接口不存在。")
            self.devSessions.validate(request)
            body = await readBody(request, 64)
            if not isinstance(body, dict) or body:
                raise ApiError(
                    400, "INVALID_DEVELOPMENT_REQUEST", "开发会话不接受账户或任务参数。"
                )
            return response(200, await self.devSessions.create(request))
        if method == "GET" and path == "/v1/auth/config":
            return response(200, self.registration.publicConfig())
        if method == "POST" and path in ["/v1/auth/register", "/v1/auth/login"]:
            if not await self.authLimits.take(
                ipAddress(request, self.options.get("trustLoopbackProxy", False)),
                self.now(),
            ):
                raise ApiError(
                    429, "AUTH_RATE_LIMITED", "登录或注册尝试过多，请稍后再试。"
                )
            registering = path.endswith("/register")
            body = await readBody(request)
            credentials = (
                self.registration.validate(body)["credentials"]
                if registering
                else validateCredentials(body)
            )
            if self.authenticating >= 4:
                raise ApiError(503, "AUTH_BUSY", "账户服务繁忙，请稍后重试。")
            self.authenticating += 1
            try:
                if not registering and not await self.accountFailures.take(
                    credentials["usernameKey"], self.now()
                ):
                    raise ApiError(
                        429,
                        "ACCOUNT_RATE_LIMITED",
                        "此账户登录尝试次数过多，请在 10 分钟后重试。",
                    )
                user = (
                    await self.registration.register(body)
                    if registering
                    else await self.store.authenticate(credentials)
                )
                if not registering:
                    await self.accountFailures.clear(credentials["usernameKey"])
            finally:
                self.authenticating -= 1
            return response(
                201 if registering else 200,
                await self.store.createSession(user, self.now()),
            )
        token = bearer(request)
        session = await self.store.session(token, self.now()) if token else None
        if not session:
            raise ApiError(
                401, "AUTH_REQUIRED", "请先登录探歌账户，或重新登录已过期的账户。"
            )
        userId = session["user"]["id"]
        if not await takeRequestLimit(
            self.userLimits, userId, self.now(), recoveryRead
        ):
            raise ApiError(
                429, "USER_RATE_LIMITED", "当前账号请求过于频繁，请稍后再试。"
            )
        if (
            method == "POST"
            and path == "/v1/conversations"
            and not await self.conversationLimits.take(userId, self.now())
        ):
            raise ApiError(
                429,
                "CONVERSATION_RATE_LIMITED",
                "今日创建对话的请求过多，请继续已有对话或稍后再试。",
            )
        conversation = await self.conversationApi(request, path, userId, token)
        if conversation:
            return response(*conversation)
        if method == "GET" and path == "/v1/account/usage":
            return response(200, await self.accountPolicy.snapshot(userId, self.now()))
        if method == "GET" and path == "/v1/auth/me":
            return response(200, session)
        if method == "POST" and path == "/v1/auth/logout":
            try:
                await self.tasks.pauseUser(userId)
            finally:
                await self.store.logout(token)
            return response(200, {"ok": True})
        if method == "GET" and path == "/v1/agent/status":
            return response(
                200,
                {
                    "configured": bool(self.model and self.model.configured),
                    "model": getattr(self.model, "model", ""),
                    **(
                        {"modelRouting": self.options["modelRouting"]}
                        if self.options.get("modelRouting")
                        else {}
                    ),
                    "conversationVersion": 1,
                    "conversationClearVersion": 1,
                    "musicIntentVersion": 1,
                    **(
                        {"routingVersion": 1, "capabilityRegistryVersion": CAPABILITY_REGISTRY_VERSION,
                         "lookupContextVersion": 1}
                        if self.router
                        else {}
                    ),
                    **(
                        {
                            "taskRuntime": bool(self.taskModel.configured),
                            "taskRuntimeVersion": 1,
                            "taskVersion": 1,
                            "taskEventsVersion": 1,
                            "catalogCacheVersion": 1,
                        }
                        if self.taskModel
                        else {}
                    ),
                },
            )
        if method == "GET" and path == "/v1/agent/capabilities":
            return response(200, capabilities())
        if method == "GET" and path == "/v1/agent/skills":
            return response(200, musicSkills())
        if method == "GET" and path == "/v1/tasks/skills":
            return response(
                200, {"skills": TASK_SKILLS, "runtime": "langgraph", "version": 1}
            )
        if method == "POST" and path == "/v1/tasks":
            return response(
                201, {"task": await self.tasks.create(userId, await readBody(request))}
            )
        if method == "GET" and re.fullmatch(
            "/v1/tasks/requests/" + ID + "/" + ID, path
        ):
            deviceId, requestId = path.split("/")[-2:]
            task = await self.tasks.store.requestReceipt(userId, deviceId, requestId)
            # 回执只证明原操作已经提交；当前任务可能已失败或进入后续版本。
            # 不走 get 的恢复逻辑，不运行模型、工具或修改检查点。
            receipt = {
                "deviceId": deviceId,
                "requestId": requestId,
                "status": "recorded" if task else "unknown",
            }
            if task:
                receipt["taskId"] = task["id"]
                # 原动作事实与原请求同事务保存；旧记录缺字段保持未知，不能从当前任务反推。
                facts = await self.tasks.store.requestFacts(userId, deviceId, requestId)
                for field in ("action", "expectedRevision"):
                    if field in facts:
                        receipt[field] = facts[field]
            return response(
                200, {"receipt": receipt, "task": await self.tasks.snapshot(task)}
            )
        if method == "GET" and re.fullmatch("/v1/tasks/latest/" + ID, path):
            return response(
                200, {"task": await self.tasks.latest(userId, path.split("/")[-1])}
            )
        if method == "GET" and re.fullmatch("/v1/tasks/" + ID + "/events", path):
            return await self.progressConnections.open(
                request, userId=userId, token=token, taskId=path.split("/")[3]
            )
        if method == "GET" and re.fullmatch("/v1/tasks/" + ID, path):
            return response(
                200, {"task": await self.tasks.get(path.split("/")[-1], userId)}
            )
        if method == "POST" and re.fullmatch("/v1/tasks/" + ID + "/actions", path):
            return response(
                200,
                {
                    "task": await self.tasks.action(
                        path.split("/")[3], userId, await readBody(request)
                    )
                },
            )
        if method == "POST" and re.fullmatch(
            "/v1/tasks/" + ID + "/steps/" + ID + "/result", path
        ):
            return response(
                200,
                {
                    "task": await self.tasks.result(
                        path.split("/")[3],
                        path.split("/")[5],
                        userId,
                        await readBody(request, 256 * 1024),
                    )
                },
            )
        if method == "POST" and path in [
            "/v1/focus/parse",
            "/v1/radio/plan",
            "/v1/radio/select",
            "/v1/assistant/plan",
        ]:
            return await self.legacy(request, path, userId, token)
        raise ApiError(404, "NOT_FOUND", "接口不存在。")

    async def legacy(self, request, path, userId, token):
        focus = path == "/v1/focus/parse"
        assistant = path == "/v1/assistant/plan"
        selection = path == "/v1/radio/select"
        model = (
            self.model
            if focus
            else self.assistantModel
            if assistant
            else self.radioModel
        )
        if not model or not model.configured:
            raise ApiError(
                503, "MODEL_NOT_CONFIGURED", "管理员尚未配置此 AI 服务，请稍后再试。"
            )
        body = await readBody(request, 64 * 1024 if selection else BODY_LIMIT)
        input = (
            validateInput(body)
            if focus
            else validateAssistantInput(body)
            if assistant
            else validateRadioSelectInput(body)
            if selection
            else validateRadioPlanInput(body)
        )
        claim = await self.tasks.acquireLegacy(userId)
        try:
            if not await self.aiLimits.take(userId, self.now()):
                raise ApiError(429, "AI_RATE_LIMITED", "AI 请求过于频繁，请稍后再试。")
            await self.tasks.reserveLegacyUsage(userId, claim)
            self.activeUsers.add(userId)

            async def execute(signal):
                parsed = await (
                    model.parse(input, {"signal": signal})
                    if focus
                    else model.select(input, {"signal": signal})
                    if selection
                    else model.plan(input, {"signal": signal})
                )
                await self.tasks.assertLegacyCurrent(claim)
                if signal.is_set():
                    raise ApiError(499, "REQUEST_CANCELLED", "请求已取消。")
                if not await self.store.session(token, self.now()):
                    raise ApiError(
                        401, "AUTH_REQUIRED", "账户已退出或登录已过期，请重新登录。"
                    )
                return parsed

            parsed = await disconnectable(request, execute, (claim or {}).get("signal"))
            return response(200, {"request": parsed} if focus else parsed)
        finally:
            self.activeUsers.discard(userId)
            await self.tasks.releaseLegacy(claim)


async def disconnectable(request, operation, leaseSignal=None):
    signal = asyncio.Event()
    if leaseSignal and leaseSignal.is_set():
        signal.set()

    async def watch():
        while True:
            message = await request.receive()
            if message["type"] == "http.disconnect":
                signal.set()
                return

    monitor = asyncio.create_task(watch())

    async def watch_lease():
        await leaseSignal.wait()
        signal.set()

    leaseMonitor = asyncio.create_task(watch_lease()) if leaseSignal else None
    try:
        return await operation(signal)
    finally:
        monitor.cancel()
        if leaseMonitor:
            leaseMonitor.cancel()
        await asyncio.gather(
            monitor, *([leaseMonitor] if leaseMonitor else []), return_exceptions=True
        )


def response(status, body):
    return JSONResponse(
        body,
        status_code=status,
        headers={"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"},
    )


def createApp(options=None, **kwargs):
    services = Services(**{**(options or {}), **kwargs})

    @asynccontextmanager
    async def lifespan(app):
        try:
            await services.initialize()
            yield
        finally:
            await services.close()

    app = FastAPI(lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    app.services = services
    for name in [
        "store",
        "tasks",
        "conversations",
        "registration",
        "accountPolicy",
        "progressConnections",
    ]:
        setattr(app, name, getattr(services, name))
    app.conversationRouter = services.router
    app.tracing = services.tracing
    app.initialize = services.initialize
    app.close = services.close

    @app.api_route(
        "/{path:path}",
        methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"],
    )
    async def dispatch(request: Request, path: str):
        try:
            # 即使宿主误开 SDK 全局开关，也不自动采集请求、任务或原始图状态。
            with disable_automatic_tracing():
                return await services.handle(request)
        except ApiError as error:
            return response(
                error.status, {"error": {"code": error.code, "message": error.message}}
            )
        except asyncio.CancelledError:
            raise
        except Exception:
            return response(
                500,
                {
                    "error": {
                        "code": "INTERNAL_ERROR",
                        "message": "后台暂时无法完成请求，请稍后重试。",
                    }
                },
            )

    return app
