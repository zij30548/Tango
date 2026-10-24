"""身份澄清补充诊断的范围、余量和真实候选守卫；不读取真实配置或调用外部服务。"""

import asyncio

import pytest

from evaluation import music_identity_followup as followup


def authorization(**changes):
    """生成本测试拥有的范围授权，无真实运行许可。"""
    return {"scope": followup.SCOPE, "manifestSeal": "a" * 64,
            "newAuthorization": True, "maximumProviderRequests": 40,
            "bindingScopeOnly": True, "historicalUnusedBudgetCarries": False,
            "statement": "离线测试：授权补充身份澄清范围最多40次", **changes}


def test_two_followup_tasks_only_complete_the_original_identity_questions():
    plan = followup.canonical_plan()
    assert len(plan["tasks"]) == 2
    assert sum(len(task["inputs"]) for task in plan["tasks"]) == 5
    assert plan["tasks"][1]["inputs"][0]["text"] == "我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。"
    assert plan["tasks"][1]["inputs"][2]["requirePreview"] is True
    for task in plan["tasks"]:
        assert task["inputs"][1] == {"kind":"select_verified_artist", "canonicalId":7763,
                                      "canonicalName":"G.E.M.邓紫棋", "requireCurrentCandidates":True}
        assert task["expect"]["autoPlay"] is False and task["expect"]["memoryWrite"] is False
    assert plan["historicalScoreRewritten"] is False and plan["overallAccuracy"] is None


@pytest.mark.parametrize("changes", [
    {"scope":"old_eighty_request_evaluation"}, {"scope":None}, {"newAuthorization":False},
    {"bindingScopeOnly":False}, {"historicalUnusedBudgetCarries":True},
    {"statement":"授权本轮最多80次，开始真实测评"}, {"statement":""},
    {"maximumProviderRequests":0}, {"maximumProviderRequests":41}, {"maximumProviderRequests":80},
    {"maximumProviderRequests":True}, {"maximumProviderRequests":"40"},
])
def test_old_scope_statement_and_excess_budget_cannot_authorize_diagnostic(changes):
    with pytest.raises(followup.IdentityFollowupError, match="explicit_identity_scope_authorization_required"):
        followup.validate_scope_authorization(authorization(**changes), "a" * 64, 73)


def test_scope_is_bound_to_this_manifest_and_available_original_remainder():
    assert followup.validate_scope_authorization(authorization(), "a" * 64, 73) == 40
    assert followup.validate_scope_authorization(authorization(maximumProviderRequests=12), "a" * 64, 12) == 12
    with pytest.raises(followup.IdentityFollowupError):
        followup.validate_scope_authorization(authorization(), "b" * 64, 73)
    with pytest.raises(followup.IdentityFollowupError):
        followup.validate_scope_authorization(authorization(), "a" * 64, 39)


def test_selection_uses_matching_current_identity_ordinal_not_first_result_or_alias():
    candidates=[{"id":99999,"name":"G.E.M.邓紫棋"}, {"id":7763,"name":"G.E.M.邓紫棋"}]
    assert followup.validate_identity_candidates(candidates) == 2
    assert followup.validate_identity_candidates([{"id":7763,"name":"G.E.M. 邓紫棋"}]) == 1
    for invalid in [[], [{"id":99999,"name":"G.E.M.邓紫棋"}],
                    [{"id":7763,"name":"邓紫棋","aliases":["G.E.M.邓紫棋"]}],
                    [{"id":"7763","name":"G.E.M.邓紫棋"}],
                    [{"name":"G.E.M.邓紫棋"}], [candidates[1],candidates[1]]]:
        with pytest.raises(followup.IdentityFollowupError, match="current_verified_identity_required"):
            followup.validate_identity_candidates(invalid)


@pytest.fixture
def prepared(tmp_path, monkeypatch):
    """准备守卫使用自有伪证据，不读取原真实场次或任何用户文件。"""
    monkeypatch.setattr(followup, "BASE", tmp_path / "runs")
    monkeypatch.setattr(followup, "frozen_sources", lambda: {"owned_source":"f" * 64})
    monkeypatch.setattr(followup, "source_evidence", lambda: {"runId":"owned-original","remaining":73,
                        "originalMaximum":80,"previousProviderRequests":7})
    return followup.prepare()


def test_prepare_never_authorizes_or_starts_provider_and_serve_rejects_old_eighty_scope(prepared, monkeypatch):
    assert not (prepared / "authorization.json").exists()
    assert not (prepared / "provider-requests.jsonl").exists()
    assert not (prepared / "owned.sqlite").exists()
    monkeypatch.setattr(followup, "_runtime", lambda: (_ for _ in ()).throw(AssertionError("未授权不得加载运行时")))
    with pytest.raises(followup.IdentityFollowupError, match="explicit_identity_scope_authorization_required"):
        asyncio.run(followup.serve(prepared))
    assert not (followup.BASE / "scope-execution-lock.json").exists()
    with pytest.raises(followup.IdentityFollowupError):
        followup.authorize(prepared, 40, "授权本轮最多80次", followup.SCOPE)


def test_scope_cannot_be_reauthorized_after_execution_or_have_sources_changed(prepared, monkeypatch):
    result=followup.authorize(prepared,40,"授权补充身份澄清范围最多40次",followup.SCOPE)
    assert result["scope"] == followup.SCOPE
    _,manifest=followup.verify(prepared,require_authorization=True)
    assert manifest["authorizedMaximum"] == 40
    followup.write_new(followup.BASE / "scope-execution-lock.json", {"runId":prepared.name})
    with pytest.raises(followup.IdentityFollowupError, match="identity_scope_already_executed"):
        followup.authorize(prepared,40,"再次授权补充身份澄清",followup.SCOPE)
    monkeypatch.setattr(followup, "frozen_sources", lambda: {"changed":"0" * 64})
    with pytest.raises(followup.IdentityFollowupError, match="frozen_identity_protocol_changed"):
        followup.verify(prepared,require_authorization=True)
