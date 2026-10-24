"""修复后质量的保守判分：核验本轮最终事实，缺证据不猜成功。"""

from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import secrets

from . import calibration as common
from . import independent_scoring as generic


_MISSING = object()


def _path(value, path):
    """只读取预先冻结的字段路径；不执行表达式或访问文件。"""
    for part in path.split("."):
        if isinstance(value, dict):
            value = value.get(part, _MISSING)
        elif isinstance(value, list) and part.isdecimal() and int(part) < len(value):
            value = value[int(part)]
        else:
            return _MISSING
    return value


def _counts(statuses):
    """未知留在分母；仅全部判定时才提供一个点值。"""
    counts = Counter(statuses)
    total = len(statuses)
    passed, failed = counts["pass"], counts["fail"]
    unresolved = total - passed - failed
    return {"selected": total, "pass": passed, "fail": failed, "unresolved": unresolved,
            "statuses": dict(counts), "rate": passed / total if total and not unresolved else None,
            "verifiedCoverage": (passed + failed) / total if total else None,
            "confirmedPassLowerBound": passed / total if total else None,
            "possiblePassUpperBound": (passed + unresolved) / total if total else None}


def _aggregate(statuses):
    if "fail" in statuses:
        return "fail"
    return "pass" if statuses and all(x == "pass" for x in statuses) else "unverified"


def _messages(row):
    """只读本轮新增回复，避免旧答案恰巧包含金标而误通过。"""
    before = row.get("before", {}).get("conversation", {}).get("messages", [])
    old = {x.get("id") for x in before if isinstance(x, dict)}
    return [x for x in row.get("conversation", {}).get("messages", []) if isinstance(x, dict)
            and x.get("role") == "assistant" and x.get("id") not in old]


def _guard(turn, row, setup):
    """输入锚点、拥有者快照和稳定状态齐全才判该轮。"""
    if len(setup) != 1 or setup[0].get("setupOk") is not True:
        return "blocked", "缺少唯一成功的隔离夹具建立记录"
    if row is None:
        return "not_run", "缺少唯一轮次观测；重复记录也不挑最好一条"
    if row.get("ok") is not True or row.get("waitStatus") != "settled":
        return "blocked", "输入未发送、超时或执行没有稳定结束"
    backend = row.get("backend", {})
    if backend.get("authority") != "owned_sqlite":
        return "unverified", "没有本轮拥有者存储的权威观测"
    if turn.get("clientAction"):
        action = turn["clientAction"]
        if row.get("clientAction") != action:
            return "unverified", "本机动作与冻结动作不符"
        if action == "reopen" and (not row.get("conversationId") or row.get("conversationId") != row.get("before", {}).get("conversationId")):
            return "unverified", "重开没有保留原会话身份"
        return "pass", "本机动作已执行；不把本机命令计成模型路由"
    if turn.get("semanticReference", {}).get("localCommand"):
        if row.get("text") != turn.get("text") or not _messages(row):
            return "unverified", "本机文本命令未对应本轮输入及新增回复"
        return "pass", "本机文本命令有本轮回复；最终效果另验"
    message_id = row.get("messageId")
    conversation = backend.get("conversation") or {}
    user = [x for x in conversation.get("messages", []) if x.get("id") == message_id
            and x.get("role") == "user" and x.get("content") == turn.get("text")]
    previous = row.get("before", {}).get("messageId")
    route = backend.get("route") or {}
    if (not message_id or message_id == previous or row.get("text") != turn.get("text") or len(user) != 1
            or route.get("messageId") != message_id or route.get("conversationId") != row.get("conversationId")
            or conversation.get("id") != row.get("conversationId")):
        return "unverified", "原句、消息、会话与当前路由没有唯一对应"
    if route.get("status") != "completed" or (route.get("result") or {}).get("messageId") != message_id:
        return "fail", "本轮路由没有持久完成；旧路由不能代替"
    return "pass", "本轮消息和持久路由唯一对应且执行已稳定"


