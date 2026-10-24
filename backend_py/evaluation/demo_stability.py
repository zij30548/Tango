"""固定提案与合成曲库的演示核心回归；不提供真实模型语义成绩。"""

from collections import Counter
from datetime import datetime, timezone
import json
import re

from . import calibration as common
from . import independent as runner
from .repair_regression import RepairWiringModel
from .scripted import brief


class DemoWiringModel(RepairWiringModel):
    """显式预写提案只驱动正式链路，不访问网络、不读取题集预期或评分结果。"""

    model = "demo-stability-wiring-fixture"

    async def route(self, input, options=None):
        text = input["instruction"]
        if text == "请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。":
            return {"value": {"intents": ["preference_proposal", "scene_research"],
                "slots": {"artist": "", "targetMinutes": 16}, "missingSlots": [], "actions": [
                    {"capability": "memory.propose", "source": "请记住我喜欢粤语", "dependsOn": [],
                     "parameters": {"operation": "set", "preference": "喜欢粤语"}},
                    {"capability": "music.task", "source": "给我一份16分钟微醺歌单，只做预览", "dependsOn": [],
                     "parameters": {"mode": "new"}}]}}
        return await super().route(input, options)

    async def call(self, kind, input, options=None):
        if kind == "plan":
            text = input.get("instruction", "")
            value = common.clone(input.get("currentBrief") or brief())
            scene = "微醺" if "微醺" in text else "通勤"
            value.update(scene=scene, styles=[], queries=[scene], retainFirst=0)
            count = re.search(r"([1-9][0-9]?)首", text)
            minutes = re.search(r"([1-9][0-9]?)分钟", text)
            if count:
                value["targetTrackCount"] = int(count[1])
            if minutes:
                value["targetMinutes"] = int(minutes[1])
            return {"value": value}
        return await super().call(kind, input, options)


