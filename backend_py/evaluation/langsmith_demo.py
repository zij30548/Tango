"""隔离执行真实音乐业务并展示受限链路；模型和曲库始终使用明确夹具。"""

import argparse
import asyncio
from datetime import datetime, timezone
import html
import json
import os
from pathlib import Path
import secrets
import tempfile
from threading import Lock
import time
from uuid import uuid4

import httpx

from tango_backend.agent_tracing import AgentTracing, LangSmithExporter, disable_automatic_tracing
from tango_backend.app import createApp
from tango_backend.compat import clone
from tango_backend.store import Store
from .scripted import ScriptedModel, controlled_catalog


PROJECT = Path(__file__).resolve().parents[2]
REGIONS = {
    "us": ("https://api.smith.langchain.com", "https://smith.langchain.com"),
    "eu": ("https://eu.api.smith.langchain.com", "https://eu.smith.langchain.com"),
    "apac": ("https://apac.api.smith.langchain.com", "https://apac.smith.langchain.com"),
    "aws-us": ("https://aws.api.smith.langchain.com", "https://aws.smith.langchain.com"),
}
FIXTURE_METADATA = {"fixture": True, "upstream_mode": "scripted", "evaluation_scope": "integration_wiring"}
INSTRUCTION = "推荐20分钟微醺粤语歌单，不要现场版"
DEVICE = "langsmith-demo-device"


class DemoError(ValueError):
    """演示仅传播固定原因码，不展示配置、供应商响应或用户库信息。"""


class MemoryExporter:
    """采集已经过产品投影的摘要；不自行调用模型、曲库或云平台。"""

    def __init__(self):
        self._events = []
        self._lock = Lock()

    def export(self, operation, payload):
        data = clone(payload)
        data.setdefault("extra", {}).setdefault("metadata", {}).update(FIXTURE_METADATA)
        with self._lock:
            self._events.append({"operation": operation, "payload": data})

    def events(self):
        with self._lock:
            return clone(self._events)


class DemoCloudExporter:
    """本机与云端接收相同摘要；失败向 worker 上报，不能冒充上传成功。"""

    def __init__(self, collector, cloud):
        self.collector, self.cloud = collector, cloud

    def export(self, operation, payload):
        data = clone(payload)
        data.setdefault("extra", {}).setdefault("metadata", {}).update(FIXTURE_METADATA)
        self.collector.export(operation, data)
        self.cloud.export(operation, data)

    def export_many(self, events):
        """同批保留本机证据并交给真正批量出口，避免为每个节点单独往返。"""
        batch = []
        for operation, payload in events:
            data = clone(payload)
            data.setdefault("extra", {}).setdefault("metadata", {}).update(FIXTURE_METADATA)
            self.collector.export(operation, data)
            batch.append((operation, data))
        if callable(getattr(self.cloud, "export_many", None)):
            self.cloud.export_many(batch)
        else:
            for operation, payload in batch:
                self.cloud.export(operation, payload)

    def close(self, timeout=0):
        self.cloud.close(timeout=timeout)


def cloud_configuration(path, region=None):
    """只向观测 SDK 交付 LangSmith 字段；不构造模型客户端或加载用户存储配置。"""
    from dotenv import dotenv_values

    path = Path(path)
    if not path.is_file() or path.is_symlink() or path.stat().st_size > 128 * 1024:
        raise DemoError("DEMO_LANGSMITH_CONFIGURATION_REQUIRED")
    values = dotenv_values(path, interpolate=False)
    config = {key: values[key] for key in ("LANGSMITH_API_KEY", "LANGSMITH_ENDPOINT", "LANGSMITH_PROJECT", "LANGSMITH_WORKSPACE_ID")
              if isinstance(values.get(key), str) and values[key]}
    config["QUIET_LANGSMITH_ENABLED"] = "1"
    if region:
        config["LANGSMITH_ENDPOINT"] = REGIONS[region][0]
    config.setdefault("LANGSMITH_ENDPOINT", REGIONS["us"][0])
    config.setdefault("LANGSMITH_PROJECT", "tango-development")
    if not config.get("LANGSMITH_API_KEY"):
        raise DemoError("DEMO_LANGSMITH_CONFIGURATION_REQUIRED")
    if "${" in config["LANGSMITH_API_KEY"]:
        raise DemoError("DEMO_LANGSMITH_CONFIGURATION_INVALID")
    return config


