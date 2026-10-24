"""单曲完整语句解析的修复与授权边界回归，不代表真实模型或曲库效果。"""

import pytest

from tango_backend.music_lookup_intent import lookupRequest


@pytest.mark.parametrize("text,expected", [
    ("请查找南渡的《月台慢行》，我只看资料，不要播放，也不要保存成我的偏好。",
     {"artist": "南渡", "title": "月台慢行", "play": False}),
    ("请播放阔叶电台的《松针雨》录音室版。",
     {"artist": "阔叶电台", "title": "松针雨", "play": True, "version": "studio"}),
    ("帮我查找《晚风来信》，先展示候选就好。",
     {"artist": "", "title": "晚风来信", "play": False}),
    ("搜索《晚风来信》，只列结果。",
     {"artist": "", "title": "晚风来信", "play": False}),
    ("麻烦你帮我搜索“你的背包”，我只看资料，不播放不保存。",
     {"artist": "", "title": "你的背包", "play": False}),
    ("可以帮我播放陈奕迅的《十年》的现场版吗？",
     {"artist": "陈奕迅", "title": "十年", "play": True, "version": "live"}),
    ("搜索《你的背包》录音室版，先别放。",
     {"artist": "", "title": "你的背包", "play": False, "version": "studio"}),
])
def test_complete_lookup_sentence_is_understood(text, expected):
    assert lookupRequest(text) == expected


@pytest.mark.parametrize("text,expected", [
    ("搜一首陈奕迅唱的《十年》，暂时别播放", {"artist": "陈奕迅", "title": "十年", "play": False}),
    ("查找一首陈奕迅演唱的《十年》录音室版", {"artist": "陈奕迅", "title": "十年", "play": False, "version": "studio"}),
    ("搜索一首陈奕迅演唱的十年", {"artist": "陈奕迅", "title": "十年", "play": False}),
    ("播放一首陈奕迅唱的《十年》，先别放", {"artist": "陈奕迅", "title": "十年", "play": False}),
    ("搜索《一首简单的歌》", {"artist": "", "title": "一首简单的歌", "play": False}),
    ("搜一首《一首简单的歌》", {"artist": "", "title": "一首简单的歌", "play": False}),
])
def test_single_song_classifier_and_performer_suffix_preserve_title_and_permissions(text, expected):
    assert lookupRequest(text) == expected


@pytest.mark.parametrize("text", [
    "不要搜一首陈奕迅唱的《十年》", "朋友说搜一首陈奕迅演唱的《十年》", "如果搜一首陈奕迅唱的《十年》就播放",
    "搜一首陈奕迅唱的《十年》，暂时别播放然后删除所有歌曲", "搜一首陈奕迅唱的《十年》和《浮夸》",
])
def test_performer_quantity_does_not_hide_another_action_or_indirect_permission(text):
    assert lookupRequest(text) is None


@pytest.mark.parametrize("text", [
    "朋友说：请播放陈奕迅的《十年》", "如果我说请播放陈奕迅的《十年》", "假设我要搜索《十年》",
    "不要播放陈奕迅的《十年》", "不是让你播放陈奕迅的《十年》", "请播放《十年》，然后删除所有歌曲",
    "搜索《十年》，同时记住我喜欢陈奕迅", "搜索《十年》，必须使用最高音质", "搜索《十年》，如果有才播放",
    "搜索《十年》录音室版，选现场版", "请播放《十年》现场版和录音室版", "搜索《十年》和《浮夸》",
    "“播放陈奕迅的十年”", "请搜索《十年》，不要保存，之后自动播放", "搜索《十年》，不要删除账号",
])
def test_non_direct_or_unrepresented_instructions_are_not_partially_executed(text):
    assert lookupRequest(text) is None


@pytest.mark.parametrize("prefix", ["搜索", "帮我找", "麻烦你帮我查找", "请你搜一下", "可以帮我查询", "你能帮我查一下"])
@pytest.mark.parametrize("quotes", [("《", "》"), ("“", "”"), ("‘", "’"), ('"', '"'), ("「", "」"), ("『", "』")])
def test_title_delimiters_are_data_boundaries_not_reported_commands(prefix, quotes):
    opening, closing = quotes
    assert lookupRequest(prefix + opening + "如果的事" + closing) == {"artist": "", "title": "如果的事", "play": False}


