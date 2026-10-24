"""完整识别只读问答和当前任务控制；不截取引用、否定或未知尾句取得权限。"""

import re


_UNTRUSTED_COMMAND = re.compile(
    r'''[“”‘’"'`「」『』？?]|他说|她说|别人说|朋友说|引用|转述|假设|假如|举例|例如|比如|示范|示例|如果|要是|除非|只有.{0,40}才|不是让你|没有让你|没让你'''
)
_TASK = (
    r"(?:(?:当前的?|这次的?|本次的?|这个|那个|刚才的|刚才那个|刚才那份|刚才暂停的|之前暂停的|"
    r"正在查询的|正在查找的|正在查的|正在找的)(?:音乐|查歌|找歌|歌单)?)?"
    r"(?:音乐|查歌|找歌|歌单|搜索)?(?:任务|查询|查找|搜索)"
)
_ACTIVE_REFERENCE = r"(?:正在(?:查|找|查找|查询|搜索)(?:歌|音乐)?的(?:那个|这个)(?:任务)?)"


def _text(source):
    """限制解析规模；空值和非文本不能被当作默认命令。"""
    return re.sub(r"\s+", "", source).strip() if isinstance(source, str) and 0 < len(source) <= 1000 else ""


def helpCommand(source):
    """完整覆盖能力和预览播放规则问题；附带写操作时交回正式路由。"""
    source = _text(source)
    if not source or re.search(r'''[“”‘’"'`「」『』]''', source):
        return False
    clauses = [value for value in re.split(r"[，,。；;！!？?\n]+", source) if value]
    subject = r"(?:(?:这款|这个|这|你的|你这个)?(?:音乐)?助手|探歌|Tango|你)"
    capability = (
        rf"(?:(?:请|请问|告诉我|介绍一下|介绍下))?{subject}(?:都)?(?:能|可以|会)(?:帮我)?"
        r"(?:做什么|做哪些(?:音乐相关的)?(?:事情|事)|帮什么忙|提供什么功能|提供哪些功能)"
    )
    simple = r"(?:你好|嗨|hello|hi|帮助|使用帮助|你能做什么|你会什么|怎么用|如何使用|音乐助手怎么用|你有什么功能)"
    playback = (
        r"(?:生成|推荐|做出|创建)(?:了)?歌单(?:之?后)?(?:会|是否会|会不会)(?:马上|立即|直接|自动)?"
        r"(?:开始)?(?:放歌|播放|播放音乐|播放歌单)(?:吗|么)?"
    )
    return bool(clauses) and all(re.fullmatch(rf"(?:{simple}|{capability}|{playback})", value, re.I) for value in clauses)


def memoryPolicyQuestion(source):
    """只读回答保存规则；引用或假设可作问题背景，但不能附带当前写入命令。"""
    source = _text(source)
    if not source:
        return False
    # 只隐藏成对的参数引用；不完整引号或引号内整条问题不授予任何操作。
    source = re.sub(r'“[^“”]{0,200}”|"[^"\n]{0,200}"|‘[^‘’]{0,200}’|「[^「」]{0,200}」', "引用例句", source)
    if re.search(r'''[“”‘’"'`「」『』]''', source):
        return False
    clauses = [value for value in re.split(r"[，,。；;！!？?\n]+", source) if value]
    found_question = False
    for clause in clauses:
        if re.fullmatch(
            r"(?:(?:请|请问)?(?:解释|说明|告诉我))?(?:保存|长期保存|记住|记录)(?:音乐)?(?:口味|偏好|喜好)?"
            r"(?:是否|是不是|需不需要|要不要|需要不需要|需要)(?:需要)?(?:我|用户)?(?:再次|明确)?确认(?:吗|么)?",
            clause,
        ) or re.fullmatch(
            r"(?:你|助手|系统)?(?:会|会不会|是否会)(?:不经(?:我的?)?确认就|未经确认就|直接|自动)?"
            r"(?:记住|保存|长期保存)(?:(?:我的?)?(?:音乐)?(?:口味|偏好|喜好)|我(?:喜欢|偏好|爱听)[^，,。；;]{1,60})?(?:吗|么)?",
            clause,
        ):
            found_question = True
        elif re.fullmatch(r"(?:别人|朋友|他|她)(?:写了|说了|说过|说|写)(?:引用例句)", clause):
            continue
        elif re.fullmatch(r"(?:这里|这)(?:只|仅|只是|仅仅|只是在|仅是在|是)?(?:在)?(?:举例|假设|转述|例子)", clause):
            continue
        elif re.fullmatch(r"(?:如果|假如|假设)我(?:以后|之后|今后)?(?:说|表示)(?:喜欢|偏好|爱听)[^，,。；;]{1,60}", clause):
            continue
        elif re.fullmatch(
            r"(?:现在|目前|这次|本次|也|先|请|替我|帮我)*(?:不要|别|不用|无需)(?:替我|帮我)?"
            r"(?:保存|记住|记录|长期保存)(?:这条|这个|我的)?(?:音乐)?(?:口味|偏好|喜好)?",
            clause,
        ):
            continue
        else:
            return False
    return found_question


