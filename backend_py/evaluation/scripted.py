"""明确编写的离线替身，只验证执行器接线，不提供模型语义成绩。"""

from tango_backend.compat import clone, iso
from tango_backend.task_schema import validateBrief


def brief(**patch):
    return {
        "kind": "research", "title": "合成曲库开发校准", "scene": "读书",
        "styles": ["纯音乐"], "queries": ["读书 纯音乐"], "artist": "",
        "requiredArtist": "", "artistOnly": False, "since": None, "until": None,
        "targetMinutes": 20, "excludeLive": False, "avoidPrevious": False,
        "excludedArtists": [], "requireReleases": False, "retainFirst": 0,
        "removeIds": [], "question": "", "adjustment": "", **patch,
    }


class ScriptedModel:
    """按公开校准场景返回预写提案；不访问网络、不读取真实模型配置。"""

    configured = True
    model = "scripted-calibration-fixture"

    def __init__(self, case_id, now):
        self.case_id, self.now, self.turn_index = case_id, now, 0

    async def route(self, input, options=None):
        text = input["instruction"]
        intents = ["scene_research"]
        actions = [{"capability": "music.task", "source": text, "dependsOn": [],
                    "parameters": {"mode": "continue" if input.get("task") else "new"}}]
        slots, missing = {"artist": "", "targetMinutes": 20}, []
        if self.case_id in ("releases", "clarification"):
            intents = ["release_research"]
            slots["artist"] = "邓紫棋"
            if self.case_id == "clarification" and self.turn_index == 0:
                slots = {"artist": "", "targetMinutes": 0}
                intents += ["clarification"]
                actions = [{"capability": "assistant.reply", "source": text, "dependsOn": [],
                            "parameters": {"kind": "clarify", "text": "请问是哪位歌手？"}}]
                missing = [{"intent": "release_research", "slot": "artist", "question": "请问是哪位歌手？"}]
        if self.case_id == "compound":
            intents = ["preference_proposal", "scene_research"]
            actions[0]["source"] = "推荐20分钟微醺歌单"
            actions.insert(0, {"capability": "memory.propose", "source": "记住我喜欢粤语", "dependsOn": [],
                               "parameters": {"operation": "set", "preference": "喜欢粤语"}})
        if self.case_id == "quoted" and self.turn_index == 1:
            intents = ["explain_result"]
            actions = [{"capability": "assistant.reply", "source": "请解释当前歌单", "dependsOn": [],
                        "parameters": {"kind": "explain", "text": ""}}]
            slots = {"artist": "", "targetMinutes": 0}
        return {"value": {"intents": intents, "slots": slots, "actions": actions, "missingSlots": missing}}

    async def call(self, kind, input, options=None):
        if kind == "plan":
            value = clone(input.get("currentBrief") or brief())
            if self.case_id in ("scene", "compound"):
                value.update(scene="微醺", styles=["粤语"], queries=["微醺 粤语"])
            if self.case_id == "scene":
                value["excludeLive"] = True
            if self.case_id == "negative":
                value.update(scene="微醺", styles=["爵士"], queries=["微醺 爵士"])
            if self.case_id in ("releases", "clarification"):
                value.update(artist="邓紫棋", requiredArtist="邓紫棋", artistOnly=True,
                             requireReleases=True, queries=[], styles=[],
                             since=iso(self.now - 30 * 86400000), until=iso(self.now))
            if self.case_id == "revision":
                value["excludeLive"] = True
                if self.turn_index == 1:
                    value.update(styles=["纯音乐", "轻快"], queries=["读书 纯音乐 轻快"], adjustment="轻快")
            if self.case_id == "catalog_failure":
                value.update(scene="跑步", styles=[], queries=["跑步"])
            value = validateBrief(value, input)
        elif kind == "research":
            scope = input["scope"]
            value = {"queries": scope["queries"], "artist": scope["artist"], "artistId": 0,
                     "since": scope["since"], "until": scope["until"], "note": "离线预写计划"}
        elif kind == "observe":
            value = {"decision": "done", "queries": [], "note": "离线预写观察"}
        elif kind == "compose":
            value = {"ids": [track["id"] for track in input["candidates"]]}
        else:
            raise ValueError("unsupported_scripted_method")
        return {"value": value}


def controlled_catalog(step, now, turn_index, device, fault=False):
    """曲库在请求前即确定；不根据模型编出的查询或参数创造正确答案。"""
    if fault:
        return {"deviceId": device, "leaseId": step["leaseId"], "revision": step["revision"],
                "error": {"code": "CATALOG_UNAVAILABLE", "message": "校准用曲库故障"}}
    release = step["skill"] == "releases.research"
    source = 880 if release else 770 + turn_index
    tags = [] if release else ["读书", "纯音乐", "粤语", "爵士", "轻快", "微醺"]
    tracks = []
    for i in range(12):
        row = {"id": (9000 if release else 1000 + turn_index * 100) + i,
               "title": "合成曲目" + str(i), "artist": "邓紫棋" if release else "合成演奏者",
               "album": "校准合成专辑", "coverUrl": "", "durationMs": 240000,
               "sourceId": source, "sourceKind": "album" if release else "playlist",
               "sourceName": "校准固定候选", "sourceTags": tags}
        if release:
            row.update(publishedAt=iso(now - (i + 1) * 86400000), artistId=7763, albumId=880)
        tracks.append(row)
    if not release:
        tracks.append({**tracks[0], "id": 8000 + turn_index, "title": "合成现场版 Live"})
    result = {"tracks": tracks, "sources": [{"id": source, "name": "校准固定候选",
              "checkedAt": iso(now), "sourceKind": "album" if release else "playlist", "tags": tags}],
              "checkedAt": iso(now), "artists": []}
    if release:
        result["resolvedArtist"] = {"id": 7763, "name": "邓紫棋"}
    return {"deviceId": device, "leaseId": step["leaseId"], "revision": step["revision"], "result": result}
