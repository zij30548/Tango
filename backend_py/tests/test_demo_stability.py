"""演示回归的证据边界反例；不调用模型、曲库、用户库或Qt进程。"""

import pytest

from evaluation import demo_stability as demo
from evaluation import independent as runner


def _check(rows, name):
    """只取被测证据规则，不用填满别的场景制造一份全绿报告。"""
    results = demo.demo_checks({"observations": rows})
    return next(value for value in results if value["name"] == name)["passed"]


def test_suite_has_ten_exposed_tasks_and_explicit_confirmation():
    suite = demo.demo_suite("2026-10-24T04:00:00Z")
    assert len(suite["cases"]) == 10
    assert sum(len(value["turns"]) for value in suite["cases"]) == 23
    assert {value["partition"] for value in suite["cases"]} == {"exposed_demo_engineering_regression"}
    assert suite["modelFixture"] == "demo-stability-v1"
    assert suite["purpose"] == "engineering_regression_only"
    events = [turn.get("clientAction") for case in suite["cases"] for turn in case["turns"]]
    assert events.count("adopt") == 2
    assert events.count("confirm_preference") == 3
    assert events.count("reopen") == 1


def test_empty_and_duplicate_observations_cannot_pass_collection_check():
    assert not _check([], "交互集合完全匹配")
    row = {"caseId": "D01", "turn": 1, "ok": True, "waitStatus": "settled"}
    assert not _check([row, row], "交互集合完全匹配")
    assert not _check([row, row], "实际交互结束:D01:1")


def test_old_help_message_cannot_count_as_current_reply():
    messages = [{"id": "old", "role": "assistant", "content": "查歌确认采用"}]
    row = {"caseId": "D01", "turn": 1, "conversation": {"messages": messages},
           "before": {"conversation": {"messages": messages}}, "recommendations": [], "catalogRequests": []}
    assert not _check([row], "帮助回答本轮产生且说明确认边界")
    row["conversation"] = {"messages": messages + [{"id": "new", "role": "assistant", "content": "查歌；保存需要确认；采用后播放"}]}
    assert _check([row], "帮助回答本轮产生且说明确认边界")


def test_fetching_target_without_display_is_not_lookup_success():
    row = {"caseId": "D02", "turn": 1, "recommendations": [],
           "catalogRequests": [{"path": "/search", "responseIds": [70005], "delivered": True}]}
    assert not _check([row], "歌名版本准确展示:D02")
    row["recommendations"] = [{"id": 70005, "version": "unknown"}]
    assert not _check([row], "歌名版本准确展示:D02")
    row["recommendations"][0]["version"] = "studio"
    assert _check([row], "歌名版本准确展示:D02")


def test_preview_or_model_claim_does_not_prove_playback():
    row = {"caseId": "D03", "turn": 1, "transcript": "已播放", "recommendations": [{"id": 70005}],
           "playback": {"queueCount": 0, "queueIds": [], "resolvedIds": []}}
    assert not _check([row], "明确播放目标真正生效")
    row["playback"].update(queueCount=1, queueIds=[70005], resolvedIds=[70005])
    assert _check([row], "明确播放目标真正生效")


@pytest.mark.parametrize("count,minutes,duration", [(5, 20, 240000), (4, 16, 240000)])
def test_exact_count_duration_and_authoritative_brief_all_required(count, minutes, duration):
    number = 1 if count == 5 else 2
    row = {"caseId": "D04", "turn": number,
           "recommendations": [{"id": index + 1, "durationMs": duration} for index in range(count)],
           "backend": {"task": {"status": "preview", "brief": {"targetTrackCount": count, "targetMinutes": minutes}}}}
    name = "曲数时长与正式任务一致:D04:" + str(number)
    assert _check([row], name)
    row["backend"]["task"]["brief"]["targetTrackCount"] = 0
    assert not _check([row], name)
    row["backend"]["task"]["brief"]["targetTrackCount"] = count
    row["recommendations"].pop()
    assert not _check([row], name)


def test_control_requires_real_original_pending_step_and_same_task():
    row = {"caseId": "D08", "turn": 1,
           "before": {"task": {"id": "original", "status": "waiting_tools", "pendingSteps": [{"id": "pending"}]}},
           "backend": {"task": {"id": "original", "status": "paused"}}}
    assert _check([row], "原任务控制生效:D08:1")
    row["before"]["task"]["pendingSteps"] = []
    assert not _check([row], "原任务控制生效:D08:1")
    row["before"]["task"]["pendingSteps"] = [{"id": "pending"}]
    row["backend"]["task"]["id"] = "another"
    assert not _check([row], "原任务控制生效:D08:1")


def test_history_requires_actual_display_before_recall():
    row = {"caseId": "D07", "turn": 1, "recommendations": [],
           "catalogRequests": [{"path": "/search", "delivered": True, "responseIds": [5008]}]}
    assert not _check([row], "历史前置作品实际展示:D07:1")
    row["recommendations"] = [{"id": 5008}]
    assert _check([row], "历史前置作品实际展示:D07:1")


def test_adoption_requires_backend_receipt_actual_queue_and_no_second_resolution():
    rows = [
        {"caseId": "D10", "turn": 1, "recommendations": [{"id": 1}, {"id": 2}]},
        {"caseId": "D10", "turn": 2, "playback": {"queueIds": [1, 2], "queueCount": 2},
         "backend": {"task": {"status": "adopted", "adoption": {"id": "receipt", "appliedAt": 123}}}},
        {"caseId": "D10", "turn": 3, "playback": {"queueIds": [1, 2], "queueCount": 2, "resolvedIds": []},
         "backend": {"task": {"status": "adopted", "adoption": {"id": "receipt", "appliedAt": 123}}}, "catalogRequests": []},
    ]
    assert _check(rows, "采用队列与回执生效:D10:2")
    assert _check(rows, "重复采用不重新解析或换回执")
    rows[1]["backend"]["task"]["status"] = "awaiting_apply"
    assert not _check(rows, "采用队列与回执生效:D10:2")
    rows[2]["catalogRequests"] = [{"path": "/song/url"}]
    assert not _check(rows, "重复采用不重新解析或换回执")
    del rows[2]["catalogRequests"]
    assert not _check(rows, "重复采用不重新解析或换回执")


def test_offline_arbitrary_demo_suite_cannot_enable_live_calls(tmp_path):
    with pytest.raises(runner.EvaluationError, match="engineering_suite_cannot_authorize_live_calls"):
        runner.prepare(tmp_path / "must-not-exist", mode="live", maximum=1,
                       authorization="不能用工程字典扩大真实调用入口", offline_suite=demo.demo_suite())
    assert not (tmp_path / "must-not-exist").exists()


@pytest.mark.asyncio
async def test_fixture_plan_returns_only_explicit_known_parameters():
    model = demo.DemoWiringModel(0)
    value = (await model.call("plan", {"instruction": "给我5首通勤歌，20分钟左右，只做预览。"}))["value"]
    assert value["targetTrackCount"] == 5
    assert value["targetMinutes"] == 20
    value = (await model.call("plan", {"instruction": "改成4首，16分钟左右，只做预览。", "currentBrief": value}))["value"]
    assert value["targetTrackCount"] == 4
    assert value["targetMinutes"] == 16