def demo_suite(clock=None):
    """冻结十条可观察业务目标；旧失败与相邻表达统一标记为暴露工程回归。"""
    clock = clock or datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    fixtures = common.read_json(runner.PROJECT / runner.CANDIDATES)["fixture_definitions"]
    safety = [{"predicate": "no_memory_write"}, {"predicate": "no_playback_change"}]

    def turn(text, expect=None, **options):
        return {"text": text, "expect": common.clone(expect if expect is not None else safety), **options}

    def case(identifier, title, turns, **options):
        return {"id": identifier, "title": title, "partition": "exposed_demo_engineering_regression",
                "user_turns": common.clone(turns), "turns": turns, **options}

    preview = [{"predicate": "preview_nonempty"}] + safety
    lookup = [{"predicate": "lookup_title", "value": "松针雨"}, {"predicate": "lookup_artist", "value": "阔叶电台"}]
    cases = [
        case("D01", "帮助明确说明查询确认与采用边界", [turn("你都能帮我做哪些事情？")]),
        case("D02", "书名号后版本的查询仅展示不播放", [turn("请搜索阔叶电台的《松针雨》录音室版，先不要播放。", lookup + safety)]),
        case("D03", "书名号后录音室版确实解析播放", [turn("请播放阔叶电台的《松针雨》录音室版。", lookup + [{"predicate": "no_memory_write"}])]),
        case("D04", "五首二十分改四首十六分后撤回", [
            turn("给我5首通勤歌，20分钟左右，只做预览。", preview),
            turn("改成4首，16分钟左右，只做预览。", preview),
            turn("撤回刚才的修改。", [{"predicate": "undo_restored", "baseline_turn": 1}] + safety)]),
        case("D05", "复合记忆场景独立执行且确认后才保存", [
            turn("请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。", [{"predicate": "pending_memory"}] + preview),
            turn("确认保存口味", [{"predicate": "preference_contains", "value": "粤语"}, {"predicate": "no_playback_change"}], clientAction="confirm_preference"),
            turn("查看我的音乐偏好", [{"predicate": "preference_contains", "value": "粤语"}] + safety)]),
        case("D06", "删单项确认后保留其他项", [
            turn("把中文优先这条偏好删掉，其他两条保留。", [{"predicate": "pending_memory"}] + safety),
            turn("确认保存口味", [{"predicate": "preference_contains", "value": "原声器乐"}, {"predicate": "preference_contains", "value": "吵闹现场版"}, {"predicate": "no_playback_change"}], clientAction="confirm_preference")],
            initialPreference="偏好原声器乐；中文优先；不喜欢吵闹现场版"),
        case("D07", "会话歌手历史只读回顾", [
            turn("搜索陶喆的《普通朋友》，只看搜索结果。", [{"predicate": "lookup_title", "value": "普通朋友"}, {"predicate": "lookup_artist", "value": "陶喆"}] + safety),
            turn("再查找周杰伦的《稻香》，不要播放。", [{"predicate": "lookup_title", "value": "稻香"}, {"predicate": "lookup_artist", "value": "周杰伦"}] + safety),
            turn("到目前为止，我在这段对话里问过哪几位歌手？")]),
        case("D08", "原待工具任务暂停继续最后取消", [
            turn("先暂停当前查歌任务。", [{"predicate": "paused", "scope": "original_task"}] + safety),
            turn("继续刚才暂停的任务。", preview),
            turn("取消这次任务，先不找了。", [{"predicate": "cancelled", "scope": "original_task"}] + safety)],
            catalogDelayMs=6000, runtimeSetupInstructions=[{"text": "请先为我做一份20分钟的通勤歌单，只预览。", "waitFor": "waiting_tools", "timeoutMs": 30000}]),
        case("D09", "已确认偏好重开仍保持但不自动执行", [
            turn("记住我喜欢原声器乐", [{"predicate": "pending_memory"}] + safety),
            turn("确认保存口味", [{"predicate": "preference_contains", "value": "原声器乐"}, {"predicate": "no_playback_change"}], clientAction="confirm_preference"),
            turn("重新打开原会话", [{"predicate": "preference_contains", "value": "原声器乐"}] + safety, clientAction="reopen")]),
        case("D10", "明确采用后队列生效且重复采用不重新解析", [
            turn("给我5首通勤歌，20分钟左右，只做预览。", preview),
            turn("采用歌单", [{"predicate": "no_memory_write"}], clientAction="adopt"),
            turn("采用歌单", [{"predicate": "no_memory_write"}], clientAction="adopt")]),
    ]
    for item in cases:
        item["setupInstructions"] = [row["text"] for row in item.get("runtimeSetupInstructions", [])]
    return {"id": "exposed-native-demo-stability-v1", "purpose": "engineering_regression_only",
            "modelFixture": "demo-stability-v1", "clock": clock,
            "fixture_definitions": common.clone(fixtures), "cases": cases,
            "scope": "固定提案和合成曲库，仅验真实Qt到正式后台的核心业务链，不是真实模型、真实曲库或听感验收"}


def _task(row):
    """任务完成以隔离后台的权威读取为准，不使用助手自述。"""
    value = row.get("backend", {}).get("task")
    return value if isinstance(value, dict) else {}


def _ids(row):
    values = row.get("recommendations")
    return [value.get("id") for value in values] if isinstance(values, list) and all(isinstance(value, dict) for value in values) else None


def _reply_delta(row):
    before = {item.get("id") for item in row.get("before", {}).get("conversation", {}).get("messages", [])}
    return "\n".join(item.get("content", "") for item in row.get("conversation", {}).get("messages", [])
                     if item.get("role") == "assistant" and item.get("id") not in before)


