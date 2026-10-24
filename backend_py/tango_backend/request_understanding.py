"""把可完整解释的直接音乐需求拆成有范围的分句，复杂自由表达仍交模型。"""

from dataclasses import dataclass
import re
from .task_track_count import trackCountRequest


@dataclass(frozen=True)
class Requirement:
    """保留当前原句与需求范围；这里不授予工具或记忆写入权限。"""

    kind: str
    source: str
    scope: str
    value: str = ""
    after_confirmation: bool = False


@dataclass(frozen=True)
class UnderstoodRequest:
    """只有全部分句均能归类时才返回，避免忽略句尾的额外要求。"""

    requirements: tuple[Requirement, ...]
    target_minutes: int | None
    revision: bool
    target_track_count: int | None = None


@dataclass(frozen=True)
class PreferenceCommand:
    """仅描述本轮提议；保存仍由设备账号与明确确认控制。"""

    operation: str
    value: str = ""
    polite_request: bool = False


_QUOTED_OR_CONDITIONAL = re.compile(
    r'''[“”‘’"'`「」『』]|他说|她说|别人说|引用|转述|假设|假如|例如|比如|举例|如果|除非|要不|还是|或者'''
)
_MUSICAL_WORD = (
    r"(?:粤语|国语|华语|中文|英语歌?|日语歌?|韩语歌?|爵士|摇滚|民谣|古典|嘻哈|说唱|电子|流行|蓝调|"
    r"轻音乐|抒情|原声器乐|电子器乐|原声|器乐|纯音乐|伴奏|人声|吵闹现场版|现场版|录音室版|快节奏|慢节奏|R&B|RNB|HIPHOP|JAZZ|LOFI|K-POP)"
)
_PREFERENCE = re.compile(
    rf"(?:(?:更喜欢|喜欢|偏好|爱听|常听|不喜欢|不爱听|不听|尽量不听|希望少听|优先(?:听|选|给我))"
    rf"\s*{_MUSICAL_WORD}\s*(?:音乐|歌曲|歌|版)?"
    rf"(?:\s*(?:(?:和|、|及|与)\s*)?{_MUSICAL_WORD}\s*(?:音乐|歌曲|歌|版)?)*|"
    rf"(?:中文|华语|国语|粤语|英语|日语|韩语)(?:音乐|歌曲|歌)?优先)",
    re.I,
)
_TEMPORARY = re.compile(
    r"(?:(?:只|仅)(?:用于|限于)?(?:今晚|今天|本次|这次)|(?:今晚|今天|本次|这次)(?:临时|只)?(?:想听|听|喜欢|偏好).+|"
    r"(?:不要|别|不用|无需)(?:再)?(?:记住|保存)(?:我)?.+|(?:不|无需|不用)(?:要)?(?:保存|记住)(?:偏好|口味|喜好)?|"
    r"(?:先|暂时)?(?:不要|别|不用|无需)(?:自动)?播放|(?:先|仅|只)(?:生成|给我|做)?预览|其他不变|其余不变|别另起一份|不要另起一份)"
)
_MUSIC_REQUEST = re.compile(
    r"(?:(?:请你|请|麻烦你|麻烦|帮我|给我|为我|只给我|只|再)){0,4}"
    r"(?:推荐|找|查找|生成|整理|来)(?:一些|几首|一份|个|一组|一下)?[^，,；;。\n]{1,160}"
)
_NOMINAL_MUSIC_REQUEST = re.compile(
    r"(?:(?:给我|为我)(?:一份|一个|一组|一些)|(?:帮我)?(?:做|制作|准备)(?:一份|一个|一组)?)"
    r"[^，,；;。\n]{1,140}(?:歌单|音乐|歌曲|歌)"
)
_SCENE_REQUEST = re.compile(
    r"(?:今晚|今天|本次|这次)?(?:跑步|通勤|睡前|学习|工作|微醺|喝酒|读书)(?:时|的时候|要)?(?:想听|听|要听).{1,120}"
)
_TASK_CONSTRAINT = re.compile(
    r"(?:(?:这次|本次)?(?:不要|别|排除)(?:放|推荐|加入|给我)?(?:现场版|LIVE|纯音乐|人声)|(?:保留|锁定)(?:前|第)\d+(?:首|首歌))", re.I
)
_EXACT_QUANTITY_ITEM = (
    r"(?:(?:正好|刚好|恰好|总共|总计|一共|合计|共|总数|数量|曲数|首数)(?:为|是)?\s*)?"
    r"[0-9零〇一二两三四五六七八九十百千]+\s*(?:首|曲)(?:歌曲|曲目|歌)?|"
    r"(?:(?:正好|刚好|恰好|总共|总计|一共|合计|共|总时长|时长)(?:为|是)?\s*)?\d+\s*分钟"
)
_EXACT_QUANTITY_CONSTRAINT = re.compile(
    rf"(?:{_EXACT_QUANTITY_ITEM})(?:\s*(?:、|和|且)\s*(?:{_EXACT_QUANTITY_ITEM}))*"
)
_REVISION = re.compile(
    r"(?:(?:请你|请|帮我|把|将|再)){0,3}(?:(?:这份|当前|刚才的|这个)?歌单)?"
    r"(?:(?:时长)?(?:改成|改为|调整为|调整成|换成|换一批|更轻快|更舒缓|少点人声|多点人声).{0,150}|"
    r"继续(?:刚才那份|刚才的|当前|这份|那份)?(?:通勤|跑步|学习|睡前|工作)?歌单)"
)

