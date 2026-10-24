"""会话开发校准：冻结、隔离执行、独立判分与人工复核；默认完全离线。"""

import argparse
import asyncio
from collections import Counter
import csv
from datetime import datetime, timezone
import hashlib
import html
import json
import math
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import time
from uuid import uuid4

import httpx

from tango_backend.app import createApp
from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.store import Store
from .scripted import ScriptedModel, controlled_catalog


PROJECT = Path(__file__).resolve().parents[2]
SUITE = "docs/evaluation-data/conversation-calibration-v1.json"
TERMINAL = {"preview", "needs_clarification", "failed", "cancelled", "paused", "budget_exhausted", "adopted", "awaiting_apply"}


class CalibrationError(ValueError):
    """固定原因码，不携带密钥、供应商正文或真实用户路径。"""


def encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def sha(value):
    return hashlib.sha256(value).hexdigest()


def read_json(path):
    if path.is_symlink() or path.stat().st_size > 4 * 1024 * 1024:
        raise CalibrationError("unsafe_json_file")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise CalibrationError("duplicate_json_key")
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(CalibrationError("invalid_json_number")))


def write_new(path, value):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags, 0o600), "w") as stream:
        stream.write(encode(value) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def checkpoint(path, value):
    temporary = path.with_name(path.name + "." + secrets.token_hex(6))
    write_new(temporary, value)
    if path.is_symlink():
        temporary.unlink()
        raise CalibrationError("checkpoint_symlink_rejected")
    os.replace(temporary, path)


def private_directory(path):
    path = Path(path).absolute()
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise CalibrationError("directory_symlink_rejected")
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    details = path.stat()
    if not stat.S_ISDIR(details.st_mode) or details.st_uid != os.getuid() or details.st_mode & 0o077:
        raise CalibrationError("private_directory_required")
    return path


def source_hashes(project=PROJECT):
    paths = [project / SUITE, project / "scripts/calibrate-agent.py", project / "backend_py/pyproject.toml", project / "backend_py/uv.lock"]
    paths += list((project / "backend_py/tango_backend").rglob("*.py"))
    paths += list((project / "backend_py/evaluation").rglob("*.py"))
    result = {}
    for path in sorted(paths):
        if not path.is_file() or path.is_symlink() or path.stat().st_nlink != 1:
            raise CalibrationError("unsafe_source_file")
        result[path.relative_to(project).as_posix()] = sha(path.read_bytes())
    return result


def request_estimate(suite):
    """按现有每轮六次任务上限估算；实际错误分流仍受独立 HTTP 硬上限约束。"""
    from tango_backend.intent_routing import deterministicRoute
    routes = sum(deterministicRoute({"instruction": t["text"], "task": None}) is None
                 for case in suite["cases"] for t in case["turns"])
    runs = sum(any(a[0] == "music.task" or a == ["task.control", "resume"] for a in t["expect"]["actions"])
               for case in suite["cases"] for t in case["turns"])
    return {"expectedRouteUpper": routes, "taskRunsIncludingResume": runs,
            "perTaskRunMaximum": 6, "expectedPathRequestUpper": routes + runs * 6,
            "note": "按预期分流估算，实际执行以调用前持久计数的硬上限为准；不是费用报价。"}


def validate_suite(suite):
    if suite.get("schemaVersion") != 1 or suite.get("purpose") != "exposed_development_calibration":
        raise CalibrationError("invalid_suite_scope")
    cases = suite.get("cases")
    if not isinstance(cases, list) or len(cases) != 10:
        raise CalibrationError("expected_ten_development_cases")
    ids = [case.get("id") for case in cases]
    if len(set(ids)) != len(ids) or any(not isinstance(v, str) or not re.fullmatch(r"[a-z_]{1,40}", v) for v in ids):
        raise CalibrationError("invalid_case_ids")
    for case in cases:
        if not isinstance(case.get("turns"), list) or not 1 <= len(case["turns"]) <= 6:
            raise CalibrationError("invalid_turn_count")
        for turn in case["turns"]:
            if not isinstance(turn.get("text"), str) or not 0 < len(turn["text"]) <= 1000:
                raise CalibrationError("invalid_turn_input")
            gold = turn.get("expect", {})
            if not gold.get("intents") or not gold.get("actions") or "taskStatus" not in gold:
                raise CalibrationError("missing_gold")
    return suite


def prepare(parent, *, mode="offline", maximum=0, authorization="", project=PROJECT):
    suite = validate_suite(read_json(project / SUITE))
    if mode not in ("offline", "live") or type(maximum) is not int or maximum < 0:
        raise CalibrationError("invalid_run_mode_or_limit")
    if mode == "offline" and maximum != 0 or mode == "live" and (maximum <= 0 or not authorization.strip()):
        raise CalibrationError("explicit_live_budget_authorization_required")
    parent = private_directory(parent)
    run = parent / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(4))
    run.mkdir(mode=0o700)
    manifest = {"schemaVersion": 1, "mode": mode, "suiteId": suite["id"], "purpose": suite["purpose"],
                "suiteDigest": sha(encode(suite).encode()),
                "caseIds": [case["id"] for case in suite["cases"]], "turns": sum(len(case["turns"]) for case in suite["cases"]),
                "maximumProviderRequests": maximum, "authorization": authorization,
                "requestEstimate": request_estimate(suite),
                "sources": source_hashes(project), "preparedAt": datetime.now(timezone.utc).isoformat()}
    manifest["seal"] = sha(encode(manifest).encode())
    write_new(run / "manifest.json", manifest)
    write_new(run / "suite.json", suite)
    write_new(run / "progress.json", {"observations": {}, "status": "prepared"})
    return run


