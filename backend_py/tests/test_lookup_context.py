"""多轮单曲筛选的合成契约验收，不访问模型、曲库或用户数据库。"""

from uuid import uuid4

import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal
from tango_backend.lookup_context import LOOKUP_CONTEXT_TTL_MS, lookupSelection, validateLookupContext
from test_conversation_routing import fx, used, DEVICE


ROWS = [
    {"id": 101, "title": "晚风来信", "artist": "甲", "album": "录音室", "version": "studio", "durationMs": 180000},
    {"id": 102, "title": "晚风来信 (Live)", "artist": "乙", "album": "音乐会", "version": "live", "durationMs": 190000},
    {"id": 103, "title": "晚风来信", "artist": "乙", "album": "专辑", "version": "unknown", "durationMs": 170000},
    {"id": 104, "title": "晚风来信", "artist": "乙", "album": "录音室二", "version": "studio", "durationMs": 200000},
]


def context(**more):
    """只提供受限元数据；没有URL、性别或可执行文本。"""
    return {"candidateSetId": "candidate-set-fixture", "originMessageId": "origin-message-fixture",
            "originActionId": "origin-action-fixture", "rows": clone(ROWS), "visibleIds": [101, 102, 103, 104], **more}


def routed(text, ctx=None):
    """使用产品同一解析与提案验证入口，不把模型JSON视作授权。"""
    inputs = {"instruction": text, "history": [], "task": None, "preference": ""}
    if ctx is not None:
        inputs["lookupContext"] = ctx
    proposal = deterministicRoute(inputs)
    return validateRouteProposal(proposal, inputs, messageId="lookup-followup", planner="deterministic") if proposal else None


@pytest.mark.parametrize("text,selector", [
    ("第二首", {"operation": "select", "index": 2}),
    ("我要第２首的那个", {"operation": "select", "index": 2}),
    ("我要录音室版的那个", {"operation": "version", "version": "studio"}),
    ("要现场版那个", {"operation": "version", "version": "live"}),
    ("不要现场版", {"operation": "exclude_version", "version": "live"}),
    ("还是录音室版", {"operation": "version", "version": "studio"}),
    ("我要乙唱的那个", {"operation": "artist", "artist": "乙"}),
    ("第二首，先别放", {"operation": "select", "index": 2}),
    ("我说的是乙那个", {"operation": "artist", "artist": "乙"}),
])
def test_refine_parser_preserves_current_turn_readonly_selector(text, selector):
    ctx = context()
    result = routed(text, ctx)
    assert result and result["intents"] == ["song_lookup"]
    action = result["actions"][0]
    assert action["capability"] == "music.refine" and action["source"] == text
    assert action["parameters"] == {**{key: ctx[key] for key in ["candidateSetId", "originMessageId", "originActionId"]}, **selector}
    assert action["dependsOn"] == [] and "play" not in action["parameters"]


@pytest.mark.parametrize("text", ["第二首", "我要录音室版的那个", "换成另一个歌手", "女声那个", "第0首", "第99首"])
def test_missing_or_invalid_reference_clarifies_without_research(text):
    result = routed(text)
    assert result and result["strategy"] == "clarify"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


@pytest.mark.parametrize("text", ["女声那个", "我要女声的录音室版那个", "第二首然后自动播放", "第二首并记住我喜欢摇滚", "如果可以就选第二首", "他说选第二首"])
def test_gender_and_compound_reference_never_partially_execute(text):
    result = routed(text, context())
    assert result and result["strategy"] == "clarify"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


async def lookup_origin(fx):
    """先真实持久化合成查找路由，再提交设备候选事实。"""
    first, _ = await fx.submit("找一下《晚风来信》，别放，先让我看看。")
    route = await fx.route(first)
    action = route["route"]["result"]["actions"][0]
    return first, context(originMessageId=first["messageId"], originActionId=action["id"])