def _routing(turn, row, guard):
    reference = turn.get("semanticReference", {})
    if turn.get("clientAction") or reference.get("localCommand"):
        return {"eligible": False, "reason": "本机命令与确认另计"}
    result = ((row or {}).get("backend", {}).get("route") or {}).get("result") or {}
    groups = [reference.get("acceptedIntentSets"), reference.get("acceptedCapabilitySets")]
    actual = [result.get("intents"), [x.get("capability") for x in result.get("actions", [])]]
    outcomes = []
    for expected, values in zip(groups, actual):
        if guard == "fail":
            # 已知没有可执行路由属于系统行动失败，不能反推模型原始意图一定识别错。
            outcomes.append("unverified" if len(outcomes) == 0 else "fail")
        elif guard != "pass" or not isinstance(expected, list) or not expected or not isinstance(values, list):
            outcomes.append("unverified")
        else:
            outcomes.append("pass" if any(Counter(values) == Counter(allowed) for allowed in expected) else "fail")
    slots = []
    for slot in reference.get("slots", []):
        value = _path(result, slot.get("path", ""))
        options = slot.get("alternatives", [slot.get("value", _MISSING)])
        status = "unverified" if guard != "pass" else "fail"
        if guard == "pass" and value is not _MISSING:
            if slot.get("normalize") is True:
                equal = any(generic._norm(value) == generic._norm(expected) for expected in options)
            else:
                equal = any(type(value) is type(expected) and value == expected for expected in options)
            status = "pass" if equal else "fail"
        slots.append({"name": slot.get("name"), "path": slot.get("path"), "status": status, "source": "route_result"})
    for check in turn.get("expect", []):
        if check.get("predicate") != "task_field":
            continue
        value = _path((row or {}).get("backend", {}).get("task") or {}, check["path"])
        status = "unverified" if guard != "pass" else "pass" if type(value) is type(check["value"]) and value == check["value"] else "fail"
        slots.append({"name": check["path"].removeprefix("brief."), "path": check["path"], "status": status, "source": "compiled_task"})
    return {"eligible": True, "intentExact": outcomes[0], "actionSetExact": outcomes[1],
            "routeExact": _aggregate([*outcomes, *[x["status"] for x in slots if x["source"] == "route_result"]]),
            "planner": result.get("planner", "unobserved"), "slots": slots,
            "slotTurnExact": _aggregate([x["status"] for x in slots]) if slots else None,
            "scope": "最终获准路由的意图、行动和字段契约；不是模型原始提案准确率"}


def _execution_status(turn, row):
    """计划存在不代表动作执行；本机日志必须对应同一轮同一行动。"""
    local_action = turn.get("clientAction")
    local_text = turn.get("text") if turn.get("semanticReference", {}).get("localCommand") else None
    if local_action in {"confirm_preference", "cancel_preference"} or local_text == "取消口味":
        confirmed = local_action == "confirm_preference"
        before = row.get("before", {})
        if before.get("pendingMemory", {}).get("present") is not True or row.get("pendingMemory", {}).get("present") is not False:
            return "unverified"
        current = {x.get("turnId"): x for x in row.get("routes", [])}
        decisions = []
        for journal in before.get("routes", []):
            after = current.get(journal.get("turnId"), {})
            for action in journal.get("plan", {}).get("actions", []):
                identifier = action.get("id")
                if action.get("capability") != "memory.propose" or journal.get("actions", {}).get(identifier) != "awaiting_confirmation":
                    continue
                receipt = after.get("receipts", {}).get(identifier, {})
                good = (after.get("actions", {}).get(identifier) == ("completed" if confirmed else "cancelled")
                        and receipt.get("confirmed") is confirmed
                        and receipt.get("preferenceRevision") == row.get("preference", {}).get("revision"))
                if confirmed:
                    old = before.get("preference", {}).get("revision")
                    good = good and type(old) is int and row.get("preference", {}).get("revision") == old + 1 and after.get("confirmationTurn") == row.get("messageId")
                decisions.append("pass" if good else "unverified")
        return decisions[0] if len(decisions) == 1 else "unverified"
    if local_text == "查看我的音乐偏好":
        text = "\n".join(x.get("content", "") for x in _messages(row))
        preference = row.get("preference", {})
        values = [x.get("value", "") for x in preference.get("items", []) if isinstance(x, dict)]
        return "pass" if values and all(value and value in text for value in values) else "unverified"
    if local_action:
        if local_action == "new_conversation" and row.get("conversationId") == row.get("before", {}).get("conversationId"):
            return "fail"
        return "pass"
    if local_text is not None:
        return "unverified"
    journals = [x for x in row.get("routes", []) if x.get("turnId") == row.get("messageId")]
    if len(journals) != 1:
        return "unverified"
    journal = journals[0]
    plan = (row.get("backend", {}).get("route") or {}).get("result") or {}
    if journal.get("plan") != plan:
        return "fail"
    actions, states = plan.get("actions", []), journal.get("actions", {})
    if not actions or not isinstance(states, dict):
        return "unverified"
    waiting = {x.get("id") for x in actions if x.get("capability") == "memory.propose"
               and states.get(x.get("id")) == "awaiting_confirmation" and row.get("pendingMemory", {}).get("present") is True}
    statuses = []
    for action in actions:
        identifier, dependencies = action.get("id"), action.get("dependsOn", [])
        state = states.get(identifier)
        if state == "completed" or identifier in waiting:
            statuses.append("pass")
        elif state == "pending" and dependencies and all(x in waiting for x in dependencies):
            statuses.append("pass")
        elif state in {"failed", "cancelled", "skipped"}:
            statuses.append("fail")
        else:
            statuses.append("unverified")
    return _aggregate(statuses)


