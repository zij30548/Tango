"""多轮候选的零付费原生工程验收；固定上游不能提供模型语义准确率。"""

from datetime import datetime, timezone
import json

from . import calibration as common
from . import independent as runner


def lookup_suite(clock=None):
    """冻结暴露原话与相邻边界；每轮都检查实际候选和独立副作用。"""
    clock = clock or datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    original = common.read_json(runner.PROJECT / runner.CANDIDATES)
    fixtures = common.clone(original["fixture_definitions"])
    # 旧十年夹具专辑名含“录音室”，并非未知版本；另建无版本依据的固定作品。
    fixtures["catalog_core"]["tracks"].append({"id": "track_13", "title": "雾中信笺", "artist": "桥下演唱者",
        "version": "unknown", "tags": [], "duration_seconds": 240})
    safety = [{"predicate": "no_memory_write"}, {"predicate": "no_playback_change"}]

    def turn(text, **options):
        return {"text": text, "expect": common.clone(safety), **options}

    def case(identifier, title, instructions):
        turns = [value if isinstance(value, dict) else turn(value) for value in instructions]
        return {"id": identifier, "title": title, "partition": "exposed_lookup_engineering_regression",
                "user_turns": common.clone(turns), "turns": turns}

    lookup = "找一下《晚风来信》，别放，先让我看看。"
    cases = [
        case("L01", "版本筛选与纠正重新核对原候选池", [lookup, "我要录音室版的那个", "换成现场版那个"]),
        case("L02", "序号只从当前展示候选选择", [lookup, "第二首"]),
        case("L03", "否定现场只留下有明确版本证据的候选", [lookup, "不要现场版"]),
        case("L04", "完整歌手身份与换另一个歌手", [lookup, "我说的是许栀那个", "不是这个歌手，换另一个歌手"]),
        case("L05", "原P03女声复合条件先澄清不部分满足", [lookup, "我说的是女声的那个，不是现场录音。", "我说的是许栀那个"]),
        case("L06", "未知性别不从姓名推断", [lookup, "女声那个"]),
        case("L07", "未知版本不因排除现场就认作录音室", ["找一下《雾中信笺》，别放，先让我看看。", "不要现场版"]),
        case("L08", "转述与条件不能产生候选选择权限", [lookup, "他说选第二首", "如果可以就选第二首"]),
        case("L09", "切话题后旧序号失效", [lookup, "你能做什么？", "第二首"]),
        case("L10", "新对话不能继承旧候选", [lookup, turn("新对话", clientAction="new_conversation"), "第二首"]),
        case("L11", "重开仅恢复历史不恢复未核验候选权限", [lookup, turn("重开原会话", clientAction="reopen"), "第二首"]),
        case("L12", "上一轮播放请求不能授权本轮筛选播放", ["帮我播放《晚风来信》", "我要录音室版的那个"]),
        case("L13", "澄清后直接完整歌手姓名可续接", [lookup, "女声那个", "许栀"]),
    ]
    return {"id": "exposed-native-lookup-context-regression-v2", "purpose": "engineering_regression_only",
            "modelFixture": "repair-regression-v1", "clock": clock,
            "fixture_definitions": fixtures, "cases": cases}


def _ids(row):
    """读取实际界面结果；缺字段不当作空候选成功。"""
    values = row.get("recommendations")
    if not isinstance(values, list):
        return None
    return [value.get("id") for value in values if isinstance(value, dict)]


def _current_route(row):
    message_id = row.get("messageId")
    matches = [route for route in row.get("routes", []) if route.get("turnId") == message_id]
    return matches[0] if len(matches) == 1 else {}


def _refine_completed(row):
    """要求本轮真实路由动作已经完成，不能只凭界面看起来相同。"""
    route = _current_route(row)
    actions = [value for value in route.get("plan", {}).get("actions", []) if value.get("capability") == "music.refine"]
    return len(actions) == 1 and route.get("actions", {}).get(actions[0].get("id")) == "completed"


def _clarified(row):
    """本轮需要明确澄清，普通旧回复和失败报错都不能替代。"""
    actions = _current_route(row).get("plan", {}).get("actions", [])
    return len(actions) == 1 and actions[0].get("capability") == "assistant.reply" and actions[0].get("parameters", {}).get("kind") == "clarify"


def _assistant_delta(row):
    """只检查当前新增助手消息，不能用旧澄清文字冒充本轮回复。"""
    before = {value.get("id") for value in row.get("before", {}).get("conversation", {}).get("messages", [])}
    return "\n".join(value.get("content", "") for value in row.get("conversation", {}).get("messages", [])
                     if value.get("role") == "assistant" and value.get("id") not in before)


