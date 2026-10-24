"""音乐需求真实验收：单次冻结题面、共享新预算，默认只准备而不调用模型。"""

import argparse
import asyncio
from collections import Counter
from datetime import datetime, timezone
import hashlib
import hmac
import json
import logging
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import shutil
import subprocess
import sys
import time


PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "backend_py"))

from evaluation.calibration import (
    CalibrationError, RequestLedger, checkpoint, encode, live_models,
    private_directory, read_json, redact, write_new,
)
from evaluation.independent import source_hashes
from tango_backend.agent_tracing import AgentTracing, disable_automatic_tracing
from tango_backend.app import createApp
from tango_backend.development_config import developmentConfiguration
from tango_backend.http_protocol import BoundedH11Protocol
from tango_backend.store import Store


BASE = PROJECT / ".local/music-requirements-live"
NETEASE_WRAPPER = PROJECT / "service/evaluation/music_requirements_netease.mjs"
QT_BINARY = PROJECT / ".local/music-requirements-live-probe/music_requirements_live_probe"
PROBE_SOURCE = PROJECT / "tests/music_requirements_live_probe.cpp"
PROBE_PROJECT = PROJECT / "tests/music_requirements_live_probe.pro"
ENTRY = PROJECT / "scripts/check-music-requirements-live.py"
MAXIMUM = 80


def canonical_plan():
    """本项目公开开发验收题；不称独立盲测，不把静止界面当业务成功。"""
    tasks = [
        {"id":"MR01","title":"歌手加歌名","inputs":["帮我查找陈奕迅的十年"],
         "expect":{"kind":"song","artist":"陈奕迅","title":"十年","autoPlay":False}},
        {"id":"MR02","title":"普通歌手作品","inputs":["帮我找一些邓紫棋的歌曲"],
         "expect":{"kind":"artist_catalog","artist":"邓紫棋","autoPlay":False}},
        {"id":"MR03","title":"用户原始三条件","inputs":["我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。"],
         "expect":{"kind":"playlist","artist":"邓紫棋","scene":"散步","minutes":[30],"autoPlay":False}},
        {"id":"MR04","title":"多轮只修改时长","inputs":["散步听邓紫棋，安排30分钟。","改成20分钟，歌手和场景都不变"],
         "expect":{"kind":"playlist","artist":"邓紫棋","scene":"散步","minutes":[30,20],"sameTask":True,"autoPlay":False}},
        {"id":"MR05","title":"查现有歌单","inputs":["帮我找一些散步歌单"],
         "expect":{"kind":"playlist_search","scene":"散步","autoPlay":False}},
        {"id":"MR06","title":"自由作品说法","inputs":["给我来点周杰伦的作品，先列8首看看"],
         "expect":{"kind":"artist_catalog","artist":"周杰伦","limit":8,"autoPlay":False}},
        {"id":"MR07","title":"长句调换条件顺序","inputs":["时间控制在半小时，全部用邓紫棋的歌，我要边走路边听。"],
         "expect":{"kind":"playlist","artist":"邓紫棋","scene":"散步","minutes":[30],"autoPlay":False}},
        {"id":"MR08","title":"单曲自由表达与禁止播放","inputs":["搜一首陈奕迅唱的《十年》，暂时别播放"],
         "expect":{"kind":"song","artist":"陈奕迅","title":"十年","autoPlay":False}},
    ]
    return {"schemaVersion":1,"id":"tango-music-requirements-live-v1","requestedMaximum":MAXIMUM,
            "purpose":"exposed_music_requirements_acceptance","tasks":tasks,
            "scoringBoundary":"根据真实曲目ID、艺人署名、来源、实际总时长和本轮路由人工复核；settled不是业务通过，听感单独人工评价。",
            "automaticRetry":False,"modelJudge":False,"historicalUnusedBudgetCarries":False}


def plan_digest():
    """冻结题面包含判据，避免执行后改题取最好结果。"""
    return hashlib.sha256(encode(canonical_plan()).encode()).hexdigest()


