"""任务输入、模型需求与本机工具结果的独立契约及歌单硬约束。"""

import re
import math
import unicodedata
from datetime import datetime, timezone
from urllib.parse import urlsplit, urlunsplit, quote, unquote
from decimal import Decimal, ROUND_HALF_UP
from .compat import clone, utf16_len, utf16_slice, safe_integer, iso, now_ms
from .errors import ApiError
from .task_duration import selectDurationSubset
from .task_style_evidence import adjustmentStyles, playlistStyleEvidence
from .task_language import catalogLanguageConflict
from .task_release_intent import releaseIntentSignal
from .task_track_count import trackCountRequest, trackCountIssue

TASK_SKILLS = {
    "scene.research": {
        "version": 1,
        "permission": "catalog.read",
        "maximumAttempts": 2,
        "timeoutMs": 45000,
        "maximumTracks": 40,
        "description": "检索并核对场景歌单来源，返回真实候选。",
    },
    "releases.research": {
        "version": 1,
        "permission": "catalog.read",
        "maximumAttempts": 2,
        "timeoutMs": 45000,
        "maximumTracks": 40,
        "description": "消歧歌手并核对指定时间范围内的真实发行。",
    },
}


def clean(s, n=1000):
    return (
        utf16_slice(re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", s).strip(), n)
        if isinstance(s, str)
        else ""
    )


def object(value):
    return isinstance(value, dict)


def validId(value):
    return safe_integer(value) and value > 0


def idList(value, max=100):
    return (
        isinstance(value, list)
        and len(value) <= max
        and all(validId(v) for v in value)
        and len(set(value)) == len(value)
    )


def identifier(value):
    return isinstance(value, str) and bool(re.fullmatch(r"[A-Za-z0-9_-]{8,100}", value))


def invalid(message="音乐任务参数无效。"):
    return ApiError(400, "INVALID_TASK_REQUEST", message)


def identity(body):
    if (
        not object(body)
        or not identifier(body.get("deviceId"))
        or not identifier(body.get("requestId"))
    ):
        raise invalid()


def createInput(body):
    identity(body)
    if "catalogContext" in body and not identifier(body["catalogContext"]):
        raise invalid("曲库会话标识无效。")
    if (
        not isinstance(body.get("instruction"), str)
        or not body["instruction"].strip()
        or utf16_len(body["instruction"]) > 1000
        or "preference" in body
        and (
            not isinstance(body["preference"], str)
            or utf16_len(body["preference"]) > 500
        )
        or "excludedIds" in body
        and not idList(body["excludedIds"])
    ):
        raise invalid()
    return {
        "deviceId": body["deviceId"],
        "requestId": body["requestId"],
        "instruction": clean(body["instruction"]),
        "preference": clean(body.get("preference"), 500),
        "excludedIds": body.get("excludedIds", []),
        "catalogContext": body.get("catalogContext", ""),
    }


def actionInput(body):
    identity(body)
    if "catalogContext" in body and not identifier(body["catalogContext"]):
        raise invalid("曲库会话标识无效。")
    action = body.get("action")
    if not validId(body.get("expectedRevision")) or action not in (
        "turn",
        "resume",
        "pause",
        "cancel",
        "adopt",
        "applied",
        "undo",
        "lock",
        "select_artist",
    ):
        raise invalid()
    if action == "turn" and (
        not isinstance(body.get("instruction"), str)
        or not body["instruction"].strip()
        or utf16_len(body["instruction"]) > 1000
    ):
        raise invalid()
    if "preference" in body and (
        not isinstance(body["preference"], str) or utf16_len(body["preference"]) > 500
    ):
        raise invalid()
    if (
        action == "lock"
        and not idList(body.get("lockedIds"), 30)
        or action == "applied"
        and not identifier(body.get("adoptionId"))
        or action == "select_artist"
        and not validId(body.get("artistId"))
    ):
        raise invalid()
    return {**body, "instruction": clean(body.get("instruction"))}


def date_ms(value):
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value if math.isfinite(value) else math.nan
    if not isinstance(value, str) or not value:
        return math.nan
    try:
        dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return math.floor(dt.timestamp() * 1000)
    except (ValueError, OverflowError):
        return math.nan


def _date(value):
    ms = date_ms(value)
    try:
        return iso(ms) if math.isfinite(ms) and ms > 0 else None
    except (ValueError, OverflowError, OSError):
        return None


def _cover(value):
    if not value:
        return ""
    try:
        url = urlsplit(value)
        if (
            url.scheme == "https"
            and not url.username
            and not url.password
            and re.fullmatch(
                r"(?:p\d+|p\d+c)\.music\.126\.net|music\.126\.net|y\.music\.163\.com",
                url.hostname or "",
            )
        ):
            host = url.hostname.lower()
            authority = host + (
                ":" + str(url.port) if url.port not in (None, 443) else ""
            )
            parts = []
            for part in (url.path or "/").split("/"):
                decoded = unquote(part).lower()
                if decoded == "..":
                    if len(parts) > 1:
                        parts.pop()
                elif decoded != ".":
                    parts.append(part)
            path = "/".join(parts) or "/"
            if url.path.endswith(("/.", "/..")) and not path.endswith("/"):
                path += "/"
            return urlunsplit(
                (
                    url.scheme,
                    authority,
                    quote(path, safe="/%:@-._~!$&'()*+,;=[]"),
                    quote(url.query, safe="/%?:@-._~!$&()*+,;=[]"),
                    quote(url.fragment, safe="/%?:@-._~!$&'()*+,;=[]"),
                )
            )
    except (ValueError, TypeError):
        pass
    return ""


def _provenance(value):
    return {
        "sourceId": value.get("sourceId") if validId(value.get("sourceId")) else 0,
        "sourceKind": value.get("sourceKind")
        if value.get("sourceKind") in ("playlist", "artist_search", "artist_catalog", "album")
        else "unknown",
        "sourceQuery": clean(value.get("sourceQuery"), 80),
    }


def toolResult(body, now):
    if (
        not object(body)
        or not identifier(body.get("deviceId"))
        or not identifier(body.get("leaseId"))
        or not safe_integer(body.get("revision"))
    ):
        raise invalid()
    if "catalogContext" in body and not identifier(body["catalogContext"]):
        raise invalid("曲库会话标识无效。")
    if "error" in body:
        if not object(body["error"]):
            raise invalid()
        return {
            "error": {
                "code": clean(body["error"].get("code"), 60) or "TOOL_FAILED",
                "message": "本机曲库查询未完成，可以恢复或继续修改要求。",
            }
        }
    value = body.get("result")
    checked = _date(value.get("checkedAt")) if object(value) else None
    if (
        not object(value)
        or not isinstance(value.get("tracks"), list)
        or len(value["tracks"]) > 40
        or not isinstance(value.get("sources"), list)
        or len(value["sources"]) > 3
        or not checked
        or abs(date_ms(checked) - now) > 300000
    ):
        raise invalid("曲库结果无效或已过期，请重新查询。")
    seen = set()
    tracks = []
    for track in value["tracks"]:
        if (
            not object(track)
            or not validId(track.get("id"))
            or track["id"] in seen
            or not clean(track.get("title"), 120)
            or not isinstance(track.get("title"), str)
            or utf16_len(track["title"]) > 120
            or not clean(track.get("sourceName"), 160)
            or not isinstance(track.get("sourceTags"), list)
            or len(track["sourceTags"]) > 6
            or any(
                not isinstance(t, str) or utf16_len(t) > 40 for t in track["sourceTags"]
            )
            or not safe_integer(track.get("durationMs"))
            or not 0 <= track["durationMs"] <= 7200000
        ):
            raise invalid("候选歌曲缺少真实元数据。")
        seen.add(track["id"])
        out = {
            "id": track["id"],
            "title": clean(track["title"], 120),
            "artist": clean(track.get("artist"), 120),
            "album": clean(track.get("album"), 120),
            "coverUrl": _cover(track.get("coverUrl")),
            "durationMs": track["durationMs"],
            "sourceName": clean(track["sourceName"], 160),
            "sourceTags": [clean(t, 40) for t in track["sourceTags"]],
            **_provenance(track),
        }
        if _date(track.get("publishedAt")):
            out["publishedAt"] = _date(track["publishedAt"])
        for key in ("artistId", "albumId"):
            if validId(track.get(key)):
                out[key] = track[key]
        if "artists" in track:
            credits = track["artists"]
            if not isinstance(credits, list) or len(credits) > 12 or any(
                not object(a) or not validId(a.get("id")) or not isinstance(a.get("name"), str)
                or not clean(a["name"], 100) or utf16_len(a["name"]) > 100 for a in credits
            ) or len({a["id"] for a in credits}) != len(credits):
                raise invalid("候选歌曲的艺人署名无效。")
            out["artists"] = [{"id": a["id"], "name": clean(a["name"], 100)} for a in credits]
        tracks.append(out)
    sources = []
    for source in value["sources"]:
        if (
            not object(source)
            or not clean(source.get("name"), 160)
            or not _date(source.get("checkedAt"))
            or abs(date_ms(_date(source["checkedAt"])) - now) > 300000
        ):
            raise invalid()
        sources.append(
            {
                "id": source["id"] if validId(source.get("id")) else 0,
                "name": clean(source["name"], 160),
                "checkedAt": _date(source["checkedAt"]),
                **_provenance(source),
                "tags": [clean(s, 40) for s in source["tags"][:6]]
                if isinstance(source.get("tags"), list)
                else [],
            }
        )
    artists = []
    for artist in (
        value.get("artists") if isinstance(value.get("artists"), list) else []
    )[:5]:
        if not object(artist):
            raise invalid()
        artists.append(
            {
                "id": artist.get("id"),
                "name": clean(artist.get("name"), 100),
                "description": clean(artist.get("description"), 160),
            }
        )
        if "aliases" in artist:
            if not isinstance(artist["aliases"], list) or len(artist["aliases"]) > 10 or any(
                not isinstance(alias, str) or not clean(alias, 100) or utf16_len(alias) > 100 for alias in artist["aliases"]
            ):
                raise invalid("歌手候选别名无效。")
            artists[-1]["aliases"] = list(dict.fromkeys(clean(alias, 100) for alias in artist["aliases"]))
    if any(not validId(a["id"]) or not a["name"] for a in artists):
        raise invalid()
    result = {
        "tracks": tracks,
        "sources": sources,
        "checkedAt": checked,
        "question": clean(value.get("question"), 300),
        "artists": artists,
    }
    if "resolvedArtist" in value:
        a = value["resolvedArtist"]
        if not object(a) or not validId(a.get("id")) or not clean(a.get("name"), 100):
            raise invalid()
        result["resolvedArtist"] = {"id": a["id"], "name": clean(a["name"], 100)}
        if "aliases" in a:
            if not isinstance(a["aliases"], list) or len(a["aliases"]) > 10 or any(
                not isinstance(alias, str) or not clean(alias, 100) or utf16_len(alias) > 100 for alias in a["aliases"]
            ):
                raise invalid("歌手别名无效。")
            result["resolvedArtist"]["aliases"] = list(dict.fromkeys(clean(alias, 100) for alias in a["aliases"]))
    return result


BRIEF_DEFAULTS = {
    "title": "待补充的音乐任务",
    "scene": "",
    "styles": [],
    "queries": [],
    "artist": "",
    "requiredArtist": "",
    "artistOnly": False,
    "since": None,
    "until": None,
    "targetMinutes": 0,
    "excludeLive": False,
    "avoidPrevious": False,
    "excludedArtists": [],
    "requireReleases": False,
    "retainFirst": 0,
    "removeIds": [],
    "adjustment": "",
}
_MISSING = Ellipsis


def _bounded(value, maximum, empty=True):
    return (
        isinstance(value, str)
        and utf16_len(value) <= maximum
        and (empty or bool(clean(value, maximum)))
    )


def _brief_date(value):
    if value is None or value == "":
        return None
    if not isinstance(value, str):
        return _MISSING
    pattern = r"(\d{4})-(\d{2})-(\d{2})(?:T(\d{2}):(\d{2})(?::(\d{2})(?:\.\d{1,9})?)?(Z|[+-]\d{2}:\d{2}))?"
    parts = re.fullmatch(pattern, value, re.ASCII)
    if not parts:
        return _MISSING
    year, month, day, hour, minute, second, zone = parts.groups()
    try:
        datetime(int(year), int(month), int(day))
        if (
            int(hour or 0) > 23
            or int(minute or 0) > 59
            or int(second or 0) > 59
            or zone
            and zone != "Z"
            and (int(zone[1:3]) > 23 or int(zone[4:]) > 59)
        ):
            return _MISSING
    except ValueError:
        return _MISSING
    return _date(value) or _MISSING


def validateBrief(value, context=None):
    if object(value) and value.get("kind") == "clarify":
        if not _bounded(value.get("question"), 300, False):
            raise ApiError(
                422, "INVALID_TASK_MODEL", "澄清必须包含一个不超过300字的具体问题。"
            )
        confirmed = (context or {}).get("currentBrief") or {}
        out = {k: clone(confirmed.get(k, v)) for k, v in BRIEF_DEFAULTS.items()}
        if "targetTrackCount" in confirmed:
            out["targetTrackCount"] = confirmed["targetTrackCount"]
        if validId(confirmed.get("artistId")):
            out["artistId"] = confirmed["artistId"]
        if confirmed.get("catalogLanguage") in ("unknown", "zh", "en", "any"):
            out["catalogLanguage"] = confirmed["catalogLanguage"]
        return {**out, "kind": "clarify", "question": clean(value["question"], 300)}
    if (
        not object(value)
        or value.get("kind") not in ("research", "edit")
        or not _bounded(value.get("title"), 80, False)
        or not isinstance(value.get("queries"), list)
        or len(value["queries"]) > 2
        or any(not _bounded(q, 80, False) for q in value["queries"])
        or not isinstance(value.get("excludedArtists"), list)
        or len(value["excludedArtists"]) > 20
        or any(not _bounded(a, 100, False) for a in value["excludedArtists"])
        or not isinstance(value.get("styles"), list)
        or len(value["styles"]) > 6
        or any(not _bounded(s, 40, False) for s in value["styles"])
        or not safe_integer(value.get("targetMinutes"))
        or not 0 <= value["targetMinutes"] <= 240
        or "targetTrackCount" in value
        and (
            not safe_integer(value["targetTrackCount"])
            or not 0 <= value["targetTrackCount"] <= 30
        )
        or type(value.get("excludeLive")) is not bool
        or type(value.get("requireReleases")) is not bool
        or not safe_integer(value.get("retainFirst"))
        or not 0 <= value["retainFirst"] <= 30
        or not idList(value.get("removeIds"), 30)
    ):
        raise ApiError(
            422, "INVALID_TASK_MODEL", "AI 未返回有效的音乐任务，请调整后重试。"
        )
    for key, maximum in [
        ("scene", 160),
        ("artist", 100),
        ("question", 300),
        ("adjustment", 160),
    ]:
        if key in value and not _bounded(value[key], maximum):
            raise ApiError(422, "INVALID_TASK_MODEL", f"AI 返回的 {key} 文本无效。")
    if (
        "avoidPrevious" in value
        and type(value["avoidPrevious"]) is not bool
        or "artistId" in value
        and (not safe_integer(value["artistId"]) or value["artistId"] < 0)
    ):
        raise ApiError(
            422, "INVALID_TASK_MODEL", "AI 返回的布尔约束或歌手编号类型无效。"
        )
    since, until = _brief_date(value.get("since")), _brief_date(value.get("until"))
    if (
        since is _MISSING
        or until is _MISSING
        or since
        and until
        and date_ms(since) > date_ms(until)
    ):
        raise ApiError(422, "INVALID_TASK_MODEL", "AI 返回的发行时间范围无效。")
    if (
        "requiredArtist" in value
        and not _bounded(value["requiredArtist"], 100)
        or "artistOnly" in value
        and type(value["artistOnly"]) is not bool
        or value.get("artistOnly") is True
        and not clean(value.get("requiredArtist"), 100)
    ):
        raise ApiError(
            422,
            "INVALID_TASK_MODEL",
            "指定歌手约束无效，请分别填写 requiredArtist 和 artistOnly。",
        )
    if value["requireReleases"] and not clean(value.get("artist"), 100):
        raise ApiError(
            422,
            "INVALID_TASK_MODEL",
            "新作查询必须在 artist 填写用户明确指定的新作歌手；不要把歌手放在 styles 中。",
        )
    if context:
        from .music_request import canonicalScene, musicRequirement, musicRequirementIssue
        if musicRequirementIssue(context.get("instruction", "")):
            raise ApiError(422, "INVALID_TASK_MODEL", "本轮时长冲突或超出范围，必须澄清后再编排。")
        requirement = musicRequirement(context.get("instruction", ""), allow_partial=True)
        if requirement and (requirement.get("artist") or (context.get("currentBrief") or {}).get("requiredArtistId")):
            # 只检查能直接从当前原文确定的条件；模型不得遗漏或替换成相近的场景。
            for field, actual in [("scene", canonicalScene(value.get("scene"))), ("targetMinutes", value.get("targetMinutes")),
                                  ("retainFirst", value.get("retainFirst"))]:
                if field in requirement and requirement[field] != actual:
                    raise ApiError(422, "INVALID_TASK_MODEL", f"当前原文明确要求的{field}未完整保留，不能生成缺条件的歌单。")
            if "artist" in requirement and (value.get("requiredArtist") != requirement["artist"] or value.get("artistOnly") is not True):
                raise ApiError(422, "INVALID_TASK_MODEL", "当前指定歌手必须填写requiredArtist并启用artistOnly，不能只作为搜索词或风格。")
        # 仅首次澄清适配器可传入已核验来源的数量；普通任务仍只看当前原文。
        pending_count = context.get("_pendingTrackCount")
        requested_count, count_issue = pending_count if isinstance(pending_count, tuple) and len(pending_count) == 2 else (trackCountRequest(context.get("instruction", "")), trackCountIssue(context.get("instruction", "")))
        if count_issue:
            raise ApiError(422, "INVALID_TASK_MODEL", "本轮曲数包含上下界、近似、区间、冲突或非直接要求；当前只支持精确1至30首，必须先澄清，不能沿用旧值或取其中端点。")
        if requested_count is not None and not 1 <= requested_count <= 30:
            raise ApiError(422, "INVALID_TASK_MODEL", "目标曲数支持1至30首；请先澄清，不能截断或丢掉用户数量。")
        if requested_count is not None and value.get("targetTrackCount", 0) != requested_count:
            raise ApiError(422, "INVALID_TASK_MODEL", "用户要求的总首数必须填入targetTrackCount；retainFirst只保留已有预览前缀，不能代替推荐曲数。")
        if value.get("targetTrackCount") and requested_count is None and value["targetTrackCount"] != (context.get("currentBrief") or {}).get("targetTrackCount", 0):
            raise ApiError(422, "INVALID_TASK_MODEL", "新增目标曲数缺少当前直接精确数量依据；否定、引用和条件不能授权数量修改。")
        if not (context.get("preview") or context.get("currentBrief")) and value["retainFirst"]:
            raise ApiError(422, "INVALID_TASK_MODEL", "尚无已有预览，不能retainFirst锁定不存在的歌曲；新歌单数量请使用targetTrackCount。")
        intent = releaseIntentSignal(
            clean(context.get("instruction")),
            artist=value.get("artist") or value.get("requiredArtist"),
        )["kind"]
        if (
            value["requireReleases"]
            and intent == "ordinary"
            and not (context.get("currentBrief") or {}).get("requireReleases")
        ):
            raise ApiError(
                422,
                "INVALID_TASK_MODEL",
                "本轮明确要求普通歌单，不能改成新作查询；指定歌手使用 requiredArtist/artistOnly。",
            )
        if (
            value["requireReleases"]
            and intent == "cancelled"
            or not value["requireReleases"]
            and intent == "requested"
        ):
            raise ApiError(
                422,
                "INVALID_TASK_MODEL",
                "requireReleases 与本轮明确的新作查询或取消要求不一致。",
            )
    current = (context or {}).get("currentBrief") or {}
    authorized = validId(value.get("artistId")) and (
        current.get("artistId") == value["artistId"]
        and current.get("artist") == value.get("artist")
        or any(
            a["id"] == value["artistId"] and a["name"] == value.get("artist")
            for a in (context or {}).get("providedArtists", [])
        )
    )
    out = {
        "kind": value["kind"],
        "title": clean(value["title"], 80),
        "scene": clean(value.get("scene"), 160),
        "styles": [clean(s, 40) for s in value["styles"]],
        "queries": list(dict.fromkeys(clean(q, 80) for q in value["queries"])),
        "artist": clean(value.get("artist"), 100) if value["requireReleases"] else "",
        "requiredArtist": clean(value.get("requiredArtist"), 100),
        "artistOnly": value.get("artistOnly") is True,
        "since": since,
        "until": until,
        "targetMinutes": value["targetMinutes"],
        "excludeLive": value["excludeLive"],
        "avoidPrevious": value.get("avoidPrevious") is True,
        "excludedArtists": [clean(a, 100) for a in value["excludedArtists"]],
        "requireReleases": value["requireReleases"],
        "retainFirst": value["retainFirst"],
        "removeIds": value["removeIds"],
        "question": clean(value.get("question"), 300),
        "adjustment": clean(value.get("adjustment"), 160),
    }
    from .music_request import canonicalScene
    out["scene"] = canonicalScene(out["scene"])
    # 旧检查点缺字段表示未限定曲数，不改写历史快照或冻结迁移向量。
    if "targetTrackCount" in value:
        out["targetTrackCount"] = value["targetTrackCount"]
    if authorized:
        out["artistId"] = value["artistId"]
    # 普通歌手ID只由已核验设备回执产生；模型输出不能新授予或更换身份。
    if validId(current.get("requiredArtistId")) and current.get("requiredArtist") == out["requiredArtist"]:
        out["requiredArtistId"] = current["requiredArtistId"]
    return out


def fixed1(value):
    # 对齐 JS toFixed：按实际二进制浮点值取最近值，正中点向上取。
    return str(
        Decimal.from_float(float(value)).quantize(
            Decimal("0.1"), rounding=ROUND_HALF_UP
        )
    )


_LIVE = re.compile(r"(?:\bLive\b|现场|演唱会|音乐会|巡演|concert)", re.I | re.ASCII)


def artistCreditMatches(track, name, artistId=0):
    """已核验身份时按真实署名ID匹配，旧无ID检查点继续使用严格显示名。"""
    if validId(artistId):
        return any(a.get("id") == artistId for a in track.get("artists", []) if object(a)) or track.get("artistId") == artistId
    canonical = lambda v: re.sub(
        r"[\s.·]", "", unicodedata.normalize("NFKC", clean(v, 160))
    ).lower()
    expected = canonical(name)
    return (
        bool(expected)
        and isinstance(track.get("artist"), str)
        and any(
            canonical(c) == expected
            for c in re.split(
                r"\s*(?:/|、|,|，|&|;|；| feat\.? | featuring )\s*",
                track["artist"],
                flags=re.I,
            )
        )
    )


def previewLocksCheck(tracks, lockedIds, originalTracks, *, requirePrefix=False):
    """按实际曲目核验锁定存在及顺序；重新编排还必须保留锁定前缀。"""
    expected = [track["id"] for track in originalTracks if track["id"] in lockedIds]
    actual = [track["id"] for track in tracks if track["id"] in lockedIds]
    valid = (
        len(set(lockedIds)) == len(lockedIds)
        and len(expected) == len(lockedIds)
        and len(set(expected)) == len(expected)
        and actual == expected
        and (not requirePrefix or [track["id"] for track in tracks[:len(expected)]] == expected)
    )
    return {
        "key": "locks", "status": "satisfied" if valid else "unmet",
        "text": f"保留 {len(lockedIds)} 首锁定歌曲及顺序。",
    }


def refreshPreviewLocksCheck(task):
    """锁定或撤回时只刷新确定性锁检查；不重排曲目、不重写其他核验结论。"""
    preview = task["preview"]
    check = previewLocksCheck(preview["tracks"], task["lockedIds"], preview["tracks"])
    check["detail"] = check["text"]
    preview["lockedIds"] = list(task["lockedIds"])
    checks = preview.get("checks", [])
    preview["checks"] = [check if value.get("key") == "locks" else value for value in checks]
    if not any(value.get("key") == "locks" for value in checks):
        preview["checks"].append(check)


def playlistSceneEvidence(track, sources, scene):
    """只依据已读取且对应曲目的来源名称或标签，不把场景文字当成音频事实。"""
    def supports(source):
        text = " ".join([source.get("name", ""), *source.get("tags", [])])
        denied = re.search(r"(?:不适合|不适用|不宜|非|不是|不要用于).{0,8}" + re.escape(scene)
                           + r"|" + re.escape(scene) + r".{0,8}(?:不适合|不适用|不宜|禁止)", text)
        return scene in text and not denied
    matched = any(
        source.get("sourceKind") == "playlist"
        and source.get("id") == track.get("sourceId")
        and source.get("name") == track.get("sourceName")
        and supports(source)
        for source in sources or []
    ) if scene else False
    return {"status": "source_matched" if matched else "unverified", "matchedScenes": [scene] if matched else [],
            "basis": "playlist_name_or_tags" if matched else "none"}


def assemblePreview(task, rankedIds=None, now=None):
    rankedIds = rankedIds or []
    now = now_ms() if now is None else now
    brief = task["brief"]
    requested = adjustmentStyles(brief, task.get("previousBrief"))
    sources = [
        s
        for b in task["branches"].values()
        for s in (b.get("result") or {}).get("sources", [])
    ]
    locked = [
        t
        for t in (task.get("preview") or {}).get("tracks", [])
        if t["id"] in task["lockedIds"]
    ]

    def hard(t):
        return (
            t["id"] not in task["excludedIds"]
            and t["id"] not in task.get("currentAvoidIds", [])
            and t["id"] not in brief["removeIds"]
            and (not t.get("publishedAt") or date_ms(t["publishedAt"]) <= now)
            and (
                not brief["excludeLive"]
                or not _LIVE.search(t["title"] + " " + t.get("album", ""))
            )
            and (
                not brief["artistOnly"]
                or artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0))
            )
            and not catalogLanguageConflict(t, sources, brief.get("catalogLanguage"))
            and not any(
                a.lower() in t.get("artist", "").lower()
                for a in brief["excludedArtists"]
            )
        )

    def valid_release(t):
        published = date_ms(t.get("publishedAt", ""))
        expected = re.sub(r"[\s.·]", "", brief["artist"]).lower()
        actual = re.sub(r"[\s.·]", "", t.get("artist", "")).lower()
        return (
            math.isfinite(published)
            and published <= now
            and (not brief["since"] or published >= date_ms(brief["since"]))
            and (not brief["until"] or published <= date_ms(brief["until"]))
            and (
                t.get("artistId") == brief["artistId"]
                if brief.get("artistId")
                else not expected or expected in actual
            )
        )

    all_tracks = {}
    for branch in task["branches"].values():
        for track in (branch.get("result") or {}).get("tracks", []):
            if branch["skill"] == "releases.research" and not valid_release(track):
                continue
            evidence = playlistStyleEvidence(
                track,
                (branch.get("result") or {}).get("sources"),
                requested or brief["styles"],
            )
            prior = all_tracks.get(track["id"]) or {}
            if (
                hard(track)
                and not (
                    prior.get("sourceSkill") == "releases.research"
                    and branch["skill"] != "releases.research"
                )
                and not (
                    (prior.get("styleEvidence") or {}).get("status") == "source_matched"
                    and evidence["status"] != "source_matched"
                    and branch["skill"] != "releases.research"
                )
            ):
                all_tracks[track["id"]] = {
                    **track,
                    "styleEvidence": evidence,
                    **({"sceneEvidence": playlistSceneEvidence(track, (branch.get("result") or {}).get("sources"), brief.get("scene", ""))}
                       if (task.get("requirementPlan") or {}).get("scene") else {}),
                    "branch": branch["skill"],
                    "sourceSkill": branch["skill"],
                }
    ordered = [
        all_tracks[i]
        for i in dict.fromkeys([*rankedIds, *all_tracks])
        if i in all_tracks
    ]
    tracks = [dict(t) for t in locked]
    used = {t["id"] for t in tracks}
    target = brief["targetMinutes"] * 60000
    target_count = brief.get("targetTrackCount", 0)
    release = lambda t: (
        (t.get("branch") or t.get("sourceSkill")) == "releases.research"
        and valid_release(t)
    )
    required = (
        1 if brief["requireReleases"] and not any(release(t) for t in tracks) else 0
    ) | (
        2
        if brief["requiredArtist"]
        and not any(artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0)) for t in tracks)
        else 0
    )
    pool = [t for t in ordered if t["id"] not in used][:58]
    for matches in (release, lambda t: artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0))):
        if not any(matches(t) for t in pool):
            candidate = next(
                (t for t in ordered if t["id"] not in used and matches(t)), None
            )
            if candidate:
                pool.append(candidate)
    for t in ordered:
        if (
            len(pool) < 60
            and t["id"] not in used
            and not any(p["id"] == t["id"] for p in pool)
        ):
            pool.append(t)
    optional = [
        {
            **t,
            "styleSupport": t["styleEvidence"]["status"] == "source_matched"
            or bool((task.get("requirementPlan") or {}).get("scene") and (t.get("sceneEvidence") or {}).get("status") == "source_matched"),
            "flags": int(release(t))
            | (2 if artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0)) else 0),
        }
        for t in pool
    ]
    if target_count:
        selection = selectDurationSubset(
            optional,
            {
                "targetMs": target,
                "baseMs": sum(t["durationMs"] for t in tracks),
                "exactCount": max(0, target_count - len(tracks)),
                "requiredFlags": required,
                "preferenceWithinTolerance": bool(task.get("preview") and requested),
            },
        )
    elif target and optional and all(t["durationMs"] == 0 for t in optional):
        selection = {
            "indices": list(range(min(len(optional), max(0, 12 - len(tracks))))),
            "algorithm": "catalog_order_unknown_duration",
            "quantizationMs": 0,
            "maximumQuantizationSpreadMs": 0,
        }
    elif target:
        selection = selectDurationSubset(
            optional,
            {
                "targetMs": target,
                "baseMs": sum(t["durationMs"] for t in tracks),
                "maximumTracks": max(0, 30 - len(tracks)),
                "minimumTracks": 0 if tracks else 1,
                "requiredFlags": required,
                "preferenceWithinTolerance": bool(task.get("preview") and requested),
            },
        )
    else:
        selected = set()
        evidence = 0
        for i, t in enumerate(optional):
            if t["flags"] & required & ~evidence:
                selected.add(i)
                evidence |= t["flags"]
        for i in range(len(optional)):
            if len(selected) >= max(0, 12 - len(tracks)):
                break
            selected.add(i)
        selection = {
            "indices": sorted(selected)[: max(0, 30 - len(tracks))],
            "algorithm": "catalog_order",
            "quantizationMs": 0,
            "maximumQuantizationSpreadMs": 0,
        }
    tracks.extend(
        {k: v for k, v in optional[i].items() if k not in ("flags", "styleSupport")}
        for i in selection["indices"]
    )
    total = sum(t["durationMs"] for t in tracks)
    known = all(t["durationMs"] > 0 for t in tracks)
    checks = [
        {
            "key": "real_ids",
            "status": "satisfied",
            "text": "曲目均来自本机曲库查询，未编造歌曲。",
        },
        previewLocksCheck(tracks, task["lockedIds"], (task.get("preview") or {}).get("tracks", []), requirePrefix=True),
        {
            "key": "duration",
            "status": "not_requested"
            if not target
            else "unverified"
            if not known
            else "satisfied"
            if abs(total - target) <= max(60000, target * 0.1)
            else "unmet",
            "text": f"目标约 {brief['targetMinutes']} 分钟，当前 {fixed1(total / 60000)} 分钟。"
            if target
            else "未设置目标时长。",
        },
        {
            "key": "exclude_live",
            "status": "not_requested"
            if not brief["excludeLive"]
            else "unmet"
            if any(_LIVE.search(t["title"] + " " + t.get("album", "")) for t in tracks)
            else "unverified",
            "text": "已排除有现场版标识的候选；标签缺失的版本无法核验。"
            if brief["excludeLive"]
            else "未要求排除现场版。",
        },
        {
            "key": "excluded_ids",
            "status": "unmet"
            if any(
                t["id"] in task["excludedIds"]
                or t["id"] in task.get("currentAvoidIds", [])
                or t["id"] in brief["removeIds"]
                for t in tracks
            )
            else "satisfied",
            "text": "检查已排除歌曲、上一批未锁定歌曲与指定移除的歌曲。",
        },
        {
            "key": "excluded_artists",
            "status": "unmet"
            if any(
                any(
                    a.lower() in t.get("artist", "").lower()
                    for a in brief["excludedArtists"]
                )
                for t in tracks
            )
            else "satisfied",
            "text": "按曲库歌手字段检查明确排除的歌手。",
        },
        {
            "key": "required_artist",
            "status": "not_requested"
            if not brief["requiredArtist"]
            else "satisfied"
            if tracks
            and any(artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0)) for t in tracks)
            and (
                not brief["artistOnly"]
                or all(artistCreditMatches(t, brief["requiredArtist"], brief.get("requiredArtistId", 0)) for t in tracks)
            )
            else "unverified"
            if tracks and all(not t.get("artist") for t in tracks)
            else "unmet",
            "text": (
                "每首歌曲的歌手署名须包含"
                if brief["artistOnly"]
                else "歌单须包含署名歌手"
            )
            + f"「{brief['requiredArtist']}」；仅核对曲库署名，不按歌名或翻唱标签推断。"
            if brief["requiredArtist"]
            else "未指定必须包含的歌手。",
        },
        {
            "key": "releases",
            "status": "not_requested"
            if not brief["requireReleases"]
            else "satisfied"
            if any(release(t) for t in tracks)
            else "unmet",
            "text": "新作仅依据曲库中的发行日期与歌手归属；候选不足会保留缺额。"
            if brief["requireReleases"]
            else "未要求近期发行。",
        },
        {
            "key": "style",
            "status": "unverified"
            if brief["styles"] or brief["scene"]
            else "not_requested",
            "text": "风格与场景依据来源标签和文本线索，未进行音频分析。",
        },
        {
            "key": "catalog_language",
            "status": (
                "unmet"
                if any(
                    catalogLanguageConflict(t, sources, brief.get("catalogLanguage"))
                    for t in tracks
                )
                else "unverified"
            )
            if brief.get("catalogLanguage") in ("zh", "en")
            else "not_requested",
            "text": "保留华语要求，排除来源明确标为英文的候选；未做音频语种识别。"
            if brief.get("catalogLanguage") == "zh"
            else "保留英文要求，排除来源明确标为华语的候选；未做音频语种识别。"
            if brief.get("catalogLanguage") == "en"
            else "未限定曲库语言来源。",
        },
    ]
    if (task.get("requirementPlan") or {}).get("scene"):
        count = sum((track.get("sceneEvidence") or {}).get("status") == "source_matched" for track in tracks)
        checks.append({"key": "scene", "status": "satisfied" if tracks and count == len(tracks) else "unverified",
                       "text": f"{len(tracks)}首中{count}首命中「{brief['scene']}」来源名称或标签；其余仅核验歌手与时长，未分析音频。"})
    if target_count:
        checks.append({
            "key": "track_count",
            "status": "satisfied" if len(tracks) == target_count else "unmet",
            "text": f"目标 {target_count} 首，当前 {len(tracks)} 首；候选不足或锁定冲突不会通过重复、造曲或丢锁补足。",
        })
    return {
        "version": (task.get("preview") or {}).get("version", 0) + 1,
        "title": brief["title"],
        "note": "基于真实曲库候选与来源主题编排，风格为软偏好；完整播放需重新检查账号权益。",
        "tracks": [
            {
                **{k: v for k, v in t.items() if k != "branch"},
                "reason": f"来自「{t['sourceName']}」，"
                + (t.get("artist", "") + " · " if t.get("artist") else "")
                + t["title"]
                + "。",
            }
            for t in tracks
        ],
        "lockedIds": list(task["lockedIds"]),
        "totalDurationMs": total,
        "checks": checks,
        "durationSelection": {
            "algorithm": selection["algorithm"],
            "quantizationMs": selection["quantizationMs"],
            "maximumQuantizationSpreadMs": selection["maximumQuantizationSpreadMs"],
            "selectionPolicy": selection.get("selectionPolicy") or "catalog_order",
            "toleranceMs": selection.get("toleranceMs") or 0,
            **({"maximumStatesPerCountAndEvidence": selection["maximumStatesPerCountAndEvidence"]} if "maximumStatesPerCountAndEvidence" in selection else {}),
        },
    }