@pytest.mark.parametrize("modifier", [
    "只看资料", "我只看资料", "先展示候选就好", "只列结果", "先列出候选即可", "只给我资料", "先让我看看",
    "把版本和时长给我就好", "先别放", "不要自动播放", "不用播放了", "不播放不保存", "不要保存偏好也不要播放",
])
def test_readonly_modifiers_override_an_initial_play_verb(modifier):
    assert lookupRequest("播放林间的《归航》，" + modifier) == {"artist": "林间", "title": "归航", "play": False}


@pytest.mark.parametrize("modifier", ["也别记住什么", "不要保存偏好", "也不要保存成我的偏好", "不保存", "不用保存为本机口味"])
def test_no_memory_clause_does_not_create_memory_or_revoke_explicit_playback(modifier):
    assert lookupRequest("播放《归航》，" + modifier) == {"artist": "", "title": "归航", "play": True}


@pytest.mark.parametrize("version,expected", [("录音室版", "studio"), ("现场版", "live"), ("LIVE版", "live"), ("Studio版", "studio")])
@pytest.mark.parametrize("shape", ["请播放林间的《归航》{version}", "请播放林间的《归航》，用{version}", "搜索林间的归航{version}"])
def test_known_version_is_always_separate_from_the_complete_title(version, expected, shape):
    text = shape.format(version=version)
    result = lookupRequest(text)
    assert result == {"artist": "林间", "title": "归航", "play": text.startswith("请播放"), "version": expected}


@pytest.mark.parametrize("text", [
    "搜索《十年", "搜索十年》", "搜索“十年》", "搜索《十年》现场版和录音室版", "搜索《十年》，用现场版，要录音室版",
    "播放《十年》，先别放，但是等会自动播放", "播放《十年》，我只看资料并且删除歌单", "搜索《十年》不要保存",
    "播放十年并下载", "搜索十年必须用最高音质", "搜索《十年》，我不需要你下载", "搜索《十年》，除非会员可用才放",
    "搜索《十年》，不要播放《浮夸》", "搜索《十年》‘现场版’", "搜索《十年》，只要时长小于3分钟的",
])
def test_unbalanced_titles_conflicts_and_unknown_operations_are_not_ignored(text):
    assert lookupRequest(text) is None


def test_title_punctuation_is_preserved_and_is_not_a_second_instruction():
    assert lookupRequest("搜索《Hello, goodbye》") == {"artist": "", "title": "Hello, goodbye", "play": False}
    assert lookupRequest("搜索《Don't Stop》") == {"artist": "", "title": "Don't Stop", "play": False}
    assert lookupRequest("搜索《不要说话》") == {"artist": "", "title": "不要说话", "play": False}


@pytest.mark.parametrize("value", [None, 1, [], {}, "", " ", "搜索《十年》\x00", "搜索《十年》\ud800", "搜索《" + "歌" * 201 + "》"])
def test_invalid_input_and_existing_utf16_limits_are_preserved(value):
    assert lookupRequest(value) is None


@pytest.mark.parametrize("prefix", ["再", "请再", "再帮我", "再次", "重新"])
@pytest.mark.parametrize("display", ["只看搜索结果", "只查看查询结果", "只看查找结果", "只看候选列表"])
def test_repeated_lookup_requests_and_search_result_display_are_complete_readonly_requests(prefix, display):
    assert lookupRequest(prefix + "查找林间的《归航》，" + display + "。") == {"artist": "林间", "title": "归航", "play": False}


@pytest.mark.parametrize("text,expected", [
    ("搜索陶喆的《普通朋友》，只看搜索结果。", {"artist": "陶喆", "title": "普通朋友", "play": False}),
    ("再查找周杰伦的《稻香》，不要播放。", {"artist": "周杰伦", "title": "稻香", "play": False}),
])
def test_persistent_route_replay_discovered_lookup_phrasings(text, expected):
    assert lookupRequest(text) == expected


@pytest.mark.parametrize("text", [
    "再查找林间的《归航》，只看搜索结果并且自动播放", "朋友说再查找林间的《归航》", "如果再查找林间的《归航》",
    "再查找林间的《归航》，只看搜索结果但永久保存", "再次不要播放林间的《归航》",
])
def test_repetition_or_result_display_does_not_hide_unconsumed_permissions(text):
    assert lookupRequest(text) is None
