"""完整解析受支持的单曲语句；只产出查询参数，不授予历史播放或记忆权限。"""

import re

from .compat import utf16_len, well_formed


_PAIRS = {"《": "》", "“": "”", "‘": "’", '"': '"', "'": "'", "「": "」", "『": "』"}
_QUOTES = frozenset("".join(_PAIRS) + "".join(_PAIRS.values()))
_VERSION = r"(?:录音室版|现场版|live(?:版)?|studio(?:版)?)"
_PREFIX = r"(?:(?:请你|麻烦你|能不能|可不可以|可以|能否|请|你能|麻烦|帮我|帮忙|替我|为我|给我|现在|立即|就|在这台电脑上|在这台电脑|我|再次|重新|再)){0,8}"
_VERBS = r"搜索|搜一下|查找|查一下|找一下|播放|放一首|放一下|听一下|听一首|想听|查询|搜|找|查|放"
_PLAY_VERBS = frozenset(["播放", "放一首", "放一下", "放", "听一下", "听一首", "想听"])
_INDIRECT = re.compile(r"他说|她说|别人说|朋友说|引用|假设|假如|举例|转述|示例|示范|如果|除非|只有.{0,40}才|不是让你|没有让你|没让你")
_UNREPRESENTED = re.compile(r"不要|不用|不想|禁止|无需|不能|不需要|别|然后|并且|同时|以及|但是|之后|自动|保存|删除|记住|如果|除非|只有|下载|分享|发送|转发|收藏|定时|必须|最高音质|无损")
_NOT_A_SONG = re.compile(r"新歌|新作|新专辑|歌单|推荐|要听|适合|首歌|所有|全部")


def _valid_text(value, maximum, required=True):
    """复用路由的字符串限制，避免直接调用解析器绕开字符与规模边界。"""
    return (
        isinstance(value, str) and utf16_len(value) <= maximum and well_formed(value)
        and not re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", value)
        and (not required or bool(value.strip()))
    )


def _clauses(command):
    """只在标题引号外分句；不把歌名中的逗号当作新的用户授权。"""
    result, current, closing = [], [], None
    for index, char in enumerate(command):
        if closing:
            current.append(char)
            if char == closing:
                closing = None
        elif char in _PAIRS:
            # 英文单词里的撇号是标题数据，不单独开启一段指令引号。
            if char == "'" and index and index + 1 < len(command) and command[index - 1].isalpha() and command[index + 1].isalpha():
                current.append(char)
                continue
            current.append(char)
            closing = _PAIRS[char]
        elif char in _QUOTES:
            return None
        elif char in "，,；;。\n":
            value = "".join(current).strip()
            if not value:
                return None
            result.append(value)
            current = []
        else:
            current.append(char)
    if closing:
        return None
    value = "".join(current).strip()
    if value:
        result.append(value)
    return result if 1 <= len(result) <= 6 else None


def _version(value):
    """返回明确版本；未知描述和多个版本不作近似解释。"""
    match = re.fullmatch(_VERSION, value, re.I)
    if not match:
        return None
    return "studio" if value.lower().startswith(("录音室", "studio")) else "live"


