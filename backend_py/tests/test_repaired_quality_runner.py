"""修后质量验收的协议、授权、分层和冻结边界；不调用模型或真实曲库。"""

import json
import os

import pytest

from evaluation import calibration as common
from evaluation import independent as runner
from evaluation import repaired_quality as quality


def _case(identifier, partition):
    """只有合成帮助消息，测试执行协议而非音乐语义。"""
    return {
        "id": identifier, "title": "协议测试 " + identifier, "partition": partition,
        "user_turns": [{"text": "你能做什么", "turnKind": "natural_language",
                        "expect": [{"predicate": "no_memory_write"}],
                        "semanticReference": {"acceptedIntentSets": [["help"]],
                                              "acceptedCapabilitySets": [["assistant.reply"]], "slots": []}}],
        "business_success_conditions": ["读取能力说明"],
        "critical_negative_assertions": ["不写偏好"],
    }


@pytest.fixture
def project(tmp_path, monkeypatch):
    root = tmp_path / "project"
    for name in (runner.CANDIDATES, "scripts/evaluate-agent.py", "scripts/evaluate-repaired-agent.py",
                 "backend_py/pyproject.toml", "backend_py/uv.lock", runner.PROBE):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("{}" if name == runner.CANDIDATES else "fixture")
    os.chmod(root / runner.PROBE, 0o700)
    fixtures = {"catalog_core": {"tracks": []}}
    for name, count in quality.PARTITION_COUNTS.items():
        partition = quality.PARTITIONS[name]
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        value = {"schemaVersion": 1, "clock": "2026-10-24T04:00:00Z", "fixture_definitions": fixtures,
                 "cases": [_case(("N" if count == 16 else "R") + str(number), partition) for number in range(count)]}
        path.write_text(json.dumps(value, ensure_ascii=False))
    (root / quality.PROTOCOL).write_text(json.dumps({"schemaVersion": 1, "protocolId": "tango-repaired-quality-v1"}))

    def exported(probe, plan, output, **options):
        value = {"id": "protocol-truth-only", "tracks": []}
        common.write_new(output, value)
        return value

    monkeypatch.setattr(runner, "export_truth", exported)
    return root


def _edit(project, relative, mutation):
    path = project / relative
    value = json.loads(path.read_text())
    mutation(value)
    path.write_text(json.dumps(value, ensure_ascii=False))


def test_offline_protocol_freezes_24_but_never_runs_fixture_model(project, tmp_path):
    run = quality.prepare(tmp_path / "runs", project=project)
    manifest, suite = runner.verify_manifest(run, project=project)
    assert manifest["maximumProviderRequests"] == 0
    assert manifest["suiteKey"] == quality.SUITE_KEY
    assert manifest["selectedTasks"] == 24
    assert suite["purpose"] == "frozen_protocol_only"
    assert suite["partitions"] == {"new_post_repair": 16, "exposed_regression": 8}
    assert "modelFixture" not in suite
    assert manifest["suiteSourceHashes"] == quality.suite_source_hashes(project=project)
    assert (run / "control.token").stat().st_mode & 0o077 == 0
    for execute in (quality.execute, runner.execute):
        with pytest.raises(runner.EvaluationError):
            execute(run, project=project)
    assert not (run / "execution-lock.json").exists()
    assert not (run / "provider-requests.jsonl").exists()


@pytest.mark.parametrize("mode,maximum,authorization", [
    ("live", 0, "本轮授权"), ("live", 200, ""), ("offline", 200, "本轮授权"),
    ("live", True, "本轮授权"), ("live", -1, "本轮授权"), ("live", 10001, "本轮授权"),
])
def test_no_historical_or_implicit_live_budget(project, tmp_path, mode, maximum, authorization):
    with pytest.raises(runner.EvaluationError):
        quality.prepare(tmp_path / "runs", mode=mode, maximum=maximum,
                        authorization=authorization, project=project)
    assert not (tmp_path / "runs").exists()