def demo_checks(observations, suite=None):
    """缺失、重复或中断交互均失败；逐条核对实际候选、确认、任务及采用回执。"""
    suite = suite or demo_suite("2026-10-24T04:00:00Z")
    raw = observations.get("observations", [])
    raw = raw if isinstance(raw, list) else []
    counts = Counter((row.get("caseId"), row.get("turn")) for row in raw if isinstance(row, dict))
    grouped = {(row.get("caseId"), row.get("turn")): row for row in raw if isinstance(row, dict) and counts[(row.get("caseId"), row.get("turn"))] == 1}
    checks = []

    def add(name, passed, detail):
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    expected = {(case["id"], index) for case in suite["cases"] for index in range(1, len(case["turns"]) + 1)}
    add("交互集合完全匹配", set(counts) == expected and all(number == 1 for number in counts.values()), "缺少、重复、额外交互不得从分母消失")
    for case in suite["cases"]:
        for index, turn in enumerate(case["turns"], 1):
            label = case["id"] + ":" + str(index)
            row = grouped.get((case["id"], index), {})
            add("实际交互结束:" + label, row.get("ok") is True and row.get("waitStatus") == "settled", row.get("waitStatus", "缺观察"))
            if turn.get("clientAction") != "confirm_preference":
                add("不擅自保存偏好:" + label, bool(row) and isinstance(row.get("preference"), dict) and row.get("preference") == row.get("before", {}).get("preference"), "比较本机实际内容与revision")
            if case["id"] != "D03" and turn.get("clientAction") != "adopt":
                before, after = row.get("before", {}).get("playback", {}), row.get("playback", {})
                add("不擅自改变播放:" + label, bool(row) and all(key in after and key in before and after[key] == before[key] for key in ("queueCount", "queueIds", "currentIndex", "state")) and not after.get("resolvedIds") and not after.get("failedIds"), "读取实际队列/状态，未采用的预览不能解析播放地址")

    row = grouped.get(("D01", 1), {})
    text = _reply_delta(row)
    add("帮助回答本轮产生且说明确认边界", bool(text) and all(value in text for value in ("歌", "确认", "采用")) and _ids(row) == [] and row.get("catalogRequests") == [], "文字检查仅证产品说明呈现，不等于模型语义质量")
    for identifier in ("D02", "D03"):
        row = grouped.get((identifier, 1), {})
        add("歌名版本准确展示:" + identifier, _ids(row) == [70005] and row["recommendations"][0].get("version") in ("studio", "录音室版"), "只允许目标作品的录音室版本，曲库取回但未展示不通过")
    row = grouped.get(("D03", 1), {})
    playback = row.get("playback", {})
    resolved = [value.get("id") if isinstance(value, dict) else value for value in playback.get("resolvedIds", [])]
    add("明确播放目标真正生效", playback.get("queueIds") == [70005] and playback.get("queueCount") == 1 and 70005 in resolved, "合成静音WAV只验执行，不证明会员或完整听感")
    for number, count, minutes in ((1, 5, 20), (2, 4, 16)):
        row = grouped.get(("D04", number), {})
        task = _task(row)
        rows = row.get("recommendations")
        duration = sum(value.get("durationMs", 0) for value in rows) if isinstance(rows, list) else -1
        add("曲数时长与正式任务一致:D04:" + str(number), isinstance(rows, list) and len(rows) == count and abs(duration - minutes * 60000) <= 120000 and task.get("status") == "preview" and task.get("brief", {}).get("targetTrackCount") == count and task.get("brief", {}).get("targetMinutes") == minutes, "数量、时长与后端目标同时满足，不能只改回复文字")
    first, second, third = (grouped.get(("D04", number), {}) for number in (1, 2, 3))
    add("连续修改与撤回均为原任务", bool(_task(first).get("id")) and len({_task(value).get("id") for value in (first, second, third)}) == 1 and len(_ids(first) or []) == 5 and len(_ids(second) or []) == 4 and _ids(third) == _ids(first), "原任务ID、实际候选ID及顺序必须恢复")
    row = grouped.get(("D05", 1), {})
    add("复合需求同时有预览和确认提议", row.get("pendingMemory", {}).get("present") is True and row.get("pendingMemory", {}).get("canConfirm") is True and bool(_ids(row)) and _task(row).get("status") == "preview", "偏好提议和独立找歌均执行，确认前偏好仍不变")
    rows = row.get("recommendations")
    add("复合场景时长实际达标", isinstance(rows, list) and bool(rows) and abs(sum(value.get("durationMs", 0) for value in rows) - 16 * 60000) <= 120000 and _task(row).get("brief", {}).get("targetMinutes") == 16, "偏好提议不能掩盖独立场景任务时长失败；相关性和听感另验")
    for identifier, number, needle in (("D05", 2, "粤语"), ("D05", 3, "粤语"), ("D09", 2, "原声器乐"), ("D09", 3, "原声器乐")):
        row = grouped.get((identifier, number), {})
        add("确认偏好实际存在:" + identifier + ":" + str(number), needle in row.get("preference", {}).get("preference", "") and row.get("preference", {}).get("revision", 0) > 0, "确认后内容和持久revision均存在")
    row = grouped.get(("D06", 2), {})
    preference = row.get("preference", {}).get("preference", "")
    add("只删中文优先且保留另外两项", "中文优先" not in preference and all(needle in preference for needle in ("原声器乐", "吵闹现场版")) and row.get("preference", {}).get("revision", 0) > row.get("before", {}).get("preference", {}).get("revision", 0), "确认后实际内容按单项变更，不能全清空或仅隐藏文字")
    for number, identifier in ((1, 5008), (2, 5006)):
        row = grouped.get(("D07", number), {})
        add("历史前置作品实际展示:D07:" + str(number), _ids(row) == [identifier], "ID来自编译探针导出的固定真值；HTTP取回但未展示不能通过")
    row = grouped.get(("D07", 3), {})
    add("历史回答来自本轮会话且不补查", all(needle in _reply_delta(row) for needle in ("陶喆", "周杰伦")) and row.get("catalogRequests") == [], "字面与只读证据；具体语义与遗漏仍需人工复核")
    for number, wanted in ((1, "paused"), (2, "preview"), (3, "cancelled")):
        row = grouped.get(("D08", number), {})
        baseline = grouped.get(("D08", 1), {}).get("before", {}).get("task", {})
        task = _task(row)
        add("原任务控制生效:D08:" + str(number), bool(baseline.get("id")) and baseline.get("status") == "waiting_tools" and bool(baseline.get("pendingSteps")) and task.get("id") == baseline["id"] and task.get("status") == wanted and (number != 2 or bool(_ids(row))), "真实pendingSteps起步，同一任务暂停/继续有预览/取消")
    saved, reopened = (grouped.get(("D09", number), {}) for number in (2, 3))
    requests = reopened.get("catalogRequests")
    add("重开同会话保留实际偏好且无新执行", bool(saved.get("conversationId")) and reopened.get("conversationId") == saved["conversationId"] and saved.get("preference") == reopened.get("preference") and isinstance(requests, list) and not any(value.get("path") in ("/search", "/song/url", "/radio/search", "/radio/playlist") for value in requests) and not _task(reopened).get("id"), "销毁窗口并重建只读恢复，不改变revision、不新查歌、不创建任务")
    second, third = (grouped.get(("D10", number), {}) for number in (2, 3))
    initial = grouped.get(("D10", 1), {})
    chosen = _ids(initial)
    for number, row in ((2, second), (3, third)):
        task = _task(row)
        add("采用队列与回执生效:D10:" + str(number), bool(chosen) and row.get("playback", {}).get("queueIds") == chosen and row.get("playback", {}).get("queueCount") == len(chosen) and task.get("status") == "adopted" and bool(task.get("adoption", {}).get("id")) and bool(task.get("adoption", {}).get("appliedAt")), "确认采用必须变成实际队列及后台adopted，不以按钮点击成功代替")
    add("重复采用不重新解析或换回执", bool(_task(second).get("adoption", {}).get("id")) and _task(second).get("adoption", {}).get("id") == _task(third).get("adoption", {}).get("id") and third.get("playback", {}).get("queueIds") == second.get("playback", {}).get("queueIds") and third.get("playback", {}).get("resolvedIds") == [] and isinstance(third.get("catalogRequests"), list) and not any(value.get("path") == "/song/url" for value in third["catalogRequests"]), "重复采用只核原回执，不重发播放解析；缺少HTTP观察也不能通过")
    return checks


def main():
    """只创建新的零预算隔离运行；首次失败保留，不自动重跑。"""
    suite = demo_suite()
    run = runner.prepare(runner.PROJECT / ".local/demo-stability-runs", offline_suite=suite)
    print("私有演示工程回归目录：" + str(run), flush=True)
    report = runner.execute(run)
    checks = demo_checks(common.read_json(run / "selected-observations.json"), suite)
    valid = report.get("sourceUnchanged") and not report.get("invalidReason") and not report.get("runError") and report["providerLedger"]["attempts"] == 0
    result = {"scope": suite["scope"], "validRun": bool(valid), "checks": checks,
              "automaticContractCompliance": report["automaticContractCompliance"],
              "realProviderRequests": report["providerLedger"]["attempts"], "semanticAccuracy": None,
              "allPassed": bool(valid) and all(value["passed"] for value in checks) and report["automaticContractCompliance"]["pass"] == report["selectedTasks"]}
    common.write_new(run / "demo-checks.json", result)
    print(json.dumps({"allPassed": result["allPassed"], "tasks": report["selectedTasks"], "interactions": report["selectedInteractions"], "checks": len(checks), "realRequests": result["realProviderRequests"]}, ensure_ascii=False))
    return 0 if result["allPassed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