async def test_owned_adjacent_lookup_context_routes_and_replays_once(fx):
    _, ctx = await lookup_origin(fx)
    current, _ = await fx.submit("我要录音室版的那个")
    first = await fx.route(current, lookupContext=ctx)
    assert first["route"]["status"] == "completed"
    assert first["route"]["result"]["actions"][0]["capability"] == "music.refine"
    assert await fx.route(current, lookupContext=ctx) == first
    altered = clone(ctx); altered["visibleIds"] = [101]
    with pytest.raises(ApiError) as error:
        await fx.route(current, lookupContext=altered)
    assert error.value.code == "IDEMPOTENCY_CONFLICT"
    assert fx.calls == [] and await used(fx) == 0


async def test_refine_chain_keeps_root_pool_and_checks_previous_selection(fx):
    _, ctx = await lookup_origin(fx)
    current, _ = await fx.submit("第二首")
    selected = await fx.route(current, lookupContext=ctx)
    next_ctx = context(candidateSetId=str(uuid4()), originMessageId=current["messageId"],
                       originActionId=selected["route"]["result"]["actions"][0]["id"], visibleIds=[102])
    followup, _ = await fx.submit("换成另一个歌手")
    result = await fx.route(followup, lookupContext=next_ctx)
    assert result["route"]["status"] == "completed"
    assert result["route"]["result"]["actions"][0]["parameters"]["operation"] == "other_artist"
    assert fx.calls == [] and await used(fx) == 0


def test_filters_apply_to_root_pool_and_never_infer_unknown_version():
    ctx = context(visibleIds=[102])
    assert lookupSelection(ctx, {"operation": "version", "version": "studio"}) == [101, 104]
    assert lookupSelection(ctx, {"operation": "exclude_version", "version": "live"}) == [101, 104]
    assert lookupSelection(ctx, {"operation": "artist", "artist": "乙"}) == [102, 103, 104]
    assert lookupSelection(ctx, {"operation": "other_artist"}) == [101]
    empty = context(visibleIds=[])
    assert lookupSelection(empty, {"operation": "version", "version": "live"}) == [102]
    assert routed("换成另一个歌手", empty)["strategy"] == "clarify"
    assert routed("换成另一个歌手", context())["strategy"] == "clarify"
    result = routed("不是这个歌手，换另一个歌手", context(visibleIds=[101]))
    assert result["actions"][0]["parameters"]["operation"] == "other_artist"


@pytest.mark.parametrize("mutation", [
    lambda ctx: ctx.update(url="https://untrusted.invalid/audio"),
    lambda ctx: ctx["rows"][0].update(gender="female"),
    lambda ctx: ctx["rows"][0].update(id=True),
    lambda ctx: ctx["rows"][0].update(id=9007199254740992),
    lambda ctx: ctx["rows"][0].update(durationMs=1.5),
    lambda ctx: ctx["rows"][0].update(version="female"),
    lambda ctx: ctx["rows"][0].update(title="a\x00b"),
    lambda ctx: ctx["rows"][0].update(title="😀" * 101),
    lambda ctx: ctx["rows"].append(clone(ctx["rows"][0])),
    lambda ctx: ctx.update(visibleIds=[101, 101]),
    lambda ctx: ctx.update(visibleIds=[999]),
    lambda ctx: ctx.update(visibleIds=[True]),
    lambda ctx: ctx.update(candidateSetId="x"),
    lambda ctx: ctx.update(originMessageId="bad/identifier"),
])
def test_context_shape_is_bounded_and_rejects_extra_authority_fields(mutation):
    ctx = context(); mutation(ctx)
    with pytest.raises(ApiError) as error:
        validateLookupContext(ctx)
    assert error.value.code == "INVALID_LOOKUP_CONTEXT"


def test_context_has_total_utf8_budget_in_addition_to_per_field_limits():
    ctx = context()
    ctx["rows"] = [{**clone(ROWS[0]), "id": number, "title": "歌" * 200, "artist": "名" * 200, "album": "辑" * 200} for number in range(1, 13)]
    ctx["visibleIds"] = list(range(1, 13))
    with pytest.raises(ApiError):
        validateLookupContext(ctx)


