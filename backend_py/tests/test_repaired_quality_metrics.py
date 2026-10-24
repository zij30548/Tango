"""核验质量指标的分母、未知账本和零Token，防止缺证据被包装成低成本成功。"""

from copy import deepcopy

import pytest

from evaluation.repaired_quality_scoring import _counts, provider_statistics, score


def _started(number=1, **overrides):
    """提供已持久预留的模型尝试；它与结束回执不是同一事实。"""
    return {"event": "started", "number": number, "role": "router", "phase": "evaluation", **overrides}


def _finished(number=1, **overrides):
    """提供完整的正常结束事实，供缺失和冲突回执反例独立修改。"""
    return {"event": "finished", "number": number, "status": "responded", "seconds": 1.5,
            "usage": {"prompt_tokens": 10, "completion_tokens": 5}, **overrides}


def test_started_without_finish_remains_charged_and_unknown():
    result = provider_statistics([_started()])
    assert result["attempts"] == 1
    assert result["outcomes"] == {"uncertain": 1}
    assert result["providerLatencySeconds"] == {"n": 0, "p50": None, "p95": None}
    assert result["tokens"]["unknownRequests"] == 1
    assert result["tokens"]["completeInputTokens"] is None
    assert result["tokens"]["completeOutputTokens"] is None


def test_duplicate_finishes_do_not_choose_fast_or_successful_receipt():
    result = provider_statistics([
        _started(), _finished(seconds=0.1),
        _finished(status="failed", seconds=9, usage={"prompt_tokens": 900, "completion_tokens": 0}),
    ])
    assert result["attempts"] == 1
    assert result["outcomes"] == {"uncertain": 1}
    assert result["providerLatencySeconds"]["n"] == 0
    assert result["tokens"]["knownRequests"] == 0
    assert result["tokens"]["unknownRequests"] == 1
    assert result["tokens"]["completeInputTokens"] is None


def test_zero_tokens_and_zero_elapsed_are_known_values():
    result = provider_statistics([
        _started(), _finished(seconds=0, usage={"prompt_tokens": 0, "completion_tokens": 0}),
    ])
    assert result["outcomes"] == {"responded": 1}
    assert result["providerLatencySeconds"] == {"n": 1, "p50": 0, "p95": 0}
    assert result["tokens"] == {
        "knownRequests": 1, "unknownRequests": 0,
        "knownInputTokens": 0, "knownOutputTokens": 0,
        "completeInputTokens": 0, "completeOutputTokens": 0,
    }


@pytest.mark.parametrize("usage", [
    None, {}, {"prompt_tokens": 0}, {"completion_tokens": 0},
    {"prompt_tokens": False, "completion_tokens": 0},
    {"prompt_tokens": 0, "completion_tokens": True},
    {"prompt_tokens": -1, "completion_tokens": 0},
    {"prompt_tokens": 1.0, "completion_tokens": 0},
])
def test_missing_or_invalid_usage_never_becomes_complete_zero_cost(usage):
    result = provider_statistics([_started(), _finished(usage=usage)])
    assert result["attempts"] == 1
    assert result["outcomes"] == {"responded": 1}
    assert result["tokens"]["unknownRequests"] == 1
    assert result["tokens"]["completeInputTokens"] is None
    assert result["tokens"]["completeOutputTokens"] is None


@pytest.mark.parametrize("seconds", [None, True, -1, float("nan"), float("inf"), "0"])
def test_unknown_or_invalid_latency_is_not_filled_with_zero(seconds):
    result = provider_statistics([_started(), _finished(seconds=seconds)])
    assert result["providerLatencySeconds"] == {"n": 0, "p50": None, "p95": None}
    assert result["attempts"] == 1


def test_setup_and_failed_roles_remain_in_same_attempt_denominator():
    events = [
        _started(1, role="router", phase="setup"), _finished(1),
        _started(2, role="planner"), _finished(2, status="failed", usage=None),
        _started(3, role="critic"),
    ]
    result = provider_statistics(events)
    assert result["attempts"] == 3
    assert result["outcomes"] == {"responded": 1, "failed": 1, "uncertain": 1}
    assert result["byRole"] == {"router": 1, "planner": 1, "critic": 1}
    assert result["byContextPhase"] == {"setup": 1, "evaluation": 2}
    assert result["tokens"]["knownRequests"] == 1
    assert result["tokens"]["unknownRequests"] == 2
    assert result["tokens"]["completeInputTokens"] is None


def test_unresolved_samples_stay_in_counts_and_block_point_rate():
    result = _counts(["pass", "fail", "unverified", "blocked", "not_run"])
    assert result["selected"] == 5
    assert result["pass"] == 1 and result["fail"] == 1 and result["unresolved"] == 3
    assert result["rate"] is None
    assert result["verifiedCoverage"] == pytest.approx(2 / 5)
    assert result["confirmedPassLowerBound"] == pytest.approx(1 / 5)
    assert result["possiblePassUpperBound"] == pytest.approx(4 / 5)


