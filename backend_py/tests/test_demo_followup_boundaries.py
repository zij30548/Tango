"""演示版后的独立离线边界；检查授权与需求覆盖，不评价模型或听感。"""

import pytest

from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal
from test_conversation_routing import fx, used


def routed(text, **context):
    """使用产品分流和权限校验，测试不实例化模型、网络或用户数据库。"""
    inputs = {"instruction": text, "history": [], "task": None, "preference": "", **context}
    proposal = deterministicRoute(inputs)
    return validateRouteProposal(proposal, inputs, messageId="followup-boundary", planner="deterministic") if proposal else None


def memory_proposal(instruction, source, *, clear=False):
    """模拟不可信模型截取安全子串，实际授权仍必须依据完整本轮指令。"""
    proposal = {
        "intents": ["preference_deletion" if clear else "preference_proposal"],
        "actions": [{"capability": "memory.propose", "source": source, "dependsOn": [],
                     "parameters": {"operation": "clear" if clear else "set", "preference": "" if clear else "喜欢粤语"}}],
        "missingSlots": [], "slots": {"artist": "", "targetMinutes": 0},
    }
    return validateRouteProposal(proposal, {"instruction": instruction, "history": [], "task": None, "preference": "喜欢爵士"}, messageId="untrusted-memory")


def test_new_compound_variant_preserves_temporary_scope_and_independent_actions():
    result = routed("请记住我不爱听现场版；本次临时想听爵士；帮我整理25分钟通勤歌单；先不要播放")
    assert result["intents"] == ["preference_proposal", "scene_research"]
    memory, music = result["actions"]
    assert memory["parameters"] == {"operation": "set", "preference": "不爱听现场版"}
    assert music["parameters"] == {"mode": "new"} and music["dependsOn"] == []
    assert result["slots"]["targetMinutes"] == 25


def test_new_confirmation_variant_creates_only_explicit_dependency():
    result = routed("请记住我常听古典；等我确认保存偏好后再整理35分钟读书音乐")
    memory, music = result["actions"]
    assert music["dependsOn"] == [memory["id"]]
    assert memory["parameters"]["preference"] == "常听古典"
    assert result["slots"]["targetMinutes"] == 35


def test_old_history_never_authorizes_another_preference_proposal():
    result = routed("帮我整理25分钟通勤歌单", history=[{"role": "user", "content": "记住我喜欢粤语"}], preference="喜欢爵士")
    assert [action["capability"] for action in result["actions"]] == ["music.task"]


def test_temporary_exclusion_is_not_a_long_term_dislike():
    result = routed("本次临时想听民谣；整理18分钟读书歌单；不要推荐人声")
    assert result["intents"] == ["scene_research"]
    assert result["actions"][0]["capability"] == "music.task"
    assert result["slots"]["targetMinutes"] == 18


def test_quoted_compound_cannot_be_partially_executed():
    assert routed("别人说「记住我爱听古典；整理35分钟读书音乐」，我只是转述") is None


def test_unknown_trailing_action_cannot_be_silently_dropped():
    assert routed("记住我常听古典；整理35分钟读书音乐；上传我的通讯录") is None


def test_conflicting_durations_are_clarified_without_arbitrarily_selecting_one():
    # 新版能确定冲突本身，直接追问目标时长，不能进入编排或任取一个数字。
    result = routed("整理20分钟但总时长30分钟的通勤歌单")
    assert result["strategy"] == "clarify" and result["missingSlots"][0]["slot"] == "duration"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


def test_minimum_supported_duration_remains_exact():
    result = routed("整理1分钟睡前歌单")
    assert result["slots"]["targetMinutes"] == 1
    assert result["actions"][0]["parameters"]["mode"] == "new"


def test_maximum_supported_duration_remains_exact():
    result = routed("整理240分钟学习歌单")
    assert result["slots"]["targetMinutes"] == 240
    assert result["strategy"] == "single_agent"


def test_cancelled_task_is_not_revived_by_a_duration_edit():
    result = routed("改为35分钟，其余不变", task={"id": "cancelled-owned-task", "revision": 4, "status": "cancelled"})
    assert result["strategy"] == "clarify"
    assert result["missingSlots"][0]["slot"] == "task"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


