"""修复后的真实质量验收入口；先冻结协议，新增付费运行须单独授权。"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys

from . import calibration as common
from . import independent as runner


SUITE_KEY = "repaired-quality-v1"
NEW_CASES = "docs/evaluation-data/repaired-quality-v1-new.json"
EXPOSED_CASES = "docs/evaluation-data/repaired-quality-v1-exposed.json"
PROTOCOL = "docs/evaluation-data/repaired-quality-v1-protocol.json"
CASE_FILES = (NEW_CASES, EXPOSED_CASES)
SOURCE_FILES = (*CASE_FILES, PROTOCOL)
PARTITIONS = {NEW_CASES: "new_post_repair", EXPOSED_CASES: "exposed_regression"}
PARTITION_COUNTS = {NEW_CASES: 16, EXPOSED_CASES: 8}
CLIENT_ACTIONS = {
    "confirm_preference", "cancel_preference", "lock_first_three", "adopt", "resume",
    "continue_pending", "undo", "new_conversation", "choose_artist_1", "reopen",
}
AFTER_ACTIONS = {"confirm_preference", "replay_preference_confirmation"}


def _source(relative, project):
    """只读取登记在仓库内的合成题集，不接收任意配置或用户数据路径。"""
    path = project / relative
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
        raise runner.EvaluationError("unsafe_or_missing_quality_suite_source")
    raw = path.read_bytes()
    try:
        value = common.read_json(path)
    except (UnicodeDecodeError, ValueError):
        raise runner.EvaluationError("invalid_quality_suite_source") from None
    if not isinstance(value, dict):
        raise runner.EvaluationError("invalid_quality_suite_source")
    return value, common.sha(raw)


def _gold_contract(turn):
    """付费前检查判据形状；未知谓词仍留待判分为不可核验，不伪造通过。"""
    field_path = re.compile(r"(?:[A-Za-z_][A-Za-z0-9_]*|[0-9]+)(?:\.(?:[A-Za-z_][A-Za-z0-9_]*|[0-9]+))*")

    def valid_path(value):
        return isinstance(value, str) and len(value) <= 300 and field_path.fullmatch(value) is not None

    reference = turn.get("semanticReference", {})
    if not isinstance(reference, dict) or ("localCommand" in reference and type(reference["localCommand"]) is not bool):
        raise runner.EvaluationError("invalid_quality_semantic_reference")
    if not turn.get("clientAction") and not reference.get("localCommand"):
        for key in ("acceptedIntentSets", "acceptedCapabilitySets"):
            groups = reference.get(key)
            if (not isinstance(groups, list) or not groups
                    or any(not isinstance(group, list) or not group
                           or any(not isinstance(item, str) or not item.strip() for item in group) for group in groups)):
                raise runner.EvaluationError("invalid_quality_allowed_route_sets")
    slots = reference.get("slots", [])
    if not isinstance(slots, list):
        raise runner.EvaluationError("invalid_quality_slots")
    for slot in slots:
        if (not isinstance(slot, dict) or not isinstance(slot.get("name"), str) or not slot["name"].strip()
                or not valid_path(slot.get("path")) or ("value" in slot) == ("alternatives" in slot)
                or ("alternatives" in slot and (not isinstance(slot["alternatives"], list) or not slot["alternatives"]))
                or ("normalize" in slot and type(slot["normalize"]) is not bool)):
            raise runner.EvaluationError("invalid_quality_slot_contract")
    manual = turn.get("manualCriteria", turn.get("manual_criteria", []))
    if not isinstance(manual, list):
        raise runner.EvaluationError("invalid_quality_manual_criteria")
    for item in manual:
        if isinstance(item, str):
            valid = bool(item.strip())
        else:
            valid = (isinstance(item, dict) and isinstance(item.get("text"), str) and bool(item["text"].strip())
                     and ("required" not in item or type(item["required"]) is bool))
        if not valid:
            raise runner.EvaluationError("invalid_quality_manual_criteria")
    for check in turn["expect"]:
        if not isinstance(check, dict) or not isinstance(check.get("predicate"), str) or not check["predicate"].strip():
            raise runner.EvaluationError("invalid_quality_predicate_contract")
        name = check["predicate"]
        if name == "task_field" and (not valid_path(check.get("path")) or "value" not in check):
            raise runner.EvaluationError("invalid_quality_task_field_contract")
        if name == "displayed_ids" and (not isinstance(check.get("ids"), list) or any(
                (type(value) is not int and not isinstance(value, str)) or isinstance(value, str) and not value.strip()
                for value in check["ids"])):
            raise runner.EvaluationError("invalid_quality_displayed_ids_contract")
        if name in ("candidate_count", "window_days") and (type(check.get("value")) is not int or check["value"] < 0):
            raise runner.EvaluationError("invalid_quality_numeric_predicate_contract")
        if name == "preview_duration_seconds" and (type(check.get("value")) not in (int, float) or check["value"] < 0
                or type(check.get("tolerance", 0)) not in (int, float) or check.get("tolerance", 0) < 0):
            raise runner.EvaluationError("invalid_quality_numeric_predicate_contract")
        if name == "same_task_status" and (not isinstance(check.get("value"), str) or not check["value"].strip()
                or ("requireSame" in check and type(check["requireSame"]) is not bool)
                or ("baseline_turn" in check and (type(check["baseline_turn"]) is not int or check["baseline_turn"] <= 0))):
            raise runner.EvaluationError("invalid_quality_task_status_contract")


def suite_source_hashes(*, project=runner.PROJECT):
    """题目、判据和合成夹具原文件一起封存，格式调整也会使旧运行失效。"""
    return {relative: _source(relative, project)[1] for relative in SOURCE_FILES}


def canonical_suite(mode, *, project=runner.PROJECT):
    """固定合并16道新题和8道暴露回归；离线只准备协议，不跑预写答案。"""
    if mode not in ("offline", "live"):
        raise runner.EvaluationError("invalid_quality_mode")
    cases, documents, seen = [], {}, set()
    fixtures = None
    for relative in CASE_FILES:
        document, _ = _source(relative, project)
        documents[relative] = document
        values = document.get("cases")
        if not isinstance(values, list) or len(values) != PARTITION_COUNTS[relative]:
            raise runner.EvaluationError("quality_selection_count_changed")
        declared_fixtures = document.get("fixture_definitions")
        if declared_fixtures is not None:
            if not isinstance(declared_fixtures, dict) or not declared_fixtures:
                raise runner.EvaluationError("quality_fixture_definition_required")
            if fixtures is not None and declared_fixtures != fixtures:
                raise runner.EvaluationError("quality_fixture_sources_disagree")
            fixtures = declared_fixtures
        for raw_case in values:
            if not isinstance(raw_case, dict):
                raise runner.EvaluationError("invalid_quality_case")
            case = common.clone(raw_case)
            identifier = case.get("id")
            if (not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", identifier)
                    or identifier in seen or not isinstance(case.get("title"), str) or not case["title"]):
                raise runner.EvaluationError("invalid_or_duplicate_quality_case")
            seen.add(identifier)
            if case.get("partition") not in (None, PARTITIONS[relative]):
                raise runner.EvaluationError("quality_partition_changed")
            case["partition"] = PARTITIONS[relative]
            turns = case.get("user_turns")
            if not isinstance(turns, list) or not 1 <= len(turns) <= 30:
                raise runner.EvaluationError("invalid_quality_turn_count")
            for turn in turns:
                if (not isinstance(turn, dict) or not isinstance(turn.get("text"), str)
                        or not 0 < len(turn["text"]) <= 1000
                        or not isinstance(turn.get("expect"), list) or not turn["expect"]):
                    raise runner.EvaluationError("quality_turn_requires_text_and_criteria")
                _gold_contract(turn)
                if turn.get("clientAction") and turn.get("turnKind") == "natural_language":
                    raise runner.EvaluationError("local_action_cannot_count_as_natural_language")
                if turn.get("clientAction") and turn["clientAction"] not in CLIENT_ACTIONS:
                    raise runner.EvaluationError("unsupported_quality_client_action")
                after_actions = turn.get("afterActions", [])
                if not isinstance(after_actions, list):
                    raise runner.EvaluationError("invalid_quality_after_actions")
                for action in after_actions:
                    name = action if isinstance(action, str) else action.get("action") if isinstance(action, dict) else None
                    if name not in AFTER_ACTIONS:
                        raise runner.EvaluationError("unsupported_quality_after_action")
                if turn.get("expectedOperation") not in (None, "set", "clear"):
                    raise runner.EvaluationError("invalid_quality_confirmation_operation")
                if turn.get("timeoutMs") is not None and (type(turn["timeoutMs"]) is not int or not 1000 <= turn["timeoutMs"] <= 180000):
                    raise runner.EvaluationError("invalid_quality_turn_timeout")
            case["turns"] = common.clone(turns)
            if "setupInstructions" in case:
                source_setup = case["setupInstructions"]
                if not isinstance(source_setup, list) or len(source_setup) > 20:
                    raise runner.EvaluationError("invalid_quality_setup_instructions")
                setup = [value if isinstance(value, str) else value.get("text")
                         if isinstance(value, dict) else None for value in source_setup]
                if any(not isinstance(value, str) or not 0 < len(value) <= 1000 for value in setup):
                    raise runner.EvaluationError("invalid_quality_setup_instructions")
                runtime_setup = case.get("runtimeSetupInstructions")
                if runtime_setup is not None:
                    if not isinstance(runtime_setup, list):
                        raise runner.EvaluationError("invalid_quality_runtime_setup")
                    runtime_texts = [value if isinstance(value, str) else value.get("text")
                                     if isinstance(value, dict) else None for value in runtime_setup]
                    if runtime_texts != setup:
                        raise runner.EvaluationError("quality_setup_sources_disagree")
                else:
                    case["runtimeSetupInstructions"] = common.clone(source_setup)
                # 后台上下文只登记原句；原生探针另保留等待状态和超时，不把它们送入模型。
                case["setupInstructions"] = setup
            if not case.get("business_success_conditions") or not case.get("critical_negative_assertions"):
                raise runner.EvaluationError("quality_business_and_safety_criteria_required")
            cases.append(case)
    if fixtures is None:
        raise runner.EvaluationError("quality_fixture_definition_required")
    protocol, _ = _source(PROTOCOL, project)
    if protocol.get("protocolId") != "tango-repaired-quality-v1" or protocol.get("schemaVersion") != 1:
        raise runner.EvaluationError("quality_protocol_version_changed")
    declared_clock = documents[NEW_CASES].get("clock")
    clock = declared_clock or datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    try:
        datetime.fromisoformat(clock.replace("Z", "+00:00"))
    except (ValueError, TypeError, AttributeError):
        raise runner.EvaluationError("invalid_quality_semantic_clock") from None
    return {
        "id": SUITE_KEY, "suite_id": SUITE_KEY,
        "purpose": "frozen_protocol_only" if mode == "offline" else "post_repair_quality_first_execution",
        "clock": clock, "fixture_definitions": common.clone(fixtures), "cases": cases,
        "partitions": {"new_post_repair": 16, "exposed_regression": 8},
        "qualityProtocol": protocol,
        "authorshipDisclosure": {
            "newCases": documents[NEW_CASES].get("authorshipDisclosure", documents[NEW_CASES].get("provenance", {})),
            "regressions": "8道已暴露回归；不能算独立新题。",
            "notRepresentativeTraffic": True,
        },
        "evaluationBoundaries": {
            "realCatalog": False, "listeningQuality": False, "modelJudge": False,
            "offlineSemanticEvidence": False, "automaticRetry": False,
            "allProviderRolesShareBudget": True, "failuresAndUncertainCount": True,
        },
    }


def estimate(*, project=runner.PROJECT):
    """估计请求机会和保守规划量；不读取密钥，也不把估计当硬限或金额。"""
    suite = canonical_suite("offline", project=project)
    turns = [turn for case in suite["cases"] for turn in case["turns"]]
    formal = sum(not turn.get("clientAction") and turn.get("turnKind", "natural_language") == "natural_language" for turn in turns)
    textual_commands = sum(not turn.get("clientAction") and turn.get("turnKind", "natural_language") != "natural_language" for turn in turns)
    setup = sum(len(case.get("runtimeSetupInstructions", [])) for case in suite["cases"])
    local = len(turns) - formal
    expected_task_turns = 0
    for turn in turns:
        reference = turn.get("semanticReference", {})
        capability_sets = reference.get("acceptedCapabilitySets", [])
        has_task = any("music.task" in values for values in capability_sets)
        has_resume = any("task.control" in values for values in capability_sets) and any(
            slot.get("name") == "action" and slot.get("value") == "resume" for slot in reference.get("slots", []))
        has_client_resume = turn.get("clientAction") in ("choose_artist_1", "resume", "continue_pending", "lock_first_three")
        expected_task_turns += bool(has_task or has_resume or has_client_resume)
    # 前置输入均按可能启动一个任务run保守预留；确认、重开等本机事件不冒充模型意图题。
    eligible_runs = expected_task_turns + setup
    return {
        "suiteKey": SUITE_KEY, "selectedTasks": len(suite["cases"]), "selectedInteractions": len(turns),
        "partitions": suite["partitions"], "formalNaturalLanguageOpportunities": formal,
        "localInteractions": local, "setupNaturalLanguageOpportunities": setup,
        "explicitTextualCommandInteractions": textual_commands,
        "routingOpportunityEstimate": formal + setup,
        "expectedEligibleTaskRuns": eligible_runs,
        "expectedPathPlanningRequests": formal + setup + eligible_runs * 6,
        "conservativePlanningRequests": (formal + setup + textual_commands) * 7,
        "planningAssumption": "预期路径按路由机会数加预写允许行为中的任务run数乘6；另给全部文本输入按7倍的极保守规划值。确定性路由可能不调用模型，模型分流可能偏离预期。两者均不是实际消耗、金额或完整路径上界承诺；所有角色仍受单独授权硬限约束。",
        "realProviderRequests": 0, "realCatalogRequests": 0,
        "authorizationRequiredBeforeLive": True, "automaticRetry": False,
    }


def prepare(parent, *, mode="offline", maximum=0, authorization="", project=runner.PROJECT):
    """每次准备建立新私有目录；历史预算和离线协议都不能变成可续跑的付费目录。"""
    if type(maximum) is not int or not 0 <= maximum <= 10000:
        raise runner.EvaluationError("invalid_quality_shared_budget")
    return runner.prepare(parent, mode=mode, maximum=maximum, authorization=authorization,
                          project=project, suite_key=SUITE_KEY)


def execute(run, *, project=runner.PROJECT):
    """执行仅限本版本的新live目录；沿用真实Qt链路、共享账本和单次执行锁。"""
    manifest, _ = runner.verify_manifest(run, project=project)
    if manifest.get("suiteKey") != SUITE_KEY or manifest.get("mode") != "live":
        raise runner.EvaluationError("quality_execution_requires_new_live_run")
    report = runner.execute(run, project=project)
    from .repaired_quality_scoring import summarize
    summarize(run)
    return report


def import_review(run, review_path):
    """具名复核只补充待审语义，不能抹去机器失败、缺证据或未执行的事实。"""
    try:
        manifest, suite = runner.verify_manifest(run, check_sources=False)
        report = common.read_json(run / "quality-report.json")
        template = common.read_json(run / "quality-review-template.json")
    except OSError:
        raise runner.EvaluationError("quality_review_requires_evidence_calibrated_import") from None
    if (not isinstance(report, dict) or not isinstance(template, dict)
            or manifest.get("suiteKey") != SUITE_KEY or manifest.get("mode") != "live"
            or report.get("realQualityEligible") is not True or report.get("mode") != "live"
            or report.get("manifestSeal") != manifest.get("seal") or report.get("suiteId") != suite.get("id")):
        raise runner.EvaluationError("quality_review_requires_valid_live_evidence")
    digest = common.sha(common.encode(report).encode())
    if (template.get("qualityReportDigest") != digest or template.get("manifestSeal") != manifest["seal"]
            or template.get("reviewerKind") != "pending" or template.get("reviewer") != ""):
        raise runner.EvaluationError("quality_review_template_binding_changed")

    def aggregate(statuses):
        """任一已证实失败优先；未知不得被其他轮的成功覆盖。"""
        return "fail" if "fail" in statuses else "pass" if statuses and all(value == "pass" for value in statuses) else "unverified"

    def counts(statuses):
        """全部选定任务保留分母，未决时只有上下界，没有完整点值。"""
        total, passed, failed = len(statuses), statuses.count("pass"), statuses.count("fail")
        unresolved = total - passed - failed
        return {"selected": total, "pass": passed, "fail": failed, "unresolved": unresolved,
                "statuses": {value: statuses.count(value) for value in sorted(set(statuses))},
                "rate": passed / total if total and not unresolved else None,
                "verifiedCoverage": (passed + failed) / total if total else None,
                "confirmedPassLowerBound": passed / total if total else None,
                "possiblePassUpperBound": (passed + unresolved) / total if total else None}

    selected = suite.get("cases", [])
    reported_cases = report.get("cases")
    if (not isinstance(reported_cases, list) or len(reported_cases) != len(selected)
            or report.get("selectedTasks") != len(selected)
            or report.get("selectedInteractions") != sum(len(case["user_turns"]) for case in selected)
            or not isinstance(report.get("partitions"), dict)
            or set(report["partitions"]) != {case["partition"] for case in selected}):
        raise runner.EvaluationError("quality_review_report_selection_changed")
    expected_rows, keys = [], set()
    for case, actual in zip(selected, reported_cases):
        if (not isinstance(actual, dict) or actual.get("caseId") != case["id"]
                or actual.get("title") != case["title"] or actual.get("partition") != case["partition"]
                or not isinstance(actual.get("turns"), list) or len(actual["turns"]) != len(case["user_turns"])):
            raise runner.EvaluationError("quality_review_report_selection_changed")
        for number, (turn, outcome) in enumerate(zip(case["user_turns"], actual["turns"]), 1):
            manual = turn.get("manualCriteria", turn.get("manual_criteria", []))
            required = [item for item in manual if not isinstance(item, dict) or item.get("required", True)]
            if (not isinstance(outcome, dict) or type(outcome.get("turn")) is not int or outcome["turn"] != number
                    or outcome.get("manualCriteria") != required
                    or outcome.get("objectiveStatus") not in ("pass", "fail", "unverified", "blocked", "not_run")):
                raise runner.EvaluationError("quality_review_report_criteria_changed")
            if required:
                key = (case["id"], number)
                if key in keys:
                    raise runner.EvaluationError("quality_review_duplicate_selection")
                keys.add(key)
                expected_rows.append({"caseId": case["id"], "turn": number, "criteria": required,
                                      "objectiveStatus": outcome["objectiveStatus"], "verdict": "pending", "reason": ""})
    if template.get("rows") != expected_rows:
        raise runner.EvaluationError("quality_review_template_criteria_changed")
    form = common.read_json(review_path)
    if (not isinstance(form, dict) or form.get("manifestSeal") != manifest["seal"]
            or form.get("qualityReportDigest") != digest):
        raise runner.EvaluationError("quality_review_form_binding_changed")
    reviewer, kind = form.get("reviewer"), form.get("reviewerKind")
    if (kind not in ("human", "agent_review") or not isinstance(reviewer, str)
            or not reviewer.strip() or len(reviewer) > 100):
        raise runner.EvaluationError("quality_review_named_human_or_agent_required")
    rows = form.get("rows")
    if not isinstance(rows, list) or len(rows) != len(expected_rows):
        raise runner.EvaluationError("quality_review_required_rows_changed")
    expected = {(row["caseId"], row["turn"]): row for row in expected_rows}
    reviewed = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"caseId", "turn", "criteria", "objectiveStatus", "verdict", "reason"}:
            raise runner.EvaluationError("quality_review_row_shape_changed")
        if not isinstance(row["caseId"], str) or type(row["turn"]) is not int:
            raise runner.EvaluationError("quality_review_required_rows_changed")
        key = (row["caseId"], row["turn"])
        target = expected.get(key)
        if key in reviewed or target is None:
            raise runner.EvaluationError("quality_review_required_rows_changed")
        if row["criteria"] != target["criteria"] or row["objectiveStatus"] != target["objectiveStatus"]:
            raise runner.EvaluationError("quality_review_frozen_criteria_changed")
        if (row["verdict"] not in ("pass", "fail", "pending", "uncertain") or not isinstance(row["reason"], str)
                or not row["reason"].strip() or len(row["reason"]) > 2000):
            raise runner.EvaluationError("quality_review_verdict_and_reason_required")
        if row["verdict"] == "pass" and row["objectiveStatus"] != "pass":
            raise runner.EvaluationError("quality_review_missing_or_failed_facts_cannot_pass")
        reviewed[key] = row

    result = common.clone(report)
    if kind == "human":
        for case in result["cases"]:
            for turn in case["turns"]:
                row = reviewed.get((case["caseId"], turn["turn"]))
                if row is not None:
                    turn["businessStatus"] = "fail" if turn["objectiveStatus"] == "fail" or row["verdict"] == "fail" else "pass" if row["verdict"] == "pass" else "unverified"
            case["businessStatus"] = aggregate([turn["businessStatus"] for turn in case["turns"]])
        result["sampleBusinessCompletion"] = counts([case["businessStatus"] for case in result["cases"]])
        for partition in result["partitions"]:
            values = [case["businessStatus"] for case in result["cases"] if case["partition"] == partition]
            result["partitions"][partition]["sampleBusinessCompletion"] = counts(values)
    completed = kind == "human" and all(row["verdict"] in ("pass", "fail") for row in rows)
    result.update(reviewer=reviewer.strip(), reviewerKind=kind, reviewerCount=1, humanReviewCompleted=completed,
                  businessSummaryChanged=kind == "human", qualityReportDigest=digest,
                  reviewTemplateDigest=common.sha(common.encode(template).encode()),
                  reviewInputDigest=common.sha(common.encode(form).encode()), reviewRows=rows,
                  reviewScope="具名单人复核；Agent复核仅作诊断，不改变业务率。不是独立多人一致性或真实用户总体准确率。")
    from secrets import token_hex
    common.write_new(run / ("quality-review-imported-" + token_hex(8) + ".json"), result)
    summary = result["sampleBusinessCompletion"]
    return {"reviewerKind": kind, "selectedTasks": result["selectedTasks"], "requiredReviewRows": len(rows),
            "confirmedPassed": summary["pass"], "unresolved": summary["unresolved"],
            "businessCompletionRate": summary["rate"], "humanReviewCompleted": completed,
            "businessSummaryChanged": result["businessSummaryChanged"],
            "partitions": {key: value["sampleBusinessCompletion"] for key, value in result["partitions"].items()}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("estimate", "prepare", "run", "review"))
    parser.add_argument("--mode", choices=("offline", "live"), default="offline")
    parser.add_argument("--run", type=Path)
    parser.add_argument("--max-provider-requests", type=int, default=0)
    parser.add_argument("--authorization", default="")
    parser.add_argument("--review-json", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "estimate":
            print(json.dumps(estimate(), ensure_ascii=False))
            return 0
        if args.command == "prepare":
            run = prepare(runner.PROJECT / ".local/repaired-quality-runs", mode=args.mode,
                          maximum=args.max_provider_requests, authorization=args.authorization)
            print(json.dumps({"run": str(run), "mode": args.mode, "suiteKey": SUITE_KEY,
                              "executionAllowed": args.mode == "live", "realProviderRequests": 0}, ensure_ascii=False))
            return 0
        if args.run is None:
            raise runner.EvaluationError("run_directory_required")
        run = common.private_directory(args.run)
        manifest, _ = runner.verify_manifest(run, check_sources=args.command != "review")
        if manifest.get("suiteKey") != SUITE_KEY or manifest["mode"] != args.mode:
            raise runner.EvaluationError("explicit_quality_suite_or_mode_mismatch")
        if args.command == "review":
            if args.review_json is None:
                raise runner.EvaluationError("review_file_required")
            print(json.dumps(import_review(run, args.review_json), ensure_ascii=False))
            return 0
        report = execute(run)
        print(json.dumps({"mode": args.mode, "suiteKey": SUITE_KEY, "tasks": report["selectedTasks"],
                          "interactions": report["selectedInteractions"], "providerLedger": report["providerLedger"],
                          "automaticContractCompliance": report.get("automaticContractCompliance"),
                          "runError": report.get("runError")}, ensure_ascii=False))
        return 0 if not report.get("runError") and report.get("sourceUnchanged") and not report.get("invalidReason") else 1
    except (runner.EvaluationError, common.CalibrationError, OSError, ValueError):
        print("修后验收未完成；请核对本轮授权、冻结协议与私有运行记录。", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
