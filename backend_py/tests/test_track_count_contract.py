"""目标曲数、锁定前缀与时长冲突的隔离回归，不调用外部模型或曲库。"""

import itertools
import json

import httpx
import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.model_common import PROMPTS
from tango_backend.task_brief_patch import mergeBriefPatch
from tango_backend.task_duration import selectDurationSubset
from tango_backend.task_model import planningInput, rankingInput, createTaskModel
from tango_backend.task_schema import assemblePreview, validateBrief
from tango_backend.task_pending_request import preparePendingContinuation, validatePendingBrief
from tango_backend.task_track_count import parseTrackCount, trackCountRequest, trackCountIssue
from test_task_runtime_migration import f, body, until, finish, raw_brief


def brief(**updates):
    return raw_brief(**{"targetMinutes": 0, "excludeLive": False, **updates})


def task_for(durations, *, count=6, minutes=0, locked=()):
    tracks = [{"id": index + 1, "title": f"录音室曲目{index + 1}", "artist": "测试艺人", "album": "固定专辑", "durationMs": duration, "sourceId": 90, "sourceKind": "playlist", "sourceName": "阅读", "sourceTags": []} for index, duration in enumerate(durations)]
    result = {"brief": brief(targetTrackCount=count), "branches": {"scene": {"skill": "scene.research", "result": {"tracks": tracks, "sources": []}}}, "lockedIds": list(locked), "excludedIds": []}
    result["brief"]["targetMinutes"] = minutes
    if locked:
        result["preview"] = {"version": 1, "tracks": [t for t in tracks if t["id"] in locked]}
    return result


@pytest.mark.parametrize("instruction,expected", [
    ("给我六首适合跑步的歌", 6), ("推荐二十三首爵士", 23), ("需要３０首轻音乐", 30),
    ("六首适合跑步的歌", 6), ("数量改成八", 8), ("推荐六首，改成八首", 8), ("推荐六首，数量改成八", 8),
    ("总共八首，保留前三首", 8), ("把六首改成八首", 8), ("推荐零首", 0),
    ("给我三十一首", 31), ("来一百首", 100), ("保留前六首", None),
    ("不要给我六首", None), ("他说“推荐六首”", None), ("例如推荐六首", None),
    ("如果要六首会怎样", None), ("推荐六首或八首", None),
])
def test_explicit_count_signal_preserves_bounds_and_authority(instruction, expected):
    assert trackCountRequest(instruction) == expected


@pytest.mark.parametrize("instruction", [
    "推荐最多六首歌", "推荐至少六首歌", "推荐不超过六首歌", "给我大约6首歌", "给我6首左右的歌",
    "推荐6到8首歌", "推荐六首到八首歌", "推荐6-8首歌", "推荐6首以内的歌", "推荐6首以上的歌",
    "推荐-6首歌", "推荐3.5首歌", "推荐六首或八首歌", "推荐6首，另一个要求是8首",
    "最多给我六首", "大约推荐六首", "歌曲数量最多6", "歌曲数量6到8", "推荐十来首歌", "推荐五六首歌", "只要不是6首就行",
])
def test_nonexact_count_cannot_be_narrowed_to_exact(instruction):
    assert trackCountRequest(instruction) is None
    assert trackCountIssue(instruction)


@pytest.mark.parametrize("instruction", ["推荐最多六首歌", "推荐6到8首歌", "改成六首左右", "数量最多6", "推荐6首或8首"])
def test_nonexact_task_proposal_cannot_keep_or_introduce_an_exact_count(instruction):
    current = brief(targetTrackCount=6)
    for source, context in [(brief(targetTrackCount=6), {"instruction": instruction}), (clone(current), {"instruction": instruction, "currentBrief": current})]:
        with pytest.raises(ApiError) as error:
            validateBrief(source, context)
        assert error.value.code == "INVALID_TASK_MODEL" and "澄清" in error.value.message
    with pytest.raises(ApiError):
        mergeBriefPatch({"kind": "edit", "changes": [], "question": "", "adjustment": ""}, {"instruction": instruction, "currentBrief": current})
    assert current["targetTrackCount"] == 6
    clarified = validateBrief({"kind": "clarify", "question": "目前只支持精确首数，请确认确切多少首。"}, {"instruction": instruction, "currentBrief": current})
    assert clarified["kind"] == "clarify" and clarified["targetTrackCount"] == 6


@pytest.mark.parametrize("instruction", ["不要改成六首", "不要给我六首", "他说“推荐六首”", "如果需要六首就推荐", "只要不是6首就行"])
def test_indirect_count_cannot_authorize_new_model_count(instruction):
    with pytest.raises(ApiError):
        validateBrief(brief(targetTrackCount=6), {"instruction": instruction})


