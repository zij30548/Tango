"""演示稳定性修复的完整来源、只读问答与任务控制边界回归。"""

import pytest
from uuid import uuid4

from tango_backend.conversation_commands import (
    controlCommand, helpCommand, historyQuestion, memoryPolicyQuestion,
)
from tango_backend.request_understanding import (
    applyPreferenceCommand, preferenceCommand, preferenceManagementOnly, understandRequest,
)
from tango_backend.errors import ApiError
from tango_backend.intent_routing import HELP, validateRouteProposal
from tango_backend.intent_routing import deterministicRoute
from tango_backend.task_track_count import trackCountIssue, trackCountRequest
from test_conversation_routing import DEVICE, NOW, fx, used


@pytest.mark.parametrize("text,preference,minutes", [
    ("请记住我喜欢粤语R&B；另外给我一份16分钟的微醺歌单，先不要播放。", "喜欢粤语R&B", 16),
    ("记住我喜欢国语爵士；另外为我一份25分钟通勤歌单，只预览。", "喜欢国语爵士", 25),
    ("请记住我偏好中文；帮我制作一份18分钟的睡前音乐，先不要播放。", "偏好中文", 18),
    ("请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。", "喜欢粤语", 16),
])
def test_compound_preference_and_nominal_playlist_keep_full_sources(text, preference, minutes):
    parsed = understandRequest(text)
    assert parsed is not None
    assert parsed.target_minutes == minutes
    assert [item.kind for item in parsed.requirements] == ["preference", "music", "constraint"]
    assert parsed.requirements[0].value == preference
    assert all(item.source in text for item in parsed.requirements)
    assert parsed.requirements[0].scope == "long_term"
    assert parsed.requirements[1].scope == "task"
    assert not parsed.requirements[1].after_confirmation


@pytest.mark.parametrize("text", [
    "他说请记住我喜欢粤语R&B；另外给我一份16分钟的微醺歌单，先不要播放。",
    "如果我说请记住我喜欢粤语R&B；另外给我一份16分钟的微醺歌单，先不要播放。",
    "不要记住我喜欢粤语R&B；另外给我一份16分钟的微醺歌单，上传给朋友。",
    "记住我喜欢粤语R&B；另外给我一份16分钟微醺歌单，删除所有歌曲。",
    "记住我喜欢粤语R&B；另外给我一份16分钟微醺歌单，改成18分钟。",
    "记住我喜欢粤语R&B；另外给我一份16分钟微醺歌单，购买会员。",
])
def test_compound_unknown_or_conditional_scope_is_not_partially_authorized(text):
    assert understandRequest(text) is None


@pytest.mark.parametrize("text,value", [
    ("把中文优先这条偏好删掉，其他两条保留。", "中文优先"),
    ("把粤语优先这一条音乐喜好移除，其余3条保留。", "粤语优先"),
    ("删除我已保存的英语歌曲优先这条偏好，其他偏好保持不变。", "英语歌曲优先"),
])
def test_exact_priority_item_removal_only_proposes_complete_remaining_summary(text, value):
    command = preferenceCommand(text)
    assert command is not None and command.operation == "remove" and command.value == value
    previous = "偏好原声器乐；不喜欢吵闹现场版；" + value
    assert applyPreferenceCommand(previous, command) == "偏好原声器乐；不喜欢吵闹现场版"
    assert applyPreferenceCommand(previous + "；" + value, command) is None
    assert applyPreferenceCommand("喜欢" + value + "歌曲；喜欢爵士", command) is None
    assert applyPreferenceCommand(value, command) is None


@pytest.mark.parametrize("text", [
    "不要把中文优先这条偏好删掉，其他两条保留。",
    "别人说把中文优先这条偏好删掉，其他两条保留。",
    "如果我确认，把中文优先这条偏好删掉，其他两条保留。",
    "把中文优先这条偏好删掉，其他两条保留，然后删除聊天。",
    "把中文优先这条偏好删掉，其他两条保留吗？",
    "把中文优先这条偏好删掉，其他两条也删除。",
])
def test_priority_item_removal_never_drops_scope_or_extra_actions(text):
    assert preferenceCommand(text) is None