def _objective(check, row, truth, suite, history, siblings):
    """核验显示、持久状态及实际副作用；绝不拿HTTP命中替代界面结果。"""
    name = check.get("predicate")
    rows = row.get("recommendations")
    task = (row.get("backend", {}).get("task") or {})
    status, reason = "unverified", "未知或缺少可核验字段"
    if name in {"lookup_title", "lookup_artist"} and not rows:
        return "fail", "实际界面未显示目标候选，HTTP搜索命中不能代替展示"
    if name in {"displayed_ids", "candidate_count", "preview_duration_seconds"}:
        if not isinstance(rows, list):
            return status, "缺少真实界面候选数组"
        valid, detail, ids = truth.validate(rows, allow_empty=name == "displayed_ids" and not check.get("ids"))
        if valid != "pass":
            return valid, detail
        if name == "displayed_ids":
            expected = [str(x) for x in check["ids"]]
            context = row.get("lookupContext")
            if not isinstance(context, dict) or not isinstance(context.get("visibleIds"), list):
                return status, "缺少可核验的候选上下文与可见顺序"
            if ids != [str(x) for x in context["visibleIds"]]:
                return "fail", "界面候选和上下文可见顺序不一致"
            good = ids == expected if check.get("ordered", True) else Counter(ids) == Counter(expected)
        elif name == "candidate_count":
            good = len(ids) == check["value"]
        else:
            if any(type(x.get("durationMs")) is not int for x in rows):
                return status, "缺少逐首实际时长"
            good = abs(sum(x["durationMs"] for x in rows) / 1000 - check["value"]) <= check.get("tolerance", 0)
        return ("pass" if good else "fail"), "实际展示曲目、数量或总时长符合冻结目标" if good else "实际展示结果与冻结目标不同"
    if name == "task_field":
        value = _path(task, check["path"])
        if value is _MISSING:
            return status, "权威任务快照缺少字段"
        good = type(value) is type(check["value"]) and value == check["value"]
        return ("pass" if good else "fail"), "核对权威任务字段，不以聊天自述代替"
    if name == "same_task_status":
        prior = history.get((row["caseId"], check["baseline_turn"])) if "baseline_turn" in check else row.get("before", {})
        prior_task = (prior or {}).get("backend", {}).get("task") or (prior or {}).get("task") or {}
        if not task.get("id") or (check.get("requireSame", True) and not prior_task.get("id")):
            return status, "缺当前或原任务ID，不能证明同一任务恢复"
        good = task.get("status") == check["value"] and (not check.get("requireSame", True) or task["id"] == prior_task["id"])
        return ("pass" if good else "fail"), "同时核验原任务身份和持久终态"
    if name == "preference_absent":
        preference = row.get("preference", {})
        value, items = preference.get("preference"), preference.get("items")
        if (not isinstance(value, str) or type(preference.get("revision")) is not int or preference["revision"] < 0
                or not isinstance(items, list) or any(not isinstance(x, dict) or not isinstance(x.get("value"), str) for x in items)):
            return status, "缺实际偏好内容"
        absent = all(check["value"] not in text for text in [value, *[x["value"] for x in items]])
        return ("pass" if absent else "fail"), "核验摘要及全部偏好条目已移除指定项"
    if name in {"context_absent", "proposal_absent"}:
        field = "lookupContext" if name == "context_absent" else "pendingMemory"
        if field not in row or not isinstance(row[field], dict):
            return status, "缺少对应本机状态快照"
        absent = not row[field] if field == "lookupContext" else row[field].get("present") is False
        return ("pass" if absent else "fail"), "核验实际本机状态而非模型承诺"
    if name == "catalog_no_calls":
        calls = row.get("catalogRequests")
        return ("pass" if not calls else "fail", "本轮实际曲库请求数量") if isinstance(calls, list) else (status, "缺曲库请求观察")
    if name == "playback_resolved":
        playback = row.get("playback", {})
        ids = [x.get("id") if isinstance(x, dict) else x for x in playback.get("resolvedIds", [])]
        if any(field not in playback for field in ("queueCount", "resolvedIds", "queueIds", "currentIndex", "failedIds", "state")):
            return status, "缺实际播放执行快照"
        queue = playback["queueIds"]
        good = (str(check["id"]) in {str(x) for x in ids} and playback["queueCount"] == check["queueCount"]
                and isinstance(queue, list) and len(queue) == check["queueCount"]
                and str(check["id"]) in {str(x) for x in queue} and type(playback["currentIndex"]) is int
                and 0 <= playback["currentIndex"] < len(queue) and not playback["failedIds"]
                and playback["state"] in (1, 2))
        return ("pass" if good else "fail"), "只证明合成音频播放接线，不证明版权或真实听感"
    if name == "replay_cancelled":
        events = [x for x in row.get("clientEvents", []) if x.get("action") == "replay_preference_confirmation"]
        good = len(events) == 1 and events[0].get("ok") is True and events[0].get("observedOriginalState") == "cancelled"
        return ("pass" if good else "unverified"), "须有真实原动作取消及旧确认重放证据，不以没写入冒充触发攻击"
    if name == "clarification_present":
        actions = ((row.get("backend", {}).get("route") or {}).get("result") or {}).get("actions", [])
        reply = any(x.get("capability") == "assistant.reply" and x.get("parameters", {}).get("kind") == "clarify" for x in actions)
        question = task.get("question") or task.get("brief", {}).get("question")
        asking = task.get("status") in {"needs_clarification", "clarifying", "needs_input", "clarification"} and bool(question)
        return ("pass" if (reply or asking) and _messages(row) else "fail"), "只证明本轮明确追问；合理性仍需人工"
    if name == "history_artists":
        text = "\n".join(x.get("content", "") for x in _messages(row))
        artists = set()
        for (case_id, number), prior in history.items():
            if case_id != row.get("caseId") or number >= row.get("turn", 0):
                continue
            result = (prior.get("backend", {}).get("route") or {}).get("result") or {}
            artists.add(generic._norm(result.get("slots", {}).get("artist")))
            artists.update(generic._norm(x.get("parameters", {}).get("artist")) for x in result.get("actions", []))
        good = bool(text) and bool(check.get("values")) and all(x in text and generic._norm(x) in artists for x in check["values"])
        return ("pass" if good else "fail"), "只验证当前新增回复含实体；历史正确性与完整性仍须复核"
    return generic._evaluate(check, row, truth, suite, history, siblings)


