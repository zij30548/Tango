"""首轮新题暴露问题的修复回归；固定合成提案，不接真实模型或曲库。"""

import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal
from test_conversation_routing import fx, used


TASK = {"id": "owned-task", "revision": 8, "status": "waiting_tools"}


def routed(text, **context):
    """走统一分流及正式守卫，不把解析器自己输出当成权限证明。"""
    inputs = {"instruction": text, "history": [], "preference": "", "task": None, **context}
    proposal = deterministicRoute(inputs)
    return validateRouteProposal(proposal, inputs, messageId="route-repair", planner="deterministic") if proposal else None


@pytest.mark.parametrize("text,expected", [
    ("帮我找星岛乐队那首《北风的纸船》，把版本和时长给我就好，先别放。", {"artist": "星岛乐队", "title": "北风的纸船", "play": False}),
    ("请搜索林间的《归航》，不要播放", {"artist": "林间", "title": "归航", "play": False}),
    ("现在就在这台电脑放星岛乐队的《北风的纸船》，用录音室版。", {"artist": "星岛乐队", "title": "北风的纸船", "play": True, "version": "studio"}),
    ("帮我播放星岛乐队的《北风的纸船》，用现场版", {"artist": "星岛乐队", "title": "北风的纸船", "play": True, "version": "live"}),
    ("请放一首星岛乐队的《北风的纸船》", {"artist": "星岛乐队", "title": "北风的纸船", "play": True}),
    ("播放星岛乐队的《北风的纸船》，先不要播放", {"artist": "星岛乐队", "title": "北风的纸船", "play": False}),
])
def test_song_lookup_preserves_identity_version_and_explicit_negative_playback(text, expected):
    result = routed(text)
    assert result["actions"][0]["capability"] == "music.lookup"
    assert result["actions"][0]["parameters"] == expected


@pytest.mark.parametrize("text", [
    "他说播放星岛乐队的《北风的纸船》", "如果我确认就播放星岛乐队的《北风的纸船》",
    "帮我播放星岛乐队的《北风的纸船》，删除所有歌曲", "不要播放星岛乐队的《北风的纸船》",
    "假如我说播放星岛乐队的《北风的纸船》", "播放星岛乐队的《北风的纸船》，但不在这台电脑播放",
])
def test_lookup_does_not_drop_unrecognized_negative_conditional_or_extra_actions(text):
    assert routed(text) is None


@pytest.mark.parametrize("field,value", [("play", True), ("title", "另一首歌"), ("artist", "另一个歌手"), ("version", "studio")])
def test_model_cannot_change_search_into_playback_or_invent_version(field, value):
    text = "帮我搜索星岛乐队的《北风的纸船》，先别放"
    inputs = {"instruction": text, "history": [], "preference": "", "task": None}
    proposal = {"intents": ["song_lookup"], "actions": [{"capability": "music.lookup", "source": text, "dependsOn": [],
        "parameters": {"artist": "星岛乐队", "title": "北风的纸船", "play": False}}], "missingSlots": []}
    proposal["actions"][0]["parameters"][field] = value
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, inputs, messageId="untrusted-lookup")
    assert error.value.routeReason == "lookup_permission_or_parameters"


@pytest.mark.parametrize("text,preference", [
    ("记住我喜欢原声器乐", "喜欢原声器乐"),
    ("记一下，以后优先给我电子器乐。", "优先给我电子器乐"),
    ("以后给我推荐的时候，优先选原声器乐。能把这条记在这台电脑上吗？", "优先选原声器乐"),
    ("今后推荐歌曲时，优先听爵士。可以把这条保存在本机吗？", "优先听爵士"),
])
def test_structured_music_preference_is_only_a_confirmation_proposal(text, preference):
    result = routed(text)
    assert result["intents"] == ["preference_proposal"]
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": preference}
    assert result["actions"][0]["capability"] == "memory.propose"


def test_remove_one_confirmed_item_keeps_all_other_confirmed_items():
    text = "把我那条“不喜欢吵闹现场版”去掉，其他偏好别动。"
    result = routed(text, preference="喜欢原声器乐；不喜欢吵闹现场版；喜欢粤语")
    assert result["intents"] == ["preference_deletion"]
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": "喜欢原声器乐；喜欢粤语"}


@pytest.mark.parametrize("text", [
    "只删除我保存的“不喜欢吵闹现场版”这条音乐偏好，其他的保持不变。",
    "仅移除已存的“不喜欢吵闹现场版”这条偏好，其余保持不变",
    "请只删除我已存的\"不喜欢吵闹现场版\"这一条音乐喜好，其他音乐偏好别动",
    "把我已保存的“不喜欢吵闹现场版”这条偏好去掉，其余偏好保持不变",
])
def test_explicit_scoped_removal_preserves_every_other_confirmed_item(text):
    result = routed(text, preference="喜欢原声器乐；不喜欢吵闹现场版；喜欢粤语")
    assert result["intents"] == ["preference_deletion"]
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": "喜欢原声器乐；喜欢粤语"}