def test_explicit_count_correction_can_replace_a_previous_nonexact_request():
    assert trackCountRequest("最多推荐六首，改成八首") == 8
    assert trackCountIssue("最多推荐六首，改成八首") is None
    assert trackCountIssue("改成六首或八首") == "conflicting_track_counts"
    assert trackCountRequest("不要六首，给我八首") == 8


def pending_count_context(first, answer, question):
    messages = [{"messageId": "count-message-01", "role": "user", "content": first, "revision": 1}, {"messageId": "count-message-02", "role": "user", "content": answer, "revision": 2}]
    return {"taskId": "count-task-01", "revision": 2, "instruction": answer, "currentBrief": {"kind": "clarify", "question": question}, "preview": [], "lockedIds": [], "hasConfirmedBrief": False, "messages": messages, "pendingRequest": {"schemaVersion": 1, "id": "count-pending-01", "taskId": "count-task-01", "rootRevision": 1, "revision": 2, "status": "awaiting_answer", "question": question, "sources": clone(messages)}}


def test_pending_artist_answer_keeps_authorized_original_track_count():
    prepared = preparePendingContinuation(pending_count_context("请找六首新歌", "邓紫棋", "哪位歌手？"))
    result = validatePendingBrief(brief(targetTrackCount=6, artist="邓紫棋", requireReleases=True), prepared)
    assert result["targetTrackCount"] == 6 and result["artist"] == "邓紫棋"
    with pytest.raises(ApiError):
        validatePendingBrief(brief(targetTrackCount=8, artist="邓紫棋", requireReleases=True), prepared)


@pytest.mark.parametrize("answer,expected", [("八首", 8), ("八", 8), ("8", 8)])
def test_pending_nonexact_count_can_only_continue_after_a_precise_answer(answer, expected):
    prepared = preparePendingContinuation(pending_count_context("请推荐最多六首读书的歌", answer, "请确认确切多少首。"))
    result = validatePendingBrief(brief(targetTrackCount=expected), prepared)
    assert result["targetTrackCount"] == expected


@pytest.mark.parametrize("answer", ["可以", "还是六首左右", "零", "31"])
def test_pending_nonexact_or_out_of_range_answer_cannot_resume_exact_old_count(answer):
    prepared = preparePendingContinuation(pending_count_context("请推荐最多六首读书的歌", answer, "请确认确切多少首。"))
    with pytest.raises(ApiError):
        validatePendingBrief(brief(targetTrackCount=6), prepared)


def test_pending_count_source_still_requires_original_message_binding():
    context = pending_count_context("请找六首新歌", "邓紫棋", "哪位歌手？")
    context["pendingRequest"]["sources"][0]["content"] = "请找八首新歌"
    with pytest.raises(ApiError) as error:
        preparePendingContinuation(context)
    assert error.value.code == "INVALID_TASK_PENDING_CONTEXT"


@pytest.mark.parametrize("number,expected", [("两", 2), ("十二", 12), ("三十", 30), ("二百三十一", 231), ("０", 0), ("一二", None)])
def test_chinese_numeric_parser(number, expected):
    assert parseTrackCount(number) == expected


@pytest.mark.parametrize("value", [True, "6", 6.5, -1, 31, None, []])
def test_invalid_model_count_is_rejected(value):
    with pytest.raises(ApiError) as error:
        validateBrief(brief(targetTrackCount=value))
    assert error.value.code == "INVALID_TASK_MODEL"


def test_requested_count_cannot_be_dropped_or_misfiled_as_retention():
    context = {"instruction": "推荐六首适合读书的音乐", "preview": []}
    for source in (brief(retainFirst=6), brief(targetTrackCount=0), brief()):
        with pytest.raises(ApiError) as error:
            validateBrief(source, context)
        assert "targetTrackCount" in error.value.message
    assert validateBrief(brief(targetTrackCount=6), context)["targetTrackCount"] == 6


@pytest.mark.parametrize("instruction,count", [("推荐零首", 0), ("来三十一首", 31)])
def test_unsupported_count_requires_clarification_instead_of_clipping(instruction, count):
    with pytest.raises(ApiError):
        validateBrief(brief(targetTrackCount=min(count, 30)), {"instruction": instruction})
    assert validateBrief({"kind": "clarify", "question": "支持1至30首，请确认目标数量。"}, {"instruction": instruction})["kind"] == "clarify"


def test_legacy_brief_does_not_gain_a_new_constraint():
    original = brief()
    normalized = validateBrief(original)
    assert "targetTrackCount" not in normalized and original == brief()
    source = task_for([240000] * 18, count=0)
    source["brief"].pop("targetTrackCount")
    preview = assemblePreview(source)
    assert len(preview["tracks"]) == 12
    assert not any(c["key"] == "track_count" for c in preview["checks"])