def verify_manifest(run, project=PROJECT, *, check_sources=True):
    run = private_directory(run)
    value = read_json(run / "manifest.json")
    unsigned = {k: v for k, v in value.items() if k != "seal"}
    if value.get("seal") != sha(encode(unsigned).encode()) or check_sources and value.get("sources") != source_hashes(project):
        raise CalibrationError("frozen_source_or_manifest_changed")
    suite = validate_suite(read_json(run / "suite.json"))
    if value.get("suiteDigest") != sha(encode(suite).encode()):
        raise CalibrationError("frozen_suite_changed")
    if value.get("caseIds") != [case["id"] for case in suite["cases"]]:
        raise CalibrationError("frozen_selection_changed")
    return value, suite


def redact(value, keys=()):
    text = encode(value)
    for key in keys:
        if key:
            text = text.replace(key, "[已脱敏]").replace(json.dumps(key, ensure_ascii=True)[1:-1], "[已脱敏]")
    text = re.sub(r"sk-[A-Za-z0-9_-]{8,}", "[已脱敏]", text)
    return json.loads(text)


class RequestLedger:
    """调用前 fsync 记录；失败和未知消耗上限，禁止从旧目录继续发起。"""

    def __init__(self, run, maximum):
        self.path, self.maximum, self.started = run / "provider-requests.jsonl", maximum, 0
        self.context = {"caseId": "", "turn": 0}
        self.file = os.fdopen(os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600), "w")
        descriptor = os.open(run, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def append(self, value):
        self.file.write(encode(value) + "\n")
        self.file.flush()
        os.fsync(self.file.fileno())

    def begin(self, role):
        if self.started >= self.maximum:
            raise ApiError(429, "CALIBRATION_REQUEST_LIMIT", "本次校准请求上限已达到。")
        self.started += 1
        self.append({"event": "started", "number": self.started, "role": role, **self.context})
        return self.started

    def close(self):
        self.file.close()


class CountedTransport:
    """包裹已有模型适配器，所有 HTTP 尝试共同计数，绝不另建供应商客户端。"""

    def __init__(self, delegate, ledger, role):
        self.delegate, self.ledger, self.role = delegate, ledger, role

    def __getattr__(self, name):
        return getattr(self.delegate, name)

    async def complete(self, *args, **kwargs):
        number, started = self.ledger.begin(self.role), time.monotonic()
        try:
            value = await self.delegate.complete(*args, **kwargs)
        except BaseException as error:
            code = getattr(error, "code", "TRANSPORT_FAILED")
            if not isinstance(code, str) or not re.fullmatch(r"[A-Z_]{1,60}", code):
                code = "TRANSPORT_FAILED"
            self.ledger.append({"event": "finished", "number": number, "status": "uncertain" if code in ("MODEL_TIMEOUT", "REQUEST_CANCELLED") or isinstance(error, asyncio.CancelledError) else "failed", "errorCode": code,
                                "seconds": round(time.monotonic() - started, 3)})
            raise
        usage = value.get("usage")
        usage = {key: usage[key] for key in ("prompt_tokens", "completion_tokens", "total_tokens")
                 if key in usage and type(usage[key]) is int and usage[key] >= 0} if isinstance(usage, dict) else None
        self.ledger.append({"event": "finished", "number": number, "status": "responded", "usage": usage or None,
                            "seconds": round(time.monotonic() - started, 3)})
        return value


def live_models(ledger, now):
    """仅真实运行入口读取后台模型配置，不采用数据库、Cookie 或其他环境设置。"""
    from dotenv import dotenv_values
    from tango_backend.model_profiles import modelProfilesConfiguration
    from tango_backend.task_model import createTaskModel
    from tango_backend.intent_routing import createRoutingModel
    values = {k: v for k, v in dotenv_values(PROJECT / "backend_py/.env", interpolate=False).items() if k.startswith("QWEN_")}
    configuration = modelProfilesConfiguration(values)
    if not configuration["profiles"]["flash"].configured:
        raise CalibrationError("live_model_not_configured")
    task = createTaskModel(modelProfiles=configuration, now=lambda: now)
    route = createRoutingModel(modelProfiles=configuration)
    task.transports = {name: CountedTransport(transport, ledger, "task:" + name) for name, transport in task.transports.items()}
    task.transport = task.transports["flash"]
    route.transport = CountedTransport(route.transport, ledger, "route:flash")
    keys = list(dict.fromkeys(profile.apiKey for profile in configuration["profiles"].values() if profile.apiKey))
    return task, route, keys, task.policySnapshot()


class Driver:
    """真实 ASGI 接口的无界面调用者；偏好确认和播放器仍需 Qt 人工验收。"""

    def __init__(self, directory, model, routing_model, now):
        self.directory, self.model, self.routing_model, self.now = directory, model, routing_model, now
        self.token, self.cid, self.task, self.preference = None, None, None, ""
        self.tools, self.truth, self.playlists = [], {}, []

    async def open(self):
        self.store = Store(self.directory / "owned.sqlite")
        self.app = createApp(store=self.store, model=self.model, taskModel=self.model, routingModel=self.routing_model,
                             now=lambda: self.now, registrationMode="open", aiPerMinute=100,
                             requestPerMinute=20000, userRequestPerMinute=20000)
        await self.app.initialize()
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=self.app), base_url="http://127.0.0.1")
        if self.token:
            self.client.headers["authorization"] = "Bearer " + self.token
        else:
            session = await self.request("POST", "/v1/auth/register", {"username": "calibration-user", "password": secrets.token_urlsafe(24)})
            self.token = session["token"]
            self.client.headers["authorization"] = "Bearer " + self.token
            response = await self.request("POST", "/v1/conversations", {"deviceId": "calibration-device", "requestId": str(uuid4())})
            self.cid = response["conversation"]["id"]

    async def close(self):
        if getattr(self, "client", None):
            await self.client.aclose()
        if getattr(self, "app", None):
            await self.app.close()
        if getattr(self, "store", None):
            await self.store.close()

    async def request(self, method, path, body=None):
        response = await self.client.request(method, path, **({"json": body} if body is not None else {}))
        value = response.json()
        if response.status_code >= 400:
            code = value.get("error", {}).get("code", "HTTP_ERROR")
            if not isinstance(code, str) or not re.fullmatch(r"[A-Z_]{1,60}", code):
                code = "HTTP_ERROR"
            raise CalibrationError(code)
        return value

    async def action(self, action, **extra):
        body = {"deviceId": "calibration-device", "requestId": str(uuid4()),
                "expectedRevision": self.task["revision"], "action": action, **extra}
        self.task = (await self.request("POST", "/v1/tasks/" + self.task["id"] + "/actions", body))["task"]

    async def settle(self, turn, index):
        started, seen = time.monotonic(), set()
        while time.monotonic() - started < 180:
            self.task = (await self.request("GET", "/v1/tasks/" + self.task["id"]))["task"]
            if self.task["status"] in TERMINAL:
                return
            for step in self.task.get("pendingSteps", []):
                if step["id"] in seen:
                    continue
                seen.add(step["id"])
                result = controlled_catalog(step, self.now, index, "calibration-device", turn.get("catalogFault", False))
                self.tools.append({"skill": step["skill"], "input": clone(step["input"]), "result": clone(result)})
                for track in result.get("result", {}).get("tracks", []):
                    self.truth[track["id"]] = track
                await self.request("POST", "/v1/tasks/" + self.task["id"] + "/steps/" + step["id"] + "/result", result)
            await asyncio.sleep(.02 if isinstance(self.model, ScriptedModel) else .25)
        raise CalibrationError("TASK_WAIT_TIMEOUT")

    async def turn(self, turn, index):
        self.partial = {}
        if turn.get("lockFirst"):
            await self.action("lock", lockedIds=[t["id"] for t in self.task["preview"]["tracks"][:turn["lockFirst"]]])
        before, tool_start = clone(self.task), len(self.tools)
        mid = str(uuid4())
        base = "/v1/conversations/" + self.cid
        message = {"deviceId": "calibration-device", "messageId": mid, "content": turn["text"]}
        await self.request("POST", base + "/messages", message)
        result = await self.request("POST", base + "/messages/" + mid + "/route", {"deviceId": "calibration-device", "preference": self.preference})
        route = result["route"]
        self.partial["route"] = clone(route)
        executed = set()
        for action in (route.get("result") or {}).get("actions", []):
            if any(v not in executed for v in action["dependsOn"]):
                continue
            cap, params = action["capability"], action["parameters"]
            if cap == "memory.propose":
                # 本轮只模拟拒绝提议；不把无界面动作当成 Qt 偏好写入证据。
                continue
            if cap == "music.task":
                if params["mode"] == "new":
                    self.task = (await self.request("POST", "/v1/tasks", {"deviceId": "calibration-device", "requestId": mid,
                                 "instruction": turn["text"], "preference": self.preference}))["task"]
                else:
                    await self.action("turn", requestId=mid, instruction=turn["text"], preference=self.preference)
                await self.settle(turn, index)
            elif cap == "task.control":
                await self.action(params["action"])
                if params["action"] == "resume":
                    await self.settle(turn, index)
            executed.add(action["id"])
        if self.task and any(a["capability"] == "music.task" for a in (route.get("result") or {}).get("actions", [])):
            await self.request("POST", base + "/events", {"deviceId": "calibration-device", "eventId": str(uuid4()), "turnId": mid,
                               "kind": "task_sync", "taskId": self.task["id"], "taskRevision": self.task["revision"]})
        usage_before = (await self.request("GET", "/v1/account/usage"))["total"]["used"]
        await self.request("POST", base + "/messages", message)
        await self.request("GET", base + "/messages/" + mid + "/route")
        if self.task:
            await self.request("GET", "/v1/tasks/" + self.task["id"])
        usage_after = (await self.request("GET", "/v1/account/usage"))["total"]["used"]
        restored_without_calls = None
        if turn.get("restartAfter"):
            await self.close()
            await self.open()
            await self.request("GET", base)
            self.task = (await self.request("GET", "/v1/tasks/" + self.task["id"]))["task"]
            restored_without_calls = (await self.request("GET", "/v1/account/usage"))["total"]["used"] == usage_after
        return {"route": route, "task": clone(self.task), "before": before, "tools": self.tools[tool_start:],
                "truth": {str(k): clone(v) for k, v in self.truth.items()}, "readbackNoCalls": usage_before == usage_after,
                "restartNoCalls": restored_without_calls, "preference": self.preference}


