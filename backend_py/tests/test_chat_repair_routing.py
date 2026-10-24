"""用户真实失败原句的离线修复回归；不表示模型、真实曲库或音频已通过。"""

from uuid import uuid4

import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.intent_routing import RoutingModel, deterministicRoute, validateRouteProposal
from test_conversation_routing import fx, used, DEVICE


def route_for(text, **context):
    input = {"instruction": text, "history": [], "task": None, "preference": "", **context}
    proposal = deterministicRoute(input)
    return validateRouteProposal(proposal, input, messageId="isolated-repair", planner="deterministic") if proposal else None


@pytest.mark.parametrize("text,intent,capability", [
    ("你能查询邓紫棋的新歌吗", "release_research", "music.task"),
    ("最近邓紫棋有没有发新歌", "release_research", "music.task"),
    ("推荐跑步要听的歌", "scene_research", "music.task"),
    ("帮我推荐睡前适合听的音乐", "scene_research", "music.task"),
    ("推荐通勤的歌曲", "scene_research", "music.task"),
    ("我之前问过你哪些歌手", "conversation_history", "assistant.reply"),
    ("帮我播放陈奕迅的十年", "song_lookup", "music.lookup"),
    ("帮我搜索陈奕迅的十年", "song_lookup", "music.lookup"),
])
def test_original_failure_and_related_clear_requests(text, intent, capability):
    result = route_for(text)
    assert result["intents"] == [intent]
    assert result["actions"][0]["capability"] == capability
    assert result["planner"] == "deterministic"


def test_new_topic_does_not_continue_previous_release_task():
    result = route_for("推荐跑步要听的歌", task={"id": "old-release", "revision": 3, "brief": {"artist": "邓紫棋"}})
    assert result["slots"]["artist"] == ""
    assert result["actions"][0]["parameters"] == {"mode": "new"}


def test_search_and_play_are_different_permissions():
    for text, play in [("帮我搜索陈奕迅的十年", False), ("帮我播放陈奕迅的十年", True)]:
        result = route_for(text)
        assert result["actions"][0]["parameters"] == {"artist": "陈奕迅", "title": "十年", "play": play}
    titled = route_for("帮我搜索《你的背包》")
    assert titled["actions"][0]["parameters"] == {"artist": "", "title": "你的背包", "play": False}


@pytest.mark.parametrize("text", [
    "不要播放陈奕迅的十年", "我不想播放陈奕迅的十年", "他说播放陈奕迅的十年", "假设我要播放陈奕迅的十年",
    "帮我播放陈奕迅的十年，然后删除所有歌曲",
])
def test_quoted_negative_or_compound_requests_do_not_get_naive_direct_route(text):
    assert deterministicRoute({"instruction": text, "history": [], "task": None, "preference": ""}) is None


def test_model_cannot_upgrade_search_to_play_or_inject_track_ids():
    input = {"instruction": "帮我搜索陈奕迅的十年", "history": [], "task": None, "preference": ""}
    original = deterministicRoute(input)
    for fields in [{"play": True}, {"trackId": 123}, {"title": "浮夸"}, {"artist": "周杰伦"}]:
        proposal = clone(original)
        proposal["actions"][0]["parameters"].update(fields)
        with pytest.raises(ApiError) as error:
            validateRouteProposal(proposal, input, messageId="isolated")
        assert error.value.code == "INVALID_ROUTE_PLAN"
        assert error.value.routeReason == "lookup_permission_or_parameters"


def test_lookup_combined_query_respects_existing_catalog_utf16_limit():
    artist, title = "甲" * 100, "乙" * 100
    text = "帮我搜索" + artist + "的" + title
    result = route_for(text)
    assert result["strategy"] == "clarify"
    assert result["missingSlots"][0]["slot"] == "support"
    proposal = {"intents": ["song_lookup"], "actions": [{"capability": "music.lookup", "source": text, "dependsOn": [],
        "parameters": {"artist": artist, "title": title, "play": False}}], "missingSlots": []}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text}, messageId="oversized")
    assert error.value.routeReason == "lookup_permission_or_parameters"
    assert route_for("帮我搜索" + artist + "的" + title[:-1])["actions"][0]["capability"] == "music.lookup"


def test_history_reply_uses_evidence_and_cannot_be_forged_as_playlist_explanation():
    result = route_for("我之前问过你哪些歌手", history=[{"role": "user", "content": "你能查询邓紫棋的新歌吗"}, {"role": "user", "content": "帮我搜索陈奕迅的十年"}])
    text = result["actions"][0]["parameters"]["text"]
    assert "邓紫棋" in text and "陈奕迅" in text and "最近 6 条" in text
    assert "曲库候选" not in text
    assert "还没有找到" in route_for("我之前问过你哪些歌手")["actions"][0]["parameters"]["text"]


def test_new_release_missing_artist_asks_before_starting_task():
    result = route_for("帮我查最近30天的新歌。")
    assert result["strategy"] == "clarify"
    assert result["intents"] == ["release_research"]
    assert result["actions"][0]["capability"] == "assistant.reply"
    assert result["missingSlots"][0]["slot"] == "artist"