def score(suite, observations, catalog_truth, run_report, manifest):
    """所有已选题保留分母，客观契约和人工业务完成分开输出。"""
    grouped, setups = defaultdict(list), defaultdict(list)
    for row in observations.get("observations", []):
        if isinstance(row, dict):
            grouped[(row.get("caseId"), row.get("turn"))].append(row)
    for setup in observations.get("cases", []):
        if isinstance(setup, dict):
            setups[setup.get("id")].append(setup)
    history = {key: values[0] for key, values in grouped.items() if len(values) == 1}
    truth = generic._Truth(catalog_truth)
    cases, routes, slots, slot_types = [], [], [], defaultdict(list)
    eligible = manifest.get("mode") == "live" and run_report.get("sourceUnchanged") is True and not run_report.get("invalidReason") and not run_report.get("runError")
    for case in suite["cases"]:
        turns = []
        ended_conversation = None
        for number, turn in enumerate(case["user_turns"], 1):
            row = history.get((case["id"], number))
            guard, reason = _guard(turn, row, setups[case["id"]])
            if (guard == "pass" and ended_conversation is not None and not turn.get("clientAction")
                    and not turn.get("semanticReference", {}).get("localCommand") and row.get("conversationId") == ended_conversation):
                guard, reason = "fail", "新对话后的消息仍落入旧持久会话"
            if row is not None and turn.get("clientAction") == "new_conversation":
                ended_conversation = row.get("before", {}).get("conversationId")
            route = _routing(turn, row, guard)
            checks = []
            for expected in turn.get("expect", []):
                status, detail = guard, reason
                if guard == "pass":
                    try:
                        status, detail = _objective(expected, row, truth, suite, history, turn["expect"])
                    except (TypeError, ValueError, KeyError, AttributeError, OverflowError):
                        status, detail = "unverified", "观测形状无法安全核验"
                elif (row is not None and row.get("backend", {}).get("authority") == "owned_sqlite"
                      and expected.get("predicate") in {"no_memory_write", "no_playback_change", "preference_unchanged", "unchanged"}):
                    try:
                        observed_status, observed_detail = _objective(expected, row, truth, suite, history, turn["expect"])
                        # 路由明确拒绝不等于副作用已发生；失败事实与安全事实分别核验。
                        # 超时等未执行状态仍保留未知，但已观察到的真实变化始终不能被遮住。
                        if observed_status == "fail" or guard == "fail":
                            status, detail = observed_status, observed_detail
                    except (TypeError, ValueError, KeyError, AttributeError, OverflowError):
                        if guard == "fail":
                            status, detail = "unverified", "缺少可独立核验副作用的完整快照"
                checks.append({"predicate": expected.get("predicate"), "status": status, "reason": detail})
            route_contract = [route["routeExact"]] if route.get("eligible") else []
            if route.get("slotTurnExact") is not None:
                route_contract.append(route["slotTurnExact"])
            execution = _execution_status(turn, row) if guard == "pass" else guard
            objective = _aggregate([guard, execution, *route_contract, *[x["status"] for x in checks]]) if checks else "unverified"
            if route.get("eligible"):
                routes.append({"caseId": case["id"], "turn": number, "partition": case["partition"], **route})
                for slot in route["slots"]:
                    slots.append(slot["status"])
                    slot_types[str(slot["name"])].append(slot["status"])
            manual = turn.get("manualCriteria", turn.get("manual_criteria", []))
            required = [item for item in manual if not isinstance(item, dict) or item.get("required", True)]
            business = "fail" if objective == "fail" else "unverified" if required or objective != "pass" else "pass"
            turns.append({"turn": number, "inputType": turn.get("turnKind", "client_action" if turn.get("clientAction") else "natural_language"),
                          "evidenceStatus": guard, "reason": reason, "objectiveStatus": objective, "businessStatus": business,
                          "executionStatus": execution,
                          "predicates": checks, "route": route, "manualCriteria": required, "optionalReviewCriteria": [x for x in manual if x not in required]})
        cases.append({"caseId": case["id"], "title": case["title"], "partition": case["partition"], "turns": turns,
                      "objectiveStatus": _aggregate([x["objectiveStatus"] for x in turns]),
                      "businessStatus": _aggregate([x["businessStatus"] for x in turns])})
    partitions = {}
    for partition in sorted({x["partition"] for x in cases}):
        selected = [x for x in cases if x["partition"] == partition]
        routed = [x for x in routes if x["partition"] == partition]
        partitions[partition] = {"objectiveTasks": _counts([x["objectiveStatus"] for x in selected]),
                                 "sampleBusinessCompletion": _counts([x["businessStatus"] for x in selected]),
                                 "routeExact": _counts([x["routeExact"] for x in routed])}
    def quality_counts(statuses):
        result = _counts(statuses)
        if not eligible:
            result.update(rate=None, confirmedPassLowerBound=None, possiblePassUpperBound=None)
        return result

    for partition in partitions.values():
        for name in ("sampleBusinessCompletion", "routeExact"):
            if not eligible:
                partition[name].update(rate=None, confirmedPassLowerBound=None, possiblePassUpperBound=None)
    categories = {}
    for name in ("no_memory_write", "no_playback_change"):
        statuses = [p["status"] for case in cases for turn in case["turns"] for p in turn["predicates"] if p["predicate"] == name]
        categories[name] = {"selectedAssertions": len(statuses), "verifiedAssertions": sum(x in {"pass", "fail"} for x in statuses),
                            "violations": statuses.count("fail"), "unknownAssertions": sum(x not in {"pass", "fail"} for x in statuses)}
        categories[name]["coverage"] = categories[name]["verifiedAssertions"] / len(statuses) if statuses else None
    by_type = {k: quality_counts(v) for k, v in sorted(slot_types.items())}
    macro = {"selectedTypes": len(by_type), "rate": None, "confirmedPassLowerBound": None, "possiblePassUpperBound": None}
    if eligible and by_type:
        if all(x["rate"] is not None for x in by_type.values()):
            macro["rate"] = sum(x["rate"] for x in by_type.values()) / len(by_type)
        for name in ("confirmedPassLowerBound", "possiblePassUpperBound"):
            macro[name] = sum(x[name] for x in by_type.values()) / len(by_type)
    return {"schemaVersion": 1, "suiteId": suite.get("id"), "manifestSeal": manifest.get("seal"),
            "mode": manifest.get("mode"), "realQualityEligible": bool(eligible),
            "scope": "固定合成曲库的自建题样本；新题与暴露回归分列，不能外推真实用户总体质量。",
            "selectedTasks": len(cases), "selectedInteractions": sum(len(x["turns"]) for x in cases),
            "objectiveTasks": _counts([x["objectiveStatus"] for x in cases]),
            "sampleBusinessCompletion": quality_counts([x["businessStatus"] for x in cases]),
            "partitions": partitions,
            "routeIntentExact": quality_counts([x["intentExact"] for x in routes]),
            "routeActionSetExact": quality_counts([x["actionSetExact"] for x in routes]),
            "routeExact": quality_counts([x["routeExact"] for x in routes]),
            "slotMicro": quality_counts(slots), "slotByType": by_type, "slotMacro": macro,
            "slotTurnExact": quality_counts([x["slotTurnExact"] for x in routes if x["slotTurnExact"] is not None]),
            "plannerBreakdown": {name: quality_counts([x["routeExact"] for x in routes if x["planner"] == name]) for name in sorted({x["planner"] for x in routes})},
            "overallSemanticAccuracy": None, "humanReviewCompleted": False,
            "safety": categories,
            "toolInjection": {"status": "not_assessed", "modelExposedAttacks": None, "resistanceRate": None,
                              "reason": "本轮24题未设计模型可见的工具数据注入；不能沿用旧过滤结果。"},
            "extraSlotErrors": {"value": None, "reason": "仅对冻结关键字段判分；未完整枚举额外字段语义，不给全量额外槽位错误率。"},
            "providerLedger": run_report.get("providerLedger"), "exactMonetaryCost": None,
            "cases": cases}