def checks(turn, observation):
    """期望来自预写金标，元数据来自固定候选，不相信模型自报的 satisfied。"""
    gold = turn["expect"]
    values = []
    def add(name, condition, applicable=True):
        values.append({"name": name, "status": "unverified" if observation is None else "passed" if condition else "failed"} if applicable else {"name": name, "status": "not_applicable"})
    result = ((observation or {}).get("route") or {}).get("result") or {}
    task = (observation or {}).get("task") or {}
    actions = result.get("actions", [])
    add("route_completed", (observation or {}).get("route", {}).get("status") == "completed")
    add("intent_exact_set", any(set(result.get("intents", [])) == set(v) for v in gold["intents"]))
    expected_actions = Counter(tuple(v) for v in gold["actions"])
    actual_actions = Counter((a["capability"], next((a["parameters"][k] for k in ("mode", "operation", "action", "kind") if k in a["parameters"]), "")) for a in actions)
    add("action_exact_set", actual_actions == expected_actions)
    for key, value in gold.get("slots", {}).items():
        add("slot:" + key, (result.get("slots") or {}).get(key) == value)
    add("task_state", task.get("status") == gold["taskStatus"])
    for key, value in gold.get("brief", {}).items():
        add("brief:" + key, (task.get("brief") or {}).get(key) == value)
    for key, aliases in gold.get("concepts", []):
        text = encode((task.get("brief") or {}).get(key, "")).lower()
        add("brief_concept:" + key + ":" + aliases[0], any(alias.lower() in text for alias in aliases))
    for aliases in gold.get("queries", []):
        text = encode([s["input"].get("queries", []) for s in (observation or {}).get("tools", [])]).lower()
        add("tool_query:" + aliases[0], any(alias.lower() in text for alias in aliases))
    if gold.get("missingSlot"):
        add("necessary_clarification", any(s.get("slot") == gold["missingSlot"] for s in result.get("missingSlots", [])))
    tracks = (task.get("preview") or {}).get("tracks", [])
    if gold.get("minutes"):
        total = sum(t.get("durationMs", 0) for t in tracks)
        target = gold["minutes"] * 60000
        add("duration", bool(tracks) and all(type(t.get("durationMs")) is int and t["durationMs"] > 0 for t in tracks) and abs(total - target) <= max(60000, target * .1))
    if gold.get("taskError"):
        add("honest_catalog_failure", (task.get("error") or {}).get("code") == gold["taskError"])
    if gold.get("independentMusic"):
        add("music_independent_of_memory", all(not a["dependsOn"] for a in actions if a["capability"] == "music.task"))
    before = (observation or {}).get("before") or {}
    old_ids = [t["id"] for t in (before.get("preview") or {}).get("tracks", [])]
    new_ids = [t["id"] for t in tracks]
    locked = before.get("lockedIds", [])
    if gold.get("retainLocked"):
        add("locked_tracks_retained", bool(locked) and all(v in new_ids for v in locked))
    if gold.get("changeUnlocked"):
        add("unlocked_tracks_changed", bool(new_ids) and set(new_ids) - set(locked) != set(old_ids) - set(locked))
    if gold.get("samePlaylist"):
        add("playlist_unchanged", bool(old_ids) and old_ids == new_ids)
    if gold.get("undoPrevious"):
        add("undo_restores_previous", new_ids == (observation or {}).get("undoExpectedIds"))
    truth = (observation or {}).get("truth", {})
    add("grounded_metadata", all(str(t["id"]) in truth and all(t.get(k) == truth[str(t["id"])].get(k) for k in ("title", "artist", "durationMs")) for t in tracks))
    add("unique_tracks", len(new_ids) == len(set(new_ids)))
    if gold.get("brief", {}).get("excludeLive"):
        add("synthetic_live_exclusion", all("Live" not in t["title"] for t in tracks))
    if gold.get("releaseWindowDays"):
        brief = task.get("brief") or {}
        try:
            days = (datetime.fromisoformat(brief["until"].replace("Z", "+00:00")) - datetime.fromisoformat(brief["since"].replace("Z", "+00:00"))).total_seconds() / 86400
        except (KeyError, TypeError, ValueError):
            days = -1
        add("release_window", abs(days - gold["releaseWindowDays"]) <= 1)
    add("no_automatic_adoption", task.get("adoption") is None and task.get("status") not in ("adopted", "awaiting_apply"))
    add("readback_no_new_calls", (observation or {}).get("readbackNoCalls") is True)
    if turn.get("restartAfter"):
        add("restart_no_new_calls", (observation or {}).get("restartNoCalls") is True)
    return values


