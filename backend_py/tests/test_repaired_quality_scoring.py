"""验证质量判分不会把旧路由、搜索命中或缺执行证据当成成功。"""

from copy import deepcopy

import pytest

from evaluation.repaired_quality_scoring import score


def sample():
    """构造一条具备真实形状的独立查找观测，用于评价器自身的反例。"""
    track = {"id": 1, "title": "题目甲", "artist": "歌手甲", "album": "专辑甲", "durationMs": 240000}
    turn = {"text": "查歌但先不播放", "expect": [{"predicate": "displayed_ids", "ids": [1]}],
            "semanticReference": {"acceptedIntentSets": [["song_lookup"]], "acceptedCapabilitySets": [["music.lookup"]],
                                  "slots": [{"name": "play", "path": "actions.0.parameters.play", "value": False}]}}
    suite = {"id": "repaired-quality-v1", "cases": [{"id": "N01", "title": "查找", "partition": "unseen_post_repair", "user_turns": [turn]}]}
    result = {"messageId": "m1", "intents": ["song_lookup"], "planner": "model", "actions": [
        {"id": "a1", "capability": "music.lookup", "parameters": {"play": False}}]}
    conversation = {"id": "c1", "messages": [{"id": "m1", "role": "user", "content": turn["text"]}]}
    row = {"caseId": "N01", "turn": 1, "text": turn["text"], "messageId": "m1", "conversationId": "c1", "ok": True,
           "lookupContext": {"visibleIds": [1]},
           "waitStatus": "settled", "recommendations": [track], "conversation": conversation, "before": {"messageId": "m0"},
           "routes": [{"turnId": "m1", "plan": result, "actions": {"a1": "completed"}}],
           "backend": {"authority": "owned_sqlite", "conversation": conversation,
                       "route": {"messageId": "m1", "conversationId": "c1", "status": "completed", "result": result}}}
    observations = {"cases": [{"id": "N01", "setupOk": True}], "observations": [row]}
    truth = {"tracks": [track]}
    report = {"sourceUnchanged": True, "providerLedger": {"attempts": 1}}
    manifest = {"mode": "live", "seal": "test-seal"}
    return suite, observations, truth, report, manifest


def scored(change=None):
    values = sample()
    if change:
        change(*values)
    return score(*values)


def test_complete_facts_are_scored_but_not_claimed_general_semantics():
    result = scored()
    assert result["objectiveTasks"]["pass"] == 1
    assert result["routeExact"]["rate"] == 1
    assert result["slotMicro"]["rate"] == 1
    assert result["realQualityEligible"] is True
    assert result["overallSemanticAccuracy"] is None


@pytest.mark.parametrize("mutation", [
    lambda row: row.update(ok=False),
    lambda row: row.update(waitStatus="timeout"),
    lambda row: row.update(messageId="m0"),
    lambda row: row.update(text="被换过的题面"),
    lambda row: row["backend"].update(authority="device_self_report"),
    lambda row: row["backend"]["route"].update(messageId="old"),
    lambda row: row["backend"]["route"].update(conversationId="other"),
    lambda row: row["backend"]["route"]["result"].update(messageId="old"),
    lambda row: row["backend"]["conversation"]["messages"].clear(),
    lambda row: row["backend"]["route"].update(status="failed"),
])
def test_incomplete_or_wrong_anchor_never_passes(mutation):
    result = scored(lambda s, o, *rest: mutation(o["observations"][0]))
    assert result["objectiveTasks"]["pass"] == 0
    assert result["routeExact"]["pass"] == 0
    assert result["selectedTasks"] == 1


@pytest.mark.parametrize("kind", ["missing", "duplicate", "failed_setup", "missing_setup", "duplicate_setup"])
def test_selection_denominator_is_never_reduced(kind):
    def change(suite, observed, *rest):
        if kind == "missing":
            observed["observations"].clear()
        elif kind == "duplicate":
            observed["observations"].append(deepcopy(observed["observations"][0]))
        elif kind == "failed_setup":
            observed["cases"][0]["setupOk"] = False
        elif kind == "missing_setup":
            observed["cases"].clear()
        else:
            observed["cases"].append(deepcopy(observed["cases"][0]))
    result = scored(change)
    assert result["objectiveTasks"]["selected"] == result["routeExact"]["selected"] == 1
    assert result["objectiveTasks"]["pass"] == 0
    assert result["objectiveTasks"]["possiblePassUpperBound"] == 1


