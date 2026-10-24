"""设备候选的有界只读筛选；元数据是设备声明，不等于后台认证了曲库事实。"""

import json
import re
import unicodedata

from .compat import clone, safe_integer, utf16_len, well_formed
from .errors import ApiError


LOOKUP_CONTEXT_TTL_MS = 10 * 60 * 1000
LOOKUP_CONTEXT_MAX_DEPTH = 32
LOOKUP_DISPLAY_LIMIT = 12
LOOKUP_CONTEXT_MAX_BYTES = 12 * 1024
_IDENTIFIER = re.compile(r"[A-Za-z0-9_-]{8,128}")
_BINDINGS = ("candidateSetId", "originMessageId", "originActionId")
_PREFIX = r"(?:(?:请你|请|帮我|给我|我想要|我想看|我要|我选|要|选|选择|看看|查看|只要|换成|还是)){0,3}"
_SUFFIX = r"(?:的)?(?:那个|那首|这首|这个)?"
_VERSION = r"(录音室版|录音室版本|studio版|现场版|现场版本|live版)"


def _text(value, maximum, *, empty=False):
    """限制UTF16长度与控制字符，避免工具字段进入无界上下文。"""
    return (isinstance(value, str) and (empty or bool(value.strip())) and utf16_len(value) <= maximum
            and well_formed(value) and not re.search(r"[\x00-\x1f\x7f]", value))


def validateLookupContext(value):
    """只验证候选形状；原会话、动作、相邻性和有效期还须由路由服务核验。"""
    valid = type(value) is dict and set(value) == {*_BINDINGS, "rows", "visibleIds"}
    if valid:
        valid = all(isinstance(value[key], str) and _IDENTIFIER.fullmatch(value[key]) for key in _BINDINGS)
        valid = valid and 8 <= len(value["originMessageId"]) <= 100 and len(value["candidateSetId"]) >= 8
        rows, visible = value["rows"], value["visibleIds"]
        valid = valid and type(rows) is list and 1 <= len(rows) <= 20 and type(visible) is list and len(visible) <= LOOKUP_DISPLAY_LIMIT
    if valid:
        ids = []
        for row in rows:
            if (type(row) is not dict or set(row) != {"id", "title", "artist", "album", "version", "durationMs"}
                    or not safe_integer(row["id"]) or not 1 <= row["id"] <= 9007199254740991
                    or not _text(row["title"], 200) or not _text(row["artist"], 200, empty=True)
                    or not _text(row["album"], 200, empty=True) or row["version"] not in ["studio", "live", "unknown"]
                    or not safe_integer(row["durationMs"]) or not 0 <= row["durationMs"] <= 86400000):
                valid = False
                break
            ids.append(row["id"])
        valid = valid and len(set(ids)) == len(ids) and all(safe_integer(item) and item in ids for item in visible) and len(set(visible)) == len(visible)
    if valid:
        valid = len(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) <= LOOKUP_CONTEXT_MAX_BYTES
    if not valid:
        raise ApiError(400, "INVALID_LOOKUP_CONTEXT", "歌曲候选上下文无效，请重新搜索。")
    return clone(value)


def _normalized(value):
    return re.sub(r"\s+", "", unicodedata.normalize("NFKC", value))