_MEMORY_UNSAFE = re.compile(
    r"他说|她说|别人说|朋友说|引用|转述|假设|假如|例如|比如|举例|示范|示例|如果|除非|只有.{0,40}才|"
    r"身份证|住址|病史|诊断|患有|密码|口令|API[ _-]?KEY|secret|token|cookie|Bearer",
    re.I,
)
_CLEAR_PREFERENCE = re.compile(
    r"(?:(?:忘掉|忘记|清空|删除|清除)(?:全部|所有)?(?:我的|我|已保存的|保存的|长期的)*(?:音乐偏好|音乐喜好|音乐口味|偏好记忆|口味记忆|偏好|喜好|口味)(?:全部|所有)?|"
    r"把(?:我的)?(?:全部|所有)(?:音乐)?(?:偏好|喜好|口味)(?:清空|删除|清除)|"
    r"把(?:我)?(?:这台电脑上|本机)(?:已经存的|已保存的|保存的)?(?:音乐)?(?:偏好|喜好|口味)(?:全部|全|都)?(?:清空|删除|清除)|"
    r"把(?:我的|我)?(?:已经存的|已保存的|保存的)?(?:音乐)?(?:偏好|喜好|口味)(?:全部|全|都)(?:清空|删除|清除))"
)


def preferenceCommand(source):
    """封闭识别音乐偏好的增删清空；引号只允许包住删除目标，不能包住命令。"""
    if not isinstance(source, str) or len(source) > 1000 or _MEMORY_UNSAFE.search(source):
        return None
    command = re.sub(r"^(?:请帮我|请你|请|麻烦你|麻烦|帮我)\s*", "", source.strip())
    command = re.sub(r"[。！!\s]+$", "", command)
    # 指向本轮完整音乐偏好陈述的礼貌保存请求，与询问助手是否自动记忆分开。
    polite = re.fullmatch(
        r"(?:以后|今后)(?:给我推荐的时候|推荐(?:音乐|歌曲|歌单)?时|给我推荐时)[，,]?\s*(.+?)[。；;]\s*"
        r"(?:能|可以|麻烦你)(?:把|将)(?:这条|这一条|这项偏好)(?:记在|保存在|保存到)(?:这台电脑上|本机)(?:吗|么)?[？?]?",
        command,
    )
    if polite and _PREFERENCE.fullmatch(polite[1].strip()):
        return PreferenceCommand("add", polite[1].strip(), True)
    if re.search(r"[？?]|(?:吗|么)\s*$", command):
        return None
    clear = re.sub(r"[，,；;]\s*我会(?:在(?:弹窗|本机)里)?确认$", "", command)
    if _CLEAR_PREFERENCE.fullmatch(clear):
        return PreferenceCommand("clear")
    # 删除只接受完整音乐条目；后续按已确认摘要逐项精确匹配，不用包含关系猜目标。
    saved_scope = r"(?:我(?:已经|已)?(?:保存|存)的|(?:已经|已)?(?:保存|存)的|我的|我)?"
    # 把字句与动宾句共用目标及作用域；去掉尾部说明后仍须完整匹配，不能吞掉其他动作。
    remove_body = re.sub(
        r"[，,；;]\s*(?:其他|其余)(?:的|[一二两三四五六七八九十\d]+条)?(?:音乐)?(?:偏好)?(?:请)?(?:别动|不变|不要动|保持不变|保留)$", "", command
    )
    inverted = re.fullmatch(
        rf"把{saved_scope}(.+?)(?:这条|这一条)(?:音乐)?(?:偏好|喜好|口味)(?:去掉|删除|移除|忘掉|删掉)",
        remove_body,
    )
    remove = re.fullmatch(
        rf"(?:把{saved_scope}(?:那条|这条)?[“\"]([^“”\"]+)[”\"](?:这条偏好)?(?:去掉|删除|移除|忘掉)|"
        rf"(?:只|仅)?(?:删除|移除|忘掉){saved_scope}(.+?)(?:这条|这一条)(?:音乐)?(?:偏好|喜好|口味))"
        r"(?:[，,；;]\s*(?:其他|其余)(?:的|[一二两三四五六七八九十\d]+条)?(?:音乐)?(?:偏好)?(?:请)?(?:别动|不变|不要动|保持不变|保留))?",
        command,
    )
    if remove or inverted:
        value = (inverted[1] if inverted else remove[1] or remove[2]).strip()
        # 参数引号可以包住完整条目，不能剥除整条指令的引号来取得授权。
        if value.startswith(("“", '"')):
            closing = "”" if value[0] == "“" else '"'
            if not value.endswith(closing):
                return None
            value = value[1:-1]
        return PreferenceCommand("remove", value) if _PREFERENCE.fullmatch(value) else None
    if re.search(r"[“”‘’\"'`「」『』]|今晚|今天|本次|这次|临时|暂时|喝酒|饮酒", command):
        return None
    add = re.fullmatch(r"(?:记住我|长期保存我|永久保存我)(?:以后|今后)?(.+)", command)
    if add and _PREFERENCE.fullmatch(add[1].strip()):
        return PreferenceCommand("add", add[1].strip())
    # 先陈述口味再要求保存，与先写保存动词共用同一音乐条目契约。
    trailing_save = re.fullmatch(
        r"我(.+?)[，,；;]\s*(?:请帮我|请你|请|麻烦你|麻烦|帮我)?(?:记住|保存)(?:这条|这一条|这个)?(?:音乐)?(?:偏好|喜好|口味)?",
        command,
    )
    if trailing_save and _PREFERENCE.fullmatch(trailing_save[1].strip()):
        return PreferenceCommand("add", trailing_save[1].strip())
    brief = re.fullmatch(r"(?:记一下|记下来)[，,]?\s*(?:以后|今后)(.+)", command)
    if brief and _PREFERENCE.fullmatch(brief[1].strip()):
        return PreferenceCommand("add", brief[1].strip())
    return None


