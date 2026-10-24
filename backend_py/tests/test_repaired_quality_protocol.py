"""预付费金标校验：错误判据在读取模型配置和启动执行前停止。"""

from copy import deepcopy

import pytest

from evaluation import independent as runner
from evaluation import repaired_quality as quality


def turn():
    """最小合法路由契约；仅用于验证评价器的输入结构。"""
    return {"text": "只读查歌", "expect": [{"predicate": "task_field", "path": "brief.targetTrackCount", "value": 6}],
            "semanticReference": {"acceptedIntentSets": [["song_lookup"]], "acceptedCapabilitySets": [["music.lookup"]],
                                  "slots": [{"name": "play", "path": "actions.0.parameters.play", "value": False}]},
            "manualCriteria": [{"id": "reason", "required": True, "text": "说明未知项"}]}


@pytest.mark.parametrize("mutation,error", [
    (lambda value: value.update(semanticReference=None), "invalid_quality_semantic_reference"),
    (lambda value: value["semanticReference"].update(localCommand="yes"), "invalid_quality_semantic_reference"),
    (lambda value: value["semanticReference"].pop("acceptedIntentSets"), "invalid_quality_allowed_route_sets"),
    (lambda value: value["semanticReference"].update(acceptedIntentSets="song_lookup"), "invalid_quality_allowed_route_sets"),
    (lambda value: value["semanticReference"].update(acceptedIntentSets=[[]]), "invalid_quality_allowed_route_sets"),
    (lambda value: value["semanticReference"].update(acceptedIntentSets=[[None]]), "invalid_quality_allowed_route_sets"),
    (lambda value: value["semanticReference"].update(acceptedCapabilitySets=[[""]]), "invalid_quality_allowed_route_sets"),
    (lambda value: value["semanticReference"].update(slots=None), "invalid_quality_slots"),
    (lambda value: value["semanticReference"]["slots"].__setitem__(0, "bad"), "invalid_quality_slot_contract"),
    (lambda value: value["semanticReference"]["slots"][0].update(path="actions[0].parameters.play"), "invalid_quality_slot_contract"),
    (lambda value: value["semanticReference"]["slots"][0].pop("value"), "invalid_quality_slot_contract"),
    (lambda value: value["semanticReference"]["slots"][0].update(alternatives=[False]), "invalid_quality_slot_contract"),
    (lambda value: value["semanticReference"]["slots"][0].update(normalize="true"), "invalid_quality_slot_contract"),
    (lambda value: value.update(manualCriteria={"text": "bad"}), "invalid_quality_manual_criteria"),
    (lambda value: value.update(manualCriteria=[None]), "invalid_quality_manual_criteria"),
    (lambda value: value.update(manualCriteria=["  "]), "invalid_quality_manual_criteria"),
    (lambda value: value["manualCriteria"][0].update(required="false"), "invalid_quality_manual_criteria"),
    (lambda value: value["manualCriteria"][0].pop("text"), "invalid_quality_manual_criteria"),
    (lambda value: value.update(expect=[None]), "invalid_quality_predicate_contract"),
    (lambda value: value.update(expect=[{"predicate": ""}]), "invalid_quality_predicate_contract"),
    (lambda value: value["expect"][0].pop("path"), "invalid_quality_task_field_contract"),
    (lambda value: value["expect"][0].pop("value"), "invalid_quality_task_field_contract"),
    (lambda value: value.update(expect=[{"predicate": "displayed_ids"}]), "invalid_quality_displayed_ids_contract"),
    (lambda value: value.update(expect=[{"predicate": "displayed_ids", "ids": [True]}]), "invalid_quality_displayed_ids_contract"),
    (lambda value: value.update(expect=[{"predicate": "candidate_count", "value": True}]), "invalid_quality_numeric_predicate_contract"),
    (lambda value: value.update(expect=[{"predicate": "preview_duration_seconds", "value": 120, "tolerance": -1}]), "invalid_quality_numeric_predicate_contract"),
    (lambda value: value.update(expect=[{"predicate": "same_task_status", "value": "paused", "baseline_turn": 0}]), "invalid_quality_task_status_contract"),
])
def test_malformed_gold_is_rejected_before_execution(mutation, error):
    value = turn()
    mutation(value)
    with pytest.raises(runner.EvaluationError, match=error):
        quality._gold_contract(value)


@pytest.mark.parametrize("raw", [
    '{"cases":[],"cases":[1]}', '{"nested":{"name":1,"name":2}}',
    '{"value":NaN}', '{"value":Infinity}', '{"value":-Infinity}',
])
def test_duplicate_keys_and_nonfinite_values_cannot_enter_frozen_sources(tmp_path, raw):
    path = tmp_path / quality.NEW_CASES
    path.parent.mkdir(parents=True)
    path.write_text(raw)
    with pytest.raises(runner.EvaluationError, match="invalid_quality_suite_source"):
        quality._source(quality.NEW_CASES, tmp_path)


def test_alternatives_local_commands_and_unknown_predicates_remain_explicit():
    value = turn()
    slot = value["semanticReference"]["slots"][0]
    slot.pop("value")
    slot["alternatives"] = [False, None]
    value["manualCriteria"] = ["由具名人员核对说明", {"required": False, "text": "可选听感备注"}]
    value["expect"] = [{"predicate": "future_unknown_predicate"}]
    quality._gold_contract(value)
    value["semanticReference"] = {"localCommand": True}
    quality._gold_contract(value)


def test_invalid_gold_stops_before_probe_export_and_new_run_directory(tmp_path, monkeypatch):
    docs = {}
    for name, count in quality.PARTITION_COUNTS.items():
        docs[name] = {"fixture_definitions": {"catalog_core": {"tracks": []}}, "cases": [
            {"id": ("N" if count == 16 else "R") + str(index), "title": "协议测试", "partition": quality.PARTITIONS[name],
             "user_turns": [deepcopy(turn())], "business_success_conditions": ["检查目标"],
             "critical_negative_assertions": ["无额外权限"]} for index in range(count)]}
    docs[quality.NEW_CASES]["cases"][0]["user_turns"][0]["semanticReference"]["slots"] = "不是数组"
    docs[quality.PROTOCOL] = {"protocolId": "tango-repaired-quality-v1", "schemaVersion": 1}
    monkeypatch.setattr(quality, "_source", lambda relative, project: (docs[relative], "synthetic-source-hash"))
    monkeypatch.setattr(runner, "export_truth", lambda *args, **kwargs: pytest.fail("错误金标不应启动探针"))
    with pytest.raises(runner.EvaluationError, match="invalid_quality_slots"):
        quality.prepare(tmp_path / "forbidden", mode="live", maximum=200, authorization="合成测试授权", project=tmp_path)
    assert not (tmp_path / "forbidden").exists()