class FixtureModel(ScriptedModel):
    """保留真实路由、任务、额度与图执行；只替换外部模型响应。"""

    def __init__(self, now):
        super().__init__("scene", now)
        self.calls = []

    async def route(self, inputs, options=None):
        self.calls.append("route")
        value = await super().route(inputs, options)
        return {**value, "usage": {"inputTokens": 3, "outputTokens": 2}}

    async def call(self, kind, inputs, options=None):
        self.calls.append(kind)
        value = await super().call(kind, inputs, options)
        return {**value, "usage": {"inputTokens": 3, "outputTokens": 2}}


async def execute_demo(tracing):
    """临时库经 ASGI 真执行到生效回执；所有外部结果由固定夹具提供。"""
    with tempfile.TemporaryDirectory(prefix="tango-langsmith-demo-") as temporary, disable_automatic_tracing():
        store = Store(Path(temporary) / "owned.sqlite")
        model = FixtureModel(int(time.time() * 1000))
        app = createApp(store=store, model=model, taskModel=model, routingModel=model,
                        tracing=tracing, registrationMode="open", aiPerMinute=100,
                        requestPerMinute=20000, userRequestPerMinute=20000)
        await app.initialize()
        try:
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://127.0.0.1") as client:
                async def request(method, path, body=None, expected=200):
                    response = await client.request(method, path, **({"json": body} if body is not None else {}))
                    if response.status_code != expected:
                        code = response.json().get("error", {}).get("code", "DEMO_HTTP_FAILED")
                        raise DemoError(code if isinstance(code, str) and code.isupper() else "DEMO_HTTP_FAILED")
                    return response.json()

                session = await request("POST", "/v1/auth/register", {
                    "username": "langsmith-demo-fixture", "password": secrets.token_urlsafe(24)}, expected=201)
                client.headers["authorization"] = "Bearer " + session["token"]
                conversation = (await request("POST", "/v1/conversations", {
                    "deviceId": DEVICE, "requestId": str(uuid4())}, expected=201))["conversation"]
                message_id = str(uuid4())
                base = "/v1/conversations/" + conversation["id"]
                message = {"deviceId": DEVICE, "messageId": message_id, "content": INSTRUCTION}
                await request("POST", base + "/messages", message)
                route_body = {"deviceId": DEVICE, "preference": ""}
                route = (await request("POST", base + "/messages/" + message_id + "/route", route_body))["route"]
                if route["status"] != "completed" or not any(
                    row["capability"] == "music.task" for row in route["result"]["actions"]
                ):
                    raise DemoError("DEMO_ROUTE_FAILED")
                task = (await request("POST", "/v1/tasks", {
                    "deviceId": DEVICE, "requestId": message_id, "instruction": INSTRUCTION,
                    "preference": ""}, expected=201))["task"]

                async def until(*statuses):
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        current = (await request("GET", "/v1/tasks/" + task["id"]))["task"]
                        if current["status"] in statuses:
                            return current
                        if current["status"] in {"failed", "budget_exhausted", "needs_clarification"}:
                            raise DemoError("DEMO_TASK_FAILED")
                        await asyncio.sleep(.01)
                    raise DemoError("DEMO_TASK_TIMEOUT")

                waiting = await until("waiting_tools")
                tools = []
                stale_rejected = False
                for step in waiting["pendingSteps"]:
                    result = controlled_catalog(step, model.now, 0, DEVICE)
                    path = "/v1/tasks/" + task["id"] + "/steps/" + step["id"] + "/result"
                    wrong = {**result, "leaseId": str(uuid4())}
                    rejected = await request("POST", path, wrong, expected=409)
                    stale_rejected = rejected.get("error", {}).get("code") == "STALE_TASK_STEP"
                    if not stale_rejected:
                        raise DemoError("DEMO_STALE_RESULT_ACCEPTED")
                    await request("POST", path, result)
                    tools.append({"skill": step["skill"], "candidateCount": len(result["result"]["tracks"]),
                                  "sourceCount": len(result["result"]["sources"])})
                preview = await until("preview")
                await request("POST", base + "/events", {"deviceId": DEVICE, "eventId": str(uuid4()),
                    "turnId": message_id, "kind": "task_sync", "taskId": task["id"],
                    "taskRevision": preview["revision"]})
                calls_before = len(model.calls)
                usage_before = (await request("GET", "/v1/account/usage"))["total"]["used"]
                await request("POST", base + "/messages", message)
                await request("POST", base + "/messages/" + message_id + "/route", route_body)
                await request("GET", "/v1/tasks/" + task["id"])
                usage_after = (await request("GET", "/v1/account/usage"))["total"]["used"]
                idempotent = len(model.calls) == calls_before and usage_before == usage_after
                if not idempotent:
                    raise DemoError("DEMO_IDEMPOTENCY_FAILED")

                async def action(kind, **extra):
                    return (await request("POST", "/v1/tasks/" + task["id"] + "/actions", {
                        "deviceId": DEVICE, "requestId": str(uuid4()), "action": kind,
                        "expectedRevision": preview["revision"], **extra}))["task"]

                requested = await action("adopt")
                if requested["status"] != "awaiting_apply":
                    raise DemoError("DEMO_ADOPTION_NOT_PENDING")
                applied = await action("applied", adoptionId=requested["adoption"]["id"])
                if applied["status"] != "adopted":
                    raise DemoError("DEMO_APPLICATION_FAILED")
                return {"fixture": True, "upstreamMode": "scripted", "scope": "actual_asgi_langgraph_with_fixture_upstreams",
                    "conversationId": conversation["id"], "messageId": message_id, "routeId": route["id"],
                    "taskId": task["id"], "routeStatus": route["status"], "taskStatus": applied["status"],
                    "previewStatus": preview["status"], "trackCount": len(preview["preview"]["tracks"]),
                    "totalDurationMs": preview["preview"]["totalDurationMs"],
                    "checks": clone(preview["preview"]["checks"]), "tools": tools,
                    "scriptedModelCalls": list(model.calls), "realModelRequests": 0, "realCatalogRequests": 0,
                    "staleLeaseRejected": stale_rejected, "replayNoNewModelCallsOrUsage": idempotent,
                    "playbackVerified": False, "recommendationQualityVerified": False}
        finally:
            await app.close()
            await store.close()


