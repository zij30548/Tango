"""新题冻结与执行边界：不靠真实模型、原生桌面或用户数据获得通过。"""

import json
import os
from pathlib import Path

import pytest

from evaluation import independent as e
from evaluation import calibration as common


@pytest.fixture
def project(tmp_path, monkeypatch):
    root = tmp_path / "project"
    for name in (e.CANDIDATES, "scripts/evaluate-agent.py", "backend_py/pyproject.toml", "backend_py/uv.lock", e.PROBE):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("{}" if name == e.CANDIDATES else "fixture")
    os.chmod(root / e.PROBE, 0o700)
    def exported(probe, plan, output, **options):
        value = {"id": "test-fixture-only", "tracks": []}
        common.write_new(output, value)
        return value
    monkeypatch.setattr(e, "export_truth", exported)
    return root


@pytest.mark.parametrize("mode,maximum,authorization", [("live", 0, "user"), ("live", 200, ""), ("offline", 1, ""), ("live", True, "user"), ("live", -1, "user"), ("invalid", 0, "")])
def test_budget_authorization_cannot_be_inferred(project, tmp_path, mode, maximum, authorization):
    with pytest.raises(e.EvaluationError):
        e.prepare(tmp_path / "runs", mode=mode, maximum=maximum, authorization=authorization, project=project)
    assert not (tmp_path / "runs").exists()


def test_default_is_distinct_zero_provider_smoke(project, tmp_path):
    run = e.prepare(tmp_path / "runs", project=project)
    manifest, suite = e.verify_manifest(run, project=project)
    assert manifest["maximumProviderRequests"] == 0
    assert manifest["purpose"] == "engineering_smoke_only"
    assert (manifest["selectedTasks"], manifest["selectedInteractions"]) == (3, 5)
    assert suite["cases"][1]["turns"][1]["clientAction"] == "confirm_preference"
    assert run.stat().st_mode & 0o077 == 0
    assert (run / "control.token").stat().st_mode & 0o077 == 0


@pytest.mark.parametrize("mutation", ["budget", "suite", "binary", "source", "prompt", "migration", "truth", "plan"])
def test_frozen_mutation_is_rejected(project, tmp_path, mutation):
    if mutation == "prompt":
        path = project / "backend_py/tango_backend/prompts.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('{"before": true}')
    if mutation == "migration":
        path = project / "backend_py/tango_backend/migrations/001_initial_mysql.sql"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('SELECT 1;')
    run = e.prepare(tmp_path / "runs", project=project)
    if mutation == "budget":
        path = run / "manifest.json"
        value = json.loads(path.read_text()); value["maximumProviderRequests"] = 200
        path.write_text(json.dumps(value))
    elif mutation == "suite":
        path = run / "suite.json"
        value = json.loads(path.read_text()); value["cases"][0]["turns"][0]["text"] = "altered"
        path.write_text(json.dumps(value))
    elif mutation == "binary":
        (project / e.PROBE).write_text("changed executable")
    elif mutation in {"truth", "plan"}:
        (run / ("catalog-truth.json" if mutation == "truth" else "plan.json")).write_text("{}")
    elif mutation == "prompt":
        (project / "backend_py/tango_backend/prompts.json").write_text('{"after": true}')
    elif mutation == "migration":
        (project / "backend_py/tango_backend/migrations/001_initial_mysql.sql").write_text('SELECT 2;')
    else:
        (project / "scripts/evaluate-agent.py").write_text("changed source")
    with pytest.raises(e.EvaluationError):
        e.verify_manifest(run, project=project)


def test_local_events_do_not_send_descriptive_placeholder_to_model():
    suite = e.smoke_suite("2026-10-24T04:00:00Z")
    for case in suite["cases"]:
        case["turns"] = case["user_turns"]
    plan, blocked = e.plan_for(suite, "private-fixture-control")
    assert not blocked
    assert plan["cases"][1]["turns"][1] == {"clientAction": "confirm_preference", "confirmGuard": {"containsValues": ["粤语"]}}


def test_missing_preconditions_remain_explicit_not_success():
    suite = {"clock": "2026-10-24T04:00:00Z", "cases": [
        {"id": "known", "turns": [{"text": "help"}]},
        {"id": "missing", "initial_fixture_requirements": ["unknown_original_request"], "turns": [{"text": "retry"}]}]}
    plan, blocked = e.plan_for(suite, "private-control")
    assert [case["id"] for case in plan["cases"]] == ["known"]
    assert blocked == [{"id": "missing", "setupOk": False, "error": "fixture_not_implemented", "blockedFixtures": ["unknown_original_request"]}]


def test_same_execution_lock_cannot_be_reused(project, tmp_path):
    run = e.prepare(tmp_path / "runs", project=project)
    common.write_new(run / "execution-lock.json", {"attempt": "first"})
    with pytest.raises(FileExistsError):
        e.execute(run, project=project)


def test_unnamed_or_edited_human_review_cannot_invent_completion(project, tmp_path):
    run = e.prepare(tmp_path / "runs", project=project)
    review = run / "edited-review.json"
    common.write_new(review, {"reviewer": "", "reviewerKind": "human", "rows": []})
    with pytest.raises(e.EvaluationError, match="named_human"):
        e.import_review(run, review)