def summarize(suite, observations, mode):
    rows, intent_pass, turn_count = [], 0, 0
    for case in suite["cases"]:
        turns = []
        for index, turn in enumerate(case["turns"]):
            observation = observations.get(case["id"], {}).get(str(index))
            grade = checks(turn, observation)
            intent_pass += any(g["name"] == "intent_exact_set" and g["status"] == "passed" for g in grade)
            turn_count += 1
            state = "failed" if any(g["status"] == "failed" for g in grade) else "unverified" if any(g["status"] == "unverified" for g in grade) else "passed"
            turns.append({"turn": index + 1, "text": turn["text"], "status": state, "checks": grade,
                          "error": (observation or {}).get("error")})
        state = "failed" if any(t["status"] == "failed" for t in turns) else "unverified" if any(t["status"] == "unverified" for t in turns) else "passed"
        rows.append({"id": case["id"], "title": case["title"], "group": case["group"], "status": state, "turns": turns})
    passed = sum(row["status"] == "passed" for row in rows)
    n, z = len(rows), 1.959963984540054
    p = passed / n
    center = (p + z*z/(2*n))/(1+z*z/n)
    half = z*math.sqrt(p*(1-p)/n+z*z/(4*n*n))/(1+z*z/n)
    return {"mode": mode, "purpose": "exposed_development_calibration", "selectedTasks": n, "selectedTurns": turn_count,
            "counts": dict(Counter(row["status"] for row in rows)), "cases": rows,
            "fixtureTaskPassRate": passed/n if mode == "offline" else None,
            "backendAcceptanceRate": passed/n if mode == "live" else None,
            "controlledTaskCompletionRate": None,
            "intentExactSetRate": intent_pass/turn_count if mode == "live" else None,
            "descriptiveWilson95": [center-half, center+half] if mode == "live" else None,
            "byGroup": {group: dict(Counter(row["status"] for row in rows if row["group"] == group)) for group in sorted({row["group"] for row in rows})},
            "productOverallCompletionRate": None, "recommendationQuality": None, "humanReview": "pending",
            "limits": ["已公开开发题，不是独立新题或真实流量抽样。", "合成曲库不能证明真实歌曲召回、发行事实或听感。", "ASGI接口与隔离SQLite不代替Qt确认/播放及生产MySQL并发验收。", "通过与否来自预设有限检查，置信区间仅为诊断集描述。"]}