def merged_runs(events):
    """按真实 create/patch 合并树，不根据预期步骤伪造不存在的 span。"""
    values = {}
    for event in events:
        payload = event["payload"]
        ident = payload["id"]
        target = values.setdefault(ident, {"id": ident})
        for key, value in payload.items():
            if key == "extra":
                target.setdefault("extra", {}).setdefault("metadata", {}).update(value.get("metadata", {}))
            else:
                target[key] = clone(value)
    return sorted(values.values(), key=lambda row: row.get("dotted_order", ""))


def render_html(report):
    """本机静态树只展示已采集摘要；转义全部文本且不嵌入远程资源。"""
    runs = report["runs"]
    roots = [row for row in runs if not row.get("parent_run_id")]
    children = {}
    for row in runs:
        children.setdefault(row.get("parent_run_id"), []).append(row)

    def node(row):
        detail = {key: row[key] for key in ("id", "run_type", "start_time", "end_time", "inputs", "outputs", "error", "extra") if key in row}
        child_html = "".join(node(child) for child in children.get(row["id"], []))
        return '<li><details open><summary>' + html.escape(row.get("name", "未知步骤")) + '</summary><pre>' + html.escape(
            json.dumps(detail, ensure_ascii=False, indent=2)) + '</pre>' + ('<ul>' + child_html + '</ul>' if child_html else '') + '</details></li>'

    return '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>探歌隔离链路演示</title><style>body{font:16px system-ui;background:#101915;color:#dce9df;margin:40px;max-width:1100px}summary{cursor:pointer;padding:8px}pre{font-size:13px;white-space:pre-wrap;background:#18261f;padding:12px;border-radius:8px}li{margin:8px 0}small{color:#afc8b8}</style><h1>探歌 Agent 执行链路</h1><p>真实 ASGI、路由、LangGraph、约束检查与状态机；模型和曲库均为固定夹具。没有调用付费模型或验证听感。</p><small>此树来自实际埋点事件；生效回执由模拟设备提交，未启动 Qt 播放。</small><ul>' + ''.join(node(row) for row in roots) + '</ul></html>'