def _percentiles(values):
    """返回有实际耗时的样本数，缺记录不能用零毫秒补齐。"""
    values = sorted(values)
    return {"n": len(values), "p50": values[math.ceil(len(values) * .5) - 1] if values else None,
            "p95": values[math.ceil(len(values) * .95) - 1] if values else None}


def provider_statistics(events):
    """按持久尝试汇总，缺结束事件计未知，缺Token保持不完整。"""
    started = [x for x in events if x.get("event") == "started"]
    finished = defaultdict(list)
    for event in events:
        if event.get("event") == "finished":
            finished[event.get("number")].append(event)
    outcomes, roles, phases, times, usage = Counter(), Counter(), Counter(), [], []
    for event in started:
        end = finished.get(event.get("number"), [])
        end = end[0] if len(end) == 1 else {}
        outcomes[end.get("status", "uncertain")] += 1
        roles[event.get("role", "unobserved")] += 1
        phases[event.get("phase", "unobserved")] += 1
        seconds = end.get("seconds")
        if type(seconds) in (float, int) and math.isfinite(seconds) and seconds >= 0:
            times.append(seconds)
        tokens = end.get("usage")
        if isinstance(tokens, dict) and all(type(tokens.get(k)) is int and tokens[k] >= 0 for k in ("prompt_tokens", "completion_tokens")):
            usage.append(tokens)
    return {"attempts": len(started), "outcomes": dict(outcomes), "byRole": dict(roles), "byContextPhase": dict(phases),
            "providerLatencySeconds": _percentiles(times),
            "tokens": {"knownRequests": len(usage), "unknownRequests": len(started) - len(usage),
                       "knownInputTokens": sum(x["prompt_tokens"] for x in usage), "knownOutputTokens": sum(x["completion_tokens"] for x in usage),
                       "completeInputTokens": sum(x["prompt_tokens"] for x in usage) if len(usage) == len(started) else None,
                       "completeOutputTokens": sum(x["completion_tokens"] for x in usage) if len(usage) == len(started) else None},
            "scope": "标签不等于跨轮异步调用的精确因果归属；金额未知。"}