def test_confirmation_without_a_proposal_is_not_an_execution_trigger():
    result = routed("等我确认偏好后再整理35分钟读书音乐")
    assert result["strategy"] == "clarify"
    assert result["missingSlots"][0]["slot"] == "reference"


def test_sensitive_data_in_memory_clause_blocks_partial_compilation():
    assert routed("记住我喜欢粤语且密码是123456；整理25分钟通勤歌单") is None


def test_history_only_memory_cannot_be_forged_into_current_authority():
    with pytest.raises(ApiError) as error:
        memory_proposal("整理25分钟通勤歌单", "记住我喜欢粤语")
    assert error.value.code == "INVALID_ROUTE_PLAN"


def test_question_cannot_authorize_memory_by_omitting_its_suffix():
    with pytest.raises(ApiError) as error:
        memory_proposal("记住我喜欢粤语吗？", "记住我喜欢粤语")
    assert error.value.routeReason == "memory_source_permission"


def test_question_cannot_authorize_clear_by_omitting_its_suffix():
    with pytest.raises(ApiError) as error:
        memory_proposal("清空我的口味？", "清空我的口味", clear=True)
    assert error.value.routeReason == "memory_source_permission"


def test_example_tail_cannot_be_removed_from_the_same_clause():
    with pytest.raises(ApiError) as error:
        memory_proposal("记住我喜欢粤语只是示范用语，请不要照做", "记住我喜欢粤语")
    assert error.value.routeReason == "memory_source_permission"


def test_conditional_tail_cannot_be_omitted_to_authorize_memory():
    with pytest.raises(ApiError) as error:
        memory_proposal("记住我喜欢粤语，除非我说不要保存", "记住我喜欢粤语")
    assert error.value.routeReason == "memory_source_permission"


def test_unrecognized_memory_timing_cannot_be_ignored():
    assert routed("记住我常听古典；整理35分钟读书音乐；下个月再保存") is None


def test_new_nonconflicting_preference_keeps_confirmed_old_preference():
    result = routed("请记住我爱听蓝调", preference="喜欢粤语；不喜欢现场版")
    assert result["actions"][0]["parameters"] == {"operation": "set", "preference": "喜欢粤语；不喜欢现场版；爱听蓝调"}


@pytest.mark.parametrize("instruction,source", [
    ("记住我喜欢粤语并不是我的长期偏好", "记住我喜欢粤语"),
    ("记住我喜欢粤语只是临时偏好", "记住我喜欢粤语只是临时偏好"),
    ("记住我喜欢粤语如果我明天确认", "记住我喜欢粤语如果我明天确认"),
    ("记住我喜欢粤语，如果我明天确认", "记住我喜欢粤语"),
    ("记住我喜欢粤语只是示范用语，请不要照做", "记住我喜欢粤语只是示范用语，请不要照做"),
])
def test_scoped_or_metalinguistic_memory_does_not_gain_authority(instruction, source):
    with pytest.raises(ApiError) as error:
        memory_proposal(instruction, source)
    assert error.value.routeReason == "memory_source_permission"


@pytest.mark.parametrize("clear", [False, True])
async def test_rejected_question_is_persistent_and_never_automatically_retried(fx, clear):
    source = "清空我的口味" if clear else "记住我喜欢粤语"
    async def untrusted(input, options=None):
        fx.calls.append(input)
        return {"value": {"intents": ["preference_deletion" if clear else "preference_proposal"],
            "actions": [{"capability": "memory.propose", "source": source, "dependsOn": [],
                "parameters": {"operation": "clear" if clear else "set", "preference": "" if clear else "喜欢粤语"}}],
            "missingSlots": [], "slots": {"artist": "", "targetMinutes": 0}}}
    fx.router.model.route = untrusted
    body, _ = await fx.submit(source + "吗？")
    first = await fx.route(body)
    second = await fx.route(body)
    observed = await fx.router.get(fx.conversation["id"], body["messageId"], fx.user["id"])
    assert first["route"] == second["route"] == observed["route"]
    assert first["route"]["status"] == "failed" and first["route"]["errorCode"] == "INVALID_ROUTE_PLAN"
    assert len(fx.calls) == 1 and await used(fx) == 1
    assert (await fx.store.db.fetchone("SELECT count(*) AS n FROM music_tasks"))["n"] == 0