def _song(payload):
    """标题有引号时按完整边界取值，版本只从引号外读取。"""
    opening = next((index for index, char in enumerate(payload) if char in _PAIRS), None)
    if opening is not None:
        quote, prefix = payload[opening], payload[:opening].strip()
        # 量词只在明确标题引号外消费，不能把《一首简单的歌》中的文字删掉。
        prefix = re.sub(r"^(?:一首|一曲)\s*", "", prefix)
        end = payload.find(_PAIRS[quote], opening + 1)
        if end < 0:
            return None
        title, suffix = payload[opening + 1:end].strip(), payload[end + 1:].strip()
        # 这里只接受一个标题；第二个标题或未理解的后缀必须交给澄清。
        if any(char in _QUOTES for char in suffix) or any(char in _QUOTES for char in prefix):
            return None
        if prefix:
            artist_match = re.fullmatch(r"(.+?)(?:演唱的|唱的|的|那首|这首)", prefix)
            if not artist_match:
                return None
            artist = artist_match[1].strip()
        else:
            artist = ""
        version = _version(suffix.removeprefix("的").strip()) if suffix else ""
        if suffix and not version:
            return None
    else:
        # 无引号标题只接受有限的常用尾缀，不静默丢弃其它条件。
        version_match = re.fullmatch(r"(.+?)(?:的)?(" + _VERSION + r")", payload, re.I)
        version = _version(version_match[2]) if version_match else ""
        identity = version_match[1] if version_match else payload
        if _NOT_A_SONG.search(identity) or any(char in _QUOTES for char in identity):
            return None
        performer = re.fullmatch(r"(?:一首|一曲)?(.+?)(?:演唱的|唱的)(.+)", identity)
        if performer:
            artist, title = performer.groups()
        elif "的" in identity:
            artist, title = identity.split("的", 1)
        else:
            parts = identity.split()
            artist, title = parts if len(parts) == 2 else ("", identity)
    if _UNREPRESENTED.search(artist) or _INDIRECT.search(artist) or (opening is None and (_UNREPRESENTED.search(title) or _INDIRECT.search(title))):
        return None
    if not _valid_text(title, 200) or not _valid_text(artist, 100, False) or title.strip() in ["歌", "歌曲", "音乐"]:
        return None
    return {"artist": artist.strip(), "title": title.strip(), **({"version": version} if version else {})}


def _modifier(clause):
    """受支持附句全部消耗后才接受；只读与禁止保存只能收紧权限。"""
    value = re.sub(r"\s+", "", clause)
    version = re.fullmatch(r"(?:用|要|选|选择|请用|请选|请选用)(" + _VERSION + r")", value, re.I)
    if version:
        return {"version": _version(version[1])}
    # 允许并列的禁止播放/保存；不接受把肯定播放藏在否定附句后面。
    negative = r"(?:先|也|暂时)?(?:别|不要|不用|无需|不)(?:自动)?(?:播放|放|播)(?:了)?"
    memory = r"(?:也|先)?(?:别|不要|不用|无需|不)(?:记住(?:什么)?|保存(?:(?:成|为|到)?(?:我的|我|本机|这台电脑上的)?(?:偏好|口味|喜好))?)"
    if re.fullmatch(negative, value):
        return {"readOnly": True}
    if re.fullmatch(memory, value):
        return {}
    if re.fullmatch(r"(?:" + negative + r")(?:也|且|和)?(?:" + memory + r")", value):
        return {"readOnly": True}
    if re.fullmatch(r"(?:" + memory + r")(?:也|且|和)?(?:" + negative + r")", value):
        return {"readOnly": True}
    if re.fullmatch(
        r"(?:我)?(?:先|只|暂时)?(?:让我看看|(?:给我|看|查看)(?:资料|信息|(?:搜索|查询|查找)?结果|候选(?:列表|结果)?)|(?:展示|显示|列|列出|看看)(?:候选|结果|候选结果))"
        r"(?:就好|即可|就行)?", value,
    ):
        return {"readOnly": True}
    if re.fullmatch(
        r"(?:把|只(?:要|显示|给我看)?|显示)?(?:版本|时长|歌名|歌手)(?:(?:和|、|及)(?:版本|时长|歌名|歌手))*"
        r"(?:给我(?:就好)?|信息(?:就好)?|即可)", value,
    ):
        return {"readOnly": True}
    return None


def lookupRequest(instruction):
    """将当前明确单曲请求转成完整参数；未知组合返回空，由上层澄清或受控模型处理。"""
    if not _valid_text(instruction, 1000):
        return None
    command = re.sub(r"[。！!？?]+$", "", instruction.strip())
    clauses = _clauses(command)
    if not clauses:
        return None
    match = re.fullmatch(_PREFIX + r"(" + _VERBS + r")(?:一下)?\s*(.+?)(?:好吗|可以吗|吗)?", clauses[0])
    if not match:
        return None
    verb, payload = match.groups()
    song = _song(payload.strip())
    if not song:
        return None
    read_only, version = False, song.get("version", "")
    for clause in clauses[1:]:
        modifier = _modifier(clause)
        if modifier is None:
            return None
        current = modifier.get("version", "")
        if current and version and current != version:
            return None
        version = current or version
        read_only = read_only or modifier.get("readOnly", False)
    return {
        "artist": song["artist"], "title": song["title"], "play": verb in _PLAY_VERBS and not read_only,
        **({"version": version} if version else {}),
    }