def preferenceManagementOnly(instruction):
    """识别仅讨论偏好管理的用途；不授权保存，也不把复合找歌吞成偏好。"""
    if not isinstance(instruction, str) or len(instruction) > 1000:
        return False
    # 可完整解析的单一偏好命令已有明确用途，仍由权限守卫判断是否可以执行。
    if preferenceCommand(instruction):
        return True
    understood = understandRequest(instruction)
    if understood and any(item.kind == "music" for item in understood.requirements):
        return False
    # “不要记住”仅限制写入；自身不构成偏好管理请求，更不能挡住临时音乐需求。
    management_source = re.sub(
        r"(?:不要|别|不用|无需|不想|不希望|不能|禁止)[^，,。；;！!\n]{0,16}(?:记住|记一下|记下来|保存|删除|删掉|移除|去掉|忘掉|忘记|清空|清除|修改|调整)",
        "", instruction,
    )
    has_management = bool(re.search(
        r"(?:记住|记一下|记下来)|(?:保存|删除|删掉|移除|去掉|忘掉|忘记|清空|清除|修改|调整).{0,60}(?:偏好|喜好|口味)|"
        r"(?:偏好|喜好|口味).{0,30}(?:保存|删除|删掉|移除|去掉|清空|清除|修改|调整)", management_source
    ))
    if not has_management:
        return False
    for clause in re.split(r"[，,。；;！!\n]+|然后|并且|同时|以及", instruction):
        scoped_command = re.sub(r"^(?:然后|并且|同时|以及|另外|再)\s*", "", clause.strip())
        scoped_command = re.sub(r"^(?:请帮我|请你|请|麻烦你|麻烦|帮我)\s*", "", scoped_command)
        # 与完整编译共用名词式音乐请求；即使另有模型处理的限定，也不能把整轮误称纯偏好。
        if _NOMINAL_MUSIC_REQUEST.fullmatch(scoped_command):
            return False
        command = re.sub(r"^(?:(?:请你|请|帮我|给我|为我|再|只)){0,4}", "", clause.strip())
        # 这些是独立业务动作；否定保存只是找歌约束，不能据此把整轮改成偏好管理。
        if re.match(r"(?:搜索|查询|查找|查一下|查|看看|查看|找一下|播放|放一首|找(?=《))", command) or re.match(
            r"(?:推荐|找|生成|整理|来).*(?:歌|音乐|人声|轻快|舒缓)", command
        ):
            return False
        if re.search(r"新歌|新作|新专辑|(?:时长)?(?:改成|改为|换成).*(?:分钟|歌单)", command):
            return False
        if _SCENE_REQUEST.fullmatch(command.removeprefix("我")) or re.match(r"(?:暂停|取消|恢复|继续|停止|撤回|撤销)", command):
            return False
    return True


