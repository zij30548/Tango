"""统一音乐需求的开发验收；正式路由和LangGraph，隔离模型、曲库与数据库。"""

from itertools import permutations
import pytest
from tango_backend.compat import clone, iso
from tango_backend.errors import ApiError
from tango_backend.intent_routing import deterministicRoute, validateRouteProposal
from tango_backend.music_request import artistCatalogRequest, artistCatalogIssue, playlistSearchRequest, musicRequirement, compileMusicBrief
from tango_backend.music_skills import musicSkills
from tango_backend.task_schema import validateBrief, toolResult, artistCreditMatches, assemblePreview, playlistSceneEvidence
from tango_backend.task_brief_patch import mergeBriefPatch
from test_task_runtime_migration import f, body, until
from test_conversation_routing import fx

ORIGINAL = "我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。"


def routed(text, **context):
    """验真实分流与正式权限校验，不只验证某一个正则。"""
    inputs = {"instruction": text, "history": [], "preference": "", "task": None, **context}
    proposal = deterministicRoute(inputs)
    return validateRouteProposal(proposal, inputs, messageId="music-requirement-fixture", planner="deterministic") if proposal else None


@pytest.mark.parametrize("text", ["帮我查找陈奕迅的十年", "搜索陈奕迅的十年", "帮我搜索陈奕迅的《十年》"])
def test_single_song_lookup_preserves_read_only_permission(text):
    result = routed(text)
    assert result["intents"] == ["song_lookup"]
    assert result["actions"][0]["parameters"] == {"artist": "陈奕迅", "title": "十年", "play": False}


@pytest.mark.parametrize("text", ["帮我找一些邓紫棋的歌曲", "帮我查找邓紫棋的歌", "邓紫棋有哪些歌", "请给我推荐邓紫棋的歌曲", "查询G.E.M.邓紫棋的作品"])
def test_artist_catalog_is_a_direct_read_only_capability(text):
    result = routed(text)
    assert result["intents"] == ["artist_catalog"]
    assert result["actions"][0]["capability"] == "music.artist_catalog"
    assert result["actions"][0]["parameters"]["artist"] == ("G.E.M.邓紫棋" if "G.E.M." in text else "邓紫棋")
    assert result["actions"][0]["parameters"]["limit"] == 12
    assert "play" not in result["actions"][0]["parameters"]


@pytest.mark.parametrize("text,artist,limit", [
    ("帮我找几首邓紫棋的歌曲", "邓紫棋", 12), ("列8首周杰伦的歌", "周杰伦", 8),
    ("列出周杰伦的8首歌曲", "周杰伦", 8), ("周杰伦列8首", "周杰伦", 8),
    ("周杰伦的作品8首", "周杰伦", 8), ("周杰伦作品八首", "周杰伦", 8),
    ("给我来8首周杰伦的歌", "周杰伦", 8), ("歌手林俊杰列三十首", "林俊杰", 30),
    ("陈奕迅的歌曲一首", "陈奕迅", 1), ("G.E.M.邓紫棋只列12首", "G.E.M.邓紫棋", 12),
])
def test_artist_catalog_quantity_is_result_limit_instead_of_song_title(text, artist, limit):
    assert artistCatalogRequest(text) == {"artist": artist, "limit": limit}
    result = routed(text)
    assert result["intents"] == ["artist_catalog"] and len(result["actions"]) == 1
    assert result["actions"][0]["parameters"] == {"artist": artist, "limit": limit}
    assert result["actions"][0]["capability"] == "music.artist_catalog"


@pytest.mark.parametrize("text", ["给我来点周杰伦的作品，先列8首看看", "先列8首看看，给我来点周杰伦的作品", "帮我找一些周杰伦的歌曲，显示八首就好"])
def test_artist_catalog_merges_same_goal_and_display_count_clauses(text):
    assert artistCatalogRequest(text) == {"artist": "周杰伦", "limit": 8}
    result = routed(text)
    assert result["intents"] == ["artist_catalog"] and len(result["actions"]) == 1
    assert result["actions"][0]["source"] == text and result["actions"][0]["parameters"]["limit"] == 8