def test_empty_selection_does_not_report_perfect_completion():
    result = _counts([])
    assert result["selected"] == 0
    assert all(result[name] is None for name in (
        "rate", "verifiedCoverage", "confirmedPassLowerBound", "possiblePassUpperBound",
    ))


def _slot_sample():
    """构造不均衡的槽位类型：两个题名正确，唯一播放权限错误。"""
    cases, rows, setups = [], [], []
    for index, title in enumerate(("合成甲", "合成乙"), 1):
        identifier, message_id = f"M{index}", f"message_{index}"
        slots = [{"name": "title", "path": "actions.0.parameters.title", "value": title}]
        if index == 1:
            slots.append({"name": "play", "path": "actions.0.parameters.play", "value": False})
        text = "查找" + title
        turn = {"text": text, "expect": [], "semanticReference": {
            "acceptedIntentSets": [["song_lookup"]],
            "acceptedCapabilitySets": [["music.lookup"]], "slots": slots,
        }}
        cases.append({"id": identifier, "title": "合成指标题", "partition": "metrics_only", "user_turns": [turn]})
        result = {"messageId": message_id, "intents": ["song_lookup"], "actions": [
            {"id": "lookup", "capability": "music.lookup", "parameters": {"title": title, "play": True}},
        ]}
        conversation = {"id": identifier, "messages": [{"id": message_id, "role": "user", "content": text}]}
        rows.append({"caseId": identifier, "turn": 1, "text": text, "messageId": message_id,
                     "conversationId": identifier, "ok": True, "waitStatus": "settled", "before": {},
                     "conversation": deepcopy(conversation),
                     "routes": [{"turnId": message_id, "plan": deepcopy(result), "actions": {"lookup": "completed"}}],
                     "backend": {"authority": "owned_sqlite", "conversation": conversation,
                                 "route": {"status": "completed", "conversationId": identifier,
                                           "messageId": message_id, "result": result}}})
        setups.append({"id": identifier, "setupOk": True})
    return {"id": "metrics_only", "cases": cases}, {"observations": rows, "cases": setups}


def _score_slots(observations_change=None, eligible=True):
    """只验证统计口径；不声称这些合成观测来自原生产品执行。"""
    suite, observations = _slot_sample()
    if observations_change:
        observations_change(observations)
    return score(suite, observations, {}, {"sourceUnchanged": eligible}, {"mode": "live"})


def test_macro_does_not_hide_failed_permission_under_many_title_slots():
    result = _score_slots()
    assert result["slotMicro"]["selected"] == 3
    assert result["slotMicro"]["rate"] == pytest.approx(2 / 3)
    assert result["slotMacro"]["selectedTypes"] == 2
    assert result["slotMacro"]["rate"] == pytest.approx(1 / 2)
    assert result["slotByType"]["play"]["rate"] == 0


def test_missing_slot_turn_remains_in_micro_and_macro_bounds():
    result = _score_slots(lambda observations: observations["observations"].pop())
    assert result["slotMicro"]["selected"] == 3
    assert result["slotMicro"]["unresolved"] == 1
    assert result["slotMicro"]["rate"] is None
    assert result["slotMacro"]["rate"] is None
    assert result["slotMacro"]["confirmedPassLowerBound"] == pytest.approx(1 / 4)
    assert result["slotMacro"]["possiblePassUpperBound"] == pytest.approx(1 / 2)


def test_changed_source_invalidates_macro_point_and_interval():
    result = _score_slots(eligible=False)
    assert result["realQualityEligible"] is False
    assert result["slotMicro"]["rate"] is None
    assert result["slotMacro"]["rate"] is None
    assert result["slotMacro"]["confirmedPassLowerBound"] is None
    assert result["slotMacro"]["possiblePassUpperBound"] is None


def test_new_conversation_label_cannot_keep_the_old_conversation_identity():
    suite, observations = _slot_sample()
    suite["cases"] = suite["cases"][:1]
    observations["observations"] = observations["observations"][:1]
    observations["cases"] = observations["cases"][:1]
    turn = suite["cases"][0]["user_turns"][0]
    turn.update(text="新对话", clientAction="new_conversation", turnKind="local_command",
                semanticReference={"localCommand": True}, expect=[{"predicate": "context_absent"}])
    row = observations["observations"][0]
    row.update(text="新对话", clientAction="new_conversation", conversationId="会话旧", lookupContext={})
    row["before"]["conversationId"] = "会话旧"
    row["backend"]["conversation"]["id"] = "会话旧"
    report = score(suite, observations, {}, {"sourceUnchanged": True}, {"mode": "live"})
    assert report["sampleBusinessCompletion"]["pass"] == 0
