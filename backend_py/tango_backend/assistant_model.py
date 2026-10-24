"""旧助手兼容入口；明确记忆命令仍经过保守授权校验。"""

import re
from datetime import datetime
from zoneinfo import ZoneInfo
from .compat import now_ms
from .errors import ApiError
from .model_common import exact, text, strings, envelope_json, PROMPTS
from .model_transport import createChatTransport


def invalidOutput():
    return ApiError(
        422, "INVALID_ASSISTANT_RESPONSE", "AI 未返回有效的音乐助手计划，请重试。"
    )


def validateAssistantInput(body):
    if (
        not exact(body, ["instruction", "history", "preference"])
        or not text(body["instruction"], 1000, True)
        or not strings(body["history"], 4, 1000)
        or not text(body["preference"], 500)
    ):
        raise ApiError(
            400, "INVALID_ASSISTANT_REQUEST", "音乐助手要求无效，请调整后重试。"
        )
    return {
        "instruction": body["instruction"].strip(),
        "history": [v.strip() for v in body["history"]],
        "preference": body["preference"].strip(),
    }


def directMemoryCommand(instruction):
    if (
        not isinstance(instruction, str)
        or re.search(r"""[“”‘’"'`「」『』?？]""", instruction)
        or re.search(
            r"(?:是否|能否|可不可以|能不能|会不会|可否|吗|么)|(?:不要|别|不用|无需|不想|不希望|不能).{0,12}(?:记住|记忆|保存|清空|清除|删除|忘掉|忘记)",
            instruction,
        )
    ):
        return ""
    command = re.sub(
        r"^(?:请帮我|请(?:你)?|麻烦(?:你)?|帮我)\s*", "", instruction.strip()
    )
    return re.sub(r"\s+", "", re.sub(r"[。！!\s]+$", "", command))


def allowsRemember(instruction):
    command = directMemoryCommand(instruction)
    if not command or re.search(
        r"(?:只是|仅仅|仅限|只限|只在)?(?:今晚|今天|本次|这次)|喝酒|饮酒|住址|身份证|病史|患有|诊断|密码",
        command,
    ):
        return False
    explicit = re.search(
        r"^(?:记住|长期保存|永久保存)(?:我(?:的(?:音乐)?(?:偏好|喜好|口味)(?:是|为|[:：])?|喜欢|偏爱|不喜欢|讨厌|不爱|更喜欢|偏好))",
        command,
    )
    persistent = re.search(
        r"^(?:以后|今后)(?:请)?(?:都)?(?:不要(?:给我|推荐)?|别(?:给我|推荐)?|少(?:给我|推荐)?|多(?:给我|推荐)?|优先(?:给我|推荐)?|只(?:给我|推荐)?|给我推荐|推荐)",
        command,
    )
    musical = re.search(
        r"音乐|歌曲|歌手|专辑|现场版|人声|纯音乐|伴奏|华语|粤语|国语|英语歌|日语歌|韩语歌|爵士|摇滚|民谣|古典|嘻哈|说唱|电子|流行|蓝调|轻音乐|抒情|节奏|R&B|RNB|HIPHOP|JAZZ|LOFI|LO-FI|K-POP",
        command,
        re.I,
    )
    return bool(
        (explicit or persistent)
        and musical
        and not re.search(r"(?:清空|清除|删除|忘掉|忘记).*(?:偏好|喜好|口味)", command)
    )


def allowsForget(instruction):
    command = directMemoryCommand(instruction)
    verb = r"(?:忘掉|忘记|清空|删除|清除)"
    all_ = r"(?:全部|所有)?"
    target = r"(?:我(?:的)?|已保存(?:的)?|保存(?:的)?|记住(?:的)?|长期(?:的)?)*(?:音乐偏好|音乐喜好|音乐口味|口味记忆|偏好记忆|偏好|喜好)"
    return bool(
        command
        and re.fullmatch(
            f"(?:{verb}{all_}{target}{all_}|(?:把|将){all_}{target}{all_}{verb})",
            command,
        )
    )


def validateAssistantPlan(r, input):
    if not isinstance(r, dict) or not text(r.get("note"), 300):
        raise invalidOutput()
    kind = r.get("kind")
    if kind == "radio":
        if (
            not exact(r, ["kind", "title", "queries", "excludedArtists", "note"])
            or not text(r["title"], 80, True)
            or not strings(r["queries"], 2, 80, True)
            or not r["queries"]
            or not strings(r["excludedArtists"], 20, 100, True)
        ):
            raise invalidOutput()
        return {
            "kind": kind,
            "title": r["title"].strip(),
            "queries": [v.strip() for v in r["queries"]],
            "excludedArtists": [v.strip() for v in r["excludedArtists"]],
            "note": "将按场景和风格寻找公开歌单，实际曲目由曲库提供，未进行音频分析。",
        }
    if kind == "releases":
        if not exact(r, ["kind", "artist", "note"]) or not text(r["artist"], 100, True):
            raise invalidOutput()
        return {
            "kind": kind,
            "artist": r["artist"].strip(),
            "note": "将查询曲库中的专辑与歌曲，并按可核验的发行时间排列。",
        }
    if kind == "remember":
        if (
            not exact(r, ["kind", "preference", "note"])
            or not text(r["preference"], 500, True)
            or not allowsRemember(input["instruction"])
        ):
            raise invalidOutput()
        return {
            "kind": kind,
            "preference": r["preference"].strip(),
            "note": "准备记住这些音乐偏好。",
        }
    if kind == "forget":
        if not exact(r, ["kind", "note"]) or not allowsForget(input["instruction"]):
            raise invalidOutput()
        return {"kind": kind, "note": "准备清空已保存的音乐偏好。"}
    if kind == "clarify":
        if not exact(r, ["kind", "note"]) or not text(r["note"], 300, True):
            raise invalidOutput()
        return {"kind": kind, "note": r["note"].strip()}
    raise invalidOutput()


class AssistantModel:
    def __init__(self, options=None, **kwargs):
        opts = {**(options or {}), **kwargs}
        self.transport = createChatTransport(opts)
        self.configured = self.transport.configured
        self.model = self.transport.model
        self.now = opts.get("now", now_ms)

    def dated(self, prompt):
        return (
            prompt
            + "\n当前日期（Asia/Shanghai）："
            + datetime.fromtimestamp(
                self.now() / 1000, ZoneInfo("Asia/Shanghai")
            ).strftime("%Y-%m-%d")
            + "。时间敏感请求必须交给实时曲库工具，不能使用训练记忆回答。"
        )

    async def plan(self, input, options=None):
        return validateAssistantPlan(
            envelope_json(
                await self.transport.complete(
                    {
                        "prompt": self.dated(PROMPTS["assistant-model"]["PROMPT"]),
                        "input": input,
                        "maxTokens": 1800,
                    },
                    options,
                ),
                "INVALID_ASSISTANT_RESPONSE",
                "AI 未返回有效的音乐助手计划，请重试。",
            ),
            input,
        )

    async def review(self, input, draft, options=None):
        return validateAssistantPlan(
            envelope_json(
                await self.transport.complete(
                    {
                        "prompt": self.dated(
                            PROMPTS["assistant-model"]["CRITIC_PROMPT"]
                        ),
                        "input": {"original": input, "draft": draft},
                        "maxTokens": 1800,
                    },
                    options,
                ),
                "INVALID_ASSISTANT_RESPONSE",
                "AI 未返回有效的音乐助手计划，请重试。",
            ),
            input,
        )


def createAssistantModel(options=None, **kwargs):
    return AssistantModel(options, **kwargs)
