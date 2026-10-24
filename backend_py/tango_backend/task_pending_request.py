"""首次需求澄清续接：只接受运行时绑定的完整、顺序不变的用户来源。"""

import re
import unicodedata
from .compat import safe_integer, utf16_len
from .errors import ApiError
from .task_schema import clean, identifier, object, validateBrief
from .task_release_intent import releaseIntentSignal
from .task_track_count import pendingTrackCount

_PENDING = {
    "schemaVersion",
    "id",
    "taskId",
    "rootRevision",
    "revision",
    "status",
    "question",
    "sources",
}
_SOURCE = {"messageId", "role", "content", "revision"}


def _invalid(reason):
    raise ApiError(
        422,
        "INVALID_TASK_PENDING_CONTEXT",
        f"待补充任务上下文无效：{reason}。请重新发起任务。",
    )


def _proposal(reason):
    raise ApiError(422, "INVALID_TASK_MODEL", f"AI 的澄清续接无效：{reason}。")


def _name(value):
    return (
        re.sub(r"[\s.·]", "", unicodedata.normalize("NFKC", value)).lower()
        if isinstance(value, str)
        else ""
    )


def _integer(value):
    return safe_integer(value) and value >= 1


def validatePendingRequest(pending, context):
    context = context or {}
    if (
        not object(pending)
        or set(pending) - _PENDING
        or (
            not safe_integer(pending.get("schemaVersion"))
            or pending.get("schemaVersion") != 1
        )
        or not identifier(pending.get("id"))
        or not identifier(pending.get("taskId"))
        or pending["taskId"] != context.get("taskId")
        or not _integer(pending.get("rootRevision"))
        or not _integer(pending.get("revision"))
        or (
            not safe_integer(context.get("revision"))
            or pending["revision"] != context.get("revision")
        )
        or pending["rootRevision"] > pending["revision"]
        or pending.get("status") != "awaiting_answer"
        or not isinstance(pending.get("question"), str)
        or not pending["question"].strip()
        or utf16_len(pending["question"]) > 300
        or not isinstance(pending.get("sources"), list)
        or not 1 <= len(pending["sources"]) <= 6
        or not isinstance(context.get("messages"), list)
    ):
        _invalid("任务、版本或来源结构不匹配")
    ids = set()
    previous_index = -1
    previous_revision = total = 0
    sources = []
    for source in pending["sources"]:
        if (
            not object(source)
            or set(source) - _SOURCE
            or not identifier(source.get("messageId"))
            or source["messageId"] in ids
            or source.get("role") != "user"
            or not isinstance(source.get("content"), str)
            or not source["content"].strip()
            or utf16_len(source["content"]) > 1000
            or not _integer(source.get("revision"))
            or source["revision"] < pending["rootRevision"]
            or source["revision"] > pending["revision"]
            or source["revision"] < previous_revision
        ):
            _invalid("来源必须是唯一、有界的原始用户消息")
        matches = [
            (i, m)
            for i, m in enumerate(context["messages"])
            if isinstance(m, dict) and m.get("messageId") == source["messageId"]
        ]
        if (
            len(matches) != 1
            or matches[0][0] <= previous_index
            or matches[0][1].get("role") != "user"
            or matches[0][1].get("content") != source["content"]
            or (
                not safe_integer(matches[0][1].get("revision"))
                or matches[0][1].get("revision") != source["revision"]
            )
        ):
            _invalid("来源编号、内容或顺序不属于当前任务的用户记录")
        previous_index, previous_revision = matches[0][0], source["revision"]
        ids.add(source["messageId"])
        total += utf16_len(source["content"])
        sources.append(
            {k: source[k] for k in ("messageId", "role", "content", "revision")}
        )
    if total > 4000 or sources[0]["revision"] != pending["rootRevision"]:
        _invalid("来源总量或初始版本无效")
    users = [
        m
        for m in context["messages"]
        if isinstance(m, dict) and m.get("role") == "user"
    ]
    if (
        not users
        or sources[0]["messageId"] != users[0].get("messageId")
        or pending["rootRevision"] != users[0].get("revision")
        or len(users) != len(sources)
        or any(
            s["messageId"] != users[i].get("messageId") for i, s in enumerate(sources)
        )
    ):
        _invalid("不能跳过首次需求或中间的用户纠正、撤销回答")
    if sources[-1]["messageId"] != users[-1].get("messageId") or sources[-1][
        "content"
    ] != context.get("instruction"):
        _invalid("仅能续接当前未解决问题的最新用户回答")
    return {
        "schemaVersion": 1,
        "id": pending["id"],
        "taskId": pending["taskId"],
        "rootRevision": pending["rootRevision"],
        "revision": pending["revision"],
        "status": "awaiting_answer",
        "question": clean(pending["question"], 300),
        "sources": sources,
    }


