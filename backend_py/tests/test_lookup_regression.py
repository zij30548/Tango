"""候选上下文验收本身的分母、零付费与缺证据保护。"""

import pytest

from evaluation import independent as runner
from evaluation.lookup_regression import lookup_checks, lookup_suite


def test_lookup_suite_preserves_original_p03_and_declares_offline_scope():
    suite = lookup_suite("2026-10-24T04:00:00Z")
    original = runner.common.read_json(runner.PROJECT / runner.CANDIDATES)
    p03 = next(value for value in original["cases"] if value["id"] == "P03")
    l05 = next(value for value in suite["cases"] if value["id"] == "L05")
    assert l05["turns"][:2] == [
        {"text": turn["text"], "expect": [{"predicate": "no_memory_write"}, {"predicate": "no_playback_change"}]}
        for turn in p03["user_turns"]]
    assert suite["purpose"] == "engineering_regression_only"
    assert suite["modelFixture"] == "repair-regression-v1"
    assert len(suite["cases"]) == 13
    assert sum(len(value["turns"]) for value in suite["cases"]) == 34
    assert all(value["partition"] == "exposed_lookup_engineering_regression" for value in suite["cases"])
    unknown = next(value for value in suite["fixture_definitions"]["catalog_core"]["tracks"] if value["id"] == "track_13")
    assert unknown["version"] == "unknown"
    assert all(value["id"] != "track_13" for value in original["fixture_definitions"]["catalog_core"]["tracks"])
    plan, blocked = runner.plan_for(suite, "isolated-control-token")
    assert not blocked
    assert len(plan["cases"]) == 13
    assert next(value for value in plan["cases"] if value["id"] == "L10")["turns"][1] == {"clientAction": "new_conversation"}
    assert next(value for value in plan["cases"] if value["id"] == "L11")["turns"][1] == {"clientAction": "reopen"}


def test_lookup_engineering_suite_cannot_authorize_paid_calls(tmp_path):
    with pytest.raises(runner.EvaluationError, match="engineering_suite_cannot_authorize_live_calls"):
        runner.prepare(tmp_path / "forbidden", mode="live", maximum=200,
                       authorization="not-a-new-user-budget", offline_suite=lookup_suite())
    assert not (tmp_path / "forbidden").exists()


def test_missing_interactions_and_final_state_fail_all_checks():
    checks = lookup_checks({"observations": []})
    assert len(checks) > 100
    assert all(value["passed"] is False for value in checks)
    ended = [value for value in checks if value["name"].startswith("实际交互结束:")]
    assert len(ended) == 34


def test_same_visible_song_without_current_completed_route_is_not_refine_success():
    checks = lookup_checks({"observations": [{
        "caseId": "L01", "turn": 2, "ok": True, "waitStatus": "settled", "messageId": "second-turn",
        "recommendations": [{"id": 70002}], "lookupContext": {"visibleIds": [70002]},
        "routes": [{"turnId": "old-turn", "plan": {"actions": [{"id": "old-action", "capability": "music.refine"}]},
                    "actions": {"old-action": "completed"}}],
    }]})
    assert next(value for value in checks if value["name"] == "真实候选筛选:L01:2")["passed"] is False


def test_old_clarification_text_cannot_count_as_current_reply():
    message = {"id": "old", "role": "assistant", "content": "请确认歌手，声线未知"}
    checks = lookup_checks({"observations": [{
        "caseId": "L06", "turn": 2, "messageId": "current", "recommendations": [{"id": 70002}, {"id": 70003}],
        "before": {"conversation": {"messages": [message]}}, "conversation": {"messages": [message]},
        "routes": [{"turnId": "current", "plan": {"actions": [{"id": "reply", "capability": "assistant.reply",
            "parameters": {"kind": "clarify"}}]}, "actions": {"reply": "completed"}}],
    }]})
    assert next(value for value in checks if value["name"] == "无依据条件保留原候选并澄清:L06:2")["passed"] is False


def test_source_freeze_includes_new_acceptance_entry(tmp_path):
    for name in (runner.CANDIDATES, "scripts/evaluate-agent.py", "backend_py/pyproject.toml", "backend_py/uv.lock",
                 "scripts/check-lookup-context.py"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixed-acceptance-input", encoding="utf-8")
    old = runner.source_hashes(tmp_path)
    assert "scripts/check-lookup-context.py" in old
    (tmp_path / "scripts/check-lookup-context.py").write_text("changed-acceptance-code", encoding="utf-8")
    assert runner.source_hashes(tmp_path) != old
