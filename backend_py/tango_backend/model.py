"""专注计划：仅解析条件，不修改播放器状态。"""

from .compat import safe_integer
from .errors import ApiError
from .model_common import exact, text, strings, envelope_json, PROMPTS
from .model_transport import createChatTransport

REQUEST_FIELDS = [
    "minutes",
    "scene",
    "source",
    "excludeLive",
    "instrumentalOnly",
    "excludedArtists",
]


def requestValid(value):
    return (
        safe_integer(value.get("minutes"))
        and 1 <= value["minutes"] <= 180
        and value.get("scene") in ["work", "study", "rest"]
        and value.get("source") in ["queue", "favorites"]
        and isinstance(value.get("excludeLive"), bool)
        and isinstance(value.get("instrumentalOnly"), bool)
        and strings(value.get("excludedArtists"), 20, 200, True)
    )


def validateInput(body):
    if (
        not exact(body, ["instruction", "currentRequest"])
        or not text(body["instruction"], 2000, True)
        or not exact(body["currentRequest"], REQUEST_FIELDS)
        or not requestValid(body["currentRequest"])
    ):
        raise ApiError(
            400,
            "INVALID_FOCUS_REQUEST",
            "听歌要求或表单条件无效，请输入 1 至 2000 字并检查条件。",
        )
    return {
        "instruction": body["instruction"].strip(),
        "currentRequest": {
            **body["currentRequest"],
            "excludedArtists": [
                v.strip() for v in body["currentRequest"]["excludedArtists"]
            ],
        },
    }


class FocusModel:
    def __init__(self, options=None, **kwargs):
        self.transport = createChatTransport(options, **kwargs)
        self.configured = self.transport.configured
        self.model = self.transport.model

    async def parse(self, input, options=None):
        result = envelope_json(
            await self.transport.complete(
                {"prompt": PROMPTS["model"]["PROMPT"], "input": input}, options
            ),
            "INVALID_MODEL_RESPONSE",
            "AI 未返回有效的完整条件，请重试或使用表单。",
        )
        if (
            not exact(result, REQUEST_FIELDS + ["unsupported"])
            or not requestValid(result)
            or not strings(result.get("unsupported"), 20, 200, True)
        ):
            raise ApiError(
                422,
                "INVALID_MODEL_RESPONSE",
                "AI 未返回有效的完整条件，请重试或使用表单。",
            )
        if result["unsupported"]:
            raise ApiError(
                422,
                "UNSUPPORTED_REQUEST",
                "当前版本仅支持时长、场景、队列/收藏、排除现场版、纯音乐和排除歌手；请调整其他要求。",
            )
        return {
            k: [v.strip() for v in result[k]] if k == "excludedArtists" else result[k]
            for k in REQUEST_FIELDS
        }


def createModel(options=None, **kwargs):
    return FocusModel(options, **kwargs)
