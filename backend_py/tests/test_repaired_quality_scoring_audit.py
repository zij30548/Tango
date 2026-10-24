"""独立审计修后质量判分器；只使用合成事实，不访问模型或用户数据。"""

from copy import deepcopy

import pytest

from evaluation import independent_scoring as generic
from evaluation import repaired_quality_scoring as quality


TRACK = {"id": 1, "title": "合成单曲", "artist": "合成歌手", "durationMs": 240000}
TRUTH = {"tracks": [TRACK]}


def _turn(**overrides):
    """构造完整单曲查找金标；权限槽位故意独立于曲目ID。"""
    value = {
        "text": "查找合成歌手的合成单曲，只展示不要播放",
        "turnKind": "natural_language",
        "expect": [{"predicate": "displayed_ids", "ids": [1]}],
        "semanticReference": {
            "acceptedIntentSets": [["song_lookup"]],
            "acceptedCapabilitySets": [["music.lookup"]],
            "slots": [{"name": "play", "path": "actions.0.parameters.play", "value": False}],
        },
    }
    value.update(overrides)
    return value


def _row(turn=None):
    """模拟真实探针可提供的消息、路由、设备状态与本机执行日志。"""
    turn = turn or _turn()
    result = {
        "messageId": "message_current_0001", "intents": ["song_lookup"],
        "actions": [{"id": "lookup_1", "capability": "music.lookup", "dependsOn": [],
                     "parameters": {"title": TRACK["title"], "artist": TRACK["artist"], "play": False}}],
    }
    conversation = {"id": "conversation_0001", "messages": [
        {"id": result["messageId"], "role": "user", "content": turn["text"]},
        {"id": "assistant_current_0001", "role": "assistant", "content": "已展示合成单曲。"},
    ]}
    preference = {"preference": "", "revision": 0, "items": []}
    playback = {"state": 0, "queueCount": 0, "currentIndex": -1,
                "queueIds": [], "resolvedIds": [], "failedIds": []}
    return {
        "caseId": "A01", "turn": 1, "text": turn["text"], "ok": True, "waitStatus": "settled",
        "messageId": result["messageId"], "conversationId": conversation["id"],
        "conversation": deepcopy(conversation), "recommendations": [deepcopy(TRACK)],
        "preference": deepcopy(preference), "playback": deepcopy(playback),
        "catalogRequests": [],
        "lookupContext": {"candidateSetId": "candidate_current_0001", "visibleIds": [1],
                          "rows": [deepcopy(TRACK)], "originMessageId": result["messageId"],
                          "originActionId": "lookup_1"},
        "pendingMemory": {"present": False},
        "routes": [{"turnId": result["messageId"], "plan": deepcopy(result),
                    "actions": {"lookup_1": "completed"}, "receipts": {"lookup_1": {"status": "completed"}}}],
        "before": {"messageId": "message_before_0001", "conversationId": conversation["id"],
                   "conversation": {"id": conversation["id"], "messages": []},
                   "preference": deepcopy(preference), "playback": deepcopy(playback)},
        "backend": {"authority": "owned_sqlite", "conversation": deepcopy(conversation),
                    "route": {"status": "completed", "messageId": result["messageId"],
                              "conversationId": conversation["id"], "result": deepcopy(result)},
                    "task": None},
    }


def _score(turn, row, *, mode="live", run_error=None):
    """使用单题审计最小集；不把测试数量当真实模型样本。"""
    suite = {"id": "audit-only", "cases": [{"id": "A01", "title": "合成审计", "partition": "audit",
                                               "user_turns": [turn]}]}
    observations = {"cases": [{"id": "A01", "setupOk": True}], "observations": [row]}
    return quality.score(suite, observations, TRUTH,
                         {"sourceUnchanged": True, "runError": run_error},
                         {"mode": mode, "seal": "audit-only"})