def utc():
    """使用实际时间记录本次演示，不沿用历史评测的冻结日期。"""
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def frozen_sources():
    """复用正式源清单，追加本轮公开驱动、匿名适配包装器和真实Qt探针二进制。"""
    result = source_hashes(PROJECT)
    for path in (Path(__file__), NETEASE_WRAPPER, QT_BINARY, PROBE_SOURCE, PROBE_PROJECT, ENTRY,
                 PROJECT / "service/package.json", PROJECT / "service/package-lock.json",
                 PROJECT / "tests/routing_fixtures.h"):
        if path.is_symlink() or not path.is_file():
            raise CalibrationError("unsafe_or_missing_demo_source")
        result[path.relative_to(PROJECT).as_posix()] = digest(path)
    return result


def prepare():
    """先冻结可审阅计划；未授权状态不能启动真实模型服务。"""
    base = private_directory(BASE)
    plan = canonical_plan()
    if (plan.get("schemaVersion") != 1 or plan.get("id") != "tango-music-requirements-live-v1"
            or len(plan.get("tasks", [])) != 8 or plan.get("requestedMaximum") != MAXIMUM):
        raise CalibrationError("invalid_demo_plan")
    run = base / ("requirements-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
                  + "-" + secrets.token_hex(4))
    run.mkdir(mode=0o700)
    manifest = {
        "schemaVersion": 1, "purpose": "actual_qt_music_requirements_acceptance",
        "maximumProviderRequests": MAXIMUM,
        "authorization": None, "freshAuthorizationRequired": True,
        "historicalUnusedBudgetCarries": False, "automaticRetry": False,
        "planDigest": plan_digest(),
        "sources": frozen_sources(), "preparedAt": utc(),
        "storage": "owned_temporary_sqlite", "realCatalog": "qt_device_adapter",
        "limitations": ["不是生产MySQL/Redis或独立新题总体语义测评", "授权serve后只读QWEN配置，不读真实用户库或Cookie",
                        "未完成的付费请求不自动重放；目录禁止再次执行", "匿名曲库不能证明会员完整播放", "Cloud追踪显式关闭", "原生Qt测试驱动正式产品窗口；窗口销毁重建不等于App或后台进程强杀恢复", "本轮必须新授权，旧60/350/200等历史余量不沿用；只运行一次，不自动重跑"],
    }
    manifest["seal"] = hashlib.sha256(encode(manifest).encode()).hexdigest()
    write_new(run / "manifest.json", manifest)
    write_new(run / "plan.json", plan)
    return run


def verify(run, *, require_authorization=False):
    """源码或计划漂移先拒绝；启动前再核对本目录新授权。"""
    run = private_directory(run)
    if run.parent != BASE or not run.name.startswith("requirements-"):
        raise CalibrationError("invalid_owned_run_directory")
    manifest = read_json(run / "manifest.json")
    unsigned = {key: value for key, value in manifest.items() if key != "seal"}
    if (manifest.get("seal") != hashlib.sha256(encode(unsigned).encode()).hexdigest()
            or manifest.get("sources") != frozen_sources()
            or manifest.get("maximumProviderRequests") != MAXIMUM
            or manifest.get("planDigest") != plan_digest()
            or read_json(run / "plan.json") != canonical_plan()
            or manifest.get("authorization") is not None
            or manifest.get("freshAuthorizationRequired") is not True):
        raise CalibrationError("frozen_source_or_plan_changed")
    if require_authorization:
        if not (run / "authorization.json").is_file():
            raise CalibrationError("fresh_numeric_authorization_required")
        authorization = read_json(run / "authorization.json")
        maximum = authorization.get("maximumProviderRequests")
        if (authorization.get("manifestSeal") != manifest["seal"]
                or type(maximum) is not int or not 1 <= maximum <= MAXIMUM
                or authorization.get("newAuthorization") is not True
                or not isinstance(authorization.get("statement"), str)
                or not authorization["statement"].strip()
                or authorization.get("historicalUnusedBudgetCarries") is not False):
            raise CalibrationError("fresh_numeric_authorization_required")
        manifest = {**manifest, "authorizedMaximum": maximum,
                    "authorizationDigest": digest(run / "authorization.json")}
    return run, manifest


