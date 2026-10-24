"""修复回归的零付费边界、预写提案和缺证据失败；不评模型语义。"""

import pytest

from evaluation import independent as runner
from evaluation.repair_regression import RepairWiringModel, repair_suite, residual_suite, residual_supplemental_checks, supplemental_checks


def test_repair_suite_keeps_exposed_scope_and_declared_confirmations():
    suite = repair_suite("2026-10-24T04:00:00Z")
    assert suite["purpose"] == "engineering_regression_only"
    assert suite["modelFixture"] == "repair-regression-v1"
    assert len(suite["cases"]) == 10
    assert sum(len(case["turns"]) for case in suite["cases"]) == 15
    plan, blocked = runner.plan_for(suite, "owned-fixture-token")
    assert not blocked
    cases = {case["id"]: case for case in plan["cases"]}
    assert cases["R04"]["turns"][1]["clientAction"] == "confirm_preference"
    assert cases["R06"]["setupInstructions"][0]["waitFor"] == "waiting_tools"
    assert cases["R09"]["untrustedToolDescription"]
    assert cases["R08"]["turns"][1]["afterActions"] == [{"action": "replay_preference_confirmation"}]
    assert cases["R10"]["initialPreference"]


def test_engineering_suite_cannot_be_used_to_authorize_real_calls(tmp_path):
    with pytest.raises(runner.EvaluationError, match="engineering_suite_cannot_authorize_live_calls"):
        runner.prepare(tmp_path / "must-not-exist", mode="live", maximum=200, authorization="unused-test-only", offline_suite=repair_suite())
    assert not (tmp_path / "must-not-exist").exists()


@pytest.mark.parametrize("text,count", [("给我六首通勤音乐", 6), ("给我12首读书音乐", 12)])
async def test_fixed_plan_models_explicit_count_without_retaining_existing_tracks(text, count):
    result = await RepairWiringModel(1792814400000).call("plan", {"instruction": text})
    assert result["value"]["targetTrackCount"] == count
    assert result["value"]["retainFirst"] == 0


def test_missing_attack_execution_or_playback_evidence_cannot_pass():
    checks = supplemental_checks({"observations": []})
    assert len(checks) == 6
    assert all(check["passed"] is False for check in checks)


def test_configured_but_undelivered_injection_and_unexecuted_replay_fail():
    checks = supplemental_checks({"observations": [
        {"caseId": "R09", "turn": 1, "ok": True, "waitStatus": "settled", "catalogRequests": [
            {"delivered": False, "untrustedFields": ["prewritten attack"]}]},
        {"caseId": "R08", "turn": 2, "ok": True, "waitStatus": "settled", "preference": {"revision": 0},
         "clientEvents": [{"action": "replay_preference_confirmation", "ok": False, "observedOriginalState": "cancelled"}]},
    ]})
    assert not next(row for row in checks if row["name"].startswith("恶意未知字段"))["passed"]
    assert not next(row for row in checks if row["name"].startswith("旧确认确实"))["passed"]


def test_residual_suite_keeps_original_ten_and_original_failed_utterances():
    clock = "2026-10-24T04:00:00Z"
    suite = residual_suite(clock)
    assert suite["cases"][:10] == repair_suite(clock)["cases"]
    assert len(suite["cases"]) == 15
    assert sum(len(case["turns"]) for case in suite["cases"]) == 25
    original = runner.common.read_json(runner.PROJECT / runner.CANDIDATES)
    source = {case["id"]: case for case in original["cases"]}
    added = {case["id"]: case for case in suite["cases"]}
    assert added["R11"]["turns"][0]["text"] == source["P03"]["user_turns"][0]["text"]
    assert added["R12"]["turns"][0]["text"] == source["B01"]["user_turns"][0]["text"]
    plan, blocked = runner.plan_for(suite, "owned-fixture-token")
    assert not blocked and len(plan["cases"]) == 15


def test_residual_checks_fail_without_real_outcome_and_delivery_evidence():
    checks = residual_supplemental_checks({"observations": []})
    assert len(checks) == 12
    assert all(check["passed"] is False for check in checks)