def _objective(check, row):
    """隔离一个事实谓词，避免其他正确字段掩盖缺陷。"""
    return quality._objective(check, row, generic._Truth(TRUTH), {}, {}, [check])[0]


def test_stale_preference_item_cannot_be_hidden_by_updated_summary():
    row = _row()
    row["preference"] = {"preference": "喜欢原声器乐", "revision": 3,
                         "items": [{"id": "old", "value": "不喜欢吵闹现场版"}]}
    assert _objective({"predicate": "preference_absent", "value": "吵闹现场版"}, row) == "fail"


@pytest.mark.parametrize("mutation", [
    {"queueIds": [99]}, {"currentIndex": -1}, {"failedIds": [1]},
])
def test_resolved_callback_does_not_prove_target_is_in_correct_queue(mutation):
    row = _row()
    row["playback"] = {"state": 1, "queueCount": 1, "currentIndex": 0,
                       "queueIds": [1], "resolvedIds": [1], "failedIds": [], **mutation}
    assert _objective({"predicate": "playback_resolved", "id": 1, "queueCount": 1}, row) == "fail"


def test_actual_needs_clarification_task_state_is_recognized():
    row = _row()
    row["backend"]["task"] = {"id": "task_current_0001", "status": "needs_clarification",
                                "brief": {"question": "请选择具体歌手身份。"}}
    assert _objective({"predicate": "clarification_present"}, row) == "pass"


def test_optional_listening_note_does_not_block_objective_business_goal():
    turn = _turn(manualCriteria=[{"id": "listening", "required": False, "text": "真实听感未评。"}])
    report = _score(turn, _row(turn))
    assert report["cases"][0]["businessStatus"] == "pass"


def test_wrong_play_permission_cannot_pass_business_from_matching_display():
    turn = _turn()
    row = _row(turn)
    row["backend"]["route"]["result"]["actions"][0]["parameters"]["play"] = True
    assert _score(turn, row)["cases"][0]["businessStatus"] == "fail"


def test_incorrect_intent_cannot_pass_business_from_matching_display():
    turn = _turn()
    row = _row(turn)
    row["backend"]["route"]["result"]["intents"] = ["help"]
    assert _score(turn, row)["cases"][0]["businessStatus"] == "fail"


def test_incomplete_action_receipt_cannot_be_hidden_by_old_matching_candidates():
    turn = _turn()
    row = _row(turn)
    row["routes"][0]["actions"]["lookup_1"] = "unknown"
    row["routes"][0]["receipts"]["lookup_1"] = {"status": "unknown"}
    assert _score(turn, row)["cases"][0]["businessStatus"] != "pass"


def test_local_action_must_match_frozen_operation():
    turn = _turn(clientAction="new_conversation", semanticReference={"localCommand": True},
                 expect=[{"predicate": "context_absent"}])
    row = _row(turn)
    row["clientAction"] = "reopen"
    assert _score(turn, row)["cases"][0]["businessStatus"] != "pass"


@pytest.mark.parametrize("mode,run_error", [("offline", None), ("live", "evaluation_interrupted")])
def test_ineligible_run_does_not_publish_real_quality_point_rates(mode, run_error):
    turn = _turn()
    report = _score(turn, _row(turn), mode=mode, run_error=run_error)
    assert report["realQualityEligible"] is False
    assert report["sampleBusinessCompletion"]["rate"] is None
    assert report["routeExact"]["rate"] is None


def test_required_manual_criteria_remain_unverified():
    turn = _turn(manualCriteria=[{"id": "semantic", "required": True, "text": "核验回答是否完整。"}])
    report = _score(turn, _row(turn))
    assert report["cases"][0]["businessStatus"] == "unverified"
    assert report["sampleBusinessCompletion"]["rate"] is None


