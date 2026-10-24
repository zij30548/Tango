"""具名复核的反伪造与缺证据边界；不访问模型、真实曲库或用户数据。"""

from copy import deepcopy
import json

import pytest

from evaluation import calibration as common
from evaluation import independent as runner
from evaluation import repaired_quality as quality
from evaluation.repaired_quality_scoring import _counts


@pytest.fixture
def review_run(tmp_path, monkeypatch):
    run = tmp_path / "owned-review"
    run.mkdir(mode=0o700)
    criterion = {"id": "explanation", "required": True, "text": "说明推荐依据与未知项"}
    suite = {"id": quality.SUITE_KEY, "cases": [
        {"id": "N01", "title": "需语义复核", "partition": "new_post_repair", "user_turns": [
            {"text": "解释依据", "manualCriteria": [criterion]}]},
        {"id": "N02", "title": "机器已核验", "partition": "new_post_repair", "user_turns": [
            {"text": "只读查歌", "manualCriteria": []}]},
        {"id": "R01", "title": "机器已证实失败", "partition": "exposed_regression", "user_turns": [
            {"text": "测试失败", "manualCriteria": [criterion]}]},
    ]}
    manifest = {"suiteKey": quality.SUITE_KEY, "mode": "live", "seal": "frozen-test-seal"}
    report = {"suiteId": quality.SUITE_KEY, "mode": "live", "manifestSeal": manifest["seal"],
              "realQualityEligible": True, "selectedTasks": 3, "selectedInteractions": 3,
              "humanReviewCompleted": False, "overallSemanticAccuracy": None, "cases": [], "partitions": {}}
    for case, objective, business in zip(suite["cases"], ("pass", "pass", "fail"), ("unverified", "pass", "fail")):
        report["cases"].append({"caseId": case["id"], "title": case["title"], "partition": case["partition"],
                                "objectiveStatus": objective, "businessStatus": business,
                                "turns": [{"turn": 1, "objectiveStatus": objective, "businessStatus": business,
                                           "manualCriteria": deepcopy(case["user_turns"][0]["manualCriteria"])}]})
    report["sampleBusinessCompletion"] = _counts([case["businessStatus"] for case in report["cases"]])
    for partition in ("new_post_repair", "exposed_regression"):
        report["partitions"][partition] = {"sampleBusinessCompletion": _counts([
            case["businessStatus"] for case in report["cases"] if case["partition"] == partition])}
    template = {"manifestSeal": manifest["seal"], "qualityReportDigest": common.sha(common.encode(report).encode()),
                "reviewerKind": "pending", "reviewer": "", "rows": []}
    for case in report["cases"]:
        for turn in case["turns"]:
            if turn["manualCriteria"]:
                template["rows"].append({"caseId": case["caseId"], "turn": 1, "criteria": turn["manualCriteria"],
                                          "objectiveStatus": turn["objectiveStatus"], "verdict": "pending", "reason": ""})
    common.write_new(run / "quality-report.json", report)
    common.write_new(run / "quality-review-template.json", template)

    def verified(path, *, check_sources):
        assert path == run and check_sources is False
        return manifest, suite

    monkeypatch.setattr(runner, "verify_manifest", verified)
    form = deepcopy(template)
    form.update(reviewerKind="human", reviewer="合成测试复核人")
    for row in form["rows"]:
        row.update(verdict="pass" if row["objectiveStatus"] == "pass" else "fail", reason="逐项核验本轮输出与冻结判据")
    return run, manifest, suite, report, template, form


def _submit(value, form=None):
    run, _, _, _, _, default = value
    path = run / "completed-form.json"
    common.write_new(path, default if form is None else form)
    return quality.import_review(run, path)


def _imported(run):
    return [common.read_json(path) for path in run.glob("quality-review-imported-*.json")]


def test_human_only_completes_eligible_rows_and_preserves_full_partition_denominators(review_run):
    run = review_run[0]
    originals = {name: (run / name).read_bytes() for name in ("quality-report.json", "quality-review-template.json")}
    result = _submit(review_run)
    assert result["humanReviewCompleted"] is True
    assert (result["selectedTasks"], result["confirmedPassed"], result["unresolved"]) == (3, 2, 0)
    assert result["businessCompletionRate"] == pytest.approx(2 / 3)
    assert result["partitions"]["new_post_repair"]["selected"] == 2
    assert result["partitions"]["new_post_repair"]["rate"] == 1
    assert result["partitions"]["exposed_regression"]["selected"] == 1
    assert result["partitions"]["exposed_regression"]["rate"] == 0
    for name, raw in originals.items():
        assert (run / name).read_bytes() == raw
    imported = _imported(run)[0]
    assert imported["cases"][0]["turns"][0]["businessStatus"] == "pass"
    assert imported["cases"][2]["businessStatus"] == "fail"
    assert imported["overallSemanticAccuracy"] is None
    assert imported["qualityReportDigest"] == review_run[4]["qualityReportDigest"]
    assert "reviewRows" not in result and "reviewer" not in result


@pytest.mark.parametrize("verdict", ["pending", "uncertain"])
def test_pending_and_uncertain_keep_null_point_estimate_and_bounds(review_run, verdict):
    review_run[5]["rows"][0].update(verdict=verdict, reason="语义仍需进一步核验")
    result = _submit(review_run)
    assert result["humanReviewCompleted"] is False
    assert result["businessCompletionRate"] is None
    assert result["unresolved"] == 1
    imported = _imported(review_run[0])[0]
    assert imported["sampleBusinessCompletion"]["confirmedPassLowerBound"] == pytest.approx(1 / 3)
    assert imported["sampleBusinessCompletion"]["possiblePassUpperBound"] == pytest.approx(2 / 3)