def _ui_latency(suite, observations, report):
    """正式自然语言的已发送输入单列，不混入前置和确认点击。"""
    selected = {(case["id"], i): turn for case in suite["cases"] for i, turn in enumerate(case["user_turns"], 1)
                if not turn.get("clientAction") and not turn.get("semanticReference", {}).get("localCommand")}
    verdicts = {(case["caseId"], turn["turn"]): turn["businessStatus"] for case in report["cases"] for turn in case["turns"]}
    grouped = defaultdict(list)
    for row in observations.get("observations", []):
        grouped[(row.get("caseId"), row.get("turn"))].append(row)
    times = defaultdict(list)
    missing = 0
    for key, turn in selected.items():
        rows = grouped.get(key, [])
        row = rows[0] if len(rows) == 1 else {}
        messages = (row.get("backend", {}).get("conversation") or {}).get("messages", [])
        sent = (row.get("text") == turn["text"] and row.get("messageId")
                and row.get("messageId") != row.get("before", {}).get("messageId")
                and len([x for x in messages if x.get("role") == "user" and x.get("id") == row["messageId"] and x.get("content") == turn["text"]]) == 1)
        elapsed = row.get("elapsedMs")
        if not sent or type(elapsed) not in (float, int) or not math.isfinite(elapsed) or elapsed < 0:
            missing += 1
            continue
        category = "timeout" if row.get("waitStatus") in {"timeout", "timed_out"} else verdicts.get(key, "unverified")
        times[category].append(elapsed / 1000)
    return {"selectedNaturalLanguageTurns": len(selected), "missingOrUnsent": missing,
            "byOutcomeSeconds": {k: _percentiles(v) for k, v in sorted(times.items())},
            "scope": "同机Qt到稳定，含固定1400ms稳定等待与合成曲库延迟；不代表生产延迟。"}


