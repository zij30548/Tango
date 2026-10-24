"""曲数契约的有限原文校验；不把推荐数量当作锁定已有曲目。"""

import re
import unicodedata


def parseTrackCount(value):
    """解析阿拉伯数字与常用中文整数，越界值仍保留给调用方明确拒绝。"""
    value = unicodedata.normalize("NFKC", value)
    if re.fullmatch(r"[0-9]+", value):
        return int(value) if len(value) <= 8 else 100000000
    digits = dict(zip("零〇一二两三四五六七八九", (0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9)))
    if value in digits:
        return digits[value]
    total, current, last_unit = 0, 0, 10000
    for character in value:
        if character in digits:
            current = digits[character]
        elif character in "十百千":
            unit = {"十": 10, "百": 100, "千": 1000}[character]
            if unit >= last_unit:
                return None
            total += (current or 1) * unit
            current, last_unit = 0, unit
        else:
            return None
    return total + current if total else None


def unquotedCountInstruction(instruction):
    """屏蔽引号和代码片段，引用中的数量不能成为执行授权。"""
    pairs = {'"': '"', "'": "'", "“": "”", "‘": "’", "「": "」", "『": "』", "`": "`", "《": "》"}
    close = None
    result = []
    for character in instruction:
        if close:
            if character == close:
                close = None
            result.append(" ")
        elif character in pairs:
            close = pairs[character]
            result.append(" ")
        else:
            result.append(character)
    return "".join(result)


def _trackCountSignal(instruction):
    """同时保留精确值与不支持原因，防止把未知数量当作用户没有约束。"""
    text = unquotedCountInstruction(instruction) if isinstance(instruction, str) else ""
    requests = []
    issues = []
    denied_count = False
    correction = r"(?:改成|改为|换成|换为|调整到|调整为|缩减到|增加到)"
    numbers = r"[0-9０-９零〇一二两三四五六七八九十百千]+"
    nonexact_prefix = r"(?:最多|至少|至多|最少|不超过|不多于|不高于|不低于|不少于|不小于|超过|大于|小于|低于|高于|多于|少于|不到|不满|大约|大概|约莫|差不多|大致|将近|接近|约)"
    pattern = r"(?P<number>[0-9０-９零〇一二两三四五六七八九十百千]+)\s*(?:首|曲)(?:歌曲|曲目|歌)?"
    for clause in re.split(r"[，,。!！?？;；:\n]|(?<![0-9０-９])\.|\.(?![0-9０-９])", text):
        if re.search(r"例如|举例|示例|样例|引用|转述|他说|用户说|工具说|如果|假如|假设|要是", clause):
            if re.search(pattern, clause):
                issues.append("indirect_count_request")
            continue
        corrected = False
        matches = list(re.finditer(pattern, clause))
        if not matches:
            # “数量改成八”也有明确字段，不依赖模型猜测省略的是时长还是首数。
            matches = list(re.finditer(r"(?:曲数|首数|歌曲数量|曲目数量|总数|数量)\s*(?:改成|改为|设成|设为|调整为|调整到|为|是|等于|=|：|:)?\s*(?:" + nonexact_prefix + r")?\s*(?P<number>" + numbers + r")", clause))
        for match in matches:
            before, after = clause[:match.start("number")], clause[match.end():]
            # “前三首”是位置前缀；“总共六首，保留前两首”分别表达总数与锁定。
            if re.search(r"(?:前(?:面)?(?:的)?|开头(?:的)?|头|第)\s*$", before):
                continue
            if re.search(r"(?:保留|锁定|固定|留住)(?:现有|当前|已有|这|那|的|\s)*$", before):
                continue
            if re.search(r"(?:改成|改为|换成|换为|调整到|调整为)", after):
                continue
            if re.search(correction, before) and not corrected:
                requests.clear()
                issues.clear()
                corrected = True
            if (
                re.search(nonexact_prefix + r"(?:给我|推荐|选|要|需要|提供|准备|来|只|再|的|为|是|正好|刚好|恰好|总共|总计|一共|合计|共|\s)*$", before)
                or re.match(r"\s*(?:以内|以下|以上|以外|左右|上下|前后|内|外|起|多|余|[+＋])", after)
                or re.match(r"\s*(?:到|至|[-~～—–])\s*" + numbers, after)
                or re.search(numbers + r"\s*(?:首|曲)?\s*(?:到|至|[-~～—–])\s*" + numbers + r"\s*(?:首|曲)", clause)
                or re.search(r"[-−]\s*$|[0-9０-９]\s*[.．]\s*$", before)
            ):
                issues.append("nonexact_track_count")
                continue
            if re.search(r"(?:不要|不用|不必|不想|不需要|无需|别|不能|禁止|取消|撤销|不是|并非|而非|不等于).{0,10}$", before):
                denied_count = True
                continue
            if re.search(r"^(?:不要|不用|不必|不需要|就算了|不变|保留|锁定)", after):
                continue
            if before.strip() and not re.search(r"(?:推荐|找|选|来|给|要|需要|放|听|准备|生成|整理|安排|正好|刚好|恰好|一共|总共|总计|合计|共|总数|数量|改成|改为|换成|换为|调整到|调整为|缩减到|增加到|只留|留下|保留到|保留为|控制在)", before):
                continue
            count = parseTrackCount(match["number"])
            if count is not None:
                requests.append(count)
            else:
                issues.append("unsupported_track_count")
        if not matches and re.search(numbers + r"(?:来|多|余|几)\s*(?:首|曲)", clause):
            issues.append("nonexact_track_count")
    if len(set(requests)) > 1:
        issues.append("conflicting_track_counts")
    if denied_count and not requests:
        issues.append("excluded_track_count")
    return (None, issues[0]) if issues else (requests[-1] if requests else None, None)


def trackCountRequest(instruction):
    """返回明确的精确总首数；上下界、近似、区间及冲突由独立原因交回澄清。"""
    return _trackCountSignal(instruction)[0]


def trackCountIssue(instruction):
    """返回不支持数量语义的固定原因，调用方不能静默省略本轮新约束。"""
    return _trackCountSignal(instruction)[1]


def pendingTrackCount(sources, question):
    """只供已核验的首次澄清来源使用；按时间合并数量，不从一般聊天恢复授权。"""
    count = issue = None
    for text in sources:
        current, problem = _trackCountSignal(text)
        if current is not None or problem:
            count, issue = current, problem
        elif re.search(r"(?:取消|撤销|去掉|不限制|不限)(?:目标|总)?(?:曲数|数量|首数)(?:限制|要求)?", text):
            count = issue = None
    if issue and len(sources) > 1 and re.search(r"曲数|首数|数量|确切.*(?:首|曲)|(?:几|多少)首", question):
        answer = unicodedata.normalize("NFKC", sources[-1]).strip(" 。！!\n\t")
        if re.fullmatch(r"[0-9零〇一二两三四五六七八九十百千]+", answer):
            exact = parseTrackCount(answer)
            if exact is not None:
                count, issue = exact, None
    return count, issue
