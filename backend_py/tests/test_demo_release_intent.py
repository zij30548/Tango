"""发行查询完整语句与来源边界回归；不调用模型、网易云或用户数据库。"""

import pytest

from tango_backend.release_intent import releaseRequestArtist
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal


@pytest.mark.parametrize("text,artist", [
    ("帮我查周棠最近90天已经发行的新作，遇到同名歌手先让我确认。", "周棠"),
    ("林俊杰近90天有哪些已经发行的新歌？先查出来给我看，不要自动播放。", "林俊杰"),
    ("你能查询邓紫棋的新歌吗", "邓紫棋"),
    ("最近邓紫棋有没有发新歌", "邓紫棋"),
    ("我想知道邓紫棋最近有没有发新歌", "邓紫棋"),
    ("想问一下邓紫棋最近有没有新歌", "邓紫棋"),
    ("给我看看邓紫棋的新歌", "邓紫棋"),
])
def test_complete_release_lookup_retains_the_original_artist(text, artist):
    assert releaseRequestArtist(text) == artist


@pytest.mark.parametrize("text", [
    "他说帮我查周棠最近90天新作", "如果可以查林俊杰最近90天新歌", "不要查邓紫棋新歌",
    "帮我查周棠最近90天新作，然后自动播放", "帮我查周棠最近90天新作，保存我的偏好", "查周棠和林俊杰的新歌",
    "帮我查周棠最近90天新作，必须全部免费", "查周棠的新歌，遇到同名歌手就自动选第一个",
])
def test_quoted_conditional_multi_artist_or_unknown_action_is_not_partially_run(text):
    assert releaseRequestArtist(text) is None


@pytest.mark.parametrize("prefix", ["查询", "你能查询", "帮我查", "麻烦你帮我看看", "我想知道", "想问一下"])
@pytest.mark.parametrize("window", ["", "最近", "最近90天", "近30天", "过去2周", "最近3个月"])
def test_current_artist_name_is_not_mixed_with_query_verbs_or_dates(prefix, window):
    assert releaseRequestArtist(prefix + "林俊杰" + window + "有哪些已经发行的新歌") == "林俊杰"


@pytest.mark.parametrize("suffix", ["的新歌", "有没有新歌", "有没有发新歌", "是否有发布的新作", "推出的新专辑有哪些", "新专辑有哪些"])
def test_compatible_simple_release_phrases_keep_the_original_name(suffix):
    assert releaseRequestArtist("查邓紫棋" + suffix) == "邓紫棋"


@pytest.mark.parametrize("modifier", [
    "先查出来给我看", "先展示候选就好", "只列结果", "我只看资料", "不要自动播放", "先别放", "不要保存偏好",
    "遇到同名歌手先让我确认", "如果有同名艺人先和我核对身份", "同名歌手由我确认歌手身份",
])
def test_whitelisted_constraints_are_consumed_without_adding_a_second_action(modifier):
    assert releaseRequestArtist("查询林俊杰近90天已经发行的新歌，" + modifier) == "林俊杰"


@pytest.mark.parametrize("text", [
    "查林俊杰近0天的新歌", "查林俊杰近10000天的新歌", "查林俊杰最近90天近30天的新歌", "最近30天查林俊杰近60天的新歌",
    "查林俊杰未来90天的新歌", "查林俊杰未发行的新歌", "查林俊杰没有发行的新歌", "查林俊杰的新歌和旧歌",
    "查林俊杰与周棠的新歌", "查林俊杰、周棠的新歌", "查林俊杰的新歌，播放第一首", "查林俊杰的新歌，如果查不到就找周棠",
    "查林俊杰的新歌，遇到同名歌手就选第一位", "查林俊杰的新歌，不要播放但自动保存", "查林俊杰的新歌，不能查就自动改搜周棠",
    "朋友让我查林俊杰的新歌", "不是让你查询林俊杰的新歌", "我不想查林俊杰新歌", "查询新歌", "查最近30天的新歌",
    "查谁的新歌", "查哪个歌手的新歌", "查林俊杰的新歌，再查唱片公司", "查林俊杰的新歌，账号给我升级会员", "最近有什么新歌",
])
def test_unknown_constraints_and_ambiguous_targets_do_not_form_a_release_route(text):
    assert releaseRequestArtist(text) is None


@pytest.mark.parametrize("value", [None, 1, {}, [], "", " ", "查林俊杰新歌\x00", "查林俊杰新歌\ud800", "查" + "甲" * 101 + "的新歌"])
def test_invalid_values_and_existing_text_limits_are_rejected(value):
    assert releaseRequestArtist(value) is None


@pytest.mark.parametrize("artist", ["五月天", "G.E.M.邓紫棋", "Taylor Swift", "许茹芸", "周棠"])
def test_valid_original_names_are_retained_without_alias_or_id_invention(artist):
    assert releaseRequestArtist("查" + artist + "的新歌") == artist


@pytest.mark.parametrize("modifier", ["只做预览", "先做预览", "仅做预览", "只给预览", "只给我预览", "先生成一份预览", "仅提供歌单预览即可"])
def test_release_preview_only_variants_are_explicit_readonly_constraints(modifier):
    text = "查询G.E.M.邓紫棋最近90天的新歌，" + modifier
    assert releaseRequestArtist(text) == "G.E.M.邓紫棋"


@pytest.mark.parametrize("modifier", ["只做预览", "先做预览", "仅做预览", "只给预览"])
def test_preview_only_release_survives_formal_routing_with_an_existing_scene_task(modifier):
    text = "查询G.E.M.邓紫棋最近90天的新歌，" + modifier
    inputs = {"instruction": text, "history": [], "preference": "喜欢粤语", "task": {
        "id": "original-scene-task", "revision": 6, "status": "adopted", "brief": {"scene": "微醺", "targetMinutes": 20},
    }}
    proposal = deterministicRoute(inputs)
    assert proposal is not None
    result = validateRouteProposal(proposal, inputs, messageId="release-preview-readonly", planner="deterministic")
    assert result["intents"] == ["release_research"]
    assert result["slots"]["artist"] == "G.E.M.邓紫棋"
    assert len(result["actions"]) == 1
    action = result["actions"][0]
    assert action["capability"] == "music.task"
    assert action["parameters"] == {"mode": "new"}
    assert action["source"] == text
    assert action["dependsOn"] == []


@pytest.mark.parametrize("modifier", [
    "只做预览后自动播放", "先做预览再保存偏好", "如果只做预览", "仅做预览并删除旧歌单", "只给预览同时发给朋友",
    "不要只做预览", "只做预览吗", "先做预览，但是直接播放", "仅生成预览，自动采用",
])
def test_preview_only_modifier_never_absorbs_unknown_positive_or_conditional_actions(modifier):
    assert releaseRequestArtist("查询林俊杰最近90天的新歌，" + modifier) is None
