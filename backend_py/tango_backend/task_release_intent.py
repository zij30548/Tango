"""保守的发行意图矛盾检查；未知表达不冒充完整语义理解。"""

import re

_RELEASE = re.compile(
    r"(?:新歌|新作|新专辑|发行|发布|推出|\breleases?\b|\bnew\s+(?:album|single|song)s?\b)",
    re.I,
)
_REQUEST = re.compile(
    r"(?:查|找|搜|看看|看下|有哪些|有什么|有没有|是否|整理|列出|盘点|加入|添加|推荐|收录|想听|想了解|\b(?:find|search|show|list|recommend)\b)",
    re.I,
)
_TIME = re.compile(
    r"(?:最近|近期|最新|近来|这周|本周|本月|今年|去年|过去|近[一二三四五六七八九十百两半\d]|\d{4}[-年/]\d|\b(?:recent|latest|since|between)\b)",
    re.I,
)
_RELEASE_OBJECT = r"(?:新歌|新作|新专辑|新发行|近期发行|发行(?:的)?(?:作品|歌曲|音乐|专辑|目录|查询)?|发布(?:的)?(?:作品|歌曲|音乐|专辑|目录|查询)?)"
_CANCEL = re.compile(
    r"(?:不要|不用|不再|不必|无需|别|不需要|不想|不想要|不喜欢|不爱|不打算|不是要|并非要|不是想|并非想)(?:(?:再|去|继续|帮我|给我|替我|查|查询|查找|找|搜索|搜|加入|添加|推荐|听|看|安排|关注|追踪|优先|只找|只听)\s*)*(?:(?:最新|最近|近期)(?:的)?)?"
    + _RELEASE_OBJECT
    + r"|(?:取消|撤销|解除|去掉|停止)(?:(?:再|继续|这个|这些|之前|刚才|的)\s*)*"
    + _RELEASE_OBJECT
    + "|"
    + _RELEASE_OBJECT
    + r"(?:查询|要求)?(?:先|暂时)?(?:不用|不必|无需|不再)(?:再)?(?:查|找|搜|研究|推荐)(?:了|啦|吧|$)"
)
_NEGATED = re.compile(
    r"(?:不是不|并非不|并不是不|不要(?:再)?取消|别(?:再)?取消|不要停止|别停止|不应取消|不该取消|不要去掉|别去掉|不要撤销|别撤销)"
)


def releaseIntentSignal(instruction, options=None, *, artist=""):
    artist = (options or {}).get("artist", artist)
    text = instruction if isinstance(instruction, str) else ""
    text = re.sub(r"```[\s\S]*?(?:```|$)", " ", text)
    text = re.sub(
        r"""“[^”]*(?:”|$)|「[^」]*(?:」|$)|『[^』]*(?:』|$)|《[^》]*(?:》|$)|"(?:\\.|[^"\\])*(?:"|$)|'(?:\\.|[^'\\])*(?:'|$)""",
        " ",
        text,
    )
    requested = cancelled = False
    for clause in filter(
        None,
        (
            s.strip()
            for s in re.split(r"[，,。；;！？!?\n]|(?:不过|但是|可是|而是)", text)
        ),
    ):
        if not _RELEASE.search(clause) or _NEGATED.search(clause):
            continue
        if _CANCEL.search(clause):
            cancelled = True
        elif _REQUEST.search(clause) or _TIME.search(clause):
            requested = True
    if requested and cancelled:
        return {"kind": "unknown", "reason": "mixed_release_instructions"}
    if cancelled:
        return {"kind": "cancelled", "reason": "explicit_release_cancellation"}
    if requested:
        return {"kind": "requested", "reason": "explicit_release_request"}
    if _RELEASE.search(text):
        return {"kind": "unknown", "reason": "release_word_without_clear_action"}
    name = (
        re.escape(artist.strip()) if isinstance(artist, str) and artist.strip() else ""
    )
    ordinary = bool(
        name
        and re.search(
            r"(?:安排|推荐|放|听|选|来|整理|给我|帮我|用).{0,12}"
            + name
            + r"(?:的)?(?:歌曲|歌|音乐|经典|老歌)|"
            + name
            + r"(?:的)?(?:歌曲|歌|音乐)(?:推荐|歌单|精选)",
            text,
        )
    )
    ordinary = ordinary or bool(
        re.search(r"(?:普通|常规|日常|经典|老歌)(?:的)?(?:歌单|歌曲|音乐)", text)
    )
    return (
        {"kind": "ordinary", "reason": "explicit_ordinary_playlist"}
        if ordinary
        else {"kind": "unknown", "reason": "outside_conservative_phrase_coverage"}
    )