def test_http_hit_without_display_is_failure_not_success():
    def change(suite, observed, *rest):
        suite["cases"][0]["user_turns"][0]["expect"] = [{"predicate": "lookup_title", "value": "题目甲"}]
        row = observed["observations"][0]
        row["recommendations"] = []
        row["catalogRequests"] = [{"status": 200, "responseIds": [1]}]
    assert scored(change)["objectiveTasks"]["fail"] == 1


@pytest.mark.parametrize("field,value", [("intents", ["song_lookup", "extra"]), ("intents", ["song_lookup", "song_lookup"]), ("actions", [])])
def test_extra_or_missing_routes_fail_exact_contract(field, value):
    result = scored(lambda s, o, *rest: o["observations"][0]["backend"]["route"]["result"].update({field: value}))
    assert result["routeExact"]["fail"] == 1


@pytest.mark.parametrize("value", [True, 0, "false", None])
def test_permission_slot_uses_exact_type_and_value(value):
    result = scored(lambda s, o, *rest: o["observations"][0]["backend"]["route"]["result"]["actions"][0]["parameters"].update(play=value))
    assert result["slotMicro"]["fail"] == 1


def test_manual_criterion_keeps_business_unresolved():
    result = scored(lambda s, *rest: s["cases"][0]["user_turns"][0].update(manualCriteria=[{"required": True, "text": "帮助是否诚实"}]))
    assert result["objectiveTasks"]["pass"] == 1
    assert result["sampleBusinessCompletion"]["rate"] is None
    assert result["sampleBusinessCompletion"]["unresolved"] == 1


@pytest.mark.parametrize("invalid", ["offline", "changed", "error", "invalid"])
def test_invalid_run_cannot_be_real_quality(invalid):
    def change(s, o, t, report, manifest):
        if invalid == "offline":
            manifest["mode"] = "offline"
        elif invalid == "changed":
            report["sourceUnchanged"] = False
        elif invalid == "error":
            report["runError"] = "timeout"
        else:
            report["invalidReason"] = "policy_changed"
    assert scored(change)["realQualityEligible"] is False


def test_unknown_predicate_does_not_pass():
    result = scored(lambda s, *rest: s["cases"][0]["user_turns"][0]["expect"].append({"predicate": "unimplemented"}))
    assert result["objectiveTasks"]["unresolved"] == 1


def test_confirm_and_reopen_are_not_model_route_samples():
    result = scored(lambda s, *rest: s["cases"][0]["user_turns"][0].update(clientAction="reopen"))
    assert result["selectedInteractions"] == 1
    assert result["routeExact"]["selected"] == 0


def test_duration_comes_from_displayed_tracks_and_truth():
    def change(s, o, *rest):
        s["cases"][0]["user_turns"][0]["expect"] = [{"predicate": "preview_duration_seconds", "value": 240}]
    assert scored(change)["objectiveTasks"]["pass"] == 1
    def forged(s, o, *rest):
        change(s, o)
        o["observations"][0]["recommendations"] = [{**o["observations"][0]["recommendations"][0], "durationMs": 120000}]
    assert scored(forged)["objectiveTasks"]["fail"] == 1


def test_resume_must_leave_paused_on_same_task():
    def change(s, o, *rest):
        s["cases"][0]["user_turns"][0]["expect"] = [{"predicate": "same_task_status", "value": "preview"}]
        row = o["observations"][0]
        row["before"]["task"] = {"id": "t1", "status": "paused"}
        row["backend"]["task"] = {"id": "t1", "status": "paused"}
    assert scored(change)["objectiveTasks"]["fail"] == 1


def test_missing_replay_is_not_attack_success():
    result = scored(lambda s, *rest: s["cases"][0]["user_turns"][0]["expect"].append({"predicate": "replay_cancelled"}))
    assert result["objectiveTasks"]["pass"] == 0