def write_review(run, report, observations):
    path = run / "human-review.csv"
    if not path.exists():
        with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["manifestSeal", "caseId", "turn", "instruction", "understanding_1_to_5", "interaction_1_to_5", "verdict_pass_fail_uncertain", "notes"])
            for case in report["cases"]:
                for turn in case["turns"]:
                    writer.writerow([report["manifestSeal"], case["id"], turn["turn"], turn["text"], "", "", "", ""])
    parts = ["<!doctype html><meta charset='utf-8'><title>探歌开发校准</title><style>body{font:16px/1.6 system-ui;max-width:980px;margin:40px auto;padding:0 24px;color:#17251f}pre{white-space:pre-wrap;background:#f2f5f3;padding:16px;border-radius:8px}h2{margin-top:40px}</style>",
             "<h1>探歌对话开发校准</h1><p>模式：" + html.escape(report["mode"]) + "。离线替身结果不代表模型效果；推荐听感、Qt 确认和真实播放尚未验收。</p>"]
    for case in report["cases"]:
        parts.append("<h2>" + html.escape(case["title"]) + " · " + case["status"] + "</h2>")
        for turn in case["turns"]:
            observation = observations.get(case["id"], {}).get(str(turn["turn"]-1)) or {}
            detail = {"route": observation.get("route"), "task": {k: (observation.get("task") or {}).get(k) for k in ("status", "brief", "preview", "error")},
                      "failedChecks": [g for g in turn["checks"] if g["status"] != "passed"]}
            parts.append("<p>" + html.escape(turn["text"]) + "</p><pre>" + html.escape(json.dumps(detail, ensure_ascii=False, indent=2)) + "</pre>")
    with os.fdopen(os.open(run / "review.html", os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600), "w") as stream:
        stream.write("\n".join(parts))