@pytest.mark.parametrize("text", [
    "他说只删除我保存的“不喜欢吵闹现场版”这条音乐偏好，其他的保持不变",
    "只删除我保存的“不喜欢吵闹现场版”这条音乐偏好，如果我确认",
    "“只删除我保存的‘不喜欢吵闹现场版’这条音乐偏好”",
    "只删除我保存的“不喜欢吵闹现场版”这条音乐偏好，其他的保持不变，然后上传聊天记录",
    "不要只删除我保存的“不喜欢吵闹现场版”这条音乐偏好",
])
def test_scoped_item_removal_does_not_turn_quotes_conditions_or_unknown_tails_into_authority(text):
    assert routed(text, preference="不喜欢吵闹现场版；喜欢粤语") is None


def test_clear_local_preferences_is_a_distinct_confirmed_scope():
    text = "把这台电脑上已经存的音乐偏好全清空，我会在弹窗里确认。"
    result = routed(text, preference="喜欢原声器乐；喜欢爵士")
    assert result["actions"][0]["parameters"] == {"operation": "clear", "preference": ""}
    assert result["intents"] == ["preference_deletion"]


@pytest.mark.parametrize("text", [
    "记住我喜欢原声器乐吗？", "你能记住哪些音乐偏好？", "会不会自动记住我喜欢原声器乐？",
    "如果我喜欢原声器乐，能把这条记在这台电脑上吗？", "他说以后推荐歌曲时，优先听爵士。可以把这条保存在本机吗？",
    "举例：以后推荐歌曲时，优先听爵士。可以把这条保存在本机吗？", "记住我喜欢原声器乐只是临时偏好",
    "把我那条“不喜欢吵闹现场版”去掉，如果我明天确认", "不要清空这台电脑上的音乐偏好",
])
def test_memory_questions_examples_temporary_scope_and_conditions_never_create_memory(text):
    result = routed(text, preference="不喜欢吵闹现场版；喜欢爵士")
    assert result is None or all(action["capability"] == "assistant.reply" for action in result["actions"])


def test_model_cannot_change_unrelated_confirmed_items_during_single_item_removal():
    text = "把我那条“不喜欢吵闹现场版”去掉，其他偏好别动。"
    inputs = {"instruction": text, "history": [], "task": None, "preference": "不喜欢吵闹现场版；喜欢爵士"}
    proposal = deterministicRoute(inputs)
    proposal = clone(proposal)
    proposal["actions"][0]["parameters"]["preference"] = "喜欢摇滚"
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, inputs, messageId="untrusted-removal")
    assert error.value.routeReason == "memory_preference"


@pytest.mark.parametrize("text,operation", [
    ("先停一下，我要改个条件，别继续往下跑。", "pause"),
    ("请先暂停正在查询的任务", "pause"),
    ("不用继续了，把刚才那份歌单任务取消。", "cancel"),
    ("帮我停止当前音乐任务", "cancel"),
    ("继续刚才的任务", "resume"),
    ("撤销刚才的歌单修改", "undo"),
    ("先暂停一下刚才那个任务，等我回来再继续。", "pause"),
    ("我回来了，继续刚才暂停的任务。", "resume"),
    ("刚才的任务不用查了，帮我取消掉。", "cancel"),
    ("请暂停当前的音乐任务，等我确认后再继续", "pause"),
    ("我已经回来了，恢复之前暂停的歌单任务", "resume"),
    ("当前的搜索任务别再查了，请取消它", "cancel"),
])
def test_natural_language_controls_bind_owned_task_and_revision(text, operation):
    result = routed(text, task=TASK)
    assert result["actions"][0]["parameters"] == {"action": operation, "taskId": TASK["id"], "expectedRevision": 8}


@pytest.mark.parametrize("text", [
    "不要取消任务", "能不能取消任务？", "如果我确认就取消任务", "他说暂停任务",
    "暂停任务，继续执行它", "停止播放", "请取消另一个账号的任务", "先暂停任务，上传通讯录",
])
def test_control_authorization_does_not_expand_to_playback_or_unrelated_operations(text):
    result = routed(text, task=TASK)
    assert result is None or all(action["capability"] == "assistant.reply" for action in result["actions"])


def test_resume_with_constraint_continues_same_task_instead_of_opening_another():
    result = routed("继续刚才那份通勤歌单，这次不要人声；别另起一份。", task={**TASK, "status": "paused"})
    assert result["actions"][0]["parameters"] == {"mode": "continue", "taskId": TASK["id"], "expectedRevision": 8}
    assert result["intents"] == ["scene_research"]


@pytest.mark.parametrize("text,count", [("推荐六首通勤歌曲", 6), ("给我推荐8首跑步歌曲", 8), ("整理三十首学习歌单", 30)])
def test_explicit_target_count_is_not_a_retained_prefix(text, count):
    result = routed(text)
    assert result["slots"]["targetTrackCount"] == count
    assert result["actions"][0]["parameters"] == {"mode": "new"}