def test_explicit_live_prepare_does_not_read_config_or_execute(project, tmp_path, monkeypatch):
    from evaluation import calibration
    monkeypatch.setattr(calibration, "live_models", lambda *args: pytest.fail("准备协议不能读真实模型配置"))
    run = quality.prepare(tmp_path / "runs", mode="live", maximum=200,
                          authorization="本轮明确授权最多200次；失败和未知计数", project=project)
    manifest, suite = runner.verify_manifest(run, project=project)
    assert manifest["maximumProviderRequests"] == 200
    assert manifest["mode"] == "live"
    assert suite["purpose"] == "post_repair_quality_first_execution"
    assert not (run / "provider-requests.jsonl").exists()
    assert not (run / "execution-lock.json").exists()


def test_registered_whitelist_does_not_relax_arbitrary_offline_suite(project, tmp_path):
    with pytest.raises(runner.EvaluationError, match="engineering_suite_cannot_authorize_live_calls"):
        runner.prepare(tmp_path / "forbidden", mode="live", maximum=200, authorization="本轮授权",
                       project=project, offline_suite={"cases": []})
    with pytest.raises(runner.EvaluationError, match="unregistered_evaluation_suite"):
        runner.prepare(tmp_path / "forbidden", project=project, suite_key="any-custom-live-suite")
    with pytest.raises(runner.EvaluationError, match="registered_and_arbitrary_suite_cannot_mix"):
        runner.prepare(tmp_path / "forbidden", project=project, suite_key=quality.SUITE_KEY, offline_suite={})
    assert not (tmp_path / "forbidden").exists()


@pytest.mark.parametrize("source", quality.SOURCE_FILES)
def test_original_suite_file_mutation_invalidates_even_unchanged_merged_suite(project, tmp_path, source):
    run = quality.prepare(tmp_path / "runs", project=project)
    path = project / source
    path.write_bytes(path.read_bytes() + b"\n")
    with pytest.raises(runner.EvaluationError, match="frozen_suite_sources_changed"):
        runner.verify_manifest(run, project=project)


def test_new_entry_code_is_frozen(project, tmp_path):
    run = quality.prepare(tmp_path / "runs", project=project)
    (project / "scripts/evaluate-repaired-agent.py").write_text("changed CLI")
    with pytest.raises(runner.EvaluationError, match="frozen_sources_or_binary_changed"):
        runner.verify_manifest(run, project=project)


@pytest.mark.parametrize("mutation,error", [
    (lambda value: value["cases"].pop(), "quality_selection_count_changed"),
    (lambda value: value["cases"][0].update(id="含中文"), "invalid_or_duplicate_quality_case"),
    (lambda value: value["cases"][0].update(partition="exposed_regression"), "quality_partition_changed"),
    (lambda value: value["cases"][0]["user_turns"][0].update(expect=[]), "quality_turn_requires_text_and_criteria"),
    (lambda value: value["cases"][0]["user_turns"][0].update(clientAction="unsupported_button", turnKind="local_event"), "unsupported_quality_client_action"),
    (lambda value: value["cases"][0]["user_turns"][0].update(clientAction="confirm_preference"), "local_action_cannot_count_as_natural_language"),
    (lambda value: value["cases"][0]["user_turns"][0].update(afterActions=[{"action": "fabricate_success"}]), "unsupported_quality_after_action"),
])
def test_invalid_or_reclassified_cases_fail_before_private_run(project, tmp_path, mutation, error):
    _edit(project, quality.NEW_CASES, mutation)
    with pytest.raises(runner.EvaluationError, match=error):
        quality.prepare(tmp_path / "runs", project=project)
    assert not (tmp_path / "runs").exists()


def test_disagreeing_fixture_sources_and_symlinks_fail_closed(project, tmp_path):
    _edit(project, quality.EXPOSED_CASES, lambda value: value["fixture_definitions"].update(other="changed"))
    with pytest.raises(runner.EvaluationError, match="quality_fixture_sources_disagree"):
        quality.canonical_suite("offline", project=project)
    path = project / quality.NEW_CASES
    target = path.with_suffix(".private")
    path.rename(target)
    path.symlink_to(target)
    with pytest.raises(runner.EvaluationError, match="unsafe_or_missing_quality_suite_source"):
        quality.suite_source_hashes(project=project)