@pytest.mark.parametrize("text", ["给我来点周杰伦的作品，先列31首看看", "给我来点周杰伦的作品，先列8到10首看看", "列5首周杰伦的歌，先列8首看看"])
def test_compound_catalog_quantity_cannot_relax_exact_range_or_conflict(text):
    assert artistCatalogRequest(text) is None and artistCatalogIssue(text) == "catalog_count"
    assert routed(text)["strategy"] == "clarify"


def test_catalog_clauses_cannot_absorb_another_artist_or_lookup_title():
    assert artistCatalogIssue("给我来点周杰伦的作品，陈奕迅列8首") == "artist_scope"
    assert artistCatalogRequest("给我来点周杰伦的作品，然后删除所有歌曲") is None
    assert artistCatalogRequest("搜索一首陈奕迅演唱的十年") is None
    assert routed("搜索一首陈奕迅演唱的十年")["actions"][0]["parameters"] == {"artist": "陈奕迅", "title": "十年", "play": False}


@pytest.mark.parametrize("quantity", ["0首", "31首", "大约8首", "8首左右", "最多8首", "至少8首", "8到10首", "8-10首", "-8首", "8.5首", "若干首", "几首左右", "十几首", "几十首", "三十多首", "好多首"])
def test_artist_catalog_unsupported_quantity_requires_clarification(quantity):
    text = "周杰伦列" + quantity
    assert artistCatalogRequest(text) is None and artistCatalogIssue(text) == "catalog_count"
    result = routed(text)
    assert result["strategy"] == "clarify" and result["missingSlots"][0]["intent"] == "artist_catalog"
    assert [action["capability"] for action in result["actions"]] == ["assistant.reply"]


@pytest.mark.parametrize("artists", ["邓紫棋和周杰伦", "邓紫棋与周杰伦", "邓紫棋、周杰伦", "邓紫棋/周杰伦", "邓紫棋&周杰伦"])
def test_parallel_artists_are_not_concatenated_into_a_single_identity(artists):
    text = "列8首" + artists + "的歌曲"
    assert artistCatalogRequest(text) is None and artistCatalogIssue(text) == "artist_scope"
    result = routed(text)
    assert result["strategy"] == "clarify" and "一位歌手" in result["missingSlots"][0]["question"]


@pytest.mark.parametrize("text", ["不要列8首周杰伦的歌", "他说周杰伦列8首", "如果能找到就列8首周杰伦的歌", "周杰伦列8首然后删除所有歌曲"])
def test_catalog_quantity_does_not_weaken_complete_current_turn_permission(text):
    assert artistCatalogRequest(text) is None
    proposal = {"intents": ["artist_catalog"], "slots": {"artist": "周杰伦", "targetMinutes": 0}, "missingSlots": [],
                "actions": [{"capability": "music.artist_catalog", "source": "周杰伦列8首" if "周杰伦列8首" in text else text,
                             "dependsOn": [], "parameters": {"artist": "周杰伦", "limit": 8}}]}
    with pytest.raises(ApiError):
        validateRouteProposal(proposal, {"instruction": text}, messageId="quantity-no-permission")


@pytest.mark.parametrize("clauses", list(permutations(["我在散步", "请你给我推荐邓紫棋的歌曲", "给我形成一个30分钟的歌单"])))
def test_same_playlist_goal_merges_all_clause_orders(clauses):
    text = "，".join(clauses) + "。"
    result = routed(text)
    assert result["intents"] == ["scene_research"] and len(result["actions"]) == 1
    assert result["slots"] == {"artist": "邓紫棋", "targetMinutes": 30}
    assert result["actions"][0]["source"] == text
    brief, plan = compileMusicBrief({"instruction": text})
    assert (brief["scene"], brief["requiredArtist"], brief["artistOnly"], brief["targetMinutes"]) == ("散步", "邓紫棋", True, 30)
    assert {value["field"] for value in plan["sources"]} == {"scene", "artist", "targetMinutes"}
    assert all(value["source"] in text and value["scope"] == "task" for value in plan["sources"])
    assert plan["autoPlay"] is False and plan["savePreference"] is False