def lookup_checks(observations, suite=None):
    """校验所有预定交互、候选来源及终态；缺观察也留在分母中失败。"""
    suite = suite or lookup_suite("2026-10-24T04:00:00Z")
    grouped = {(row.get("caseId"), row.get("turn")): row for row in observations.get("observations", [])}
    checks = []

    def add(name, passed, detail):
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    for case in suite["cases"]:
        for index, turn in enumerate(case["turns"], 1):
            key = (case["id"], index)
            row = grouped.get(key, {})
            label = case["id"] + ":" + str(index)
            add("实际交互结束:" + label, row.get("ok") is True and row.get("waitStatus") == "settled", row.get("waitStatus", "缺少观察"))
            before = row.get("before", {})
            playback, old_playback = row.get("playback", {}), before.get("playback", {})
            add("播放及偏好均无副作用:" + label,
                bool(row) and row.get("preference") == before.get("preference") and
                all(playback.get(field) == old_playback.get(field) for field in ("queueCount", "queueIds", "currentIndex")) and
                not playback.get("resolvedIds") and not playback.get("failedIds"),
                "检查实际播放队列、解析调用及本机偏好，不以助手自述判定")
            if index > 1:
                requests = row.get("catalogRequests")
                add("后续指代不重新搜索或请求播放:" + label,
                    isinstance(requests, list) and not any(value.get("path") in ("/search", "/song/url") for value in requests),
                    "仅允许筛选原候选池；缺少HTTP观察不能通过")
                add("后续指代不创建音乐任务:" + label,
                    bool(row) and not (row.get("task") or {}).get("id") and not (row.get("backend", {}).get("task") or {}).get("id"),
                    "澄清、历史与筛选均不能误派场景任务")
            if index == 1:
                expected = [70013] if case["id"] == "L07" else [70002, 70003]
                context = row.get("lookupContext", {})
                add("首轮真实候选及来源:" + label,
                    _ids(row) == expected and context.get("visibleIds") == expected and bool(context.get("candidateSetId")) and
                    context.get("originMessageId") == row.get("messageId") and bool(context.get("originActionId")),
                    "先建立实际搜索候选，不能将缺少前置条件的后续失败当作测试通过")

    for identifier, number, expected in [
        ("L01", 2, [70002]), ("L01", 3, [70003]), ("L02", 2, [70003]),
        ("L03", 2, [70002]), ("L04", 2, [70002]), ("L04", 3, [70003]), ("L05", 3, [70002]), ("L13", 3, [70002]),
    ]:
        row = grouped.get((identifier, number), {})
        context = row.get("lookupContext", {})
        previous = grouped.get((identifier, number - 1), {}).get("lookupContext", {})
        add("真实候选筛选:" + identifier + ":" + str(number),
            _ids(row) == expected and context.get("visibleIds") == expected and _refine_completed(row),
            "核对界面候选、当前受限上下文及路由完成事实")
        add("原池保留且来源更新:" + identifier + ":" + str(number),
            {value.get("id") for value in context.get("rows", [])} == {70002, 70003} and
            bool(context.get("candidateSetId")) and context.get("candidateSetId") != previous.get("candidateSetId") and
            context.get("originMessageId") == row.get("messageId") and bool(context.get("originActionId")),
            "纠正基于原池，成功后形成新集合和本轮来源，不扩大播放权限")

    for identifier, number in [("L05", 2), ("L06", 2), ("L08", 2), ("L08", 3), ("L13", 2)]:
        row = grouped.get((identifier, number), {})
        add("无依据条件保留原候选并澄清:" + identifier + ":" + str(number),
            _ids(row) == [70002, 70003] and _clarified(row) and not _refine_completed(row) and bool(_assistant_delta(row)),
            "性别未知、转述与条件均不能部分满足或悄悄缩小结果")
    row = grouped.get(("L07", 2), {})
    context = row.get("lookupContext", {})
    add("未知版本不伪造录音室证据", bool(row) and all(value.get("version") == "unknown" for value in context.get("rows", []) if value.get("id") == 70013)
        and any(value.get("id") == 70013 for value in context.get("rows", []))
        and not any(value.get("version") == "studio" for value in row.get("recommendations", [])) and bool(_assistant_delta(row)),
        "原版本缺失保留unknown；不能用没有Live字样推出录音室")
    for identifier in ("L09", "L10", "L11"):
        row = grouped.get((identifier, 3), {})
        add("失效指代不恢复旧候选:" + identifier,
            bool(row) and not row.get("lookupContext") and _ids(row) == [] and _clarified(row) and not _refine_completed(row) and bool(_assistant_delta(row)),
            "切话题、新对话与重开只恢复历史，新的指代必须重新建立来源")
    row = grouped.get(("L12", 2), {})
    add("历史播放授权未传给筛选", _ids(row) == [70002] and _refine_completed(row) and not row.get("playback", {}).get("resolvedIds"),
        "上轮播放存在歧义，本轮只有版本选择，没有新播放授权")
    return checks


def main():
    """每次新建零预算运行；失败与无效运行均保留，不自动重跑。"""
    suite = lookup_suite()
    run = runner.prepare(runner.PROJECT / ".local/lookup-context-runs", offline_suite=suite)
    print("私有候选上下文回归目录：" + str(run), flush=True)
    report = runner.execute(run)
    observations = common.read_json(run / "selected-observations.json")
    checks = lookup_checks(observations, suite)
    valid = report.get("sourceUnchanged") and not report.get("invalidReason") and not report.get("runError") and report["providerLedger"]["attempts"] == 0
    result = {"scope": "已暴露与相邻题的固定上游原生工程回归，不提供模型语义准确率", "validRun": bool(valid),
              "automaticContractCompliance": report["automaticContractCompliance"], "checks": checks,
              "realProviderRequests": report["providerLedger"]["attempts"], "semanticAccuracy": None,
              "allPassed": bool(valid) and all(value["passed"] for value in checks) and report["automaticContractCompliance"]["pass"] == report["selectedTasks"]}
    common.write_new(run / "lookup-checks.json", result)
    print(json.dumps({"allPassed": result["allPassed"], "tasks": report["selectedTasks"], "checks": len(checks), "realRequests": result["realProviderRequests"]}, ensure_ascii=False))
    return 0 if result["allPassed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