def applyPreferenceCommand(previous, command):
    """计算确认后的完整候选摘要；删单项不能顺带清空或改写其他偏好。"""
    if not isinstance(previous, str):
        return None
    if command.operation == "clear":
        return ""
    if command.operation == "add":
        return mergePreference(previous, command.value)
    items = [item.strip() for item in re.split(r"[；;。\n]+", previous) if item.strip()]
    if items.count(command.value) != 1:
        return None
    remaining = [item for item in items if item != command.value]
    return "；".join(remaining) if remaining else None


def understandRequest(instruction):
    """识别受限的复合需求；否定、引用与未识别分句不能变成额外动作。"""
    if not isinstance(instruction, str) or len(instruction) > 1000:
        return None
    if _QUOTED_OR_CONDITIONAL.search(instruction) or re.search(r"[？?]|(?:吗|么)\s*[。！!]*$", instruction):
        return None
    # 只按明确的动作连接词拆分；偏好里的“和/与”仍留在同一条需求内。
    clauses = [part.strip() for part in re.split(
        r"[，,。；;！!\n]+|然后|并且|同时|以及|(?<!后)(?=再(?:请|帮我|给我|为我|推荐|找|生成|整理))", instruction
    ) if part.strip()]
    if not 1 <= len(clauses) <= 8:
        return None
    requirements = []
    music_count, memory_count = 0, 0
    revision = False
    durations = []
    quantity_count = 0
    for clause in clauses:
        source = clause
        command = re.sub(r"^(?:然后|并且|同时|以及|另外|再)\s*", "", source)
        command = re.sub(r"^(?:请帮我|请你|请|麻烦你|麻烦|帮我)\s*", "", command)
        memory = re.fullmatch(r"(?:记住我|长期保存我|永久保存我)(.+)", command)
        if memory:
            value = memory[1].strip()
            if not _PREFERENCE.fullmatch(value):
                return None
            memory_count += 1
            requirements.append(Requirement("preference", source, "long_term", value))
            continue
        if re.fullmatch(
            r"(?:(?:忘掉|忘记|清空|删除|清除)(?:全部|所有)?(?:我的|我|已保存的|保存的|长期的)*(?:音乐偏好|音乐喜好|音乐口味|偏好记忆|口味记忆|偏好|喜好|口味)(?:全部|所有)?|把(?:我的)?(?:全部|所有)(?:音乐)?(?:偏好|喜好|口味)(?:清空|删除|清除))",
            command,
        ):
            memory_count += 1
            requirements.append(Requirement("preference_clear", source, "long_term"))
            continue
        confirmation = re.match(r"^(?:等我)?确认(?:保存)?(?:口味|偏好)?后再", command)
        after_confirmation = bool(confirmation)
        if confirmation:
            command = command[confirmation.end():].strip()
        if _MUSIC_REQUEST.fullmatch(command) or _NOMINAL_MUSIC_REQUEST.fullmatch(command) or _SCENE_REQUEST.fullmatch(command) or _REVISION.fullmatch(command):
            # 指定单曲、发行、删除与账号等复合能力不能被误编译为场景找歌。
            if re.search(r"新歌|新作|新专辑|搜索|播放|删除|清空|取消|暂停|恢复|撤回|记住|保存|账号|会员|购买|定时", command):
                return None
            if not re.search(r"音乐|歌曲|歌单|歌|人声|轻快|舒缓|时长|分钟|小时|一批", command):
                return None
            music_count += 1
            revision |= bool(_REVISION.fullmatch(command)) or bool(re.search(r"更|这些|当前|刚才|其余|其他|保留|锁定", command))
            # 分钟参数只提升明确整数；其余时长表达保留给模型，不擅自估算。
            if re.search(
                r"[零〇一二两三四五六七八九十百半]+(?:分钟|小时)|\d+(?:\.\d+)?小时|\d+\.\d+分钟|"
                r"[+\-−]\d+分钟|\d+\s*(?:到|至|[-~～])\s*\d+分钟", command
            ):
                return None
            durations.extend(int(value) for value in re.findall(r"(?<![\d.])(\d+)分钟", command))
            requirements.append(Requirement("music", source, "task", after_confirmation=after_confirmation))
            continue
        if not after_confirmation and _EXACT_QUANTITY_CONSTRAINT.fullmatch(command):
            # 独立数量分句仅约束本次歌单；不把范围、约数或中文时长猜成精确值。
            quantity_count += 1
            durations.extend(int(value) for value in re.findall(r"(?<![\d.])(\d+)\s*分钟", command))
            requirements.append(Requirement("constraint", source, "task", command))
            continue
        if not after_confirmation and (_TEMPORARY.fullmatch(command) or _TASK_CONSTRAINT.fullmatch(command)):
            requirements.append(Requirement("constraint", source, "task", command))
            continue
        return None
    if memory_count > 1 or music_count > 1 or not (memory_count or music_count) or quantity_count and not music_count:
        return None
    if len(set(durations)) > 1:
        return None
    # 只有临时说明时不形成任务；有确认依赖却没有记忆提议时必须追问。
    return UnderstoodRequest(tuple(requirements), durations[0] if durations else None, revision, trackCountRequest(instruction))


def mergePreference(previous, proposed):
    """保留无冲突旧摘要；不能可靠合并的冲突留给既有模型和确认流程。"""
    if not isinstance(previous, str):
        return None
    previous = previous.strip()
    if not previous or proposed in re.split(r"[；;。\n]+", previous):
        return previous or proposed
    # 对同一类有正反要求时不做字面拼接，以免把矛盾偏好保存下来。
    for token in re.findall(_MUSICAL_WORD, proposed, re.I):
        if token.lower() in previous.lower():
            return None
    return previous + "；" + proposed
