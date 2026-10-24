"""受限需求编译的开发回归，隔离模型、真实曲库和用户数据库。"""

import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal
from tango_backend.request_understanding import understandRequest
from test_conversation_routing import fx, used


def route(text, **context):
    """走实际入口及同一个权限校验器，不直接验一个孤立正则。"""
    inputs = {"instruction": text, "history": [], "task": None, "preference": "", **context}
    proposal = deterministicRoute(inputs)
    return validateRouteProposal(proposal, inputs, messageId="bounded-understanding", planner="deterministic") if proposal else None


@pytest.mark.parametrize("text,preference,minutes", [
    ("记住我喜欢粤语，再推荐20分钟微醺歌单。", "喜欢粤语", 20),
    ("请记住我偏好爵士，并且给我推荐30分钟通勤歌曲", "偏好爵士", 30),
    ("长期保存我爱听摇滚，然后帮我生成40分钟跑步歌单", "爱听摇滚", 40),
    ("永久保存我不喜欢现场版；推荐15分钟睡前音乐", "不喜欢现场版", 15),
    ("推荐20分钟跑步的歌，并且记住我喜欢摇滚", "喜欢摇滚", 20),
    ("记住我喜欢粤语 R&B 音乐，再推荐20分钟微醺歌单", "喜欢粤语 R&B 音乐", 20),
    ("记住我喜欢粤语再推荐20分钟跑步歌单", "喜欢粤语", 20),
])
def test_compound_request_compiles_separate_independent_actions(text, preference, minutes):
    result = route(text)
    assert result["intents"] == ["preference_proposal", "scene_research"]
    memory, music = result["actions"]
    assert memory["capability"] == "memory.propose"
    assert memory["parameters"] == {"operation": "set", "preference": preference}
    assert music["capability"] == "music.task" and music["parameters"] == {"mode": "new"}
    assert music["dependsOn"] == []
    assert result["slots"]["targetMinutes"] == minutes
    assert memory["source"] in text and music["source"] in text


@pytest.mark.parametrize("connector", ["确认后再", "确认保存后再", "等我确认后再", "确认保存口味后再", "确认偏好后再"])
def test_only_explicit_confirmation_adds_memory_dependency(connector):
    result = route("记住我喜欢粤语；" + connector + "推荐20分钟跑步歌单")
    memory, music = result["actions"]
    assert music["dependsOn"] == [memory["id"]]
    assert result["slots"]["targetMinutes"] == 20


@pytest.mark.parametrize("text", [
    "不要记住我喜欢粤语，只给我推荐20分钟微醺歌单",
    "只用于今晚，推荐20分钟粤语微醺歌单",
    "本次临时想听爵士，推荐20分钟睡前音乐",
    "推荐20分钟跑步歌单，先不要播放",
    "推荐20分钟跑步歌单，别放现场版",
])
def test_temporary_constraints_and_negative_memory_never_propose_memory(text):
    result = route(text)
    assert result["intents"] == ["scene_research"]
    assert [action["capability"] for action in result["actions"]] == ["music.task"]
    assert result["actions"][0]["dependsOn"] == []


def test_all_clauses_keep_source_and_scope_for_mixed_scene_request():
    text = "记住我喜欢粤语，今晚跑步听20分钟，别放现场版，先不要播放"
    parsed = understandRequest(text)
    assert [(r.kind, r.scope) for r in parsed.requirements] == [
        ("preference", "long_term"), ("music", "task"), ("constraint", "task"), ("constraint", "task")]
    result = route(text)
    assert result["slots"]["targetMinutes"] == 20 and len(result["actions"]) == 2
    assert all(r.source in text for r in parsed.requirements)


@pytest.mark.parametrize("text", [
    "他说记住我喜欢粤语，再推荐20分钟微醺歌单",
    "他说‘记住我喜欢粤语’，再推荐20分钟微醺歌单",
    "假如记住我喜欢粤语，再推荐20分钟微醺歌单",
    "能不能记住我喜欢粤语，再推荐20分钟微醺歌单？",
    "不是让你记住我喜欢粤语，再推荐20分钟微醺歌单",
    "记住我喜欢粤语，再搜索陈奕迅的十年",
    "记住我喜欢粤语，再推荐20分钟歌单，然后删除所有歌曲",
    "记住我喜欢粤语，再推荐20分钟歌单，发送给我的朋友",
    "记住我喜欢粤语，再推荐20分钟歌单，改成30分钟",
    "记住我今晚喜欢粤语，再推荐20分钟歌单",
    "记住我的密码，再推荐20分钟歌单",
    "记住我喜欢粤语或爵士，再推荐20分钟歌单",
    "记住我喜欢粤语，先推荐20分钟歌单之后再保存",
])
def test_unsafe_unsupported_or_incomplete_coverage_keeps_existing_model_boundary(text):
    assert route(text) is None