@pytest.mark.parametrize("text", ["推荐30分钟邓紫棋的散步歌单", "帮我生成邓紫棋的30分钟散步歌单", "推荐邓紫棋的散步歌曲，总时长30分钟"])
def test_compact_synonyms_preserve_three_constraints(text):
    brief, _ = compileMusicBrief({"instruction": text})
    assert (brief["scene"], brief["requiredArtist"], brief["targetMinutes"]) == ("散步", "邓紫棋", 30)


@pytest.mark.parametrize("scene", ["我在走路", "边走边听"])
def test_explicit_scene_alias_and_half_hour_normalize_with_original_sources(scene):
    text = scene + "，给我推荐邓紫棋的半小时歌单"
    brief, request = compileMusicBrief({"instruction": text})
    assert (brief["scene"], brief["requiredArtist"], brief["targetMinutes"]) == ("散步", "邓紫棋", 30)
    assert all(source["source"] in text for source in request["sources"])
    assert next(source for source in request["sources"] if source["field"] == "targetMinutes")["source"].endswith("半小时歌单")


@pytest.mark.parametrize("modifier", ["全部用", "只用", "仅用", "全都用"])
def test_artist_scope_modifier_is_not_part_of_artist_name(modifier):
    text = "我要边走路边听，" + modifier + "邓紫棋的歌，形成一份歌单，时间控制在半小时"
    request = musicRequirement(text, allow_partial=True)
    assert (request["artist"], request["scene"], request["targetMinutes"]) == ("邓紫棋", "散步", 30)
    assert all(source["source"] in text for source in request["sources"])


def test_unknown_duration_does_not_drop_known_artist_and_scene_or_force_zero_minutes():
    text = "我在走路，给我推荐邓紫棋的1.5小时歌单"
    request = musicRequirement(text, allow_partial=True)
    assert request["complete"] is False and request["artist"] == "邓紫棋" and request["scene"] == "散步"
    assert "targetMinutes" not in request and compileMusicBrief({"instruction": text}) is None
    brief, _ = compileMusicBrief({"instruction": ORIGINAL})
    verified = validateBrief({**brief, "scene": "走路", "targetMinutes": 90}, {"instruction": text})
    assert verified["scene"] == "散步" and verified["targetMinutes"] == 90
    with pytest.raises(ApiError):
        validateBrief({**brief, "scene": "", "targetMinutes": 90}, {"instruction": text})
    proposal = {"intents": ["scene_research"], "slots": {"artist": "邓紫棋", "targetMinutes": 90}, "missingSlots": [],
                "actions": [{"capability": "music.task", "source": text, "dependsOn": [], "parameters": {"mode": "new"}}]}
    assert validateRouteProposal(proposal, {"instruction": text}, messageId="unknown-duration") ["slots"]["targetMinutes"] == 90


@pytest.mark.parametrize("text", ["帮我找一些散步歌单", "请搜索通勤的歌单", "查找喝酒歌单"])
def test_existing_playlist_search_does_not_create_or_play(text):
    result = routed(text)
    assert result["intents"] == ["playlist_search"]
    assert result["actions"][0]["capability"] == "music.playlist_search"
    assert result["actions"][0]["parameters"] == playlistSearchRequest(text)
    assert playlistSearchRequest(ORIGINAL) is None


@pytest.mark.parametrize("prefix", ["不要", "他说", "假设", "如果", "引用：", "不是让你"])
def test_quoted_conditional_and_negative_clauses_do_not_grant_catalog_permission(prefix):
    text = prefix + "帮我找一些邓紫棋的歌曲"
    assert artistCatalogRequest(text) is None
    proposal = {"intents": ["artist_catalog"], "slots": {"artist": "邓紫棋", "targetMinutes": 0}, "missingSlots": [],
                "actions": [{"capability": "music.artist_catalog", "source": "帮我找一些邓紫棋的歌曲", "dependsOn": [], "parameters": {"artist": "邓紫棋", "limit": 12}}]}
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text}, messageId="unauthorized-fixture")
    assert error.value.routeReason == "catalog_permission_or_parameters"