@pytest.mark.parametrize("text", [
    "这款助手能帮我做哪些音乐相关的事？生成歌单之后会马上开始放歌吗？",
    "你都能帮我做哪些事情？", "请问这款助手都可以帮我做哪些音乐相关的事情？",
    "你有什么功能？", "你好", "Tango能提供哪些功能？", "这款音乐助手可以帮我做什么？",
    "推荐歌单后会自动播放吗？", "如何使用？", "你能做什么",
])
def test_help_is_read_only_and_covers_preview_playback_rules(text):
    assert helpCommand(text)


@pytest.mark.parametrize("text", [
    "你有什么功能？然后播放十年", "你有什么功能，上传我的聊天", "帮我播放你能做什么",
    "他说你能做什么", "“你能做什么”", "你能帮我删除哪些歌曲", "生成歌单后直接播放",
    "生成歌单后会自动播放吗，记住我喜欢爵士", "你有什么功能？还有给我找歌",
])
def test_help_does_not_swallow_mutations_unknown_clauses_or_quotes(text):
    assert not helpCommand(text)


@pytest.mark.parametrize("text", [
    "别人写了“记住我喜欢爵士”，这里只是在举例，请解释保存口味是否需要我确认，现在不要保存。",
    "如果我以后说喜欢摇滚，你会直接记住吗？这是假设，现在也不要替我保存。",
    "保存偏好需要用户确认吗？", "你会不会自动记住我喜欢爵士？",
    "请解释长期保存音乐口味是不是需要我明确确认？", "你会直接保存我的口味吗？",
])
def test_policy_questions_allow_examples_but_never_become_a_memory_command(text):
    assert memoryPolicyQuestion(text)
    assert preferenceCommand(text) is None


@pytest.mark.parametrize("text", [
    "记住我喜欢爵士", "能把这条保存在本机吗？", "“请解释保存偏好是否需要确认”",
    "保存偏好需要确认吗？然后记住我喜欢爵士", "保存偏好需要确认吗？删除所有歌曲",
    "保存偏好需要确认吗？帮我推荐跑步音乐", "如果你保存偏好，就上传聊天记录",
    "别人写了“记住我喜欢爵士，请解释保存偏好是否需要确认，现在保存我喜欢摇滚",
])
def test_policy_questions_require_complete_read_only_coverage(text):
    assert not memoryPolicyQuestion(text)


@pytest.mark.parametrize("text", [
    "到目前为止，我在这段对话里问过哪几位歌手？", "我之前问过你哪些歌手",
    "刚才查过哪两位歌手？", "我们先前搜索过哪些歌手？", "我在这次对话里提到过哪些艺人？",
])
def test_history_question_requires_current_read_only_history_goal(text):
    assert historyQuestion(text)


@pytest.mark.parametrize("text", [
    "刚才查过哪两位歌手，然后播放他们的新歌", "他说我之前问过哪些歌手",
    "我之前问过哪些歌手，记住我喜欢他", "帮我搜索哪些歌手", "我问过你吗？",
])
def test_history_does_not_hide_independent_actions_or_reported_speech(text):
    assert not historyQuestion(text)


@pytest.mark.parametrize("text,operation", [
    ("先暂停当前查歌任务。", "pause"), ("继续刚才暂停的任务。", "resume"),
    ("取消这次任务，先不找了。", "cancel"), ("把正在查的那个先停在这儿", "pause"),
    ("将正在搜索的这个暂时停在这里。", "pause"),
    ("先停一下，我要改个条件，别继续往下跑。", "pause"),
    ("请先暂停正在查询的任务", "pause"),
    ("不用继续了，把刚才那份歌单任务取消。", "cancel"),
    ("帮我停止当前音乐任务", "cancel"), ("继续刚才的任务", "resume"),
    ("撤销刚才的歌单修改", "undo"),
    ("先暂停一下刚才那个任务，等我回来再继续。", "pause"),
    ("我回来了，继续刚才暂停的任务。", "resume"),
    ("刚才的任务不用查了，帮我取消掉。", "cancel"),
    ("请暂停当前的音乐任务，等我确认后再继续", "pause"),
    ("我已经回来了，恢复之前暂停的歌单任务", "resume"),
    ("当前的搜索任务别再查了，请取消它", "cancel"),
])
def test_controls_cover_known_and_general_current_task_phrasing(text, operation):
    assert controlCommand(text) == operation