def import_review(run, path):
    """人工表与运行清单绑定；保留未知、重复和缺失，评分不改变自动验收记录。"""
    manifest, suite = verify_manifest(run, check_sources=False)
    if path.suffix.lower() != ".csv" or path.is_symlink() or path.stat().st_size > 1024 * 1024:
        raise CalibrationError("unsafe_review_file")
    expected = {(case["id"], str(index+1)): turn["text"] for case in suite["cases"] for index, turn in enumerate(case["turns"])}
    reviews, seen = [], set()
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            key = row.get("caseId"), row.get("turn")
            if key in seen or key not in expected or row.get("manifestSeal") != manifest["seal"] or row.get("instruction") != expected[key]:
                raise CalibrationError("review_selection_mismatch")
            seen.add(key)
            grades = [row.get("understanding_1_to_5", "").strip(), row.get("interaction_1_to_5", "").strip()]
            verdict = row.get("verdict_pass_fail_uncertain", "").strip()
            if any(v and v not in ("1", "2", "3", "4", "5") for v in grades) or verdict not in ("", "pass", "fail", "uncertain"):
                raise CalibrationError("invalid_human_grade")
            if verdict in ("pass", "fail") and any(not v for v in grades):
                raise CalibrationError("incomplete_human_grade")
            reviews.append({"caseId": key[0], "turn": int(key[1]), "understanding": int(grades[0]) if grades[0] else None,
                            "interaction": int(grades[1]) if grades[1] else None, "verdict": verdict or "pending",
                            "notes": row.get("notes", "")[:2000]})
    if seen != set(expected):
        raise CalibrationError("review_rows_missing")
    result = {"manifestSeal": manifest["seal"], "reviewerCount": 1, "mode": manifest["mode"],
              "counts": dict(Counter(r["verdict"] for r in reviews)), "reviews": reviews,
              "scope": "单人理解与交互复核；离线替身不产生真实模型评分，不代表真实推荐听感。"}
    write_new(run / ("human-review-" + secrets.token_hex(4) + ".json"), result)
    return {k: result[k] for k in ("mode", "reviewerCount", "counts", "scope")}