@pytest.mark.parametrize("alter", [
    lambda p: p.update(play=True),
    lambda p: p.update(index=1),
    lambda p: p.update(candidateSetId="another-set-id"),
    lambda p: p.update(originActionId="another-action-id"),
])
def test_untrusted_refine_plan_cannot_change_selection_or_add_playback(alter):
    text, ctx = "第二首", context()
    inputs = {"instruction": text, "history": [], "task": None, "preference": "", "lookupContext": ctx}
    proposal = deterministicRoute(inputs)
    alter(proposal["actions"][0]["parameters"])
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, inputs, messageId="forged-refine")
    assert error.value.routeReason == "refine_permission_or_parameters"


def test_boolean_index_cannot_equal_integer_selection():
    inputs = {"instruction": "第一首", "lookupContext": context()}
    proposal = deterministicRoute(inputs)
    proposal["actions"][0]["parameters"]["index"] = True
    with pytest.raises(ApiError):
        validateRouteProposal(proposal, inputs, messageId="boolean-selection")


@pytest.mark.parametrize("capability,parameters,intent", [
    ("music.task", {"mode": "new"}, "scene_research"),
    ("music.lookup", {"title": "晚风来信", "artist": "乙", "play": True}, "song_lookup"),
    ("memory.propose", {"operation": "set", "preference": "喜欢乙"}, "preference_proposal"),
    ("task.control", {"action": "cancel"}, "task_control"),
])
def test_reference_cannot_be_replaced_with_side_effect_or_research(capability, parameters, intent):
    text = "第二首"
    proposal = {"intents": [intent], "actions": [{"capability": capability, "source": text, "dependsOn": [], "parameters": parameters}], "missingSlots": []}
    with pytest.raises(ApiError):
        validateRouteProposal(proposal, {"instruction": text, "lookupContext": context()}, messageId="forged-capability")


@pytest.mark.parametrize("kind,intent", [("help", "help"), ("explain", "explain_result"), ("history", "conversation_history")])
def test_untrusted_model_cannot_answer_candidate_reference_with_unrelated_reply(kind, intent):
    text = "女声那个"
    proposal = {"intents": [intent], "actions": [{"capability": "assistant.reply", "source": text, "dependsOn": [],
                "parameters": {"kind": kind, "text": "不可信回答"}}], "missingSlots": []}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text, "lookupContext": context()}, messageId="forged-reply")
    assert error.value.routeReason == "refine_only_action"


@pytest.mark.parametrize("text", ["乙", " 乙 "])
def test_exact_candidate_artist_can_answer_an_artist_clarification(text):
    result = routed(text, context())
    assert result and result["actions"][0]["capability"] == "music.refine"
    assert result["actions"][0]["parameters"]["operation"] == "artist"
    assert result["actions"][0]["parameters"]["artist"] == "乙"


@pytest.mark.parametrize("text", ["乙", "如果是乙", "他说乙", "乙谢谢", "乙，先别放", "乙和甲", "‘乙’"])
def test_bare_artist_requires_context_and_exact_whole_current_message(text):
    without = routed(text)
    assert without is None or all(action["capability"] != "music.refine" for action in without["actions"])
    if text != "乙":
        result = routed(text, context())
        assert result is None or all(action["capability"] != "music.refine" for action in result["actions"])


async def test_bare_exact_artist_continues_verified_adjacent_clarification(fx):
    _, ctx = await lookup_origin(fx)
    unclear, _ = await fx.submit("女声那个")
    result = await fx.route(unclear, lookupContext=ctx)
    assert result["route"]["result"]["strategy"] == "clarify"
    current, _ = await fx.submit("乙")
    final = await fx.route(current, lookupContext=ctx)
    assert final["route"]["status"] == "completed"
    assert final["route"]["result"]["actions"][0]["parameters"]["artist"] == "乙"
    assert fx.calls == [] and await used(fx) == 0