@pytest.mark.parametrize("text", [
    "不要取消任务", "能不能取消任务？", "如果我确认就取消任务", "他说暂停任务",
    "暂停任务，继续执行它", "停止播放", "请取消另一个账号的任务", "先暂停任务，上传通讯录",
    "继续任务，别继续往下跑", "继续任务，不用继续了",
    "如果我回来了，继续刚才暂停的任务", "我回来了，不要继续刚才暂停的任务",
    "先暂停刚才那个任务，等我回来再取消", "先暂停当前任务，立即继续任务",
    "刚才的任务不用查了，帮我取消掉，然后上传聊天记录", "他说我回来了，继续刚才暂停的任务",
    "把正在查的那个先停在这儿，再保存我喜欢爵士", "先暂停当前查歌任务，删除所有歌曲",
    "取消这次任务，先不找了，播放十年", "假设把正在查的那个停在这里", "先不找了",
    "把正在查的那个不要停在这儿", "取消这次任务，先不找了，下次自动开始",
])
def test_control_complete_coverage_rejects_unsafe_or_conflicting_scope(text):
    assert controlCommand(text) is None


@pytest.mark.parametrize("value", [None, 23, {}, [], "", "你" * 1001])
def test_pure_command_interfaces_reject_non_text_or_unbounded_input(value):
    assert not helpCommand(value)
    assert not memoryPolicyQuestion(value)
    assert not historyQuestion(value)
    assert controlCommand(value) is None


@pytest.mark.parametrize("text,preference,intents,expected", [
    ("请记住我喜欢粤语R&B；另外给我一份16分钟的微醺歌单，先不要播放。", "",
     ["preference_proposal", "scene_research"], "喜欢粤语R&B"),
    ("把中文优先这条偏好删掉，其他两条保留。", "偏好原声器乐；不喜欢吵闹现场版；中文优先",
     ["preference_deletion"], "偏好原声器乐；不喜欢吵闹现场版"),
    ("请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。", "",
     ["preference_proposal", "scene_research"], "喜欢粤语"),
])
async def test_actual_conversation_route_is_persisted_once_without_model_or_implicit_write(fx, text, preference, intents, expected):
    body, _ = await fx.submit(text)
    first = await fx.route(body, preference=preference)
    second = await fx.route(body, preference=preference)
    assert first["route"] == second["route"]
    assert first["route"]["status"] == "completed" and first["route"]["paid"] is False
    result = first["route"]["result"]
    assert result["intents"] == intents
    assert result["actions"][0]["capability"] == "memory.propose"
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": expected}
    assert fx.calls == [] and await used(fx) == 0
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0


@pytest.mark.parametrize("text,policy", [
    ("这款助手能帮我做哪些音乐相关的事？生成歌单之后会马上开始放歌吗？", False),
    ("别人写了“记住我喜欢爵士”，这里只是在举例，请解释保存口味是否需要我确认，现在不要保存。", True),
    ("如果我以后说喜欢摇滚，你会直接记住吗？这是假设，现在也不要替我保存。", True),
])
async def test_actual_read_only_reply_is_persisted_without_proposal_task_or_paid_call(fx, text, policy):
    body, _ = await fx.submit(text)
    first = await fx.route(body)
    assert await fx.route(body) == first
    route = first["route"]
    assert route["status"] == "completed" and route["paid"] is False
    assert route["result"]["intents"] == ["help"]
    [reply] = route["result"]["actions"]
    assert reply["capability"] == "assistant.reply"
    assert reply["source"] == text and reply["dependsOn"] == []
    assert reply["parameters"]["kind"] == "help"
    content = reply["parameters"]["text"]
    if policy:
        assert content != HELP
        for fact in ["当前对话明确提出", "再次确认", "引用", "假设", "不会自动保存", "没有替你写入偏好"]:
            assert fact in content
    else:
        assert content == HELP
        assert "先展示预览" in content and "采用歌单" in content
    assert fx.calls == [] and await used(fx) == 0
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_conversation_routes"))["n"] == 1