def test_model_cannot_omit_any_explicit_artist_scene_or_duration():
    brief, _ = compileMusicBrief({"instruction": ORIGINAL})
    for field, bad in [("scene", ""), ("requiredArtist", ""), ("artistOnly", False), ("targetMinutes", 0)]:
        with pytest.raises(ApiError) as error:
            validateBrief({**brief, field: bad}, {"instruction": ORIGINAL})
        assert error.value.code == "INVALID_TASK_MODEL"
    proposal = deterministicRoute({"instruction": ORIGINAL})
    proposal["slots"]["targetMinutes"] = 0
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": ORIGINAL}, messageId="omitted-condition")
    assert error.value.routeReason == "required_music_slot"


@pytest.mark.parametrize("parameters", [None, {}, {"artist": "邓紫棋", "limit": 0}, {"artist": "邓紫棋", "limit": 31},
    {"artist": "周杰伦", "limit": 12}, {"artist": "邓紫棋", "limit": 12, "play": True}])
def test_catalog_null_extra_effects_wrong_subject_and_limit_cannot_bypass_contract(parameters):
    text = "帮我找一些邓紫棋的歌曲"
    proposal = deterministicRoute({"instruction": text})
    proposal["actions"][0]["parameters"] = parameters
    with pytest.raises(ApiError) as error:
        validateRouteProposal(proposal, {"instruction": text}, messageId="catalog-invalid-shape")
    assert error.value.routeReason == "catalog_permission_or_parameters"


def test_null_catalog_parameters_do_not_authorize_an_unrecognized_or_negative_request():
    text = "不要找邓紫棋的歌"
    proposal = {"intents": ["artist_catalog"], "slots": {"artist": "", "targetMinutes": 0}, "missingSlots": [],
                "actions": [{"capability": "music.artist_catalog", "source": text, "dependsOn": [], "parameters": None}]}
    with pytest.raises(ApiError):
        validateRouteProposal(proposal, {"instruction": text}, messageId="catalog-null-bypass")


def test_conflicting_duration_cannot_be_silently_selected_by_model():
    text = "生成邓紫棋的30分钟散步歌单，总时长20分钟"
    assert routed(text)["strategy"] == "clarify"
    brief, _ = compileMusicBrief({"instruction": ORIGINAL})
    with pytest.raises(ApiError):
        validateBrief(brief, {"instruction": text})


def test_followups_preserve_other_conditions_and_do_not_reuse_old_artist_identity():
    brief, _ = compileMusicBrief({"instruction": ORIGINAL})
    brief["requiredArtistId"] = 7763
    changed, _ = compileMusicBrief({"instruction": "改成20分钟", "currentBrief": brief})
    assert (changed["scene"], changed["requiredArtist"], changed["requiredArtistId"], changed["targetMinutes"]) == ("散步", "邓紫棋", 7763, 20)
    switched, _ = compileMusicBrief({"instruction": "换成周杰伦", "currentBrief": changed})
    assert (switched["scene"], switched["requiredArtist"], switched["targetMinutes"]) == ("散步", "周杰伦", 20)
    assert "requiredArtistId" not in switched
    retained, _ = compileMusicBrief({"instruction": "保留前三首", "currentBrief": switched, "preview": [{"id": n} for n in range(1, 7)]})
    assert retained["retainFirst"] == 3 and retained["targetMinutes"] == 20 and retained["requiredArtist"] == "周杰伦"
    assert routed("保留前三首")["strategy"] == "clarify"


