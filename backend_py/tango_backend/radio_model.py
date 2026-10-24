"""私人电台：规划检索与排序真实候选，推荐理由由曲库事实生成。"""

import re
from .compat import safe_integer, utf16_slice
from .errors import ApiError
from .model_common import exact, text, strings, envelope_json, PROMPTS
from .model_transport import createChatTransport

COMMON_FIELDS = ["instruction", "history", "preference"]
CANDIDATE_FIELDS = ["id", "title", "artist", "album", "sourceName", "sourceTags"]
BASIS_NOTE = PROMPTS["radio-model"]["BASIS_NOTE"]


def commonValid(body):
    return (
        text(body.get("instruction"), 1000, True)
        and strings(body.get("history"), 4, 1000)
        and text(body.get("preference"), 500)
    )


def commonInput(body):
    return {
        "instruction": body["instruction"].strip(),
        "history": [v.strip() for v in body["history"]],
        "preference": body["preference"].strip(),
    }


def invalidInput():
    return ApiError(
        400, "INVALID_RADIO_REQUEST", "电台要求或候选歌曲无效，请调整后重试。"
    )


def invalidOutput():
    return ApiError(
        422, "INVALID_RADIO_RESPONSE", "AI 未返回有效的真实候选歌单，请重试。"
    )


def validateRadioPlanInput(body):
    if not exact(body, COMMON_FIELDS) or not commonValid(body):
        raise invalidInput()
    return commonInput(body)


def validateRadioSelectInput(body):
    if (
        not exact(body, COMMON_FIELDS + ["candidates", "excludedIds"])
        or not commonValid(body)
        or not isinstance(body["candidates"], list)
        or not 1 <= len(body["candidates"]) <= 40
        or not isinstance(body["excludedIds"], list)
        or len(body["excludedIds"]) > 60
        or not all(safe_integer(v) and v > 0 for v in body["excludedIds"])
        or len(set(body["excludedIds"])) != len(body["excludedIds"])
    ):
        raise invalidInput()
    ids = set()
    for item in body["candidates"]:
        if (
            not exact(item, CANDIDATE_FIELDS)
            or not safe_integer(item["id"])
            or item["id"] <= 0
            or item["id"] in ids
            or not text(item["title"], 120, True)
            or not text(item["artist"], 120)
            or not text(item["album"], 120)
            or not text(item["sourceName"], 160, True)
            or not strings(item["sourceTags"], 6, 40)
        ):
            raise invalidInput()
        ids.add(item["id"])
    if ids <= set(body["excludedIds"]):
        raise ApiError(
            422, "NO_RADIO_CANDIDATES", "这批候选都已听过，请换一个主题再试。"
        )
    return {
        **commonInput(body),
        "candidates": [
            {
                k: [v.strip() for v in t[k]]
                if k == "sourceTags"
                else t[k].strip()
                if isinstance(t[k], str)
                else t[k]
                for k in CANDIDATE_FIELDS
            }
            for t in body["candidates"]
        ],
        "excludedIds": body["excludedIds"][:],
    }


def noteWithBasis(note):
    return (
        utf16_slice(BASIS_NOTE + " " + note.strip(), 300)
        if note.strip()
        else BASIS_NOTE
    )


def displayText(value, maximum):
    clean = re.sub(r"\s+", " ", value).strip()
    return clean if text(clean, maximum) else utf16_slice(clean, maximum - 1) + "…"


def groundedReason(candidate):
    source = displayText(candidate["sourceName"], 60)
    artist = displayText(candidate["artist"], 30)
    title = displayText(candidate["title"], 40)
    return f"来自「{source}」歌单：" + (artist + " · " if artist else "") + title + "。"


def parsePlan(envelope):
    r = envelope_json(
        envelope, "INVALID_RADIO_RESPONSE", "AI 未返回有效的真实候选歌单，请重试。"
    )
    if (
        not exact(r, ["title", "queries", "excludedArtists", "note"])
        or not text(r["title"], 80, True)
        or not strings(r["queries"], 2, 80, True)
        or not r["queries"]
        or not strings(r["excludedArtists"], 20, 100, True)
        or not text(r["note"], 300)
    ):
        raise invalidOutput()
    return {
        "title": r["title"].strip(),
        "queries": [v.strip() for v in r["queries"]],
        "excludedArtists": [v.strip() for v in r["excludedArtists"]],
        "note": noteWithBasis(r["note"]),
    }


def parseSelection(envelope, input):
    r = envelope_json(
        envelope, "INVALID_RADIO_RESPONSE", "AI 未返回有效的真实候选歌单，请重试。"
    )
    if (
        not exact(r, ["title", "note", "tracks"])
        or not text(r["title"], 80, True)
        or not text(r["note"], 300)
        or not isinstance(r["tracks"], list)
        or not 1 <= len(r["tracks"]) <= 12
    ):
        raise invalidOutput()
    candidates = {t["id"]: t for t in input["candidates"]}
    excluded = set(input["excludedIds"])
    seen = set()
    for t in r["tracks"]:
        if (
            not exact(t, ["id", "reason"])
            or not safe_integer(t["id"])
            or t["id"] not in candidates
            or t["id"] in excluded
            or t["id"] in seen
            or not text(t["reason"], 160, True)
        ):
            raise invalidOutput()
        seen.add(t["id"])
    return {
        "title": r["title"].strip(),
        "note": noteWithBasis(
            f"已从真实候选中选出 {len(r['tracks'])} 首，完整播放以账号权限为准。"
        ),
        "tracks": [
            {"id": t["id"], "reason": groundedReason(candidates[t["id"]])}
            for t in r["tracks"]
        ],
    }


class RadioModel:
    def __init__(self, options=None, **kwargs):
        self.transport = createChatTransport(options, **kwargs)
        self.configured = self.transport.configured
        self.model = self.transport.model

    async def plan(self, input, options=None):
        return parsePlan(
            await self.transport.complete(
                {
                    "prompt": PROMPTS["radio-model"]["PLAN_PROMPT"],
                    "input": input,
                    "maxTokens": 1600,
                },
                options,
            )
        )

    async def select(self, input, options=None):
        return parseSelection(
            await self.transport.complete(
                {
                    "prompt": PROMPTS["radio-model"]["SELECT_PROMPT"],
                    "input": input,
                    "maxTokens": 2600,
                },
                options,
            ),
            input,
        )


def createRadioModel(options=None, **kwargs):
    return RadioModel(options, **kwargs)