def authorize(path, maximum, statement):
    """只在用户新授权后手动调用；独占文件绑定本次冻结版本，不能增额或覆盖。"""
    if type(maximum) is not int or not 1 <= maximum <= MAXIMUM or not isinstance(statement, str) or not statement.strip():
        raise CalibrationError("fresh_numeric_authorization_required")
    run, manifest = verify(path)
    if (run / "execution-lock.json").exists():
        raise CalibrationError("executed_run_cannot_authorize_again")
    write_new(run / "authorization.json", {"schemaVersion": 1, "manifestSeal": manifest["seal"],
        "maximumProviderRequests": maximum, "newAuthorization": True, "statement": statement,
        "authorizedAt": utc(), "historicalUnusedBudgetCarries": False,
        "allRolesFailuresAndUnknownCount": True, "automaticRetry": False})
    return {"status": "new_authorization_recorded_no_model_calls", "maximumProviderRequests": maximum}


def launch_qt(path):
    """只使用本轮连接文件启动实际App；默认不继承模型、Cloud或数据库环境。"""
    run, manifest = verify(path, require_authorization=True)
    connection, netease = read_json(run / "connection.json"), read_json(run / "netease-connection.json")
    if (connection.get("manifestSeal") != manifest["seal"] or netease.get("manifestSeal") != manifest["seal"]
            or netease.get("anonymous") is not True or connection.get("dataDir") != str(run / "qt-data")):
        raise CalibrationError("owned_connection_mismatch")
    for document in (connection, netease):
        pid = document.get("pid")
        if type(pid) is not int or pid <= 1:
            raise CalibrationError("owned_service_pid_missing")
        os.kill(pid, 0)
    previous = sorted(run.glob("qt-process-*.json"))
    if previous:
        pid = read_json(previous[-1]).get("pid")
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise CalibrationError("previous_owned_qt_still_running")
    number = len(previous) + 1
    if number > 1:
        raise CalibrationError("probe_must_execute_only_once")
    allowed = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TMPDIR")
    environment = {key: os.environ[key] for key in allowed if key in os.environ}
    environment.update(connection["qtEnvironment"])
    environment.update(netease["qtEnvironment"])
    environment.update({"LANGSMITH_TRACING": "false", "LANGCHAIN_TRACING_V2": "false",
                        "MUSICPLAYER_REDIS_PORT": "invalid-isolated-demo"})
    with (run / ("qt-" + str(number) + ".log")).open("xb") as output:
        process = subprocess.Popen([str(QT_BINARY), "--output", str(run / "real-qt-observations.json")], cwd=PROJECT, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
    write_new(run / ("qt-process-" + str(number) + ".json"), {"pid": process.pid,
        "startedAt": utc(), "binarySha256": digest(QT_BINARY), "manifestSeal": manifest["seal"]})
    return {"status": "owned_real_catalog_qt_probe_started", "pid": process.pid, "launchNumber": number}


def launch_catalog(path):
    """只启动本轮匿名适配器；不复用16381服务、不读旧Cookie，且只允许启动一次。"""
    run, manifest = verify(path, require_authorization=True)
    connection = read_json(run / "connection.json")
    if connection.get("manifestSeal") != manifest["seal"] or connection.get("dataDir") != str(run / "qt-data"):
        raise CalibrationError("owned_connection_mismatch")
    if type(connection.get("pid")) is not int or connection["pid"] <= 1:
        raise CalibrationError("owned_service_pid_missing")
    os.kill(connection["pid"], 0)
    write_new(run / "catalog-launch-lock.json", {"at": utc(), "manifestSeal": manifest["seal"]})
    executable = shutil.which("node")
    if not executable:
        raise CalibrationError("node_executable_missing")
    allowed = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TMPDIR")
    environment = {key: os.environ[key] for key in allowed if key in os.environ}
    with (run / "catalog-process.log").open("xb") as output:
        process = subprocess.Popen([executable, str(NETEASE_WRAPPER), "--run", str(run)],
                                   cwd=PROJECT, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    write_new(run / "catalog-process.json", {"pid": process.pid, "startedAt": utc(),
              "manifestSeal": manifest["seal"]})
    return {"status": "owned_anonymous_catalog_starting", "pid": process.pid}


def write_token(path):
    """令牌只写当前私有目录的0600独占文件，不进入输出或启动参数。"""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(secrets.token_hex(32) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def summarized_requests(path):
    """结束时仅聚合调用状态和Token，缺少回执的尝试仍计数。"""
    rows = []
    if path.exists():
        with path.open() as stream:
            rows = [json.loads(line) for line in stream if line.strip()]
    starts = [row for row in rows if row["event"] == "started"]
    finishes = {row["number"]: row for row in rows if row["event"] == "finished"}
    statuses, roles, errors = Counter(), Counter(), Counter()
    usage = Counter()
    known_usage = 0
    for row in starts:
        finish = finishes.get(row["number"], {})
        statuses[finish.get("status", "unknown")] += 1
        roles[row["role"]] += 1
        if finish.get("errorCode"):
            errors[finish["errorCode"]] += 1
        if finish.get("usage"):
            known_usage += 1
            usage.update(finish["usage"])
    return {"started": len(starts), "outcomes": dict(statuses), "roles": dict(roles),
            "errorCodes": dict(errors), "usage": dict(usage),
            "usageKnownRequests": known_usage, "usageUnknownRequests": len(starts) - known_usage}


async def ready(server, base_url):
    """只向当前绑定端口检查健康与就绪；此检查没有模型或用户权限。"""
    import httpx
    async with httpx.AsyncClient(trust_env=False, timeout=1) as client:
        for _ in range(100):
            if server.started:
                try:
                    health = await client.get(base_url + "/health")
                    readiness = await client.get(base_url + "/ready")
                    h, r = health.json(), readiness.json()
                    if (health.status_code == 200 and readiness.status_code == 200
                            and h.get("app") == "quiet-app-backend"
                            and h.get("pid") == os.getpid()
                            and h.get("runtimeLanguage") == "python"
                            and h.get("storage") == "sqlite"
                            and h.get("developmentMode") is True
                            and h.get("routingFeaturesVersion") == 1
                            and h.get("modelRoutingVersion") == 1
                            and r.get("ready") is True):
                        return {"health": h, "ready": r}
                except (httpx.HTTPError, ValueError):
                    pass
            await asyncio.sleep(0.1)
    raise CalibrationError("isolated_service_not_ready")


class ContextASGI:
    """独立控制令牌只关联本轮题号，不授予产品权限，也不能调用模型或改变任务。"""

    def __init__(self, product, ledger, token, run):
        self.product, self.ledger, self.token, self.run = product, ledger, token, run
        self.sequence = [(task["id"], turn) for task in canonical_plan()["tasks"]
                         for turn in range(len(task["inputs"]) + 1)]
        self.position = 0

    async def __call__(self, scope, receive, send):
        if scope.get("type") != "http" or scope.get("path") != "/__music_requirements__/context":
            return await self.product(scope, receive, send)
        headers = dict(scope.get("headers", []))
        accepted = False
        if scope.get("method") == "POST" and hmac.compare_digest(
                headers.get(b"x-tango-evaluation-control", b""), self.token.encode()):
            body = b""
            while True:
                event = await receive()
                if event.get("type") != "http.request":
                    break
                body += event.get("body", b"")
                if len(body) > 4096:
                    break
                if not event.get("more_body", False):
                    try:
                        value = json.loads(body)
                        key = (value["caseId"], value["turn"])
                        accepted = (set(value) == {"caseId", "turn"} and type(value["turn"]) is int
                                    and self.position < len(self.sequence) and key == self.sequence[self.position])
                    except (ValueError, KeyError, TypeError):
                        accepted = False
                    break
            if accepted:
                self.position += 1
                self.ledger.context = {"caseId": key[0], "turn": key[1],
                                       "phase": "setup" if key[1] == 0 else "evaluation"}
                checkpoint(self.run / "active-context.json", self.ledger.context)
                self.ledger.append({"event": "context", **self.ledger.context,
                                    "startedBefore": self.ledger.started})
        payload = encode({"accepted": accepted}).encode()
        await send({"type": "http.response.start", "status": 200 if accepted else 403,
                    "headers": [(b"content-type", b"application/json"),
                                (b"content-length", str(len(payload)).encode())]})
        await send({"type": "http.response.body", "body": payload})


async def serve(path):
    """只有显式serve才加载QWEN配置，所有模型HTTP都复用同一RequestLedger。"""
    import uvicorn
    run, manifest = verify(path, require_authorization=True)
    write_new(run / "execution-lock.json", {"pid": os.getpid(), "startedAt": utc(),
                                           "manifestSeal": manifest["seal"]})
    maximum = manifest["authorizedMaximum"]
    ledger = RequestLedger(run, maximum)
    ledger.context = {"caseId": "actual_qt_desktop", "turn": 0}
    store, app, listener, server_task = None, None, None, None
    original_signals, received_signals = {}, []
    keys, error_code, status = [], None, "starting"
    started_at = time.monotonic()
    try:
        data_dir = private_directory(run / "qt-data")
        token_file = run / "development.token"
        write_token(token_file)
        control_token_file = run / "control.token"
        write_token(control_token_file)
        development = await developmentConfiguration(
            {"QUIET_DEVELOPMENT_MODE": "1", "QUIET_DEVELOPMENT_TOKEN_FILE": str(token_file)},
            {"host": "127.0.0.1"},
        )
        # 唯一读取模型私密配置的入口只提取QWEN_*，不加载server的存储profile。
        task_model, routing_model, keys, policy = live_models(ledger, int(time.time() * 1000))
        write_new(run / "model-policy.json", redact(policy, keys))
        store = Store(run / "owned.sqlite")
        with disable_automatic_tracing():
            app = createApp(
                store=store, model=task_model, taskModel=task_model, routingModel=routing_model,
                developmentSession=development, registrationMode="invite_only", tracing=AgentTracing(enabled=False),
                aiPerMinute=1000, aiDailyLimit=10000, aiGlobalDailyLimit=maximum,
                aiGlobalConcurrency=2, requestPerMinute=6000, userRequestPerMinute=4000,
                modelRouting={"version": 1, "strategy": policy.get("strategy", "flash_only"),
                              "selection": "server_policy"},
            )
        app = ContextASGI(app, ledger, control_token_file.read_text().strip(), run)
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(64)
        listener.setblocking(False)
        port = listener.getsockname()[1]
        base_url = "http://127.0.0.1:" + str(port)
        config = uvicorn.Config(
            app, host="127.0.0.1", port=port, http=BoundedH11Protocol,
            access_log=False, log_config=None, log_level="critical", proxy_headers=False,
            timeout_keep_alive=5, timeout_graceful_shutdown=5,
            limit_concurrency=100, h11_max_incomplete_event_size=16384,
        )
        server = uvicorn.Server(config)

        def stop(signum, frame):
            # Uvicorn结束时会重发收到的信号，接住该信号后才能可靠写完最终账本。
            received_signals.append(signum)
            server.should_exit = True

        for signum in (signal.SIGTERM, signal.SIGINT):
            original_signals[signum] = signal.signal(signum, stop)
        server_task = asyncio.create_task(server.serve(sockets=[listener]))
        checks = await ready(server, base_url)
        write_new(run / "ready.json", {**checks, "observedAt": utc()})
        write_new(run / "connection.json", {
            "schemaVersion": 1, "pid": os.getpid(), "backendURL": base_url,
            "tokenFile": str(token_file), "dataDir": str(data_dir),
            "controlTokenFile": str(control_token_file),
            "maximumProviderRequests": maximum, "manifestSeal": manifest["seal"],
            "neteaseServicePending": True,
            "qtEnvironment": {"MUSICPLAYER_BACKEND_URL": base_url,
                              "MUSICPLAYER_DEVELOPMENT_MODE": "1",
                              "MUSICPLAYER_DEVELOPMENT_TOKEN_FILE": str(token_file),
                              "MUSICPLAYER_DATA_DIR": str(data_dir),
                              "MUSICPLAYER_PROJECT_DIR": str(PROJECT)},
        })
        status = "ready"
        print(encode({"status": status, "connectionFile": str(run / "connection.json")}), flush=True)
        await server_task
        status = "stopped"
    except (KeyboardInterrupt, asyncio.CancelledError):
        status = "interrupted"
    except BaseException as error:
        candidate = str(error) if isinstance(error, CalibrationError) else getattr(error, "code", "DESKTOP_DEMO_FAILED")
        error_code = candidate if re.fullmatch(r"[A-Za-z_]{1,80}", candidate or "") else "DESKTOP_DEMO_FAILED"
        status = "failed"
    finally:
        if server_task and not server_task.done():
            server.should_exit = True
            try:
                await asyncio.wait_for(server_task, timeout=12)
            except BaseException:
                server_task.cancel()
                await asyncio.gather(server_task, return_exceptions=True)
        if listener:
            listener.close()
        if store:
            await store.close()
        ledger.close()
        calls = summarized_requests(run / "provider-requests.jsonl")
        checkpoint(run / "report.json", {
            "schemaVersion": 1, "purpose": manifest["purpose"], "status": status,
            "errorCode": error_code, "finishedAt": utc(), "seconds": round(time.monotonic() - started_at, 3),
            "receivedSignals": received_signals,
            "manifestSeal": manifest["seal"], "sources": manifest["sources"],
            "sourceUnchangedAtShutdown": manifest["sources"] == frozen_sources(),
            "maximumProviderRequests": maximum, "authorizationDigest": manifest["authorizationDigest"],
            "requests": calls,
            "remainingAfterThisRun": maximum - calls["started"],
            "historicalUnusedBudgetCarries": False, "cloudTraceRequests": 0,
            "publicBudgetLedgerAutomaticallyModified": False,
            "limits": manifest["limitations"], "productOverallCompletionRate": None,
        })
        for signum, handler in original_signals.items():
            signal.signal(signum, handler)
    return 1 if status == "failed" else 0


def main(argv=None):
    """默认只展示命令，避免误运行即开始真实模型服务。"""
    os.umask(0o077)
    # 上游错误正文与完整聊天不进入控制台；模型尝试账本只含固定元数据。
    logging.disable(logging.CRITICAL)
    parser = argparse.ArgumentParser(description="隔离Qt真实演示：prepare不付费，serve须本轮新授权且最多80次")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("prepare")
    service = commands.add_parser("serve")
    service.add_argument("--run", type=Path, required=True)
    authorization = commands.add_parser("authorize")
    authorization.add_argument("--run", type=Path, required=True)
    authorization.add_argument("--maximum", type=int, required=True)
    authorization.add_argument("--statement", required=True)
    launch = commands.add_parser("launch-probe")
    launch.add_argument("--run", type=Path, required=True)
    catalog = commands.add_parser("launch-catalog")
    catalog.add_argument("--run", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            run = prepare()
            print(encode({"status": "prepared_no_model_calls", "runDirectory": str(run)}), flush=True)
            return 0
        if args.command == "authorize":
            print(encode(authorize(args.run, args.maximum, args.statement)), flush=True)
            return 0
        if args.command == "launch-probe":
            print(encode(launch_qt(args.run)), flush=True)
            return 0
        if args.command == "launch-catalog":
            print(encode(launch_catalog(args.run)), flush=True)
            return 0
        return asyncio.run(serve(args.run))
    except BaseException as error:
        code = str(error) if isinstance(error, CalibrationError) else "DESKTOP_DEMO_PREPARATION_FAILED"
        print(encode({"status": "failed", "errorCode": code if re.fullmatch(r"[a-zA-Z_]{1,80}", code) else "DESKTOP_DEMO_PREPARATION_FAILED"}), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