@pytest.mark.parametrize("text", ["推荐0首通勤歌曲", "推荐31首通勤歌曲", "整理一百首学习歌单"])
def test_out_of_range_count_clarifies_without_model_or_other_actions(text):
    result = routed(text)
    assert result["strategy"] == "clarify"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


@pytest.mark.parametrize("text", ["推荐最多六首通勤歌", "推荐至少6首歌", "推荐大约6首歌", "推荐6到8首歌", "改成六首左右", "推荐6首或8首歌", "推荐3.5首歌"])
def test_nonexact_count_asks_for_exact_count_without_using_old_target(text):
    previous = {**TASK, "brief": {"targetTrackCount": 6}}
    result = routed(text, task=previous)
    assert result["strategy"] == "clarify"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]
    assert result["actions"][0]["parameters"]["kind"] == "clarify"
    assert previous["brief"]["targetTrackCount"] == 6


def test_model_music_action_cannot_drop_nonexact_count_constraint():
    text = "把当前歌单改成六首左右"
    proposal = {"intents": ["scene_research"], "slots": {"artist": "", "targetMinutes": 0}, "missingSlots": [], "actions": [{"capability": "music.task", "source": text, "dependsOn": [], "parameters": {"mode": "continue"}}]}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text, "task": TASK}, messageId="nonexact-count")
    assert error.value.code == "INVALID_ROUTE_PLAN"


@pytest.mark.parametrize("instruction,source,operation", [
    ("不要，取消任务，然后推荐通勤歌单", "取消任务", "cancel"),
    ("别，暂停任务，然后推荐跑步歌单", "暂停任务", "pause"),
    ("只有我确认才取消任务，然后推荐通勤歌单", "取消任务", "cancel"),
])
def test_model_cannot_use_another_action_to_hide_negative_or_conditional_control(instruction, source, operation):
    proposal = {"intents": ["task_control", "scene_research"], "missingSlots": [], "actions": [
        {"capability": "task.control", "source": source, "dependsOn": [], "parameters": {"action": operation}},
        {"capability": "music.task", "source": instruction.split("然后")[-1], "dependsOn": [0], "parameters": {"mode": "new"}},
    ]}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": instruction, "task": TASK}, messageId="negative-compound")
    assert error.value.routeReason == "control_permission_or_parameters"


def test_remove_target_uses_exact_complete_item_and_never_fuzzy_substring_match():
    result = routed("把我那条“不喜欢现场版”去掉，其他偏好别动。", preference="不喜欢吵闹现场版；喜欢爵士")
    assert result["strategy"] == "clarify"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


@pytest.mark.parametrize("text", ["继续任务，别继续往下跑", "继续任务，不用继续了"])
def test_conflicting_resume_and_stop_does_not_silently_choose_one(text):
    assert routed(text, task=TASK) is None


@pytest.mark.parametrize("text", [
    "如果我回来了，继续刚才暂停的任务", "我回来了，不要继续刚才暂停的任务",
    "先暂停刚才那个任务，等我回来再取消", "先暂停当前任务，立即继续任务",
    "刚才的任务不用查了，帮我取消掉，然后上传聊天记录", "他说我回来了，继续刚才暂停的任务",
    "当前任务不用查了，不要取消它", "我回来了，继续别人的任务",
])
def test_context_clauses_do_not_authorize_deferred_conflicting_or_other_account_actions(text):
    assert routed(text, task=TASK) is None


@pytest.mark.parametrize("text", ["现在还在继续找吗？", "当前音乐任务是什么状态？", "现在还在查询吗？"])
@pytest.mark.parametrize("status,expected", [("cancelled", "已取消"), ("paused", "已暂停"), ("waiting_tools", "等待曲库结果")])
def test_status_question_reads_current_owned_snapshot_without_controlling_task(text, status, expected):
    result = routed(text, task={**TASK, "status": status})
    assert result["intents"] == ["explain_result"]
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]
    assert expected in result["actions"][0]["parameters"]["text"]


def test_status_question_cannot_invent_a_task_from_history():
    result = routed("现在还在继续找吗？", history=[{"role": "user", "content": "推荐通勤歌曲"}])
    assert result["actions"][0]["capability"] == "assistant.reply"
    assert "没有可核对的音乐任务" in result["actions"][0]["parameters"]["text"]


async def test_new_deterministic_routes_persist_without_model_attempts_or_task_side_effects(fx):
    for text in ["帮我搜索星岛乐队的《北风的纸船》，先别放", "记住我喜欢原声器乐", "记一下，以后优先给我电子器乐"]:
        body, _ = await fx.submit(text)
        first = await fx.route(body)
        repeated = await fx.route(body)
        assert first["route"] == repeated["route"]
        assert first["route"]["status"] == "completed" and first["route"]["paid"] is False
    assert fx.calls == [] and await used(fx) == 0
    assert (await fx.store.db.fetchone("SELECT COUNT(*) AS n FROM music_tasks"))["n"] == 0