def music_catalog(step, now, *, artist_id=7763, name="G.E.M.邓紫棋", requested="邓紫棋", scene=True):
    """真实署名夹具包含合作署名及一个同名但不同ID的对抗候选。"""
    source_name = "散步音乐精选" if scene else "歌手作品"
    kind = "playlist" if scene else "artist_catalog"
    result = {"tracks": [], "sources": [{"id": 77, "name": source_name, "sourceKind": kind, "tags": ["散步"] if scene else [], "checkedAt": iso(now)}],
              "checkedAt": iso(now), "artists": [], "resolvedArtist": {"id": artist_id, "name": name, "aliases": [requested]}}
    for index in range(11):
        result["tracks"].append({"id": artist_id * 100 + index, "title": "曲目" + str(index), "artist": name,
                                 "artists": [{"id": artist_id if index < 10 else 999999, "name": name}],
                                 "album": "真实元数据夹具", "durationMs": 200000, "sourceId": 77, "sourceKind": kind,
                                 "sourceName": source_name, "sourceTags": ["散步"] if scene else []})
    result["tracks"][0]["artists"].append({"id": 19, "name": "合作歌手"})
    return {"deviceId": "fixture-device", "leaseId": step["leaseId"], "revision": step["revision"], "result": result}


async def finish_music(runtime, task, fx, **options):
    waiting = await until(runtime, task["id"], fx.user, "waiting_tools", "failed", "needs_clarification")
    assert waiting["status"] == "waiting_tools", waiting.get("error")
    for step in waiting["pendingSteps"]:
        await runtime.result(task["id"], step["id"], fx.user, music_catalog(step, fx.time[0], **options))
    return await until(runtime, task["id"], fx.user, "preview", "failed", "needs_clarification")


async def test_actual_runtime_three_constraints_alias_ids_followups_and_no_plan_model(f):
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body(instruction=ORIGINAL))
    first = await finish_music(runtime, created, f)
    assert first["status"] == "preview", first.get("error")
    assert first["brief"]["requiredArtistId"] == 7763
    assert len(first["preview"]["tracks"]) == 9 and first["preview"]["totalDurationMs"] == 1800000
    assert all(artistCreditMatches(track, "邓紫棋", 7763) for track in first["preview"]["tracks"])
    assert {check["key"]: check["status"] for check in first["preview"]["checks"]}["scene"] == "satisfied"
    assert first["requirementPlan"]["scene"] == "散步" and first["adoption"] is None
    assert all(kind != "plan" for kind, _ in f.model.calls)
    assert any(entry["node"] == "understand_compiled" for entry in first["trace"])
    changed = await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=first["revision"], instruction="改成20分钟"))
    second = await until(runtime, first["id"], f.user, "preview", "failed", "waiting_tools")
    assert second["status"] == "preview" and second["preview"]["totalDurationMs"] == 1200000
    assert (second["brief"]["scene"], second["brief"]["requiredArtistId"]) == ("散步", 7763)
    switched = await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=second["revision"], instruction="换成周杰伦"))
    third = await finish_music(runtime, switched, f, artist_id=6452, name="周杰伦", requested="周杰伦")
    assert third["status"] == "preview" and third["brief"]["requiredArtistId"] == 6452
    assert third["preview"]["totalDurationMs"] == 1200000
    retained = await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=third["revision"], instruction="保留前三首"))
    fourth = await until(runtime, first["id"], f.user, "preview", "failed", "needs_clarification")
    assert fourth["status"] == "preview"
    assert fourth["lockedIds"] == [track["id"] for track in third["preview"]["tracks"][:3]]
    assert fourth["preview"]["tracks"][:3] == third["preview"]["tracks"][:3]
    assert all(kind != "plan" for kind, _ in f.model.calls)
    assert (await f.store.db.fetchone("SELECT count(*) AS n FROM music_task_calls WHERE node IN ('understand','understand_correction')"))["n"] == 0
    # 撤回同时恢复条件来源和候选，不能只把旧歌曲画回来而让下次修改继续使用新歌手候选。
    undone = await runtime.action(first["id"], f.user, body(action="undo", expectedRevision=fourth["revision"]))
    previous = await runtime.action(first["id"], f.user, body(action="undo", expectedRevision=undone["revision"]))
    assert previous["brief"]["requiredArtistId"] == 7763 and previous["requirementPlan"]["artist"] == "邓紫棋"
    assert {source["field"] for source in previous["requirementPlan"]["sources"]} >= {"scene", "artist", "targetMinutes"}
    resumed = await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=previous["revision"], instruction="改成30分钟"))
    restored = await until(runtime, first["id"], f.user, "preview", "failed", "needs_clarification")
    assert restored["status"] == "preview" and restored["preview"]["totalDurationMs"] == 1800000
    assert all(artistCreditMatches(track, "邓紫棋", 7763) for track in restored["preview"]["tracks"])
    assert len([source for source in restored["requirementPlan"]["sources"] if source["field"] == "targetMinutes"]) == 1


