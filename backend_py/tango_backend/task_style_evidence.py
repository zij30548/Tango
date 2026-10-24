"""只从曲库来源名称、标签建立风格证据，不据此声称测量过音频。"""

import re
import unicodedata
from .compat import utf16_len, safe_integer

ALIASES = [
    [
        "轻快",
        "欢快",
        "轻盈",
        "愉悦",
        "愉快",
        "活泼",
        "upbeat",
        "lively",
        "cheerful",
        "bouncy",
    ],
    ["安静", "宁静", "静谧", "quiet", "peaceful"],
    ["舒缓", "舒缓慢调", "soothing", "calm"],
    ["R&B", "rnb", "节奏蓝调", "rhythm and blues"],
    ["爵士", "jazz"],
    ["华语", "国语", "中文", "mandarin"],
    ["纯音乐", "器乐", "instrumental"],
    ["摇滚", "rock"],
    ["电子", "electronic", "edm"],
    ["民谣", "folk"],
]


def normalized(value):
    return unicodedata.normalize("NFKC", str(value or "")).lower().strip()


def styleQueryAlternatives(style):
    wanted = normalized(style)
    group = next(
        (items for items in ALIASES if any(normalized(t) == wanted for t in items)),
        None,
    )
    if group:
        return [item for item in group if normalized(item) != wanted]
    alternatives = []
    for items in ALIASES:
        for term in items:
            escaped = re.escape(term)
            pattern = re.compile(
                ("(^|[^a-z0-9])(" + escaped + ")(?=$|[^a-z0-9])")
                if term.isascii()
                else "()(" + escaped + ")",
                re.I,
            )
            if not pattern.search(str(style)):
                continue
            replacement = next(
                (i for i in items if normalized(i) != normalized(term)), None
            )
            if replacement:
                candidate = pattern.sub(
                    lambda m: m.group(1) + replacement, str(style), count=1
                )
                if (
                    utf16_len(candidate) <= 40
                    and normalized(candidate) != wanted
                    and candidate not in alternatives
                ):
                    alternatives.append(candidate)
    return alternatives


_NEGATIVE = re.compile(
    r"(?:无|非|不|没有|没|未|未必|缺少|缺乏|缺失|排除|拒绝|避免|告别|远离|不要|无需|不再|谈不上|不能保证|无法保证|是否|是不是|可能|也许|或许|大概|似乎|貌似|不见得|不一定|不确定|无法确定|不能确定|\bno|\bnot|\bwithout|\bnon|\bexclude|\blacking|\black(?:s|ing)?(?:\s+of)?|\bfree\s+of|\bmaybe|\bperhaps|\bpossibly|\bwhether|\bmight(?:\s+be)?|\bmay(?:\s+be)?)[\s\-]*(?:(?:任何|一点|一些|太|很|够|怎么|那么|这么|再|的|真的|a|an|the|any|very|so|really|quite|much)[\s\-]*)*$",
    re.I | re.ASCII,
)
_UNCERTAIN = re.compile(
    r"^\s*(?:[-—]\s*free\b|(?:感|程度|得)?(?:不明显|不太明显|不强|不足|不够)|欠缺|缺乏|缺失|未确认|不确定|不保证|与否|存疑|吗|[？?]|的反面)",
    re.I | re.ASCII,
)


def styleLabelMatches(text, style):
    wanted, label = normalized(style), normalized(text)
    if not wanted:
        return False
    group = next(
        (items for items in ALIASES if any(normalized(i) == wanted for i in items)),
        [style],
    )
    positive = uncertain = False
    for item in group:
        term = normalized(item)
        if not term:
            continue
        pattern = (
            r"r\s*&\s*b"
            if term == "r&b"
            else re.sub(r"\s+", lambda _: r"\s+", re.escape(term).replace(r"\ ", " "))
        )
        expression = re.compile(
            r"(?<![a-z0-9])(?:" + pattern + r")(?![a-z0-9])"
            if term.isascii()
            else pattern,
            re.I,
        )
        for match in expression.finditer(label):
            before = re.split(
                r"[，,。.!！？?；;|\n]",
                label[max(0, match.start() - 60) : match.start()],
            )[-1]
            after = label[match.end() : match.end() + 40]
            if _NEGATIVE.search(before) or _UNCERTAIN.search(after):
                uncertain = True
            else:
                positive = True
    return positive and not uncertain


def adjustmentStyles(brief, previousBrief):
    if not previousBrief or not brief or not brief.get("styles"):
        return []
    mentioned = [
        s for s in brief["styles"] if styleLabelMatches(brief.get("adjustment"), s)
    ]
    if mentioned:
        return mentioned
    old = set(map(normalized, previousBrief.get("styles", [])))
    return [s for s in brief["styles"] if normalized(s) not in old]


def playlistStyleEvidence(track, sources, styles):
    unknown = {"status": "unverified", "matchedStyles": [], "basis": "none"}
    source_id = track.get("sourceId")
    if (
        track.get("sourceKind") != "playlist"
        or not safe_integer(source_id)
        or source_id <= 0
    ):
        return unknown
    source = next(
        (
            s
            for s in sources or []
            if s.get("id") == source_id
            and s.get("name") == track.get("sourceName")
            and s.get("sourceKind") in (None, "", "playlist")
        ),
        None,
    )
    if not source:
        return unknown
    label = " ".join([source["name"], *source.get("tags", [])])
    matched = [s for s in styles or [] if styleLabelMatches(label, s)][:6]
    return (
        {
            "status": "source_matched",
            "matchedStyles": matched,
            "basis": "playlist_name_or_tags",
        }
        if matched
        else unknown
    )


def adjustmentAssessment(task, preview):
    styles = adjustmentStyles(task.get("brief"), task.get("previousBrief"))
    if not task.get("preview") or not styles:
        return None
    unlocked = lambda tracks: [
        t for t in tracks if t["id"] not in task.get("lockedIds", [])
    ]
    before = [t["id"] for t in unlocked(task["preview"]["tracks"])]
    after = [t["id"] for t in unlocked(preview["tracks"])]
    changed, accepted = before != after, preview["ranking"]["status"] == "model"
    fresh = [
        b
        for b in task["branches"].values()
        if (b.get("step") or {}).get("revision") == task["revision"] and b.get("result")
    ]
    evidence_ids = []
    for track in unlocked(preview["tracks"]):
        for branch in fresh:
            actual = next(
                (
                    t
                    for t in branch["result"]["tracks"]
                    if t["id"] == track["id"]
                    and t.get("sourceId") == track.get("sourceId")
                    and t.get("sourceName") == track.get("sourceName")
                ),
                None,
            )
            if (
                actual
                and playlistStyleEvidence(actual, branch["result"]["sources"], styles)[
                    "status"
                ]
                == "source_matched"
            ):
                evidence_ids.append(track["id"])
                break
    return {
        "status": "degraded"
        if not accepted
        else "unchanged"
        if not changed
        else "source_supported"
        if evidence_ids
        else "unverified",
        "styles": styles,
        "rankingAccepted": accepted,
        "changedUnlockedIds": [
            value
            for i, value in enumerate(after)
            if i >= len(before) or before[i] != value
        ],
        "evidenceTrackIds": evidence_ids,
        "listeningQuality": "not_measured",
    }
