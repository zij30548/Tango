"""统一解释可完整覆盖的音乐目标与条件；原文证据不授予播放或记忆权限。"""

import re
from .compat import clone
from .task_track_count import parseTrackCount
from .music_lookup_intent import lookupRequest

_SCENES = ("散步", "跑步", "通勤", "睡前", "学习", "工作", "微醺", "喝酒", "读书")
_SCENE_ALIASES = {"走路": "散步", "边走边听": "散步", "边走路边听": "散步"}
_UNSAFE = re.compile(r'''[“”‘’"'`「」『』《》？?]|他说|她说|别人说|朋友说|引用|转述|假设|假如|例如|比如|举例|如果|除非|要不|还是|或者|不是让|不想|不要|不用|不能|禁止|别|记住|保存|删除|清空|播放|新歌|新作|新专辑|账号|会员|购买|定时''')
_PREFIX = re.compile(r"^(?:请你|请|麻烦你|麻烦|帮我|给我|为我|我想|我要|我现在|我正在|我在|现在|正在|然后|并且|同时|以及|再|推荐|查找|查一下|查询|搜索|找|生成|形成|整理|制作|准备|做|来|一个|一份|一组|一些|几首|一下|全部用|全都用|只用|仅用|在)+")
_ARTIST = r"[\w\u3400-\u9fff .·&+\-]{1,100}"


def _direct(text):
    """仅当前直接命令可进入确定性执行；否定、转述与条件整体保留原边界。"""
    return isinstance(text, str) and 0 < len(text) <= 1000 and not _UNSAFE.search(text)


def artistCatalogRequest(text):
    """作品浏览与指定单曲、场景编排分开，展示结果不自动播放。"""
    return _artist_catalog_signal(text)[0]


def artistCatalogIssue(text):
    """作品浏览的数量与歌手范围不确定时只追问，不任取数量或拼接两个艺人。"""
    return _artist_catalog_signal(text)[1]


def _artist_catalog_signal(text):
    if not _direct(text) or re.search(r"歌单|分钟|小时", text):
        return None, None
    lookup = lookupRequest(text)
    if lookup and lookup["title"] not in ("作品", "歌曲", "歌", "音乐"):
        # 数量位置不能把已完整识别的指定单曲重新包成一个虚构歌手。
        return None, None
    value = re.sub(r"[。！!\s]+$", "", text.strip())
    number = r"[0-9０-９零〇一二两三四五六七八九十百千]+"
    amount = re.compile(r"(?:(?:大约|大概|差不多|约|最多|至少|至多|最少|不超过|不少于)\s*)?"
                        rf"(?:[+＋\-−]?{number}(?:[.．]{number})?(?:\s*(?:到|至|[-~～—])\s*{number})?(?:来|多|余|几)?|几(?:十|百)?|若干|很多|好多)\s*(?:首|曲)"
                        r"(?:\s*(?:左右|上下|以内|以下|以上|多|余))?")
    clauses = [clause.strip() for clause in re.split(r"[，,；;\n]", value) if clause.strip()]
    if len(clauses) > 1:
        if len(clauses) > 3:
            return None, None
        bases, limits, issues = [], [], []
        for clause in clauses:
            found = list(amount.finditer(clause))
            display = amount.sub("", clause)
            if found and re.fullmatch(r"(?:(?:先|只|暂时|请|给我|帮我)){0,4}(?:列出|列|展示|显示|看看|看)?(?:歌曲|作品|曲目|歌)?(?:看看|就好|即可)?", display):
                limit, issue = _catalog_limit(found, number)
                if issue:
                    issues.append(issue)
                else:
                    limits.append(limit)
                continue
            base, issue = _artist_catalog_signal(clause)
            if issue:
                issues.append(issue)
                continue
            if not base:
                return None, None
            bases.append(base)
            if found:
                limits.append(base["limit"])
        if len(bases) != 1:
            return None, "artist_scope" if bases or issues else None
        if issues:
            return None, issues[0]
        if len(set(limits)) > 1:
            return None, "catalog_count"
        return {**bases[0], "limit": limits[0] if limits else bases[0]["limit"]}, None
    counts = list(amount.finditer(value))
    remaining = amount.sub("", value)
    catalog_artist = r"[\w\u3400-\u9fff .·&+\-、/]{1,100}"
    match = re.fullmatch(rf"(?:(?:请你|请|麻烦你|麻烦|帮我|给我|为我)){{0,4}}"
                         rf"(?:推荐|查找|查询|搜索|看看|查看|列出|找|查|列|来)(?:一些|一下|一点|点)?({catalog_artist}?)的(?:歌曲|作品|歌|音乐)(?:吧)?", remaining)
    if not match:
        match = re.fullmatch(rf"({catalog_artist}?)(?:有哪些|的哪些|有什么|哪些)(?:歌曲|作品|歌|音乐)", remaining)
    if not match and counts:
        # 数量可在作品之前或之后；只有完整的浏览句式才能把数字解释成结果上限。
        match = re.fullmatch(rf"(?:(?:请你|请|麻烦你|麻烦|帮我|给我|为我)){{0,4}}"
                             rf"(?:列出|列|找|查找|查询|搜索|推荐|来)?({catalog_artist}?)(?:的)?(?:歌曲|作品|歌|音乐)?"
                             r"(?:给我列出|给我列|只列出|只列|列出|列|给我|来)?(?:歌曲|作品|歌|曲目)?(?:吧)?", remaining)
    artist = match[1].strip() if match else ""
    artist = re.sub(r"^(?:歌手|艺人)", "", artist)
    if not artist or not _artist_name(artist) or re.search("|".join(_SCENES) + r"|推荐|查找|歌曲|作品|哪些", artist):
        return None, None
    if re.search(r".{2,}(?:和|与|以及|及|&|＆|、|/).{2,}", artist):
        return None, "artist_scope"
    if re.search(r"^(?:列出|列|只列|来|找|给我|保留|锁定|我要|我想|第|前)|(?:列|只)$", artist):
        return None, None
    limit, issue = _catalog_limit(counts, number)
    return (None, issue) if issue else ({"artist": artist, "limit": limit}, None)


