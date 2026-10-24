"""真实验收入口的离线边界测试；不读取密钥、真实曲库、用户库或启动桌面。"""

import asyncio
import json

import pytest

from evaluation import music_requirements_live as live
from evaluation.calibration import CountedTransport, RequestLedger
from tango_backend.errors import ApiError


@pytest.fixture
def owned(tmp_path, monkeypatch):
    """准备私有拥有者目录，冻结伪哈希只供入口守卫测试。"""
    root = tmp_path / "runs"
    monkeypatch.setattr(live, "BASE", root)
    monkeypatch.setattr(live, "frozen_sources", lambda: {"owned-test-source": "a" * 64})
    monkeypatch.setattr(live, "live_models", lambda *args: (_ for _ in ()).throw(AssertionError("禁止加载真实配置")))
    return live.prepare()


def test_plan_covers_original_three_requirements_and_multi_turn_without_claiming_blind_quality():
    plan = live.canonical_plan()
    assert len(plan["tasks"]) == 8
    assert sum(len(task["inputs"]) for task in plan["tasks"]) == 9
    assert plan["tasks"][2]["inputs"] == ["我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。"]
    assert plan["tasks"][3]["expect"]["minutes"] == [30, 20]
    assert plan["purpose"] == "exposed_music_requirements_acceptance"
    assert plan["modelJudge"] is False


def test_prepare_is_zero_calls_and_requires_fresh_authorization(owned):
    assert not (owned / "provider-requests.jsonl").exists()
    assert not (owned / "owned.sqlite").exists()
    _, manifest = live.verify(owned)
    assert manifest["authorization"] is None
    assert manifest["historicalUnusedBudgetCarries"] is False
    with pytest.raises(live.CalibrationError, match="fresh_numeric_authorization_required"):
        live.verify(owned, require_authorization=True)


@pytest.mark.parametrize("maximum", [0, -1, 81, 500, True, "80", 1.5])
def test_authorization_cannot_exceed_new_eighty_cap_or_reuse_other_budget(owned, maximum):
    with pytest.raises(live.CalibrationError, match="fresh_numeric_authorization_required"):
        live.authorize(owned, maximum, "本轮新授权")
    assert not (owned / "authorization.json").exists()


def test_authorization_is_bound_to_frozen_version_and_cannot_be_overwritten(owned, monkeypatch):
    live.authorize(owned, 3, "仅授权此冻结版本最多3次")
    _, manifest = live.verify(owned, require_authorization=True)
    assert manifest["authorizedMaximum"] == 3
    with pytest.raises(FileExistsError):
        live.authorize(owned, 80, "不能覆盖原授权增额")
    monkeypatch.setattr(live, "frozen_sources", lambda: {"changed": "b" * 64})
    with pytest.raises(live.CalibrationError, match="frozen_source_or_plan_changed"):
        live.verify(owned, require_authorization=True)


def test_execution_lock_rejects_later_authorization_and_both_launches_require_new_auth(owned):
    for entry in [live.launch_qt, live.launch_catalog]:
        with pytest.raises(live.CalibrationError, match="fresh_numeric_authorization_required"):
            entry(owned)
    (owned / "execution-lock.json").write_text("{}")
    with pytest.raises(live.CalibrationError, match="executed_run_cannot_authorize_again"):
        live.authorize(owned, 80, "不能在执行后补授权")


def test_modified_frozen_plan_is_rejected_even_if_task_count_stays_eight(owned):
    plan = live.read_json(owned / "plan.json")
    plan["tasks"][0]["inputs"][0] = "更容易的另一题"
    (owned / "plan.json").write_text(json.dumps(plan))
    with pytest.raises(live.CalibrationError, match="frozen_source_or_plan_changed"):
        live.verify(owned)


def test_all_roles_failures_and_unknown_requests_share_one_hard_cap(tmp_path):
    class Delegate:
        """只模拟已有传输适配器，不具备外网请求能力。"""
        def __init__(self, fail=False):
            self.calls, self.fail = 0, fail

        async def complete(self):
            self.calls += 1
            if self.fail:
                raise ApiError(504, "MODEL_TIMEOUT", "模拟未知结果")
            return {"usage": {"total_tokens": 3}}

    ledger = RequestLedger(tmp_path, 2)
    route, task = Delegate(True), Delegate()

    async def execute():
        with pytest.raises(ApiError):
            await CountedTransport(route, ledger, "route:flash").complete()
        await CountedTransport(task, ledger, "task:plus").complete()
        with pytest.raises(ApiError) as caught:
            await CountedTransport(task, ledger, "task:flash").complete()
        assert caught.value.code == "CALIBRATION_REQUEST_LIMIT"

    try:
        asyncio.run(execute())
    finally:
        ledger.close()
    assert route.calls == 1 and task.calls == 1 and ledger.started == 2
    report = live.summarized_requests(tmp_path / "provider-requests.jsonl")
    assert report["started"] == 2
    assert report["outcomes"] == {"uncertain": 1, "responded": 1}


def test_context_control_cannot_call_product_or_repeat_a_turn(tmp_path):
    class Product:
        """控制端点不应派发到真实业务。"""
        async def __call__(self, *args):
            raise AssertionError("控制操作不能触发产品或模型")

    ledger = RequestLedger(tmp_path, 1)
    wrapper = live.ContextASGI(Product(), ledger, "a" * 64, tmp_path)

    async def request(token, turn):
        events = []
        async def receive():
            return {"type": "http.request", "body": json.dumps({"caseId":"MR01","turn":turn}).encode()}
        async def send(event):
            events.append(event)
        await wrapper({"type":"http","method":"POST","path":"/__music_requirements__/context",
                       "headers":[(b"x-tango-evaluation-control",token)]}, receive, send)
        return events[0]["status"]

    async def execute():
        assert await request(b"wrong", 0) == 403
        assert await request(b"a" * 64, 1) == 403
        assert await request(b"a" * 64, 0) == 200
        assert await request(b"a" * 64, 0) == 403
        assert ledger.started == 0
        assert ledger.context == {"caseId":"MR01","turn":0,"phase":"setup"}

    try:
        asyncio.run(execute())
    finally:
        ledger.close()
