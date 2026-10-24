"""原题遗漏与相邻表达的离线回归；错误模型提案也必须服从用途边界。"""

import pytest

from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal


def inputs(text, preference=""):
    """每次只使用合成上下文，不接模型、曲库或用户数据库。"""
    return {"instruction": text, "history": [], "task": None, "preference": preference}


def routed(text, preference=""):
    """与产品共用需求入口和权限校验器。"""
    value = inputs(text, preference)
    proposal = deterministicRoute(value)
    return validateRouteProposal(proposal, value, messageId="residual-route", planner="deterministic") if proposal else None


def proposed(text, capability, parameters, intent, preference="", source=None):
    """构造不可信模型提案；验证器必须独立判断完整用户授权。"""
    proposal = {"intents": [intent], "actions": [{"capability": capability, "source": text if source is None else source,
        "dependsOn": [], "parameters": parameters}], "missingSlots": [], "slots": {"artist": "", "targetMinutes": 0}}
    return validateRouteProposal(proposal, inputs(text, preference), messageId="residual-untrusted")


@pytest.mark.parametrize("text", [
    "我喜欢原声器乐，请帮我记住", "我偏好爵士，帮我保存这条偏好", "记住我以后喜欢原声器乐",
])
def test_preference_statement_and_save_share_one_bound_command(text):
    result = routed(text)
    assert result and result["intents"] == ["preference_proposal"]
    assert [a["capability"] for a in result["actions"]] == ["memory.propose"]
    assert result["actions"][0]["parameters"]["operation"] == "set"


@pytest.mark.parametrize("text", ["把我喜欢粤语这条偏好删掉，其他的不变", "把我已保存的喜欢粤语这条偏好移除，其余偏好别动"])
def test_inverted_single_preference_deletion_preserves_other_items(text):
    result = routed(text, "喜欢粤语；偏好爵士")
    assert result and result["intents"] == ["preference_deletion"]
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": "偏好爵士"}


@pytest.mark.parametrize("text", ["把已保存的音乐偏好全部删除", "把我的音乐偏好全清空"])
def test_clear_scope_can_appear_before_the_delete_verb(text):
    result = routed(text, "喜欢粤语；偏好爵士")
    assert result and result["actions"][0]["parameters"] == {"operation": "clear", "preference": ""}


@pytest.mark.parametrize("text", ["我喜欢原声器乐，请帮我记住", "把我喜欢粤语这条偏好删掉，其他的不变", "修改我的音乐偏好", "删掉我以前保存的那条音乐偏好"])
def test_preference_only_request_cannot_become_a_music_task(text):
    with pytest.raises(ApiError):
        proposed(text, "music.task", {"mode": "new"}, "scene_research", "喜欢粤语；偏好爵士")


@pytest.mark.parametrize("text", ["修改我的音乐偏好", "删掉我以前保存的那条音乐偏好"])
def test_ambiguous_preference_management_only_clarifies(text):
    result = routed(text, "喜欢粤语；偏好爵士")
    assert result and result["strategy"] == "clarify"
    assert [a["capability"] for a in result["actions"]] == ["assistant.reply"]


@pytest.mark.parametrize("text", [
    "如果我喜欢原声器乐，请帮我记住", "他说我喜欢原声器乐，请帮我记住", "我喜欢原声器乐，不要帮我记住",
    "我今晚喜欢原声器乐，请帮我记住", "“我喜欢原声器乐，请帮我记住”", "我喜欢原声器乐，请帮我记住吗？",
])
def test_save_variants_keep_conditional_quoted_negative_and_temporary_boundaries(text):
    result = routed(text)
    assert result is None or all(a["capability"] == "assistant.reply" for a in result["actions"])
    with pytest.raises(ApiError):
        proposed(text, "memory.propose", {"operation": "set", "preference": "喜欢原声器乐"}, "preference_proposal")


def test_compound_preference_and_music_still_has_both_actions():
    result = routed("记住我喜欢粤语，然后推荐20分钟通勤歌单")
    assert [a["capability"] for a in result["actions"]] == ["memory.propose", "music.task"]


