"""开发校准执行器的工程验证：零付费、真实ASGI/LangGraph、错误评分与预算守卫。"""

import asyncio
import csv
import json
import os
from pathlib import Path

import httpx
import pytest

from evaluation import calibration as c
from tango_backend.errors import ApiError


def suite():
    return c.validate_suite(c.read_json(c.PROJECT / c.SUITE))


def test_frozen_dataset_is_ten_tasks_seventeen_turns_with_separate_fault_group():
    data = suite()
    assert len(data["cases"]) == 10
    assert sum(len(case["turns"]) for case in data["cases"]) == 17
    estimate = c.request_estimate(data)
    # 明确复合需求与偏好可由受限编译分流；这里只更新当前派生估算，冻结题库与原成绩不改。
    assert estimate == {"expectedRouteUpper": 6, "taskRunsIncludingResume": 11,
                        "perTaskRunMaximum": 6, "expectedPathRequestUpper": 72,
                        "note": "按预期分流估算，实际执行以调用前持久计数的硬上限为准；不是费用报价。"}
    assert {case["group"] for case in data["cases"]} == {"routine", "boundary", "fault"}


@pytest.mark.parametrize("mode,maximum,authorization", [("live", 0, "user"), ("live", 80, ""), ("offline", 1, ""), ("other", 0, ""), ("offline", True, "")])
def test_paid_preparation_requires_explicit_new_positive_budget(tmp_path, mode, maximum, authorization):
    with pytest.raises(c.CalibrationError):
        c.prepare(tmp_path, mode=mode, maximum=maximum, authorization=authorization)
    assert not list(tmp_path.iterdir())