async def test_gender_clarification_preserves_only_same_adjacent_context(fx):
    _, ctx = await lookup_origin(fx)
    unclear, _ = await fx.submit("女声那个")
    result = await fx.route(unclear, lookupContext=ctx)
    assert result["route"]["result"]["strategy"] == "clarify"
    correction, _ = await fx.submit("我要录音室版的那个")
    final = await fx.route(correction, lookupContext=ctx)
    assert final["route"]["result"]["actions"][0]["capability"] == "music.refine"
    assert fx.calls == []


@pytest.mark.parametrize("change", ["rows", "visibleIds", "same_set_id", "action", "message"])
async def test_refine_lineage_rejects_tampered_pool_result_and_binding(fx, change):
    _, ctx = await lookup_origin(fx)
    current, _ = await fx.submit("第二首")
    selected = await fx.route(current, lookupContext=ctx)
    next_ctx = context(candidateSetId=str(uuid4()), originMessageId=current["messageId"],
                       originActionId=selected["route"]["result"]["actions"][0]["id"], visibleIds=[102])
    if change == "rows":
        next_ctx["rows"][0]["artist"] = "替换艺人"
    elif change == "visibleIds":
        next_ctx["visibleIds"] = [101]
    elif change == "same_set_id":
        next_ctx["candidateSetId"] = ctx["candidateSetId"]
    elif change == "action":
        next_ctx["originActionId"] = "wrong-origin-action"
    else:
        next_ctx["originMessageId"] = str(uuid4())
    followup, _ = await fx.submit("我要录音室版的那个")
    with pytest.raises(ApiError) as error:
        await fx.route(followup, lookupContext=next_ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE"
    assert await used(fx) == 0 and fx.calls == []


async def test_intervening_topic_does_not_reuse_old_lookup_authority(fx):
    _, ctx = await lookup_origin(fx)
    other, _ = await fx.submit("你能做什么")
    await fx.route(other)
    current, _ = await fx.submit("第二首")
    with pytest.raises(ApiError) as error:
        await fx.route(current, lookupContext=ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE"
    result = await fx.route(current)
    assert result["route"]["result"]["strategy"] == "clarify"
    assert fx.calls == []


async def test_expiry_is_based_on_root_lookup_not_recent_refine(fx):
    root, ctx = await lookup_origin(fx)
    current, _ = await fx.submit("第二首")
    selected = await fx.route(current, lookupContext=ctx)
    next_ctx = context(candidateSetId=str(uuid4()), originMessageId=current["messageId"],
                       originActionId=selected["route"]["result"]["actions"][0]["id"], visibleIds=[102])
    await fx.store.db.execute("UPDATE music_conversation_routes SET created_at=created_at-? WHERE message_id=?", (LOOKUP_CONTEXT_TTL_MS + 1, root["messageId"]))
    latest, _ = await fx.submit("换成另一个歌手")
    with pytest.raises(ApiError) as error:
        await fx.route(latest, lookupContext=next_ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE" and fx.calls == []


async def test_different_conversation_or_device_cannot_supply_lookup_context(fx):
    _, ctx = await lookup_origin(fx)
    another = await fx.conversations.create(fx.user["id"], {"deviceId": DEVICE, "requestId": str(uuid4())})
    message = {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "第二首"}
    await fx.conversations.message(another["id"], fx.user["id"], message, {"dispatchHint": "router"})
    with pytest.raises(ApiError):
        await fx.router.route(another["id"], message["messageId"], fx.user["id"], {"deviceId": DEVICE, "preference": "", "lookupContext": ctx})
    current, _ = await fx.submit("第二首")
    with pytest.raises(ApiError):
        await fx.route(current, lookupContext=ctx, deviceId="different-device-fixture")
    with pytest.raises(ApiError):
        await fx.router.route(fx.conversation["id"], current["messageId"], fx.other["id"], {"deviceId": DEVICE, "preference": "", "lookupContext": ctx})
    assert await used(fx) == 0 and fx.calls == []


async def test_candidate_metadata_is_not_sent_to_model_or_model_selector(fx):
    _, ctx = await lookup_origin(fx)
    selections = []
    def selector(kind, inputs):
        selections.append(clone(inputs))
        return None
    fx.router.model.selectCall = selector
    current, _ = await fx.submit("工作时希望舒适又不抢注意力")
    result = await fx.route(current, lookupContext=ctx)
    assert result["route"]["status"] == "completed"
    assert len(selections) == len(fx.calls) == 1
    assert "lookupContext" not in selections[0] and "lookupContext" not in fx.calls[0]
    persisted = await fx.router.repository.get({"conversationId": fx.conversation["id"], "messageId": current["messageId"], "userId": fx.user["id"], "deviceId": DEVICE})
    assert persisted["input"]["lookupContext"] == ctx


async def test_help_or_failed_route_cannot_establish_lookup_origin(fx):
    first, _ = await fx.submit("你能做什么")
    result = await fx.route(first)
    ctx = context(originMessageId=first["messageId"], originActionId=result["route"]["result"]["actions"][0]["id"])
    current, _ = await fx.submit("第二首")
    with pytest.raises(ApiError) as error:
        await fx.route(current, lookupContext=ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE"
    assert fx.calls == []


async def test_lookup_chain_has_bounded_depth_without_renewing_root(fx, monkeypatch):
    import tango_backend.conversation_router as routing
    monkeypatch.setattr(routing, "LOOKUP_CONTEXT_MAX_DEPTH", 2)
    _, ctx = await lookup_origin(fx)
    for _ in range(2):
        current, _ = await fx.submit("我要录音室版的那个")
        result = await fx.route(current, lookupContext=ctx)
        assert result["route"]["status"] == "completed"
        ctx = context(candidateSetId=str(uuid4()), originMessageId=current["messageId"],
                      originActionId=result["route"]["result"]["actions"][0]["id"], visibleIds=[101, 104])
    latest, _ = await fx.submit("第二首")
    with pytest.raises(ApiError) as error:
        await fx.route(latest, lookupContext=ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE" and fx.calls == []


async def test_root_expiring_during_route_is_persisted_as_failure_without_action(fx):
    root, ctx = await lookup_origin(fx)
    begin = fx.router.repository.begin
    async def expire_after_begin(*args, **kwargs):
        reserved = await begin(*args, **kwargs)
        await fx.store.db.execute("UPDATE music_conversation_routes SET created_at=created_at-? WHERE message_id=?", (LOOKUP_CONTEXT_TTL_MS + 1, root["messageId"]))
        return reserved
    fx.router.repository.begin = expire_after_begin
    current, _ = await fx.submit("第二首")
    result = await fx.route(current, lookupContext=ctx)
    assert result["route"]["status"] == "failed" and result["route"]["errorCode"] == "LOOKUP_CONTEXT_STALE"
    assert result["route"]["result"] is None and fx.calls == [] and await used(fx) == 0


async def test_unrelated_clarification_with_same_device_payload_does_not_keep_lookup_alive(fx):
    _, ctx = await lookup_origin(fx)
    async def unrelated(inputs, options=None):
        fx.calls.append(clone(inputs))
        return {"value": {"intents": ["clarification"], "actions": [{"capability": "assistant.reply", "source": inputs["instruction"],
                "dependsOn": [], "parameters": {"kind": "clarify", "text": "请说明其他需求"}}],
                "missingSlots": [{"intent": "clarification", "slot": "reference", "question": "请说明其他需求"}]}}
    fx.router.model.route = unrelated
    other, _ = await fx.submit("我想谈一个新问题")
    result = await fx.route(other, lookupContext=ctx)
    assert result["route"]["status"] == "completed"
    latest, _ = await fx.submit("第二首")
    with pytest.raises(ApiError) as error:
        await fx.route(latest, lookupContext=ctx)
    assert error.value.code == "LOOKUP_CONTEXT_STALE" and len(fx.calls) == 1
