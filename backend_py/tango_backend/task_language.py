"""跨轮保留明确语言要求；来源标签不等于音频语种识别。"""

import re
import unicodedata
from .compat import utf16_slice, safe_integer

_VALUES = {"zh", "en", "any", "unknown"}
_END = r"[，,。!！？?；;\n]|(?<!\d)\.|\.(?!\d)"
_EXPRESSIONS = {
    "zh": r"(?:中文|华语|華語|華语|华語|国语|國語|國语|普通话|汉语|\bmandarin\b|\bchinese\b)",
    "en": r"(?:英文|英语|英語|\benglish\b)",
}
_ACTIONS = "夹杂|夹带|混入|混进|混杂|掺入|掺杂|掺进|穿插|插入|加入|加进|添加|出现|包含|带上|播放|推荐|放|推|来|要|听|选|夹|混|掺|插|加|带"
_NUMERAL = r"(?:\d+(?:\.\d+)?|[零〇一二三四五六七八九十百千万两半]+)"
_QUANTITY = rf"(?:(?:哪怕|即便|连)?(?:大约|大概|差不多|总共|约)?{_NUMERAL}\s*(?:个\s*)?(?:分钟|小时|分|首|曲|段)(?:的)?)"
_TEMPORAL = "(?:暂时|接下来|现在|目前|暂且|这次|先)"
_BRIDGE = rf"(?:任何|全部|所有|再|那么|过多|更多|太多|少量|半点|的|给我|帮我|一点|一些|点|些|进来|进去|(?:哪怕|即便)?[一二三四五六七八九十两\d]+(?:点|首|段|句|些|种)|在(?:歌单|歌曲|音乐|中间|其中)(?:里面|里|中)?|往(?:歌单|歌曲|音乐)(?:里|中)?|让(?:歌单|歌曲|音乐)?|{_ACTIONS})"
_NEGATIVE = rf"(?:不要|不想(?:要|听)?|不喜欢|不需要|不是|并非|不能|不许|不准|不必|无需|排除|去掉|拒绝|避免|别|非|不|\bno\b|\bnot\b|\bwithout\b|\bexclude\b)\s*(?:{_BRIDGE}\s*)*$"
_NEGATIVE_AFTER = rf"^\s*(?:歌(?:曲|单)?|音乐)?\s*(?:都|也)?(?:不要|不用|排除|取消|别|不能|不许|不准|不必|无需|不(?:再)?(?:{_ACTIONS}|需要|喜欢|想(?:听|放|要)?))"
_CORRECTED = r"(?:不是|并非|并不是|没有说|没说|不是说|并不是说)\s*(?:不要|不想要|不想听|不听|不喜欢|不需要|不放|不混|不夹|不掺)\s*$|(?:别|不要|不能|不该|不想)\s*不(?:再)?(?:放|听|要|推荐|混入|夹入)?\s*$|\bnot\s+(?:without|excluding)\s*$"
_PERMISSION_BEFORE = r"(?:不再排除|不要排除|别排除|不排除|不用排除|不必排除|取消排除|撤销排除|允许(?:加入|混入|播放|听)?|也(?:可以|能)(?:加入|混入|播放|听)?)\s*$"
_PERMISSION_AFTER = (
    r"^\s*(?:歌(?:曲|单)?|音乐)?\s*(?:也可以|也行|也能听|可以加入|可以混入|可以放)"
)
_SWITCHED = r"(?:改成|改为|换成|换为|切换到|切到|改听|换听|变成|这次(?:要|听|用)|现在(?:要|听)|\bswitch to)\s*(?:(?:只听|只要|全是|全|一些|一点|更多|点)\s*)*$"


def _has(pattern, text):
    return bool(re.search(pattern, text, re.I | re.ASCII))


def _normalize(value):
    return (
        unicodedata.normalize("NFKC", value).lower().strip()
        if isinstance(value, str)
        else ""
    )


def _grammar(value):
    value = re.sub(_QUANTITY, "", value)
    return re.sub(
        _TEMPORAL
        + rf"(?=\s*(?:{_TEMPORAL}|{_QUANTITY}|不|别|只|仅|全|都|也|再|仍|还|改|换|切|{_ACTIONS}|$))",
        "",
        value,
    )