def _catalog_limit(counts, number):
    """单句及分句数量共用精确值校验，不能因合并显示要求而放宽范围。"""
    if not counts:
        return 12, None
    if len(counts) != 1:
        return None, "catalog_count"
    token = counts[0][0].strip()
    if re.fullmatch(r"几\s*(?:首|曲)", token):
        return 12, None
    exact = re.fullmatch(rf"({number})\s*(?:首|曲)", token)
    limit = parseTrackCount(exact[1]) if exact else None
    if limit is None or not 1 <= limit <= 30:
        return None, "catalog_count"
    return limit, None


def playlistSearchRequest(text):
    """明确查找已有歌单才走只读搜索，制作和时长要求仍交给编排任务。"""
    if not _direct(text) or re.search(r"分钟|小时|生成|形成|制作|编排|[，,；;\n]", text):
        return None
    value = re.sub(r"[。！!\s]+$", "", text.strip())
    match = re.fullmatch(r"(?:(?:请你|请|麻烦你|麻烦|帮我|给我|为我)){0,4}(?:查找|搜索|找|查询)(?:一些|几个|一份|一下)?(.{1,80}?)(?:的)?歌单(?:吧)?", value)
    if not match:
        return None
    keyword = match[1].strip().removesuffix("的").strip()
    return {"keyword": keyword, "limit": 5} if keyword else None


def musicRequirementIssue(text):
    """互相冲突的精确时长先澄清，不让第二次模型提取选择其中一个数字。"""
    if not _direct(text) or not re.search(r"歌单|推荐|生成|形成", text):
        return None
    if re.search(r"(?:原来|之前|从)\d+\s*分钟.{0,20}(?:改|调整|换|变)", text):
        return None
    values = [int(value) for value in re.findall(r"(?<![\d.])(\d+)\s*分钟", text)]
    if len(set(values)) > 1:
        return "conflicting_duration"
    if values and not all(1 <= value <= 240 for value in values):
        return "invalid_duration"
    return None


def _prefix(value):
    return _PREFIX.sub("", value).strip()


def canonicalScene(value):
    """明确场景同义词使用统一值，保留原文来源以便解释归一化。"""
    return _SCENE_ALIASES.get(value, value)


def _artist_name(value):
    """风格调整和语法残词不能冒充人名；不确定名称交原模型或身份澄清。"""
    styles = {"爵士", "摇滚", "民谣", "古典", "嘻哈", "说唱", "电子", "流行", "蓝调", "轻音乐", "抒情", "原声", "纯音乐", "伴奏", "粤语", "国语", "华语", "中文", "英语", "日语", "韩语", "r&b", "rnb", "lofi", "jazz"}
    return 2 <= len(value) <= 100 and value.casefold() not in styles and not re.search(r"^(?:更|少|多|前|第|用|适合|总|当前|其他|其余|想听|要听|爱听|听)|轻快|舒缓|人声|歌单|歌曲|首|分钟|另一个|别的歌手", value)