def historyQuestion(source):
    """只读识别本会话歌手历史问题；不能把后续找歌或播放一起吞掉。"""
    source = _text(source)
    if not source or re.search(r'''[“”‘’"'`「」『』]|然后|并且|同时|以及''', source):
        return False
    source = re.sub(r"[？?。!！]+$", "", source)
    return bool(re.fullmatch(
        r"(?:到目前为止[，,]?)?(?:我|我们)?(?:之前|以前|刚才|先前|这次对话里|这段对话里|在这段对话里|在这次对话里)?"
        r"(?:问过你|问过|问了|查过|查询过|搜过|搜索过|提过|提到过)"
        r"(?:哪(?:些|几位|几个|位|两位|两名)|什么)(?:歌手|艺人)",
        source,
    ))


def controlCommand(source):
    """把完整当前任务命令映射到既有动作；等待条件不授权自动恢复。"""
    source = _text(source)
    if not source or _UNTRUSTED_COMMAND.search(source):
        return None
    clauses = [part for part in re.split(r"[，,。；;！!\n]+", source) if part]
    found, contexts = [], []
    for clause in clauses:
        command = re.sub(r"^(?:(?:请你|请|麻烦你|麻烦|帮我|先|现在|立即|马上)){1,4}", "", clause)
        operation = None
        for name, verb in [("pause", r"(?:暂停|停一下)"), ("cancel", r"(?:取消|停止)"), ("resume", r"(?:恢复|继续)")]:
            if re.fullmatch(rf"(?:{verb}(?:一下)?{_TASK}(?:一下)?|(?:把|将){_TASK}{verb}(?:一下)?)", command):
                operation = name
                break
        if re.fullmatch(rf"(?:把|将)(?:{_TASK}|{_ACTIVE_REFERENCE})(?:先|暂时)?停在(?:这儿|这里)", command):
            operation = "pause"
        elif re.fullmatch(r"(?:暂停|暂停一下|停一下)", command):
            operation = "pause"
        elif re.fullmatch(r"(?:取消|取消一下|取消掉|取消它)", command):
            operation = "cancel"
        elif re.fullmatch(r"(?:撤回|撤销)(?:刚才的|刚才|上一次的|上一条|上一次|最近的)?(?:歌单)?(?:修改|操作)?(?:一下)?", command):
            operation = "undo"
        if operation:
            found.append(operation)
        elif re.fullmatch(r"等我(?:回来|确认)(?:后)?再继续", command):
            contexts.append("pause")
        elif re.fullmatch(r"我(?:已经)?回来了", command):
            contexts.append("resume")
        elif re.fullmatch(rf"(?:{_TASK}(?:不用查了|不要查了|别再查了)|(?:先)?(?:不找了|不查了|别找了))", command):
            contexts.append("cancel")
        elif not re.fullmatch(r"(?:我要改(?:个|一个)?条件|我想改(?:个|一个)?条件|别继续往下跑|不用继续了|不要继续了)", command):
            return None
    if not found or any(value != found[0] for value in contexts):
        return None
    if found[0] == "resume" and re.search(r"别继续往下跑|不用继续了|不要继续了", source):
        return None
    return found[0] if len(set(found)) == 1 else None