def _conversation(text):
    text = re.sub(r"```[\s\S]*?(?:```|$)", lambda m: " " * len(m.group()), text)

    def mask(m):
        content = next((g for g in m.groups() if g is not None), "")
        before = text[max(0, m.start() - 25) : m.start()]
        label = bool(
            re.fullmatch(
                r"(?:中文|华语|華語|国语|國語|英文|英语|英語|english|chinese|mandarin)(?:歌|歌曲|音乐|歌单)?",
                content.strip(),
            )
        )
        request = _has(r"(?:给我|来点|想听|只听|听|选|放|要)\s*$", _grammar(before))
        return (
            content
            if not m.group().startswith("《") and label and request
            else " " * len(m.group())
        )

    return re.sub(
        r"“([^”]*)”|「([^」]*)」|『([^』]*)』|《([^》]*)》|\"([^\"\n]*)\"|'([^'\n]*)'",
        mask,
        text,
    )


def _non_music(before, after):
    if _has(
        r"^\s*(?:回答|回复|交流|显示|输出|说明|讲解|沟通|介绍|翻译|字幕|界面|菜单|电影|书籍|小说|教材|文章|文档|网页|网站|软件|课程|新闻|台词|对白)",
        after,
    ):
        return True
    if _has(
        r"(?:学|学习|学好|练|练习|练好|复习|教|说|讲|读|写|背)\s*$", before
    ) and not _has(r"^\s*(?:歌|歌曲|音乐|歌单)", after):
        return True
    if _has(r"^\s*(?:学习|考试|作业|单词|语法|口语|作文|阅读理解)", after) and not _has(
        r"^\s*(?:学习|考试|作业).*?(?:歌单|歌曲|音乐)", after
    ):
        return True
    return _has(
        r"(?:歌词|歌名|标题|曲名|这首歌|歌曲)(?:里|中|叫|是|包含|提到|出现|写着)\s*$",
        before,
    )


def _mentions(text, conversation=False):
    positive, excluded, exclusive = set(), set(), set()
    switches, permissions = [], []
    aggregate = (
        [
            (m.start(), m.end())
            for m in re.finditer(r"(?:中英文|中英双语|双语|中英混合)", text)
        ]
        if conversation
        else []
    )
    for language, expression in _EXPRESSIONS.items():
        for match in re.finditer(expression, text, re.ASCII):
            start, end = match.span()
            if any(left <= start and end <= right for left, right in aggregate):
                continue
            raw_before = re.split(_END, text[:start])[-1][-80:]
            raw_after = re.split(_END, text[end:])[0][:80]
            before, after = (
                (_grammar(raw_before), _grammar(raw_after))
                if conversation
                else (raw_before, raw_after)
            )
            if conversation and _non_music(before, after) or _has(_CORRECTED, before):
                continue
            if conversation and (
                _has(_PERMISSION_BEFORE, before) or _has(_PERMISSION_AFTER, after)
            ):
                permissions.append({"language": language, "start": start})
                continue
            next_language = re.search(
                r"中文|华语|華語|国语|國語|普通话|汉语|英文|英语|英語|\b(?:english|chinese|mandarin)\b",
                after,
                re.ASCII,
            )
            targets_next = bool(
                next_language and _has(_NEGATIVE, after[: next_language.start()])
            )
            negated = (
                _has(_NEGATIVE, before)
                or _has(_NEGATIVE_AFTER, after)
                and not targets_next
                or _has(r"除了\s*$", before)
                and _has(r"^\s*(?:以外|之外)?\s*(?:其他|其它|别的|什么|都)", after)
            )
            excepted = negated and _has(
                r"^\s*(?:以外|之外|外)\s*的?\s*(?:歌|歌曲|音乐|曲目)", after
            )
            if negated and not excepted:
                excluded.add(language)
                positive.discard(language)
                permissions = [p for p in permissions if p["language"] != language]
            else:
                positive.add(language)
                excluded.discard(language)
                if conversation and _has(
                    r"(?:只(?:听|放|要|选)|仅(?:听|放|要|选)|全是|全部是)\s*$", before
                ):
                    exclusive.add(language)
                if conversation and _has(_SWITCHED, before):
                    switches.append({"language": language, "start": start})
    return positive, excluded, exclusive, switches, permissions