def pendingActionText(value):
    if not isinstance(value, str):
        _invalid("来源正文必须为文本")
    value = re.sub(r"```[\s\S]*?(?:```|$)", lambda m: " " * len(m.group()), value)
    pairs = {'"': '"', "'": "'", "“": "”", "「": "」", "『": "』", "《": "》", "`": "`"}
    closing = None
    escaped = False
    result = []
    for i, character in enumerate(value):
        if closing:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == closing:
                closing = None
            result.append(" ")
            continue
        if (
            character == "'"
            and i > 0
            and i + 1 < len(value)
            and re.fullmatch("[a-z]", value[i - 1], re.I)
            and re.fullmatch("[a-z]", value[i + 1], re.I)
        ):
            result.append(character)
            continue
        if character in pairs:
            closing = pairs[character]
            result.append(" ")
        else:
            result.append(character)
    return "".join(result).strip()


def preparePendingContinuation(value):
    if not value or value.get("pendingRequest") is None:
        return None
    if (
        value.get("hasConfirmedBrief") is not False
        or (value.get("currentBrief") or {}).get("kind") != "clarify"
        or not isinstance(value.get("preview"), list)
        or value["preview"]
        or not isinstance(value.get("lockedIds"), list)
        or value["lockedIds"]
    ):
        _invalid("已确认任务、已有预览或锁定不能转为首次需求续接")
    pending = validatePendingRequest(value["pendingRequest"], value)
    if len(pending["sources"]) < 2:
        _invalid("尚未收到当前问题的用户回答")
    sources = [
        {**s, "content": pendingActionText(s["content"])} for s in pending["sources"]
    ]
    provider = {
        "instruction": sources[-1]["content"],
        "currentBrief": None,
        "pendingRequestId": pending["id"],
        "pendingSources": sources,
        "pendingQuestion": pending["question"],
        "preference": clean(value.get("preference"), 500),
        "preview": [],
        "providedArtists": [],
    }
    if value.get("validationFeedback"):
        provider["validationFeedback"] = {
            "invalidReason": clean(
                value["validationFeedback"].get("invalidReason"), 500
            ),
            "instruction": "只纠正上一份完整需求；仍只依据pendingSources，最新用户纠正和撤销优先。",
        }
    return {
        "providerInput": provider,
        "validationContext": {
            "instruction": value.get("instruction"),
            "currentBrief": None,
            "providedArtists": [],
        },
        "pendingSources": sources,
    }


def validatePendingBrief(value, prepared):
    # prepared来源已绑定真实消息ID和revision；只为数量恢复构建内部校验上下文。
    count = pendingTrackCount([s["content"] for s in prepared["pendingSources"]], prepared["providerInput"]["pendingQuestion"])
    brief = validateBrief(value, {**prepared["validationContext"], "_pendingTrackCount": count})
    if brief["kind"] == "clarify":
        return brief
    if brief["retainFirst"] or brief["removeIds"]:
        _proposal("没有已确认预览，不能锁定或移除曲目")
    texts = [s["content"] for s in prepared["pendingSources"]]
    source_text = "\n".join(map(_name, texts))
    for field in ("artist", "requiredArtist"):
        if brief[field] and _name(brief[field]) not in source_text:
            _proposal(
                f"{field} 必须来自当前未完成任务中未引用的用户姓名；不能从助手、工具或模型记忆猜测"
            )
    if not any(
        re.search(
            r"(?:歌|音乐|音樂|曲|专辑|專輯|发行|發行|发布|發布|推出|上架|上线|\bmusic\b|\bsongs?\b|\balbums?\b|\breleases?\b)",
            t,
            re.I | re.ASCII,
        )
        for t in texts
    ):
        _proposal("没有未引用的音乐任务原话，请先澄清用户实际要执行的需求")
    signals = [releaseIntentSignal(t) for t in texts]
    known = [s for s in signals if s["kind"] != "unknown"]
    kind = known[-1]["kind"] if known else None
    if kind == "requested" and not brief["requireReleases"]:
        _proposal(
            "回答缺失歌手不能丢弃原始发行要求；取消或改为普通歌曲需依据本轮明确要求"
        )
    if kind in ("cancelled", "ordinary") and brief["requireReleases"]:
        _proposal("最新明确取消或改为普通歌曲后，不能从较早消息或引用恢复发行查询")
    return brief