def write_artifacts(directory, report):
    """新建私有产物，拒绝符号链接及覆盖现有文件。"""
    directory = Path(directory).absolute()
    if any(parent.is_symlink() for parent in (directory, *directory.parents)):
        raise DemoError("DEMO_UNSAFE_OUTPUT")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if directory.stat().st_mode & 0o077:
        raise DemoError("DEMO_PRIVATE_OUTPUT_REQUIRED")
    for name, content in (("report.json", json.dumps(report, ensure_ascii=False, indent=2) + "\n"),
                          ("trace.html", render_html(report))):
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(directory / name, flags, 0o600), "w") as stream:
            stream.write(content)


def main(argv=None):
    parser = argparse.ArgumentParser(description="隔离执行探歌并展示受限 trace；默认不上传")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--upload", action="store_true", help="明确授权仅上传此次夹具执行的脱敏摘要")
    parser.add_argument("--region", choices=tuple(REGIONS))
    args = parser.parse_args(argv)
    collector = MemoryExporter()
    cloud = None
    console_url = project_name = None
    if args.upload:
        config = cloud_configuration(PROJECT / "backend_py" / ".env", args.region)
        try:
            cloud = LangSmithExporter.from_env(config)
        except ValueError:
            raise DemoError("DEMO_LANGSMITH_CONFIGURATION_INVALID") from None
        endpoint = config["LANGSMITH_ENDPOINT"].rstrip("/")
        console_url = next(url for api, url in REGIONS.values() if api == endpoint)
        project_name = config["LANGSMITH_PROJECT"]
    elif args.region:
        parser.error("--region 只与 --upload 一起使用")
    observer = AgentTracing(enabled=True, exporter=DemoCloudExporter(collector, cloud) if cloud else collector)
    try:
        result = asyncio.run(execute_demo(observer))
        drained = observer.flush(timeout=30 if cloud else 5)
    finally:
        observer.close(timeout=5)
    events = collector.events()
    runs = merged_runs(events)
    roots = [row for row in runs if not row.get("parent_run_id")]
    health = observer.health()
    uploaded = bool(cloud and drained and health["failed"] == 0 and health["dropped"] == 0 and health["pending"] == 0)
    result.update({"traceIds": [row["id"] for row in roots], "runs": runs, "events": events,
                   "exportHealth": health, "uploaded": uploaded, "uploadRequested": bool(cloud),
                   "langsmithConsole": console_url, "langsmithProject": project_name,
                   "cloudReadbackVerified": False})
    directory = args.output or PROJECT / ".local" / "langsmith-demo" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(4))
    write_artifacts(directory, result)
    print(json.dumps({key: result[key] for key in ("fixture", "taskStatus", "trackCount", "traceIds", "realModelRequests", "realCatalogRequests", "uploaded", "exportHealth")}, ensure_ascii=False))
    print(str(directory / "trace.html"))
    print(str(directory / "report.json"))
    if cloud:
        print("LangSmith 控制台：" + console_url + "；项目：" + project_name)
        print("按报告中的 traceIds 筛选；发送成功不等于已经人工确认云端展示。")
    return 0 if roots and drained and health["failed"] == 0 and health["dropped"] == 0 and health["pending"] == 0 else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except DemoError as error:
        print(str(error), file=__import__("sys").stderr)
        raise SystemExit(1)