async def test_runtime_unverified_scene_is_honest_and_identity_failure_does_not_commit(f):
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body(instruction=ORIGINAL))
    waiting = await until(runtime, created["id"], f.user, "waiting_tools", "failed")
    step = waiting["pendingSteps"][0]
    bad = music_catalog(step, f.time[0])
    bad["result"]["resolvedArtist"] = {"id": 77, "name": "另一位", "aliases": []}
    with pytest.raises(ApiError) as error:
        await runtime.result(created["id"], step["id"], f.user, bad)
    assert error.value.code == "CATALOG_ARTIST_UNVERIFIED"
    assert (await runtime.get(created["id"], f.user))["status"] == "waiting_tools"
    await runtime.result(created["id"], step["id"], f.user, music_catalog(step, f.time[0], scene=False))
    preview = await until(runtime, created["id"], f.user, "preview", "failed")
    assert preview["status"] == "preview"
    checks = {check["key"]: check for check in preview["preview"]["checks"]}
    assert checks["duration"]["status"] == "satisfied" and checks["required_artist"]["status"] == "satisfied"
    assert checks["scene"]["status"] == "unverified" and "未分析音频" in checks["scene"]["text"]


async def test_runtime_model_patch_updates_current_requirement_sources_and_undo(f):
    """模型修改与编译修改共用当前需求；身份选择、锁定和撤回不串版本。"""
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body(instruction=ORIGINAL))
    waiting = await until(runtime, created["id"], f.user, "waiting_tools", "failed")
    step = waiting["pendingSteps"][0]
    choice = music_catalog(step, f.time[0])
    choice["result"].update(tracks=[], sources=[], question="请选择当前曲库中的歌手。",
                            artists=[{"id": 99881, "name": "邓紫棋"}, {"id": 7763, "name": "G.E.M.邓紫棋"}])
    choice["result"].pop("resolvedArtist")
    await runtime.result(created["id"], step["id"], f.user, choice)
    pending = await until(runtime, created["id"], f.user, "needs_clarification", "failed")
    assert pending["status"] == "needs_clarification"
    original_sources = clone(pending["requirementPlan"]["sources"])
    selected = await runtime.action(created["id"], f.user, body(action="select_artist", artistId=7763, expectedRevision=pending["revision"]))
    first = await finish_music(runtime, selected, f)
    assert first["status"] == "preview" and first["brief"]["requiredArtistId"] == 7763
    assert first["requirementPlan"]["sources"] == original_sources
    locked_ids = [track["id"] for track in first["preview"]["tracks"][:3]]
    locked = await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=first["revision"], lockedIds=locked_ids))

    def model_patch(inputs):
        # 走正式逐字段补丁权限校验；故意使用不完整编译的附句覆盖模型路径。
        minutes = 20 if "20分钟" in inputs["instruction"] else 30
        return mergeBriefPatch({"kind": "edit", "changes": [{"field": "targetMinutes", "op": "set", "value": minutes,
                               "evidence": f"改成{minutes}分钟"}], "question": "", "adjustment": ""}, inputs)

    f.model.followup = model_patch
    instruction = "改成20分钟，歌手和场景都不变"
    assert compileMusicBrief({"instruction": instruction, "currentBrief": locked["brief"]}) is None
    await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=locked["revision"], instruction=instruction))
    second = await until(runtime, first["id"], f.user, "preview", "failed", "waiting_tools", "needs_clarification")
    assert second["status"] == "preview", second.get("error")
    assert second["brief"]["targetMinutes"] == 20 and second["preview"]["totalDurationMs"] == 1200000
    assert second["requirementPlan"]["targetMinutes"] == 20
    assert second["requirementPlan"]["scene"] == "散步" and second["requirementPlan"]["artist"] == "邓紫棋"
    assert second["brief"]["requiredArtistId"] == 7763 and second["lockedIds"] == locked_ids
    assert second["preview"]["tracks"][:3] == first["preview"]["tracks"][:3]
    assert second["adoption"] is None
    changed_source = [source for source in second["requirementPlan"]["sources"] if source["field"] == "targetMinutes"]
    assert changed_source == [{"field": "targetMinutes", "value": 20, "source": "改成20分钟", "scope": "task"}]
    assert [source for source in second["requirementPlan"]["sources"] if source["field"] != "targetMinutes"] == [source for source in original_sources if source["field"] != "targetMinutes"]
    assert len([kind for kind, _ in f.model.calls if kind == "plan"]) == 1

    undone = await runtime.action(first["id"], f.user, body(action="undo", expectedRevision=second["revision"]))
    assert undone["brief"]["targetMinutes"] == undone["requirementPlan"]["targetMinutes"] == 30
    assert undone["requirementPlan"]["sources"] == original_sources
    assert undone["brief"]["requiredArtistId"] == 7763 and undone["lockedIds"] == locked_ids
    await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=undone["revision"], instruction=instruction))
    again = await until(runtime, first["id"], f.user, "preview", "failed", "waiting_tools", "needs_clarification")
    assert again["status"] == "preview" and again["requirementPlan"]["targetMinutes"] == 20
    assert again["brief"]["requiredArtistId"] == 7763 and again["lockedIds"] == locked_ids
    await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=again["revision"], instruction="改成30分钟，歌手和场景都不变"))
    final = await until(runtime, first["id"], f.user, "preview", "failed", "waiting_tools", "needs_clarification")
    assert final["status"] == "preview" and final["requirementPlan"]["targetMinutes"] == 30
    assert final["brief"]["targetMinutes"] == 30 and final["preview"]["totalDurationMs"] == 1800000
    assert final["brief"]["requiredArtistId"] == 7763 and final["lockedIds"] == locked_ids
    assert len([source for source in final["requirementPlan"]["sources"] if source["field"] == "targetMinutes"]) == 1