@pytest.mark.parametrize("minutes", [0, 241, 999])
def test_blocking_duration_asks_before_any_memory_or_music_action(minutes):
    result = route(f"记住我喜欢粤语，再推荐{minutes}分钟微醺歌单")
    assert result["strategy"] == "clarify"
    assert result["intents"] == ["preference_proposal", "scene_research"]
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]
    assert result["missingSlots"][0]["slot"] == "duration"


@pytest.mark.parametrize("text", ["推荐二十分钟跑步歌单", "推荐1.5小时跑步歌单", "推荐20.5分钟跑步歌单",
    "推荐-1分钟跑步歌单", "推荐+20分钟跑步歌单", "推荐10到20分钟跑步歌单", "推荐10～20分钟跑步歌单"])
def test_non_integer_duration_is_not_guessed(text):
    assert route(text) is None


def test_revision_binds_current_task_and_does_not_resurrect_old_memory_request():
    task = {"id": "owned-current-task", "revision": 7, "status": "preview"}
    result = route("时长改成30分钟，其他不变", task=task,
        history=[{"role": "user", "content": "记住我喜欢粤语，再推荐20分钟微醺歌单"}])
    assert result["intents"] == ["scene_research"]
    assert result["actions"][0]["parameters"] == {"mode": "continue", "taskId": task["id"], "expectedRevision": 7}
    assert result["slots"]["targetMinutes"] == 30
    assert route("时长改成30分钟，其他不变")["missingSlots"][0]["slot"] == "task"
    assert route("时长改成30分钟，其他不变", task={**task, "status": "cancelled"})["strategy"] == "clarify"


def test_standalone_memory_preserves_nonconflicting_previous_summary():
    result = route("记住我喜欢粤语", preference="偏好爵士；不喜欢现场版")
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": "偏好爵士；不喜欢现场版；喜欢粤语"}
    assert route("记住我喜欢粤语", preference="不喜欢粤语") is None
    assert route("记住我喜欢粤语", preference="其他偏好" * 125) is None
    assert route("记住我喜欢粤语", preference="喜欢粤语")["actions"][0]["parameters"]["preference"] == "喜欢粤语"


@pytest.mark.parametrize("text", ["清空我的口味", "忘记所有音乐偏好", "清除全部偏好", "删除我的音乐喜好"])
def test_explicit_clear_is_only_a_confirmation_proposal(text):
    result = route(text, preference="喜欢粤语")
    assert result["intents"] == ["preference_deletion"]
    assert result["actions"][0]["parameters"] == {"operation": "clear", "preference": ""}
    assert result["actions"][0]["capability"] == "memory.propose"


@pytest.mark.parametrize("text", ["不要清空我的口味", "能不能清空我的口味", "他说清空我的口味", "忘记这条口味"])
def test_partial_ambiguous_quoted_and_negative_clear_never_become_clear(text):
    assert route(text) is None


def test_missing_single_preference_target_asks_without_clearing_anything():
    result = route("删除我喜欢粤语这条偏好")
    assert result["strategy"] == "clarify"
    assert result["missingSlots"][0]["slot"] == "reference"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


def test_original_validator_still_rejects_source_truncation_and_missing_authority():
    text = "不要记住我喜欢粤语，只给我推荐20分钟微醺歌单"
    proposal = deterministicRoute({"instruction": "记住我喜欢粤语，再推荐20分钟微醺歌单", "preference": ""})
    proposal = clone(proposal)
    # 即使另一处曾生成合法计划，也不能移植到带否定的本轮命令。
    proposal["actions"][1]["source"] = "推荐20分钟微醺歌单"
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text, "preference": ""}, messageId="unauthorized")
    assert error.value.routeReason == "memory_source_permission"


def test_confirmation_dependency_without_subject_asks_instead_of_running():
    result = route("确认保存后再推荐20分钟跑步歌单")
    assert result["strategy"] == "clarify" and result["missingSlots"][0]["slot"] == "reference"


def test_repeated_known_terms_are_not_confused_with_positive_preference():
    assert route("记住我喜欢粤语", preference="不喜欢粤语") is None
    assert route("记住我不喜欢现场版", preference="喜欢现场版") is None
    assert route("记住我喜欢粤语", preference="不喜欢现场版；喜欢粤语")["actions"][0]["parameters"]["preference"] == "不喜欢现场版；喜欢粤语"


async def test_compound_route_is_persisted_idempotently_without_a_model_call(fx):
    text = "记住我喜欢粤语，再推荐20分钟微醺歌单"
    body, _ = await fx.submit(text)
    first = await fx.route(body)
    second = await fx.route(body)
    assert first["route"] == second["route"]
    assert first["route"]["status"] == "completed" and first["route"]["paid"] is False
    assert first["route"]["result"]["intents"] == ["preference_proposal", "scene_research"]
    assert fx.calls == [] and await used(fx) == 0
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0