def test_agent_review_is_diagnostic_and_cannot_change_human_completion_or_business_rates(review_run):
    form = review_run[5]
    form.update(reviewerKind="agent_review", reviewer="测试开发Agent")
    result = _submit(review_run)
    imported = _imported(review_run[0])[0]
    assert result["reviewerKind"] == "agent_review"
    assert result["businessSummaryChanged"] is result["humanReviewCompleted"] is False
    assert imported["sampleBusinessCompletion"] == review_run[3]["sampleBusinessCompletion"]
    assert imported["partitions"] == review_run[3]["partitions"]
    assert imported["cases"] == review_run[3]["cases"]
    assert imported["reviewRows"][0]["verdict"] == "pass"


@pytest.mark.parametrize("mutation,error", [
    (lambda value: value.update(manifestSeal="other-run"), "quality_review_form_binding_changed"),
    (lambda value: value.update(qualityReportDigest="other-report"), "quality_review_form_binding_changed"),
    (lambda value: value.update(reviewer=""), "quality_review_named_human_or_agent_required"),
    (lambda value: value.update(reviewerKind="model_judge"), "quality_review_named_human_or_agent_required"),
    (lambda value: value.update(reviewerKind="pending"), "quality_review_named_human_or_agent_required"),
    (lambda value: value["rows"].pop(), "quality_review_required_rows_changed"),
    (lambda value: value["rows"].append(deepcopy(value["rows"][0])), "quality_review_required_rows_changed"),
    (lambda value: value["rows"].__setitem__(1, deepcopy(value["rows"][0])), "quality_review_required_rows_changed"),
    (lambda value: value["rows"][0].update(caseId="unselected"), "quality_review_required_rows_changed"),
    (lambda value: value["rows"][0].update(turn=True), "quality_review_required_rows_changed"),
    (lambda value: value["rows"][0].update(criteria=[]), "quality_review_frozen_criteria_changed"),
    (lambda value: value["rows"][0].update(objectiveStatus="fail"), "quality_review_frozen_criteria_changed"),
    (lambda value: value["rows"][0].update(reason=""), "quality_review_verdict_and_reason_required"),
    (lambda value: value["rows"][0].update(verdict="success"), "quality_review_verdict_and_reason_required"),
    (lambda value: value["rows"][0].update(verdict="pending", reason=""), "quality_review_verdict_and_reason_required"),
    (lambda value: value["rows"][0].update(extra="改写权限"), "quality_review_row_shape_changed"),
    (lambda value: value["rows"][1].update(verdict="pass"), "quality_review_missing_or_failed_facts_cannot_pass"),
])
def test_forged_or_incomplete_review_never_writes_a_completion(review_run, mutation, error):
    mutation(review_run[5])
    with pytest.raises(runner.EvaluationError, match=error):
        _submit(review_run)
    assert _imported(review_run[0]) == []


@pytest.mark.parametrize("objective", ["unverified", "blocked", "not_run"])
def test_missing_or_unexecuted_objective_facts_cannot_be_promoted_to_pass(review_run, objective):
    run, _, _, report, template, form = review_run
    report["cases"][0]["turns"][0]["objectiveStatus"] = objective
    digest = common.sha(common.encode(report).encode())
    template["qualityReportDigest"] = form["qualityReportDigest"] = digest
    template["rows"][0]["objectiveStatus"] = form["rows"][0]["objectiveStatus"] = objective
    (run / "quality-report.json").write_text(common.encode(report))
    (run / "quality-review-template.json").write_text(common.encode(template))
    with pytest.raises(runner.EvaluationError, match="quality_review_missing_or_failed_facts_cannot_pass"):
        _submit(review_run)


@pytest.mark.parametrize("mutation", [
    lambda manifest, report: manifest.update(mode="offline"),
    lambda manifest, report: manifest.update(suiteKey="legacy"),
    lambda manifest, report: report.update(realQualityEligible=False),
    lambda manifest, report: report.update(mode="offline"),
    lambda manifest, report: report.update(manifestSeal="other-run"),
])
def test_only_registered_valid_live_quality_reports_can_be_reviewed(review_run, mutation):
    run, manifest, _, report, _, _ = review_run
    mutation(manifest, report)
    (run / "quality-report.json").write_text(common.encode(report))
    with pytest.raises(runner.EvaluationError, match="quality_review_requires_valid_live_evidence"):
        _submit(review_run)


def test_changed_template_and_report_cannot_be_imported_as_original(review_run):
    run, _, _, report, template, _ = review_run
    template["rows"][0]["criteria"] = []
    (run / "quality-review-template.json").write_text(common.encode(template))
    with pytest.raises(runner.EvaluationError, match="quality_review_template_criteria_changed"):
        _submit(review_run)


def test_repeated_import_creates_new_exclusive_artifacts_without_overwrite(review_run):
    run = review_run[0]
    _submit(review_run)
    first = next(run.glob("quality-review-imported-*.json"))
    original = first.read_bytes()
    path = run / "second-form.json"
    common.write_new(path, review_run[5])
    quality.import_review(run, path)
    assert len(_imported(run)) == 2
    assert first.read_bytes() == original