def test_local_events_are_not_sent_as_natural_language_and_setup_is_counted(project):
    def mutate(value):
        case = value["cases"][0]
        case.update(initialPreference="喜欢原声器乐", requiresFixtures=["confirmed_preferences"], setupInstructions=["推荐读书歌单"])
        case["user_turns"].append({"text": "本机确认偏好", "clientAction": "confirm_preference", "turnKind": "local_event",
                                   "expectedOperation": "set", "expect": [{"predicate": "preference_contains", "value": "原声器乐"}]})
    _edit(project, quality.NEW_CASES, mutate)
    suite = quality.canonical_suite("offline", project=project)
    plan, blocked = runner.plan_for(suite, "private-control")
    assert not blocked
    first = plan["cases"][0]
    assert first["initialPreference"] == "喜欢原声器乐"
    assert first["requiresFixtures"] == ["confirmed_preferences"]
    assert first["setupInstructions"] == ["推荐读书歌单"]
    assert first["turns"][1] == {"clientAction": "confirm_preference", "confirmGuard": {"containsValues": ["原声器乐"]}, "expectedOperation": "set"}
    estimate = quality.estimate(project=project)
    assert (estimate["selectedTasks"], estimate["selectedInteractions"], estimate["localInteractions"]) == (24, 25, 1)
    assert estimate["formalNaturalLanguageOpportunities"] == 24
    assert estimate["setupNaturalLanguageOpportunities"] == 1
    assert estimate["conservativePlanningRequests"] == 175
    assert estimate["realProviderRequests"] == estimate["realCatalogRequests"] == 0


def test_running_task_setup_keeps_native_wait_without_putting_control_fields_in_model_context(project):
    def mutate(value):
        value["cases"][0].update(
            setupInstructions=[{"text": "预览读书歌单", "waitFor": "waiting_tools", "timeoutMs": 90000}],
            catalogDelayMs=10000, requiresFixtures=["controllable_running_task"])
    _edit(project, quality.NEW_CASES, mutate)
    suite = quality.canonical_suite("offline", project=project)
    assert suite["cases"][0]["setupInstructions"] == ["预览读书歌单"]
    plan, _ = runner.plan_for(suite, "private-control")
    assert plan["cases"][0]["setupInstructions"] == [{"text": "预览读书歌单", "waitFor": "waiting_tools", "timeoutMs": 90000}]
    assert plan["cases"][0]["catalogDelayMs"] == 10000
    assert plan["cases"][0]["requiresFixtures"] == ["controllable_running_task"]


def test_freeze_includes_quality_tests_and_gate_definition(project, tmp_path):
    for name in ("backend_py/tests/test_repaired_quality_scoring.py", "scripts/verify-harness.py"):
        path = project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("frozen")
    run = quality.prepare(tmp_path / "runs", project=project)
    manifest, _ = runner.verify_manifest(run, project=project)
    assert "backend_py/tests/test_repaired_quality_scoring.py" in manifest["sources"]
    assert "scripts/verify-harness.py" in manifest["sources"]
    (project / "scripts/verify-harness.py").write_text("weakened")
    with pytest.raises(runner.EvaluationError, match="frozen_sources_or_binary_changed"):
        runner.verify_manifest(run, project=project)


def test_legacy_default_is_still_three_engineering_cases(project, tmp_path):
    run = runner.prepare(tmp_path / "legacy", project=project)
    manifest, suite = runner.verify_manifest(run, project=project)
    assert "suiteKey" not in manifest
    assert suite["purpose"] == "engineering_smoke_only"
    assert manifest["selectedTasks"] == 3


def test_new_review_never_inherits_legacy_unverified_overall_rate(tmp_path):
    with pytest.raises(runner.EvaluationError, match="quality_review_requires_evidence_calibrated_import"):
        quality.import_review(tmp_path, tmp_path / "unverified-human-form.json")


def test_legacy_review_cannot_bypass_new_quality_evidence_gate(project, tmp_path):
    run = quality.prepare(tmp_path / "runs", project=project)
    with pytest.raises(runner.EvaluationError, match="registered_quality_suite_requires_evidence_calibrated_review"):
        runner.import_review(run, run / "legacy-review.json")