def musicRequirement(text, *, allow_partial=False):
    """把同一歌单的多句描述合成一个目标，完整消费才允许省去规划模型。"""
    if not _direct(text):
        return None
    clauses = [part.strip() for part in re.split(r"[，,。；;！!\n]+|然后|并且|同时|以及", text) if part.strip()]
    if not 1 <= len(clauses) <= 8:
        return None
    fields, sources, unknown = {}, [], []
    revision = False
    action = False

    def put(field, value, source):
        if field in fields and fields[field] != value:
            return False
        fields[field] = value
        sources.append({"field": field, "value": value, "source": source, "scope": "task"})
        return True

    for source in clauses:
        value = _prefix(source)
        # 明确半小时属于单位换算；保留的证据仍是用户原文，不改写source。
        value = re.sub(r"半(?:个)?小时", "30分钟", value)
        unsupported_duration = (
            r"[零〇一二两三四五六七八九十百半]+(?:分钟|小时)|\d+(?:\.\d+)?小时|\d+\.\d+分钟|"
            r"[+\-−]\d+分钟|\d+\s*(?:到|至|[-~～])\s*\d+分钟"
        )
        if re.search(unsupported_duration, value):
            if not allow_partial:
                return None
            # 不支持的单位或区间仍交模型理解，但不能因此丢掉同句明确歌手与场景。
            unknown.append(source)
            value = re.sub(unsupported_duration, "", value)
        # 同一轮互相矛盾的时长不选一个端点，保留给显式澄清。
        numbers = re.findall(r"(?<![\d.+\-−])(\d+)\s*分钟", value)
        for number in numbers:
            if not put("targetMinutes", int(number), source):
                return None
        if numbers:
            value = re.sub(r"(?<![\d.])\d+\s*分钟", "", value)
        retained = re.fullmatch(r"(?:保留|锁定)(?:前|最前)([0-9一二两三四五六七八九十]+)首(?:歌曲|歌)?", value)
        if retained:
            token = retained[1]
            count = int(token) if token.isdigit() else {"一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}.get(token)
            if count is None or not put("retainFirst", count, source):
                return None
            revision = action = True
            continue
        change = re.match(r"^(?:(?:这份|当前|刚才的|这个)?歌单)?(?:时长)?(?:改成|改为|调整为|调整成|换成)", value)
        if change:
            revision = action = True
            value = value[change.end():].strip()
            if value and re.fullmatch(_ARTIST, value) and _artist_name(value) and not numbers and not any(scene in value for scene in _SCENES):
                if not put("artist", value.removesuffix("的歌曲").removesuffix("的歌"), source):
                    return None
                continue
        if value in ("其他不变", "其余不变"):
            continue
        for alias in sorted(_SCENE_ALIASES, key=len, reverse=True):
            value = value.replace(alias, _SCENE_ALIASES[alias])
        for scene in _SCENES:
            if scene in value:
                if not put("scene", scene, source):
                    return None
                value = value.replace(scene, "")
        value = re.sub(r"^(?:的时候|时|时候|适合|用于)", "", value)
        value = re.sub(r"(?:的时候|的时候听|时听)", "", value)
        value = _prefix(value)
        artist_match = re.fullmatch(rf"({_ARTIST})的(?:歌曲|作品|歌|音乐|歌单)(?:吧)?", value)
        if artist_match and _artist_name(artist_match[1].strip()):
            if not put("artist", artist_match[1].strip(), source):
                return None
            value = ""
        value = re.sub(r"^(?:总时长|时长|时间)(?:为|是|控制在)?", "", value)
        value = re.sub(r"^(?:的)?(?:歌单|歌曲|音乐|歌)(?:吧|即可|就好)?$", "", value)
        if value:
            unknown.append(source)
        if re.search(r"推荐|生成|形成|制作|整理|准备|歌单|改成|调整|换成|保留|锁定", source):
            action = True
    if not fields or not action or unknown and not allow_partial:
        return None
    return {"version": 1, "goal": "revise_playlist" if revision else "create_playlist", "complete": not unknown,
            **fields, "sources": sources, "autoPlay": False, "savePreference": False}


def compileMusicBrief(inputs):
    """已确定字段直接进入同一brief，修改只覆盖本轮字段并继承已确认条件。"""
    plan = musicRequirement(inputs.get("instruction", ""))
    if not plan:
        return None
    old = inputs.get("currentBrief") or {}
    # 本阶段只把可核验的指定歌手编排及其修改切到零规划调用；其余旧场景流程保留原模型路径。
    if not plan.get("artist") and not (old.get("requiredArtist") and old.get("artistOnly") and not old.get("requireReleases")):
        return None
    if plan["goal"] == "revise_playlist" and (not old or old.get("kind") == "clarify"):
        return None
    if inputs.get("pendingRequest"):
        return None
    from .task_schema import BRIEF_DEFAULTS, validateBrief
    brief = {**clone(BRIEF_DEFAULTS), **clone(old)} if old else clone(BRIEF_DEFAULTS)
    brief.update(kind="edit" if old else "research", question="", adjustment="")
    for key in ("scene", "targetMinutes", "retainFirst"):
        if key in plan:
            brief[key] = plan[key]
    if "artist" in plan:
        if plan["artist"] != brief.get("requiredArtist"):
            brief.pop("requiredArtistId", None)
        brief.update(requiredArtist=plan["artist"], artistOnly=True, requireReleases=False, artist="", since=None, until=None)
        brief.pop("artistId", None)
    artist, scene = brief.get("requiredArtist", ""), brief.get("scene", "")
    brief["title"] = (artist + " " + scene + "歌单").strip()
    brief["queries"] = list(dict.fromkeys(part for part in [" ".join(filter(None, [artist, scene])), scene or artist] if part))[:2]
    return validateBrief(brief, inputs), plan