@pytest.mark.parametrize("text", ["查询新歌", "帮我查新歌", "你能查询新歌吗", "最近有什么新歌", "查最近的新歌"])
def test_query_verbs_are_never_invented_as_artist_names(text):
    result = route_for(text)
    assert result["strategy"] == "clarify"
    assert result["slots"]["artist"] == ""


def test_revision_like_recommendation_keeps_model_context_instead_of_resetting_task():
    # 新增受限编译可明确承接当前任务，仍绑定旧任务版本而非另开任务。
    result = route_for("推荐更轻快的歌", task={"id": "old-preview", "revision": 4})
    assert result["actions"][0]["parameters"] == {"mode": "continue", "taskId": "old-preview", "expectedRevision": 4}


@pytest.mark.parametrize("envelope", [{"choices": 1}, {"choices": {}}, {"choices": []}, {"choices": [{"message": {"content": "{}"}, "finish_reason": "length"}]}])
def test_nonconforming_model_envelope_has_safe_structural_diagnosis(envelope):
    with pytest.raises(ApiError) as error:
        RoutingModel.parse(None, envelope)
    assert error.value.code == "INVALID_ROUTE_PLAN"
    assert error.value.routeReason == "model_envelope"


@pytest.mark.parametrize("mutation,reason", [
    ("shape", "memory_shape"), ("preference", "memory_preference"),
    ("intent", "memory_intent"), ("source", "memory_source_permission"),
])
def test_memory_rejection_reasons_distinguish_structure_content_intent_and_permission(mutation, reason):
    input = {"instruction": "记住我喜欢粤语，再推荐20分钟微醺歌单。", "history": [], "task": None, "preference": ""}
    proposal = {"intents": ["preference_proposal", "scene_research"], "slots": {"artist": "", "targetMinutes": 20}, "missingSlots": [],
        "actions": [{"capability": "memory.propose", "source": "记住我喜欢粤语", "dependsOn": [], "parameters": {"operation": "set", "preference": "喜欢粤语"}},
            {"capability": "music.task", "source": "推荐20分钟微醺歌单", "dependsOn": [], "parameters": {"mode": "new"}}]}
    if mutation == "shape":
        proposal["actions"][0]["parameters"]["unsupportedKey"] = True
    elif mutation == "preference":
        proposal["actions"][0]["parameters"]["preference"] = "密码"
    elif mutation == "intent":
        proposal["intents"] = ["scene_research"]
    else:
        proposal["actions"][0]["source"] = "喜欢粤语"
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, input, messageId="memory-reason")
    assert error.value.code == "INVALID_ROUTE_PLAN" and error.value.routeReason == reason


async def test_actual_router_dispatches_original_clear_requests_without_model_or_quota(fx):
    for text in ["你能查询邓紫棋的新歌吗", "推荐跑步要听的歌", "帮我播放陈奕迅的十年", "帮我搜索陈奕迅的十年"]:
        body, _ = await fx.submit(text)
        response = await fx.route(body)
        assert response["route"]["status"] == "completed"
        assert response["route"]["paid"] is False
    assert fx.calls == [] and await used(fx) == 0


async def test_history_reads_prior_owned_conversation_beyond_six_messages_and_never_replays(fx):
    first, _ = await fx.submit("你能查询邓紫棋的新歌吗")
    await fx.route(first)
    second, _ = await fx.submit("帮我搜索陈奕迅的十年")
    await fx.route(second)
    # 未路由的无关消息不会把整个聊天送给模型，也不使已保存歌手消失。
    for number in range(9):
        await fx.submit("普通开发消息" + str(number))
    body, _ = await fx.submit("我之前问过你哪些歌手")
    result = await fx.route(body)
    reply = result["route"]["result"]["actions"][0]["parameters"]["text"]
    assert "邓紫棋" in reply and "陈奕迅" in reply and "最近 200 条已路由消息" in reply
    assert result["conversation"]["messages"][-1]["source"] == "route"
    assert (await fx.route(body))["route"] == result["route"]
    other = await fx.conversations.create(fx.user["id"], {"deviceId": DEVICE, "requestId": str(uuid4())})
    assert await fx.router.historyArtists(other["id"], fx.user["id"], DEVICE) == []
    assert await fx.router.historyArtists(fx.conversation["id"], fx.other["id"], DEVICE) == []
    assert await fx.router.historyArtists(fx.conversation["id"], fx.user["id"], "different-device") == []
    assert fx.calls == [] and await used(fx) == 0


async def test_route_rejection_logs_bounded_reason_without_chat_or_proposal(fx, caplog):
    secret = "PRIVATE_CHAT_SHOULD_NOT_BE_LOGGED"
    async def invalid(input, options=None):
        return {"value": {"intents": ["scene_research"], "actions": [], "missingSlots": [], "secret": secret}}
    fx.router.model.route = invalid
    body, _ = await fx.submit(secret)
    result = await fx.route(body)
    assert result["route"]["status"] == "failed"
    assert result["route"]["errorCode"] == "INVALID_ROUTE_PLAN"
    assert result["route"]["result"] is None
    assert "route_plan_rejected" in caplog.text and "reason=proposal_shape" in caplog.text
    assert body["messageId"] in caplog.text and secret not in caplog.text