def test_negative_memory_constraint_does_not_block_model_music_proposal():
    result = proposed("我这次喝酒想听20分钟爵士，不要把这次喜好记住。", "music.task", {"mode": "new"}, "scene_research")
    assert result["actions"][0]["capability"] == "music.task"


@pytest.mark.parametrize("tail", ["最近发行的曲目", "最近发了什么", "最近发了什么。"])
def test_uncompiled_compound_query_still_accepts_separate_memory_and_research(tail):
    query = "查一下邓紫棋" + tail
    text = "记住我喜欢粤语，再" + query
    proposal = {"intents": ["preference_proposal", "release_research"], "actions": [
        {"capability": "memory.propose", "source": "记住我喜欢粤语", "dependsOn": [], "parameters": {"operation": "set", "preference": "喜欢粤语"}},
        {"capability": "music.task", "source": query, "dependsOn": [], "parameters": {"mode": "new"}},
    ], "missingSlots": [], "slots": {"artist": "邓紫棋", "targetMinutes": 0}}
    result = validateRouteProposal(proposal, inputs(text), messageId="compound-query")
    assert [a["capability"] for a in result["actions"]] == ["memory.propose", "music.task"]


@pytest.mark.parametrize("text,source", [
    ("记住我喜欢粤语吗", "记住我喜欢粤语"),
    ("记住我喜欢粤语么", "记住我喜欢粤语"),
    ("记住我喜欢粤语什么", "记住我喜欢粤语"),
    ("记住我喜欢粤语，再查一下邓紫棋最近发了什么吗", "记住我喜欢粤语"),
    ("记住我喜欢粤语，再查一下邓紫棋最近发了什么？", "记住我喜欢粤语"),
    ("如果记住我喜欢粤语，再查一下邓紫棋最近发了什么", "记住我喜欢粤语"),
    ("记住我喜欢粤语只是举例，再查一下邓紫棋最近发了什么", "记住我喜欢粤语"),
    ("他说记住我喜欢粤语，再查一下邓紫棋最近发了什么", "记住我喜欢粤语"),
])
def test_query_word_exception_does_not_authorize_memory_questions_or_truncated_sources(text, source):
    with pytest.raises(ApiError):
        proposed(text, "memory.propose", {"operation": "set", "preference": "喜欢粤语"}, "preference_proposal", source=source)


@pytest.mark.parametrize("text,title", [
    ("找一下《晚风来信》，别放，先让我看看。", "晚风来信"),
    ("找一下《窗边的航线》，只给我资料，别播放，也别记住什么。", "窗边的航线"),
    ("搜索《晚风来信》，只看资料，不要保存偏好", "晚风来信"),
])
def test_original_readonly_lookup_modifiers_do_not_require_model_luck(text, title):
    result = routed(text)
    assert result and result["intents"] == ["song_lookup"]
    assert result["actions"][0]["parameters"] == {"artist": "", "title": title, "play": False}
    with pytest.raises(ApiError):
        proposed(text, "music.lookup", {"artist": "", "title": title, "play": True}, "song_lookup")
    with pytest.raises(ApiError):
        proposed(text, "memory.propose", {"operation": "set", "preference": "喜欢爵士"}, "preference_proposal")


@pytest.mark.parametrize("tail", ["发送给我的朋友", "删除所有歌曲", "如果可以再播放", "请自动保存到我的账号"])
def test_lookup_does_not_silently_drop_unrecognized_trailing_actions(tail):
    text = "搜索《晚风来信》，" + tail
    assert routed(text) is None
    with pytest.raises(ApiError):
        proposed(text, "music.lookup", {"artist": "", "title": "晚风来信", "play": False}, "song_lookup")


@pytest.mark.parametrize("text", ["我想知道邓紫棋最近有没有发新歌", "想问一下邓紫棋最近有没有新歌", "给我看看邓紫棋的新歌"])
def test_release_query_prefix_is_not_an_artist_name(text):
    result = routed(text)
    assert result and result["slots"]["artist"] == "邓紫棋"