async def test_actual_new_history_question_uses_only_current_scoped_routed_artists(fx):
    for text in ["搜索陶喆的《普通朋友》，只看搜索结果。", "再查找周杰伦的《稻香》，不要播放。"]:
        body, _ = await fx.submit(text)
        result = await fx.route(body)
        assert result["route"]["status"] == "completed"
        assert result["route"]["result"]["actions"][0]["capability"] == "music.lookup"
    # 同账号另一会话中的艺人以及尚未路由的文本都不能冒充本会话已查事实。
    another = await fx.conversations.create(fx.user["id"], {"deviceId": DEVICE, "requestId": str(uuid4())})
    other_body = {"deviceId": DEVICE, "messageId": str(uuid4()), "content": "搜索林俊杰的《江南》"}
    await fx.conversations.message(another["id"], fx.user["id"], other_body, {"dispatchHint": "router"})
    result = await fx.router.route(another["id"], other_body["messageId"], fx.user["id"], {"deviceId": DEVICE, "preference": ""})
    assert result["route"]["status"] == "completed"
    await fx.submit("搜索孙燕姿的《遇见》")
    body, _ = await fx.submit("到目前为止，我在这段对话里问过哪几位歌手？")
    first = await fx.route(body)
    assert await fx.route(body) == first
    route = first["route"]
    assert route["status"] == "completed" and route["paid"] is False
    assert route["result"]["intents"] == ["conversation_history"]
    [reply] = route["result"]["actions"]
    assert reply["capability"] == "assistant.reply" and reply["parameters"]["kind"] == "history"
    content = reply["parameters"]["text"]
    assert "陶喆" in content and "周杰伦" in content
    assert "林俊杰" not in content and "孙燕姿" not in content
    assert fx.calls == [] and await used(fx) == 0
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0


@pytest.mark.parametrize("text,operation,status", [
    ("先暂停当前查歌任务。", "pause", "waiting_tools"),
    ("继续刚才暂停的任务。", "resume", "paused"),
    ("取消这次任务，先不找了。", "cancel", "preview"),
    ("把正在查的那个先停在这儿", "pause", "waiting_tools"),
])
async def test_actual_persistent_control_routes_bind_owned_original_task_and_revision(fx, text, operation, status):
    original, _ = await fx.submit("先生成一份20分钟的跑步歌单，只预览。")
    task = {"id": str(uuid4()), "userId": fx.user["id"], "deviceId": DEVICE, "revision": 8,
            "stateVersion": 1, "status": status, "createdAt": NOW, "updatedAt": NOW,
            "messages": [{"messageId": str(uuid4()), "role": "user", "content": original["content"], "revision": 8}]}
    await fx.runtime.store.save(task)
    await fx.conversations.event(fx.conversation["id"], fx.user["id"], {
        "deviceId": DEVICE, "eventId": str(uuid4()), "turnId": original["messageId"],
        "kind": "task_sync", "taskId": task["id"], "taskRevision": 8, "content": "",
    }, fx.runtime)
    body, _ = await fx.submit(text)
    first = await fx.route(body)
    assert await fx.route(body) == first
    route = first["route"]
    assert route["status"] == "completed" and route["paid"] is False
    assert route["result"]["intents"] == ["task_control"]
    [action] = route["result"]["actions"]
    assert action["capability"] == "task.control" and action["source"] == text
    assert action["parameters"] == {"action": operation, "taskId": task["id"], "expectedRevision": 8}
    assert fx.calls == [] and await used(fx) == 0
    # 路由只保存受控行动计划，不能冒充已经执行了暂停/恢复/取消。
    current = await fx.runtime.store.load(task["id"], fx.user["id"])
    assert current["status"] == status and current["revision"] == 8
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 1