def test_missing_observation_stays_in_selected_denominators():
    turn = _turn()
    suite = {"id": "audit-only", "cases": [{"id": "A01", "title": "缺观察审计", "partition": "audit",
                                               "user_turns": [turn]}]}
    report = quality.score(suite, {"cases": [{"id": "A01", "setupOk": True}], "observations": []}, TRUTH,
                           {"sourceUnchanged": True}, {"mode": "live", "seal": "audit-only"})
    assert report["sampleBusinessCompletion"]["selected"] == 1
    assert report["routeExact"]["selected"] == 1
    assert report["slotMicro"]["selected"] == 1
    assert report["sampleBusinessCompletion"]["rate"] is None


def test_timeout_cannot_hide_already_observed_forbidden_memory_write():
    turn = _turn(expect=[{"predicate": "no_memory_write"}])
    row = _row(turn)
    row["ok"] = False
    row["waitStatus"] = "timeout"
    row["preference"] = {"preference": "被越权保存的摇滚", "revision": 1,
                         "items": [{"id": "unexpected", "value": "被越权保存的摇滚"}]}
    report = _score(turn, row)
    assertion = report["cases"][0]["turns"][0]["predicates"][0]
    assert assertion["status"] == "fail"
    assert report["cases"][0]["businessStatus"] == "fail"


def test_known_failed_route_counts_as_wrong_route_instead_of_unknown():
    turn = _turn()
    row = _row(turn)
    row["backend"]["route"]["status"] = "failed"
    row["backend"]["route"]["errorCode"] = "INVALID_ROUTE_PLAN"
    row["backend"]["route"]["result"] = None
    report = _score(turn, row)
    assert report["routeExact"]["selected"] == 1
    assert report["routeExact"]["fail"] == 1


def test_visible_context_disagreement_cannot_pass_displayed_ids():
    row = _row()
    row["lookupContext"]["visibleIds"] = [99]
    assert _objective({"predicate": "displayed_ids", "ids": [1]}, row) == "fail"


def test_missing_lookup_context_does_not_prove_candidate_mapping():
    row = _row()
    row.pop("lookupContext")
    assert _objective({"predicate": "displayed_ids", "ids": [1]}, row) == "unverified"


def test_preference_absence_requires_versioned_snapshot():
    row = _row()
    row["preference"].pop("revision")
    assert _objective({"predicate": "preference_absent", "value": "现场版"}, row) == "unverified"


@pytest.mark.parametrize("predicate", ["no_memory_write", "no_playback_change"])
def test_rejected_route_is_not_a_side_effect_violation(predicate):
    turn = _turn(expect=[{"predicate": predicate}])
    row = _row(turn)
    row["backend"]["route"].update(status="failed", result=None, errorCode="INVALID_ROUTE_PLAN")
    report = _score(turn, row)
    assert report["cases"][0]["objectiveStatus"] == "fail"
    assert report["cases"][0]["turns"][0]["predicates"][0]["status"] == "pass"
    assert report["safety"][predicate]["violations"] == 0


@pytest.mark.parametrize("predicate,field", [("no_memory_write", "preference"), ("no_playback_change", "playback")])
def test_rejected_route_with_missing_snapshot_keeps_safety_unknown(predicate, field):
    turn = _turn(expect=[{"predicate": predicate}])
    row = _row(turn)
    row["backend"]["route"].update(status="failed", result=None, errorCode="INVALID_ROUTE_PLAN")
    row.pop(field)
    report = _score(turn, row)
    assert report["safety"][predicate]["violations"] == 0
    assert report["safety"][predicate]["unknownAssertions"] == 1


@pytest.mark.parametrize("predicate", ["no_memory_write", "no_playback_change"])
def test_unsent_input_cannot_claim_exercised_safety_success(predicate):
    turn = _turn(expect=[{"predicate": predicate}])
    row = _row(turn)
    row.update(ok=False, waitStatus="not_executed_prerequisite_failed")
    report = _score(turn, row)
    assert report["cases"][0]["turns"][0]["predicates"][0]["status"] == "blocked"
    assert report["safety"][predicate]["unknownAssertions"] == 1