def test_target_count_is_executed_without_locking_a_new_playlist():
    source = task_for([240000] * 18)
    preview = assemblePreview(source)
    assert len(preview["tracks"]) == 6
    assert source["lockedIds"] == [] and preview["lockedIds"] == []
    assert next(c for c in preview["checks"] if c["key"] == "track_count")["status"] == "satisfied"


def test_count_and_duration_are_both_checked_when_incompatible():
    preview = assemblePreview(task_for([240000] * 12, count=6, minutes=40))
    assert len(preview["tracks"]) == 6 and preview["totalDurationMs"] == 1440000
    checks = {c["key"]: c["status"] for c in preview["checks"]}
    assert checks["track_count"] == "satisfied" and checks["duration"] == "unmet"


def test_count_duration_selection_agrees_with_exhaustive_small_fixture():
    durations = [73000, 122000, 189000, 247000, 314000, 382000, 453000]
    selected = selectDurationSubset([{"durationMs": d} for d in durations], exactCount=3, targetMs=780000)
    error = abs(sum(durations[i] for i in selected["indices"]) - 780000)
    optimum = min(abs(sum(c) - 780000) for c in itertools.combinations(durations, 3))
    assert len(selected["indices"]) == 3 and error == optimum


def test_extreme_durations_keep_exact_count_and_bounded_state_metadata():
    candidates = [{"durationMs": 7200000 - index * 11003} for index in range(60)]
    result = selectDurationSubset(candidates, exactCount=30, targetMs=60000)
    assert len(result["indices"]) == 30 and len(set(result["indices"])) == 30
    assert result["maximumStatesPerCountAndEvidence"] == 256
    assert result["withinTolerance"] is False


def test_count_selection_keeps_required_source_evidence_within_the_count():
    candidates = [{"durationMs": 180000, "flags": 0}, {"durationMs": 180000, "flags": 1}, {"durationMs": 180000, "flags": 2}]
    result = selectDurationSubset(candidates, exactCount=2, targetMs=360000, requiredFlags=3)
    assert result["indices"] == [1, 2]


def test_candidate_shortage_is_explicit_and_never_duplicates_tracks():
    preview = assemblePreview(task_for([240000] * 3, count=6))
    assert len(preview["tracks"]) == len({t["id"] for t in preview["tracks"]}) == 3
    assert next(c for c in preview["checks"] if c["key"] == "track_count")["status"] == "unmet"


def test_existing_locks_keep_order_and_take_slots_from_total_count():
    preview = assemblePreview(task_for([240000] * 18, locked=(1, 2)))
    assert len(preview["tracks"]) == 6 and [t["id"] for t in preview["tracks"][:2]] == [1, 2]


def test_unknown_durations_do_not_prevent_count_or_invent_time_verification():
    preview = assemblePreview(task_for([0] * 12, count=6, minutes=20))
    checks = {c["key"]: c["status"] for c in preview["checks"]}
    assert len(preview["tracks"]) == 6 and checks["duration"] == "unverified" and checks["track_count"] == "satisfied"


def patch(field, op="set", value=8, evidence="改成八首"):
    change = {"field": field, "op": op, "evidence": evidence}
    if op != "clear":
        change["value"] = value
    return {"kind": "edit", "changes": [change], "question": "", "adjustment": ""}


def test_count_patch_preserves_duration_styles_and_locked_prefix():
    current = brief(targetTrackCount=6, retainFirst=2)
    current["targetMinutes"] = 20
    result = mergeBriefPatch(patch("targetTrackCount"), {"instruction": "改成八首", "currentBrief": current})
    assert result["targetTrackCount"] == 8 and result["retainFirst"] == 2
    assert result["targetMinutes"] == 20 and result["styles"] == current["styles"]


def test_count_limit_can_only_be_cleared_explicitly():
    current = brief(targetTrackCount=6)
    cleared = mergeBriefPatch(patch("targetTrackCount", "clear", evidence="取消曲数限制"), {"instruction": "取消曲数限制", "currentBrief": current})
    assert cleared["targetTrackCount"] == 0 and cleared["styles"] == current["styles"]
    for instruction in ("不要取消曲数限制", "保留前八首", "他说“改成八首”", "更轻快一点"):
        with pytest.raises(ApiError):
            mergeBriefPatch(patch("targetTrackCount", "clear", evidence=instruction), {"instruction": instruction, "currentBrief": current})


@pytest.mark.parametrize("instruction,value", [("保留前八首", 8), ("不要给我八首", 8), ("他说“改成八首”", 8), ("改成八首", 6)])
def test_count_patch_rejects_untrusted_or_mismatched_evidence(instruction, value):
    with pytest.raises(ApiError):
        mergeBriefPatch(patch("targetTrackCount", value=value, evidence=instruction), {"instruction": instruction, "currentBrief": brief(targetTrackCount=6)})