@pytest.mark.parametrize("instruction,source,operation", [
    ("不要先暂停当前查歌任务。", "暂停当前查歌任务", "pause"),
    ("他说把正在查的那个先停在这儿", "把正在查的那个先停在这儿", "pause"),
    ("如果我确认，取消这次任务，先不找了。", "取消这次任务", "cancel"),
    ("取消这次任务，先不找了，然后播放十年", "取消这次任务", "cancel"),
    ("我回来了，不要继续刚才暂停的任务。", "继续刚才暂停的任务", "resume"),
])
def test_formal_guard_rejects_model_source_that_removes_negative_reported_or_unknown_clauses(instruction, source, operation):
    task = {"id": "owned-original-task", "revision": 8, "status": "waiting_tools"}
    proposal = {"intents": ["task_control"], "missingSlots": [], "actions": [
        {"capability": "task.control", "source": source, "dependsOn": [], "parameters": {"action": operation}},
    ]}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": instruction, "history": [], "task": task, "preference": ""}, messageId="guarded-control")
    assert error.value.code == "INVALID_ROUTE_PLAN"
    assert error.value.routeReason == "control_permission_or_parameters"


@pytest.mark.parametrize("text,count,minutes", [
    ("做一份跑步用的歌单，正好5首、总共20分钟，先给我预览。", 5, 20),
    ("制作一份工作歌单，刚好六首，合计18分钟，只预览。", 6, 18),
    ("准备一份通勤歌单，恰好8首和总时长24分钟，先不要播放。", 8, 24),
    ("生成睡前音乐，总计4首且时长16分钟，先给我预览。", 4, 16),
])
def test_nominal_playlist_quantities_remain_complete_explicit_constraints(text, count, minutes):
    parsed = understandRequest(text)
    assert parsed is not None and parsed.target_minutes == minutes and parsed.target_track_count == count
    assert all(item.source in text for item in parsed.requirements)
    inputs = {"instruction": text, "history": [], "task": None, "preference": ""}
    proposal = deterministicRoute(inputs)
    assert proposal is not None
    result = validateRouteProposal(proposal, inputs, messageId="nominal-quantity", planner="deterministic")
    assert result["intents"] == ["scene_research"]
    assert result["slots"]["targetMinutes"] == minutes and result["slots"]["targetTrackCount"] == count
    assert [item["capability"] for item in result["actions"]] == ["music.task"]


@pytest.mark.parametrize("text", [
    "做一份跑步用的歌单，正好5首、总共20.5分钟，先给我预览。",
    "做一份跑步用的歌单，正好5首、总共二十分钟，先给我预览。",
    "做一份跑步用的歌单，正好5首、总共20分钟，上传给朋友。",
    "记住我喜欢爵士，正好5首、总共20分钟。", "正好5首、总共20分钟。",
    "如果做一份跑步用的歌单，正好5首、总共20分钟，先给我预览。",
])
def test_quantitative_clauses_do_not_authorize_unknown_or_missing_business_actions(text):
    assert understandRequest(text) is None


@pytest.mark.parametrize("modifier", ["正好", "刚好", "恰好", "总计"])
def test_exact_count_modifiers_do_not_override_denial_approximation_or_ranges(modifier):
    assert trackCountRequest(modifier + "5首") == 5
    for text in ["不要" + modifier + "5首", "大约" + modifier + "5首", "最多给我" + modifier + "5首",
                 modifier + "5首左右", modifier + "5首到6首", modifier + "5首或6首"]:
        assert trackCountRequest(text) is None
        assert trackCountIssue(text)


@pytest.mark.parametrize("text", [
    "请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。",
    "请记住我喜欢爵士；另外为我一份睡前歌单，只做预览。",
    "记住我喜欢粤语；制作一份工作音乐，时长控制在二十分钟。",
    "记住我喜欢民谣；准备一份读书歌单，只生成预览。",
])
def test_nominal_music_request_is_not_classified_as_preference_management_only(text):
    assert preferenceManagementOnly(text) is False


@pytest.mark.parametrize("text", [
    "记住我喜欢粤语", "把中文优先这条偏好删掉，其他两条保留。",
    "请解释保存口味是否需要我确认", "修改我的音乐偏好",
])
def test_actual_preference_management_stays_scoped_without_music_request(text):
    assert preferenceManagementOnly(text) is True


@pytest.mark.parametrize("text", [
    "请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览后上传聊天。",
    "他说请记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。",
    "如果我说记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览。",
    "不要记住我喜欢粤语，再给我一份16分钟微醺歌单，只做预览后自动播放。",
])
def test_preview_tail_cannot_swallow_unknown_actions_reported_or_conditional_commands(text):
    assert understandRequest(text) is None