def _number(value):
    """只接受明确的正整数序号，不解释约数、区间或小数。"""
    if value.isascii() and value.isdecimal():
        return int(value)
    digits = {"一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9}
    if value in digits:
        return digits[value]
    if value == "十":
        return 10
    if value.startswith("十") and value[1:] in digits:
        return 10 + digits[value[1:]]
    if value.endswith("十") and value[:-1] in digits:
        return 10 * digits[value[:-1]]
    return 0


def _version(label):
    return "studio" if label.lower().startswith(("录音室", "studio")) else "live"


def _selector(instruction, context):
    """整句匹配有限选择语法；未知附句不能被截去后获得播放或记忆授权。"""
    command = re.sub(r"[。!?！？]+$", "", _normalized(instruction))
    if command in ["不是这个歌手,换另一个歌手", "不是这个歌手,换成另一个歌手"]:
        return {"operation": "other_artist"}
    clauses = [part for part in re.split(r"[,;。\n]+", command) if part]
    if not clauses or len(clauses) > 3 or any(not re.fullmatch(
        r"(?:(?:先|只|暂时)?(?:别|不要|不用|无需)(?:自动)?(?:播放|放|播)(?:了)?|先让我看看|只(?:给我|看|查看)(?:资料|信息))", part)
        for part in clauses[1:]):
        return None
    command = clauses[0]
    if re.search(r"他说|她说|别人说|朋友说|如果|假如|假设|除非|引用|转述|女声|男声|[‘’“”\"'`「」『』]", command):
        return None
    match = re.fullmatch(_PREFIX + r"第([0-9一二两三四五六七八九十零百]+)(?:首|个)" + _SUFFIX, command)
    if match:
        return {"operation": "select", "index": _number(match[1])}
    match = re.fullmatch(_PREFIX + _VERSION + _SUFFIX, command, re.I)
    if match:
        return {"operation": "version", "version": _version(match[1])}
    match = re.fullmatch(r"(?:请|帮我)?(?:不要|排除|去掉|别要)" + _VERSION + _SUFFIX, command, re.I)
    if match:
        return {"operation": "exclude_version", "version": _version(match[1])}
    if re.fullmatch(_PREFIX + r"(?:另一个|其他)(?:歌手)(?:唱的)?" + _SUFFIX, command):
        return {"operation": "other_artist"}
    # 歌手必须完整匹配设备候选里的名字，不能从模型、历史或声线推断。
    artists = list(dict.fromkeys(row["artist"] for row in (context or {}).get("rows", []) if row["artist"].strip()))
    for artist in artists:
        normalized_name = _normalized(artist)
        # 对澄清的简短回答只接受整句完整姓名；附句、转述和条件不能被裁掉后冒充回答。
        if _normalized(instruction) == normalized_name:
            return {"operation": "artist", "artist": artist}
        name = re.escape(normalized_name)
        if (re.fullmatch(_PREFIX + name + r"(?:唱的|的)" + _SUFFIX, command)
                or re.fullmatch(r"我说的是" + name + r"(?:唱的)?" + _SUFFIX, command)):
            return {"operation": "artist", "artist": artist}
    return None


def lookupReference(instruction, context=None):
    """返回明确选择或必要追问；普通新话题不进入候选分支。"""
    command = _normalized(instruction)
    if re.search(r"推荐|歌单|搜索|查找|查询|找一下|新歌|新作|新专辑", command):
        return None
    if re.search(r"偏好|喜好|口味|记住|保存", command) and not re.search(r"第[0-9一二两三四五六七八九十零百]+(?:首|个)|另一个歌手|我说的是", command):
        # 管理偏好时提及“现场版”不是在筛选歌曲；明确序号再夹带新操作则仍须整句澄清。
        return None
    selector = _selector(instruction, context)
    looks_related = bool(selector or re.search(r"第[0-9一二两三四五六七八九十零百]+(?:首|个)|(?:录音室|现场|studio|live)版|女声|男声|另一个歌手|我说的是.+(?:那个|那首)", command, re.I))
    if not looks_related:
        return None
    if not context:
        return {"question": "当前没有可继续筛选的歌曲候选。请先给出歌名搜索，再选择候选。"}
    if selector is None:
        return {"question": "请用候选序号、完整歌手名或已知版本选择。现有资料不能可靠判断男女声；复合条件需要先明确，尚未筛选或播放。"}
    if selector["operation"] == "select" and not 1 <= selector["index"] <= len(context["visibleIds"]):
        return {"question": "这个序号不在当前展示的候选中。请核对序号，或重新按版本、歌手筛选。"}
    if selector["operation"] == "other_artist":
        current = [row["artist"] for row in context["rows"] if row["id"] in context["visibleIds"]]
        if not current or len(set(current)) != 1 or not current[0].strip():
            return {"question": "当前候选还没有唯一歌手，无法确定“另一个”相对谁。请先选一首或给出完整歌手名。"}
    return {"selector": selector}


def lookupSelection(context, selector):
    """确定性派生展示ID；未知版本不会被排除条件冒充成已核验的另一版本。"""
    rows, visible, operation = context["rows"], context["visibleIds"], selector["operation"]
    if operation == "select":
        index = selector["index"]
        return [visible[index - 1]] if 1 <= index <= len(visible) else []
    if operation == "version":
        selected = [row for row in rows if row["version"] == selector["version"]]
    elif operation == "exclude_version":
        selected = [row for row in rows if row["version"] in ["studio", "live"] and row["version"] != selector["version"]]
    elif operation == "artist":
        selected = [row for row in rows if row["artist"] == selector["artist"]]
    elif operation == "other_artist":
        artists = {row["artist"] for row in rows if row["id"] in visible}
        selected = [row for row in rows if len(artists) == 1 and row["artist"].strip() and row["artist"] not in artists]
    else:
        return []
    return [row["id"] for row in selected[:LOOKUP_DISPLAY_LIMIT]]


def lookupParameters(context, selector):
    """来源标识由已核验上下文绑定，模型不能替换候选集合或加入副作用字段。"""
    return {**{key: context[key] for key in _BINDINGS}, **selector}
