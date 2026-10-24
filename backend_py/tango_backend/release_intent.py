"""完整识别单歌手发行查询；日期和限制保留在原始指令交给任务规划与校验。"""

import re

from .compat import utf16_len, well_formed


_WINDOW = r"(?:(?:最近|近|过去|近来)(?:[1-9]\d{0,3}(?:天|周|个月|月|年))|最近|最新)"
_PREFIX = re.compile(r"^(?:(?:请你|麻烦你|能不能|可不可以|可以|能否|请|请问|你能|麻烦|帮我|帮忙|替我|为我|给我)){1,8}")
_QUERY = re.compile(r"^(?:查询|查一下|查查|搜索|看看|查看|找一下|查)")
_QUESTION = r"(?:有没有|是否有|是否|有哪些|有什么|有)?"
_ISSUED = r"(?:(?:已经|已)?(?:发行了?|发布了?|推出了?|发了?))?"
_MAIN = re.compile(
    r"(?P<artist>.+?)(?P<window>" + _WINDOW + r")?(?:的)?" + _QUESTION + _ISSUED
    + r"(?:什么|哪些)?(?:的)?(?:新歌|新作|新专辑)(?:了吗|吗|么|了|有哪些|有什么)?"
)
_UNSAFE_ARTIST = re.compile(
    r"他说|她说|别人说|朋友说|朋友让我|他让我|她让我|引用|转述|假设|假如|举例|比如|例如|示例|示范|如果|除非|只有|"
    r"不是|没有|没让|不要|不用|不想|不能|禁止|无需|别|然后|并且|同时|以及|但是|之后|"
    r"保存|记住|删除|播放|自动|推荐|歌单|分钟|小时|最近|最新|过去|\d+(?:天|周|个月|月|年)|"
    r"发行|发布|推出|有没有|是否|有哪些|有什么|已经|未发行|新歌|新作|新专辑|[、/&]|和|与|及"
)
_NO_ARTIST = frozenset(["", "哪位歌手", "哪个歌手", "歌手", "有没有", "有什么", "有哪些", "是否", "是否有", "有", "什么", "哪些", "谁", "的", "最近", "最新"])


def _modifier(clause):
    """仅接受只读展示、禁止副作用及先确认身份；其它新动作不作局部执行。"""
    value = re.sub(r"\s+", "", clause)
    # 预览只限制呈现范围，不授予采用、播放、保存或旧任务续接权限。
    if re.fullmatch(
        r"(?:先|只|仅|暂时)?(?:做|生成|给我|给|提供|展示)(?:一份|一个)?(?:歌单)?预览(?:就好|即可|就行)?", value,
    ):
        return True
    if re.fullmatch(
        r"(?:(?:如果|若|遇到|遇上|发现)(?:有)?|有)?(?:同名|重名)(?:歌手|艺人)(?:时|的话)?"
        r"(?:请)?(?:先)?(?:让我|由我|和我)(?:确认|核对)(?:身份|歌手身份)?", value,
    ):
        return True
    if re.fullmatch(
        r"(?:先|只|暂时)?(?:查出来|查到|查询结果|找出来|展示(?:候选|结果)?|显示(?:候选|结果)?|列(?:出)?(?:候选|结果)?)"
        r"(?:给我(?:看|看看)?|让我看看)?(?:就好|即可|就行)?", value,
    ):
        return True
    if re.fullmatch(r"(?:我)?(?:先|只|暂时)?(?:看|查看|给我)(?:资料|信息|候选|结果)(?:就好|即可|就行)?", value):
        return True
    if re.fullmatch(r"(?:先|也|暂时)?(?:别|不要|不用|无需|不)(?:自动)?(?:播放|放|播)(?:了)?", value):
        return True
    if re.fullmatch(r"(?:先|也)?(?:别|不要|不用|无需|不)(?:记住(?:什么)?|保存(?:(?:成|为|到)?(?:我的|本机)?(?:偏好|口味|喜好))?)", value):
        return True
    return False


def releaseRequestArtist(instruction):
    """仅在整句属于单歌手新作查询时提取姓名，不从条件、历史或模型补造歌手。"""
    if (
        not isinstance(instruction, str) or not instruction.strip() or utf16_len(instruction) > 1000
        or not well_formed(instruction) or re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", instruction)
        or re.search(r"[“”‘’\"'`「」『』《》]", instruction)
    ):
        return None
    clauses = [value.strip() for value in re.split(r"[，,；;。！？!?\n]+", instruction.strip()) if value.strip()]
    if not 1 <= len(clauses) <= 6 or any(not _modifier(value) for value in clauses[1:]):
        return None
    command = _PREFIX.sub("", clauses[0], count=1).strip()
    command = re.sub(r"^(?:我)?(?:想知道|想问一下|想查询|想查一下)", "", command, count=1).strip()
    command = _QUERY.sub("", command, count=1).strip()
    leading_window = re.match(_WINDOW, command)
    if leading_window:
        command = command[leading_window.end():].strip()
    match = _MAIN.fullmatch(command)
    if not match:
        return None
    # 两个显式窗口可能冲突，不选其中一个；继续交给上层澄清。
    if leading_window and match["window"]:
        return None
    artist = match["artist"].strip()
    if (
        artist in _NO_ARTIST or utf16_len(artist) > 100 or _UNSAFE_ARTIST.search(artist) or artist.endswith(("未", "没", "不"))
        or re.search(r"(?:查询|搜索|查找|查看|看看|查查|帮我|请问)", artist)
    ):
        return None
    return artist