def test_freeze_rejects_changed_manifest_and_does_not_overwrite(tmp_path):
    run = c.prepare(tmp_path)
    manifest = c.read_json(run / "manifest.json")
    manifest["maximumProviderRequests"] = 999
    (run / "manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(c.CalibrationError, match="frozen_source_or_manifest_changed"):
        c.verify_manifest(run)
    with pytest.raises(FileExistsError):
        c.write_new(run / "manifest.json", {})


def test_source_changes_invalidate_freeze(tmp_path, monkeypatch):
    run = c.prepare(tmp_path)
    original = c.source_hashes()
    monkeypatch.setattr(c, "source_hashes", lambda project=c.PROJECT: {**original, "changed.py": "changed"})
    with pytest.raises(c.CalibrationError, match="frozen_source"):
        c.verify_manifest(run)


def test_paths_reject_public_directories_and_symlinks(tmp_path):
    public = tmp_path / "public"
    public.mkdir(mode=0o755)
    os.chmod(public, 0o755)
    with pytest.raises(c.CalibrationError, match="private_directory_required"):
        c.prepare(public)
    (tmp_path / "link").symlink_to(tmp_path, target_is_directory=True)
    with pytest.raises(c.CalibrationError, match="symlink"):
        c.prepare(tmp_path / "link")


def test_duplicate_keys_and_nonfinite_values_are_rejected(tmp_path):
    path = tmp_path / "input.json"
    for text in ('{"x":1,"x":2}', '{"x":NaN}'):
        path.write_text(text)
        with pytest.raises(c.CalibrationError):
            c.read_json(path)


async def test_budget_is_durable_before_send_and_shared_across_roles(tmp_path):
    ledger = c.RequestLedger(tmp_path, 1)
    calls = []
    class Delegate:
        async def complete(self, *args):
            saved = [json.loads(line) for line in ledger.path.read_text().splitlines()]
            assert saved[-1]["event"] == "started"
            calls.append(True)
            await asyncio.sleep(0)
            return {"usage": {"prompt_tokens": 3, "completion_tokens": 2, "private": "must-not-save"}}
    try:
        a = c.CountedTransport(Delegate(), ledger, "route:flash")
        b = c.CountedTransport(Delegate(), ledger, "task:plus")
        results = await asyncio.gather(*(a.complete({}) if i % 2 else b.complete({}) for i in range(8)), return_exceptions=True)
        assert len(calls) == ledger.started == 1
        assert sum(isinstance(r, ApiError) and r.code == "CALIBRATION_REQUEST_LIMIT" for r in results) == 7
        assert "must-not-save" not in ledger.path.read_text()
    finally:
        ledger.close()


@pytest.mark.parametrize("code", ["MODEL_TIMEOUT", "REQUEST_CANCELLED", "MODEL_UNAVAILABLE"])
async def test_failed_and_uncertain_requests_are_counted_without_retry(tmp_path, code):
    ledger = c.RequestLedger(tmp_path, 1)
    class Delegate:
        async def complete(self, *args):
            raise ApiError(502, code, "sk-sensitive-key-must-not-save")
    try:
        transport = c.CountedTransport(Delegate(), ledger, "route")
        with pytest.raises(ApiError):
            await transport.complete({})
        with pytest.raises(ApiError, match="上限"):
            await transport.complete({})
        events = [json.loads(line) for line in ledger.path.read_text().splitlines()]
        assert ledger.started == 1 and len(events) == 2
        assert events[-1]["status"] == ("failed" if code == "MODEL_UNAVAILABLE" else "uncertain")
        assert "sensitive" not in ledger.path.read_text()
    finally:
        ledger.close()


async def test_real_model_adapters_are_wired_through_one_counter_without_private_file(tmp_path, monkeypatch):
    import dotenv
    from tango_backend.model_transport import ChatTransport
    ledger, clients = c.RequestLedger(tmp_path, 2), []
    monkeypatch.setattr(dotenv, "dotenv_values", lambda *args, **kwargs: {
        "QWEN_API_KEY": "isolated-fixture-secret", "QWEN_BASE_URL": "http://127.0.0.1:9/v1",
        "QWEN_MODEL": "fixture-flash", "QWEN_MODEL_STRATEGY": "flash_only"})
    original = ChatTransport.__init__
    payloads = []
    def handler(request):
        payloads.append(json.loads(request.content))
        return httpx.Response(200, json={"choices": [{"finish_reason": "stop", "message": {"content": json.dumps({"intents": ["help"], "actions": [], "missingSlots": []})}}]})
    def initialize(self, *args, **kwargs):
        original(self, *args, **kwargs)
        self.client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        clients.append(self.client)
    monkeypatch.setattr(ChatTransport, "__init__", initialize)
    try:
        task, route, keys, snapshot = c.live_models(ledger, 1792728000000)
        assert task.policy is not None and snapshot["strategy"] == "flash_only"
        await route.route({"instruction": "隔离路由测试"})
        await task.complete("隔离测试规则", {"instruction": "隔离任务测试"}, 100, kind="plan")
        with pytest.raises(ApiError, match="上限"):
            await route.route({"instruction": "不能发生第三次请求"})
        assert len(payloads) == ledger.started == 2
        assert all(p["model"] == "fixture-flash" for p in payloads)
        assert "isolated-fixture-secret" not in ledger.path.read_text()
        assert keys == ["isolated-fixture-secret"]
    finally:
        ledger.close()
        await asyncio.gather(*(client.aclose() for client in clients))


def test_unknown_results_stay_in_all_denominators_and_offline_has_no_quality_score():
    data = suite()
    for mode in ("offline", "live"):
        report = c.summarize(data, {}, mode)
        assert report["selectedTasks"] == 10 and report["selectedTurns"] == 17
        assert report["counts"] == {"unverified": 10}
        assert report["productOverallCompletionRate"] is report["recommendationQuality"] is None
        assert report["controlledTaskCompletionRate"] is None
        assert report["backendAcceptanceRate"] == (0 if mode == "live" else None)
        assert report["intentExactSetRate"] == (0 if mode == "live" else None)


def test_scorer_does_not_trust_self_reported_satisfied_or_legal_json():
    turn = suite()["cases"][1]["turns"][0]
    observation = {"route": {"status": "completed", "result": {"intents": ["help"], "slots": {}, "actions": []}},
                   "task": {"status": "preview", "brief": {"targetMinutes": 20, "excludeLive": True},
                            "preview": {"tracks": [{"id": 99999, "title": "伪造", "artist": "未知", "durationMs": 1000}],
                                        "constraints": [{"status": "satisfied"}]}}, "truth": {}, "readbackNoCalls": True}
    checks = {v["name"]: v["status"] for v in c.checks(turn, observation)}
    assert checks["intent_exact_set"] == checks["action_exact_set"] == checks["duration"] == checks["grounded_metadata"] == "failed"


def test_redaction_and_html_escaping_do_not_treat_model_text_as_markup(tmp_path):
    value = c.redact({"text": "key=<script>alert(1)</script> sk-abcdefghi"}, ["alert(1)"])
    assert "alert(1)" not in c.encode(value) and "sk-abcdefghi" not in c.encode(value)
    report = {"manifestSeal": "fixture", "mode": "offline", "cases": [{"id": "a", "title": "<script>", "status": "failed",
              "turns": [{"turn": 1, "text": "<img src=x>", "checks": []}]}]}
    c.write_review(tmp_path, report, {})
    html = (tmp_path / "review.html").read_text()
    assert "<script>" not in html and "<img" not in html and "&lt;script&gt;" in html


async def test_ten_scripted_tasks_execute_real_asgi_langgraph_and_readonly_restart(tmp_path, monkeypatch):
    async def forbidden(*args, **kwargs):
        pytest.fail("离线校准不得发起供应商请求")
    monkeypatch.setattr(httpx.AsyncHTTPTransport, "handle_async_request", forbidden)
    monkeypatch.setattr(c, "live_models", lambda *args: pytest.fail("离线不能加载真实模型配置"))
    run = c.prepare(tmp_path)
    report = await c.execute(run)
    assert report.get("invalidReason") is None and report["runError"] is None
    assert report["counts"] == {"passed": 10}, [(v["id"], v["turns"]) for v in report["cases"] if v["status"] != "passed"]
    assert report["providerRequests"] == 0 and (run / "provider-requests.jsonl").read_text() == ""
    assert report["intentExactSetRate"] is report["controlledTaskCompletionRate"] is None
    observed = c.read_json(run / "progress.json")["observations"]
    assert observed["recovery"]["1"]["restartNoCalls"] is True
    assert observed["catalog_failure"]["0"]["task"]["error"]["code"] == "CATALOG_QUERY_FAILED"
    assert observed["compound"]["0"]["preference"] == ""
    review = c.import_review(run, run / "human-review.csv")
    assert review["counts"] == {"pending": 17}
    with pytest.raises(FileExistsError):
        await c.execute(run)
    # 修改已有输出必须造成判分失败，不能只检查执行器有没有生成报告。
    altered = c.clone(observed)
    altered["scene"]["0"]["task"]["preview"]["tracks"][0]["durationMs"] = 1
    changed = c.summarize(suite(), altered, "live")
    assert changed["counts"]["failed"] >= 1 and changed["backendAcceptanceRate"] < 1


@pytest.mark.parametrize("mutation", ["duplicate", "missing", "wrong_run", "invalid_grade", "changed_instruction"])
def test_human_reviews_reject_mixed_or_forged_rows(tmp_path, mutation):
    run = c.prepare(tmp_path)
    manifest, data = c.verify_manifest(run)
    report = c.summarize(data, {}, "offline")
    report["manifestSeal"] = manifest["seal"]
    c.write_review(run, report, {})
    path = run / "human-review.csv"
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields, rows = reader.fieldnames, list(reader)
    if mutation == "duplicate": rows.append(rows[0])
    if mutation == "missing": rows.pop()
    if mutation == "wrong_run": rows[0]["manifestSeal"] = "other"
    if mutation == "invalid_grade": rows[0]["understanding_1_to_5"] = "6"
    if mutation == "changed_instruction": rows[0]["instruction"] = "别的原话"
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
    with pytest.raises(c.CalibrationError):
        c.import_review(run, path)