async def execute(run, *, project=PROJECT):
    manifest, suite = verify_manifest(run, project)
    write_new(run / "execution-lock.json", {"startedAt": datetime.now(timezone.utc).isoformat(), "pid": os.getpid()})
    ledger = RequestLedger(run, manifest["maximumProviderRequests"])
    now = int(datetime.fromisoformat(suite["clock"].replace("Z", "+00:00")).timestamp()*1000)
    observations, keys, model_snapshot, run_error = {}, [], None, None
    try:
        if manifest["mode"] == "live":
            task_model, routing_model, keys, model_snapshot = live_models(ledger, now)
            write_new(run / "model-policy.json", redact(model_snapshot, keys))
        for case in suite["cases"]:
            if manifest["mode"] == "live" and ledger.started >= ledger.maximum:
                break
            directory = run / case["id"]
            directory.mkdir(mode=0o700)
            if manifest["mode"] == "offline":
                task_model = routing_model = ScriptedModel(case["id"], now)
            driver = Driver(directory, task_model, routing_model, now)
            try:
                await driver.open()
                original_ids = None
                for index, turn in enumerate(case["turns"]):
                    ledger.context = {"caseId": case["id"], "turn": index+1}
                    if isinstance(task_model, ScriptedModel):
                        task_model.turn_index = index
                    started = time.monotonic()
                    try:
                        observation = await driver.turn(turn, index)
                        if index == 0 and driver.task:
                            original_ids = [t["id"] for t in (driver.task.get("preview") or {}).get("tracks", [])]
                        if turn["expect"].get("undoPrevious"):
                            observation["undoExpectedIds"] = original_ids
                        observation["seconds"] = round(time.monotonic() - started, 3)
                        observations.setdefault(case["id"], {})[str(index)] = redact(observation, keys)
                    except CalibrationError as error:
                        observations.setdefault(case["id"], {})[str(index)] = redact({**getattr(driver, "partial", {}),
                            "task": driver.task, "error": str(error), "seconds": round(time.monotonic()-started, 3)}, keys)
                        break
                    finally:
                        checkpoint(run / "progress.json", {"status": "running", "observations": observations})
            finally:
                await driver.close()
    except (Exception, KeyboardInterrupt, asyncio.CancelledError) as error:
        # 未知异常只记录类型；付费请求的 started 记录保留，不从旧目录续跑。
        run_error = str(error) if isinstance(error, CalibrationError) else type(error).__name__
    finally:
        ledger.close()
        checkpoint(run / "progress.json", {"status": "stopped", "observations": observations})
    report = summarize(suite, observations, manifest["mode"])
    report.update(manifestSeal=manifest["seal"], providerRequests=ledger.started, modelSnapshot=redact(model_snapshot, keys),
                  runError=run_error)
    times = sorted(o["seconds"] for turns in observations.values() for o in turns.values() if "seconds" in o)
    events = [json.loads(line) for line in (run / "provider-requests.jsonl").read_text().splitlines()]
    finished = {v["number"]: v for v in events if v["event"] == "finished"}
    report["providerOutcomes"] = dict(Counter(finished.get(n, {}).get("status", "uncertain") for n in range(1, ledger.started+1)))
    known_usage = [v["usage"] for v in finished.values() if v.get("usage") and all(k in v["usage"] for k in ("prompt_tokens", "completion_tokens"))]
    report["tokens"] = {"knownRequests": len(known_usage), "unknownRequests": ledger.started-len(known_usage),
                        "inputTokens": sum(v["prompt_tokens"] for v in known_usage) if len(known_usage) == ledger.started else None,
                        "outputTokens": sum(v["completion_tokens"] for v in known_usage) if len(known_usage) == ledger.started else None,
                        "verifiedCost": None}
    report["latencySeconds"] = {"observedTurns": len(times), "p50": times[math.ceil(len(times)*.5)-1] if times else None,
                                "p95": times[math.ceil(len(times)*.95)-1] if times else None,
                                "scope": "同机ASGI与固定曲库；不是生产延迟。"}
    try:
        verify_manifest(run, project)
    except CalibrationError:
        report.update(invalidReason="source_changed_during_run", controlledTaskCompletionRate=None, backendAcceptanceRate=None, intentExactSetRate=None,
                      fixtureTaskPassRate=None, descriptiveWilson95=None)
    write_new(run / "report.json", report)
    write_review(run, report, observations)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "run", "score", "review"))
    parser.add_argument("--run", type=Path)
    parser.add_argument("--mode", choices=("offline", "live"), default="offline")
    parser.add_argument("--max-provider-requests", type=int, default=0)
    parser.add_argument("--authorization", default="", help="仅记录用户明确授权引用；参数本身不构成授权。")
    parser.add_argument("--review-csv", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            run = prepare(PROJECT / ".local/calibration-runs", mode=args.mode, maximum=args.max_provider_requests, authorization=args.authorization)
            print(json.dumps({"run": str(run), "tasks": 10, "mode": args.mode}, ensure_ascii=False))
            return 0
        if not args.run:
            raise CalibrationError("run_directory_required")
        run = private_directory(args.run)
        if args.command == "run":
            manifest, _ = verify_manifest(run)
            if args.mode != manifest["mode"]:
                raise CalibrationError("explicit_execution_mode_required")
            report = asyncio.run(execute(run))
        elif args.command == "review":
            if not args.review_csv:
                raise CalibrationError("review_csv_required")
            result = import_review(run, args.review_csv)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        else:
            manifest, suite = verify_manifest(run)
            observations = read_json(run / "progress.json")["observations"]
            report = summarize(suite, observations, manifest["mode"])
            write_new(run / ("rescored-" + secrets.token_hex(4) + ".json"), report)
        print(json.dumps({k: report[k] for k in ("mode", "selectedTasks", "selectedTurns", "counts", "backendAcceptanceRate", "controlledTaskCompletionRate", "intentExactSetRate")}, ensure_ascii=False))
        return 0 if report["counts"].get("passed", 0) == report["selectedTasks"] and not report.get("invalidReason") and not report.get("runError") else 1
    except (CalibrationError, OSError, ValueError):
        print("校准未完成；请检查冻结清单、权限和私有运行记录。", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