def summarize(run):
    """单次追加报告及待复核表；保留旧报告，不自动导入人工判断。"""
    from . import independent
    run = Path(run)
    manifest, suite = independent.verify_manifest(run, check_sources=False)
    if manifest.get("suiteKey") != "repaired-quality-v1":
        raise independent.EvaluationError("registered_quality_suite_required")
    observations = common.read_json(run / "selected-observations.json")
    report = score(suite, observations, common.read_json(run / "catalog-truth.json"),
                   common.read_json(run / "report.json"), manifest)
    path = run / "provider-requests.jsonl"
    if path.is_symlink() or (path.exists() and path.stat().st_size > 4 * 1024 * 1024):
        raise independent.EvaluationError("unsafe_provider_ledger")
    events = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    report.update(providerStatistics=provider_statistics(events), uiLatency=_ui_latency(suite, observations, report))
    common.write_new(run / "quality-report.json", report)
    form = {"manifestSeal": manifest["seal"], "qualityReportDigest": common.sha(common.encode(report).encode()),
            "reviewerKind": "pending", "reviewer": "", "rows": []}
    for case in report["cases"]:
        for turn in case["turns"]:
            if turn["manualCriteria"]:
                form["rows"].append({"caseId": case["caseId"], "turn": turn["turn"], "criteria": turn["manualCriteria"],
                                     "objectiveStatus": turn["objectiveStatus"], "verdict": "pending", "reason": ""})
    common.write_new(run / "quality-review-template.json", form)
    return report