def test_model_projections_and_prompts_keep_count_distinct_from_retention():
    source = {"brief": brief(targetTrackCount=6, retainFirst=2), "candidates": [{"id": 100, "durationMs": 240000}], "currentBrief": brief(targetTrackCount=6), "preview": []}
    assert rankingInput(source)["brief"]["targetTrackCount"] == 6
    assert planningInput(source)["currentBrief"]["targetTrackCount"] == 6
    for key in ("PLAN", "FOLLOWUP_PLAN", "CONTINUE_PLAN", "COMPOSE"):
        assert "targetTrackCount" in PROMPTS["task-model"][key] and "retainFirst" in PROMPTS["task-model"][key]


@pytest.mark.parametrize("value,valid", [(brief(targetTrackCount=6), True), (brief(retainFirst=6), False), (brief(targetTrackCount="六"), False)])
async def test_real_model_adapter_enforces_count_after_fake_http_response(value, valid):
    sent = []
    def response(request):
        sent.append(json.loads(request.content))
        return httpx.Response(200, json={"choices": [{"finish_reason": "stop", "message": {"content": json.dumps(value, ensure_ascii=False)}}]})
    async with httpx.AsyncClient(transport=httpx.MockTransport(response)) as client:
        model = createTaskModel({"apiKey": "isolated-count-fixture", "baseUrl": "http://127.0.0.1:9/v1", "model": "fixed-count-fixture", "client": client})
        if valid:
            result = await model.plan({"instruction": "推荐六首适合阅读的歌曲", "currentBrief": None, "preview": []})
            assert result["value"]["targetTrackCount"] == 6
        else:
            with pytest.raises(ApiError) as error:
                await model.plan({"instruction": "推荐六首适合阅读的歌曲", "currentBrief": None, "preview": []})
            assert error.value.code == "INVALID_TASK_MODEL"
        assert len(sent) == 1 and "targetTrackCount" in sent[0]["messages"][0]["content"]


async def test_runtime_executes_fixed_count_and_persists_it(f):
    f.model.initial = brief(targetTrackCount=6)
    runtime = await f.runtime()
    created = await runtime.create(f.user, body(instruction="推荐六首适合读书的音乐"))
    preview, _ = await finish(runtime, created, f.user, f.time[0])
    assert len(preview["preview"]["tracks"]) == 6 and preview["brief"]["targetTrackCount"] == 6
    assert "目标 6 首" in preview["briefSummary"] and preview["lockedIds"] == []
    saved = await runtime.store.load(created["id"], f.user)
    assert saved["brief"]["targetTrackCount"] == 6
    await runtime.close()
    restarted = await f.runtime()
    reopened = await restarted.get(created["id"], f.user)
    assert reopened["brief"]["targetTrackCount"] == 6 and len(reopened["preview"]["tracks"]) == 6


async def test_runtime_lock_count_conflict_is_visible_persistent_and_idempotent(f):
    f.model.initial = brief(targetTrackCount=6)
    runtime = await f.runtime()
    created = await runtime.create(f.user, body(instruction="推荐六首读书的歌"))
    preview, _ = await finish(runtime, created, f.user, f.time[0])
    locks = [t["id"] for t in preview["preview"]["tracks"][:4]]
    locked = await runtime.action(created["id"], f.user, body(action="lock", expectedRevision=preview["revision"], lockedIds=locks))
    f.model.followup = lambda _: {"targetTrackCount": 3}
    changed = await runtime.action(created["id"], f.user, body(action="turn", expectedRevision=locked["revision"], instruction="改成三首"))
    result = await until(runtime, changed["id"], f.user, "needs_clarification", "failed")
    assert result["status"] == "needs_clarification" and result["lockedIds"] == locks
    matching = [e for e in result["messages"] if e["role"] == "assistant" and e["revision"] == result["revision"] and e["content"] == result["question"]]
    assert len(matching) == 1 and matching[0]["messageId"]
    saved = await runtime.store.load(created["id"], f.user)
    await runtime.checkpoint(saved, "clarify_readback_fixture")
    again = await runtime.get(created["id"], f.user)
    assert [e["messageId"] for e in again["messages"] if e["content"] == result["question"]] == [matching[0]["messageId"]]


async def test_runtime_incompatible_count_and_duration_reports_both(f):
    f.model.initial = brief(targetTrackCount=6)
    f.model.initial["targetMinutes"] = 40
    runtime = await f.runtime()
    created = await runtime.create(f.user, body(instruction="推荐六首读书的歌，约40分钟"))
    result, _ = await finish(runtime, created, f.user, f.time[0])
    assert len(result["preview"]["tracks"]) == 6
    assert "尚未满足" in result["messages"][-1]["content"] and "40" in result["messages"][-1]["content"]