def catalogLanguageForTurn(instruction, previous=None):
    previous_value = (
        previous
        if isinstance(previous, str)
        else (previous or {}).get("catalogLanguage")
    )
    retained = (
        previous_value
        if isinstance(previous_value, str) and previous_value in _VALUES
        else "unknown"
    )
    text = _conversation(utf16_slice(_normalize(instruction), 8000))
    if not text:
        return retained
    unrestricted = r"(?:不限|不限制|不限定|不区分|不挑|不要求|不拘)(?:任何|歌曲的|歌的|音乐的|演唱)?(?:语言|语种)|(?:语言|语种)(?:都行|均可|不限|无所谓|不重要|随意|不限制|没要求|没有要求)|(?:不局限于|不限于|不只(?:是|限)|不仅(?:仅)?)(?:中文|华语|国语|英文|英语)"
    latest_clear = -1
    for m in re.finditer(unrestricted, text):
        before = re.split(_END, text[: m.start()])[-1]
        after = re.split(_END, text[m.end() :])[0]
        if (
            not _has(r"(?:不是|并非|并不是|不要|别)\s*$", before)
            and not _non_music(before, after)
            and not (
                _has(r"回答|回复|交流|输出|翻译|界面", before + after)
                and not _has(r"(?:音乐|歌曲|歌单)", before + after)
            )
        ):
            latest_clear = m.end()
    for m in re.finditer(r"(?:中英文|中英双语|双语|中英混合)", text):
        before = re.split(_END, text[: m.start()])[-1]
        after = text[m.end() :]
        if (
            not _has(_NEGATIVE, before)
            and not _has(_NEGATIVE_AFTER, after)
            and not _non_music(before, after)
        ):
            latest_clear = m.end()
    active = text[latest_clear:] if latest_clear >= 0 else text
    positive, excluded, exclusive, switches, permissions = _mentions(active, True)
    if switches:
        chosen = max(switches, key=lambda s: s["start"])
        return (
            "any"
            if any(
                p["start"] > chosen["start"] and p["language"] != chosen["language"]
                for p in permissions
            )
            else chosen["language"]
        )
    if any(
        positive
        and p["language"] not in positive
        or not positive
        and retained in ("zh", "en")
        and p["language"] != retained
        for p in permissions
    ):
        return "any"
    if permissions and not positive and not excluded:
        return "any"
    if len(positive) == 2:
        return "unknown" if len(exclusive) == 2 else "any"
    if len(positive) == 1:
        return next(iter(positive))
    if excluded:
        return (
            "unknown"
            if latest_clear >= 0
            else retained
            if retained in ("zh", "en") and retained not in excluded
            else "unknown"
        )
    return "any" if latest_clear >= 0 else retained


def catalogLanguageQueryPrefix(language):
    return "华语" if language == "zh" else "英文" if language == "en" else ""


def catalogLanguageConflict(track, sources, language):
    source_id = (track or {}).get("sourceId")
    if (
        language not in ("zh", "en")
        or not track
        or track.get("sourceKind") != "playlist"
        or not safe_integer(source_id)
        or source_id <= 0
    ):
        return False
    source = next(
        (
            s
            for s in sources or []
            if isinstance(s, dict)
            and s.get("id") == source_id
            and s.get("name") == track.get("sourceName")
            and s.get("sourceKind") in (None, "", "playlist")
        ),
        None,
    )
    if not source:
        return False
    label = _normalize(
        " ".join(
            [source["name"], *[t for t in source.get("tags", []) if isinstance(t, str)]]
        )
    )
    if _has(r"中英文|中英双语|双语|中英混合", label):
        return False
    positive, *_ = _mentions(label)
    return len(positive) == 1 and ("en" if language == "zh" else "zh") in positive