def test_skill_registry_maps_only_registered_executable_capabilities_and_declares_boundaries():
    from tango_backend.capability_registry import CAPABILITY_REGISTRY
    skills = musicSkills()
    assert set(skills["skills"]) >= {"song.lookup", "artist.catalog", "playlist.find", "scene.playlist"}
    assert all(skill["capability"] in CAPABILITY_REGISTRY for skill in skills["skills"].values())
    assert skills["mcp"] is False and skills["dynamicMultiAgent"] is False
    skills["skills"].clear()
    assert musicSkills()["skills"]


@pytest.mark.parametrize("name", ["不适合散步", "非散步场景", "散步不适用"])
def test_scene_source_negative_text_cannot_become_positive_evidence(name):
    track = {"sourceId": 1, "sourceName": name, "sourceKind": "playlist"}
    source = {"id": 1, "name": name, "tags": ["散步"], "sourceKind": "playlist"}
    assert playlistSceneEvidence(track, [source], "散步")["status"] == "unverified"


async def test_artist_catalog_history_uses_persisted_owned_route_without_model(fx):
    request, _ = await fx.submit("帮我找一些邓紫棋的歌曲")
    first = await fx.route(request)
    assert first["route"]["result"]["intents"] == ["artist_catalog"]
    history, _ = await fx.submit("我之前问过哪些歌手")
    result = await fx.route(history)
    assert "邓紫棋" in result["route"]["result"]["actions"][0]["parameters"]["text"]
    assert fx.calls == []
