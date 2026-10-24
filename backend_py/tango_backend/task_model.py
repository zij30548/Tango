"""音乐任务模型适配：结构化需求、受控修改、检索建议和真实候选位置排序。"""

import json
import re
from datetime import datetime
from zoneinfo import ZoneInfo
from .compat import (
    now_ms,
    clone,
    safe_integer,
    utf16_slice,
    utf16_len,
    js_json,
    strict_json_loads,
    js_truthy,
)
from .errors import ApiError
from .model_common import PROMPTS
from .model_transport import createChatTransport
from .task_schema import clean


MODEL_ROUTING_VERSION = "qwen-task-routing-v1"
MODEL_SELECTION_REASONS = {
    "routing_flash", "flash_baseline", "plus_baseline", "simple_task",
    "multi_intent", "dependent_actions", "pending_continuation",
    "confirmed_revision", "multiple_constraints",
}


def modelMetadata(envelope):
    """只承接 HTTP 边界已经过滤的响应标识，不保存完整响应或推理。"""
    raw = envelope.get("_transport") if isinstance(envelope, dict) else None
    return (
        {k: raw.get(k) for k in ("providerModel", "providerResponseId")}
        if isinstance(raw, dict)
        else None
    )


class ModelRoutingPolicy:
    """确定性模型分工；选择模型不增加 Agent，也不授予工具权限。"""

    def __init__(self, configuration):
        self.profiles = configuration["profiles"]
        self.strategy = configuration["strategy"]
        if self.strategy not in ("flash_only", "task_split", "plus_only"):
            raise ValueError("Invalid model routing strategy")

    def snapshot(self):
        return {
            "policyVersion": MODEL_ROUTING_VERSION,
            "strategy": self.strategy,
            "profiles": {
                name: profile.publicDescriptor()
                for name, profile in self.profiles.items()
                if name == "flash" or self.strategy != "flash_only"
            },
        }

    def validate(self, snapshot):
        # 密钥不在公开指纹内；轮换密钥可以继续，改变模型或参数必须显式处理。
        if snapshot is not None:
            actual = (
                {k: v for k, v in snapshot.items() if k != "legacyFlash"}
                if isinstance(snapshot, dict) else None
            )
            if actual != self.snapshot():
                raise ApiError(
                    409, "MODEL_POLICY_MISMATCH",
                    "任务保存的模型策略与当前配置不同；未自动换模型，请恢复原配置或新建任务。",
                )

    def complexity(self, inputs):
        context = inputs.get("conversationContext")
        context = context if isinstance(context, dict) else {}
        intents = context.get("intents")
        intents = set(intents) if isinstance(intents, list) and all(isinstance(v, str) for v in intents) else set()
        if {"scene_research", "release_research"} <= intents:
            return "multi_intent"
        if safe_integer(context.get("dependencyCount")) and context["dependencyCount"] > 0:
            return "dependent_actions"
        pending = inputs.get("pendingRequest")
        pending = pending if isinstance(pending, dict) else {}
        if isinstance(pending.get("sources"), list) and len(pending["sources"]) > 1:
            return "pending_continuation"
        if inputs.get("currentBrief") and (
            inputs.get("preview") or inputs.get("hasConfirmedBrief")
        ):
            return "confirmed_revision"
        brief = inputs.get("scope") or inputs.get("currentBrief") or {}
        brief = brief if isinstance(brief, dict) else {}
        if brief.get("requireReleases") and (brief.get("scene") or brief.get("queries")):
            return "multi_intent"
        groups = [
            bool(brief.get("scene")), bool(brief.get("styles")),
            bool(brief.get("targetMinutes") or brief.get("targetTrackCount")),
            bool(brief.get("requiredArtist") or brief.get("artistOnly")),
            bool(brief.get("excludedArtists") or brief.get("excludeLive")),
            bool(brief.get("retainFirst") or brief.get("removeIds") or brief.get("avoidPrevious")),
            brief.get("catalogLanguage") in ("zh", "en"),
            bool(brief.get("requireReleases") or brief.get("since") or brief.get("until")),
        ]
        return "multiple_constraints" if sum(groups) >= 3 else None

    def select(self, kind, inputs, snapshot=None):
        if kind not in ("route", "plan", "research", "observe", "compose"):
            raise ValueError("Unknown model purpose")
        self.validate(snapshot)
        profile, reason = "flash", "simple_task"
        if kind == "route":
            reason = "routing_flash"
        elif self.strategy == "flash_only":
            reason = "flash_baseline"
        elif self.strategy == "plus_only":
            profile, reason = "plus", "plus_baseline"
        elif kind in ("plan", "research"):
            complexity = self.complexity(inputs)
            if complexity:
                profile, reason = "plus", complexity
        selected = self.profiles[profile]
        if not selected.configured:
            raise ApiError(
                503, "MODEL_PROFILE_UNAVAILABLE",
                "当前任务需要的模型尚未启用或配置完整；未自动切换其他模型。",
            )
        descriptor = selected.publicDescriptor()
        return {
            "profile": profile, "configuredModel": descriptor["model"],
            "configFingerprint": descriptor["fingerprint"],
            "policyVersion": MODEL_ROUTING_VERSION, "strategy": self.strategy,
            "purpose": kind, "reason": reason,
        }

    def validateSelection(self, selection, kind):
        if not isinstance(selection, dict):
            raise ApiError(409, "MODEL_SELECTION_INVALID", "模型选择缺少受控描述。")
        profile = self.profiles.get(selection.get("profile"))
        descriptor = profile.publicDescriptor() if profile else {}
        required = (
            "flash" if kind == "route" or self.strategy == "flash_only"
            or self.strategy == "task_split" and kind in ("observe", "compose")
            else "plus" if self.strategy == "plus_only" else None
        )
        if (
            set(selection) != {"profile", "configuredModel", "configFingerprint", "policyVersion", "strategy", "purpose", "reason"}
            or selection.get("policyVersion") != MODEL_ROUTING_VERSION
            or selection.get("strategy") != self.strategy
            or selection.get("purpose") != kind
            or selection.get("reason") not in MODEL_SELECTION_REASONS
            or selection.get("configuredModel") != descriptor.get("model")
            or selection.get("configFingerprint") != descriptor.get("fingerprint")
            or required is not None and selection.get("profile") != required
            or not profile or not profile.configured
        ):
            raise ApiError(409, "MODEL_POLICY_MISMATCH", "已预留的模型配置发生变化；未发起其他模型请求。")


def outputInvalid(result=None):
    error = ApiError(422, "INVALID_TASK_MODEL", "AI 返回的任务步骤无效。")
    if isinstance(result, dict):
        error.taskUsage = result.get("usage")
        error.taskModelMetadata = result.get("modelMetadata")
    raise error


def obj(value):
    return isinstance(value, dict)


def terms(value):
    return (
        isinstance(value, list)
        and len(value) <= 2
        and all(isinstance(s, str) and s.strip() and utf16_len(s) <= 80 for s in value)
    )


def validId(value):
    return safe_integer(value) and value > 0


def planningInput(input):
    preview = input.get("preview") or []
    if (
        not isinstance(preview, list)
        or len(preview) > 30
        or any(not obj(t) or not validId(t.get("id")) for t in preview)
        or len({t["id"] for t in preview}) != len(preview)
    ):
        outputInvalid()
    rest = {
        k: clone(v)
        for k, v in input.items()
        if k
        not in [
            "lockedIds",
            "taskId",
            "revision",
            "messages",
            "pendingRequest",
            "hasConfirmedBrief",
        ]
    }
    current = clone(input.get("currentBrief"))
    if current:
        current.pop("removeIds", None)
    rest.update(
        currentBrief=current,
        preview=[
            {
                "position": i + 1,
                "title": clean(t.get("title"), 120),
                "artist": clean(t.get("artist"), 120),
                "locked": t["id"] in input.get("lockedIds", [])
                or t.get("locked") is True,
            }
            for i, t in enumerate(preview)
        ],
        removedTrackCount=len((input.get("currentBrief") or {}).get("removeIds") or []),
    )
    return rest


def rankingInput(input):
    candidates = input.get("candidates")
    if (
        not isinstance(candidates, list)
        or len(candidates) > 60
        or any(not obj(t) or not validId(t.get("id")) for t in candidates)
        or len({t["id"] for t in candidates}) != len(candidates)
    ):
        outputInvalid()
    indices = {t["id"]: i for i, t in enumerate(candidates)}
    source = input.get("brief") if obj(input.get("brief")) else {}
    known = {
        str(int(v))
        for v in [
            *indices,
            *input.get("lockedIds", []),
            *input.get("previousOrder", []),
            *source.get("removeIds", []),
            *input.get("excludedIds", []),
        ]
        if validId(v)
    }

    def text(value):
        if not isinstance(value, str):
            return ""
        return re.sub(
            r"[0-9]+",
            lambda m: (
                (
                    "候选[" + str(indices[int(m[0])]) + "]"
                    if int(m[0]) in indices
                    else "已排除曲目"
                )
                if m[0] in known
                else m[0]
            ),
            value,
        )

    def strings(value):
        return (
            [text(s) for s in value if isinstance(s, str)]
            if isinstance(value, list)
            else []
        )

    def positions(value):
        return (
            list(
                dict.fromkeys(indices[v] for v in value if validId(v) and v in indices)
            )
            if isinstance(value, list)
            else []
        )

    brief = {
        k: text(source.get(k))
        for k in [
            "title",
            "scene",
            "artist",
            "requiredArtist",
            "since",
            "until",
            "adjustment",
            "catalogLanguage",
        ]
    }
    brief.update({k: strings(source.get(k)) for k in ["styles", "excludedArtists"]})
    brief.update(
        {
            k: source.get(k) is True
            for k in ["artistOnly", "excludeLive", "avoidPrevious", "requireReleases"]
        }
    )
    brief.update(
        {
            k: source[k] if safe_integer(source.get(k)) else 0
            for k in ["targetMinutes", "targetTrackCount", "retainFirst"]
        }
    )
    brief["removeIndices"] = positions(source.get("removeIds"))
    projected = []
    for i, t in enumerate(candidates):
        row = {
            k: text(t.get(k))
            for k in ["title", "artist", "album", "sourceName", "publishedAt"]
        }
        row.update(
            index=i,
            durationMs=t["durationMs"] if safe_integer(t.get("durationMs")) else 0,
            sourceTags=strings(t.get("sourceTags")),
        )
        projected.append(row)
    return {
        "brief": brief,
        **({"preference": text(clean(input.get("preference"), 500))} if "preference" in input else {}),
        "priorityLimit": min(12, len(candidates)),
        "candidates": projected,
        "lockedIndices": positions(input.get("lockedIds")),
        "previousOrder": positions(input.get("previousOrder")),
    }


def jsonEnvelope(envelope):
    choices = envelope.get("choices") if obj(envelope) else None
    providerUsage = envelope.get("usage") if obj(envelope) else None
    usage = (
        {
            "inputTokens": providerUsage["prompt_tokens"],
            "outputTokens": providerUsage["completion_tokens"],
        }
        if obj(providerUsage)
        and safe_integer(providerUsage.get("prompt_tokens"))
        and providerUsage["prompt_tokens"] >= 0
        and safe_integer(providerUsage.get("completion_tokens"))
        and providerUsage["completion_tokens"] >= 0
        else None
    )
    choice = (
        choices[0]
        if isinstance(choices, list) and len(choices) == 1 and obj(choices[0])
        else {}
    )
    message = choice.get("message")
    if (
        choice.get("finish_reason") != "stop"
        or not obj(message)
        or not isinstance(message.get("content"), str)
        or js_truthy(message.get("tool_calls"))
        or js_truthy(message.get("function_call"))
        or js_truthy(message.get("refusal"))
    ):
        error = ApiError(
            422, "INVALID_TASK_MODEL", "AI 返回的任务数据无效，请调整后重试。"
        )
        error.taskUsage = usage
        error.taskDiagnostic = {"reasonCounts": {"invalidEnvelope": 1}}
        raise error
    try:
        value = strict_json_loads(message["content"])
    except (ValueError, RecursionError):
        error = ApiError(422, "INVALID_TASK_MODEL", "AI 未返回有效 JSON。")
        error.taskProposal = utf16_slice(message["content"], 8000)
        error.taskUsage = usage
        error.taskDiagnostic = {"reasonCounts": {"malformedJson": 1}}
        raise error from None
    return {"value": value, "usage": usage}


class TaskModel:
    def __init__(self, options=None, **kwargs):
        opts = {**(options or {}), **kwargs}
        self.transport = createChatTransport(opts)
        self.policy = ModelRoutingPolicy(opts["modelProfiles"]) if opts.get("modelProfiles") else None
        self.transports = {
            name: createChatTransport({
                **profile.transportOptions(),
                **({"client": opts["client"]} if opts.get("client") else {}),
            })
            for name, profile in self.policy.profiles.items()
            if profile.configured
        } if self.policy else {}
        if self.policy and "flash" in self.transports:
            self.transport = self.transports["flash"]
        self.configured = self.policy.profiles["flash"].configured if self.policy else self.transport.configured
        self.model = self.transport.model
        self.now = opts.get("now", now_ms)

    def policySnapshot(self):
        return self.policy.snapshot() if self.policy else None

    def validatePolicy(self, snapshot):
        if self.policy:
            self.policy.validate(snapshot)

    def selectCall(self, kind, inputs, snapshot=None):
        return self.policy.select(kind, inputs, snapshot) if self.policy else None

    async def complete(self, prompt, input, maxTokens, options=None, *, kind=None):
        date = datetime.fromtimestamp(
            self.now() / 1000, ZoneInfo("Asia/Shanghai")
        ).strftime("%Y-%m-%d")
        options = dict(options or {})
        selection = options.get("modelSelection") or self.selectCall(kind, input)
        transport = self.transport
        if selection:
            self.policy.validateSelection(selection, kind)
            transport = self.transports[selection["profile"]]
        envelope = await transport.complete(
                {
                    "prompt": prompt + "\n当前日期（Asia/Shanghai）：" + date + "。",
                    "input": input,
                    "maxTokens": maxTokens,
                },
                options,
            )
        metadata = modelMetadata(envelope)
        try:
            result = jsonEnvelope(envelope)
        except ApiError as error:
            error.taskModelMetadata = metadata
            raise
        if metadata is not None:
            result["modelMetadata"] = metadata
        return result

    async def call(self, kind, input, options=None):
        options = dict(options or {})
        if self.policy and not options.get("modelSelection"):
            options["modelSelection"] = self.selectCall(kind, input)
        return await getattr(self, kind)(input, options)

    async def plan(self, input, options=None):
        from .task_schema import validateBrief
        from .task_brief_patch import mergeBriefPatch
        from .task_pending_request import (
            preparePendingContinuation,
            validatePendingBrief,
        )

        options = dict(options or {})
        if self.policy and not options.get("modelSelection"):
            options["modelSelection"] = self.selectCall("plan", input)
        pending = preparePendingContinuation(input)
        prompt = PROMPTS["task-model"][
            "CONTINUE_PLAN"
            if pending
            else "FOLLOWUP_PLAN"
            if input.get("currentBrief")
            else "PLAN"
        ]
        result = await self.complete(
            prompt,
            pending["providerInput"] if pending else planningInput(input),
            2200,
            options,
            kind="plan",
        )
        try:
            result["value"] = (
                validatePendingBrief(result["value"], pending)
                if pending
                else mergeBriefPatch(result["value"], input)
                if input.get("currentBrief")
                else validateBrief(result["value"], input)
            )
        except ApiError as error:
            if error.code == "INVALID_TASK_MODEL":
                error.taskProposal = utf16_slice(js_json(result["value"]), 8000)
                error.taskUsage = result["usage"]
                error.taskModelMetadata = result.get("modelMetadata")
            raise
        return result

    async def research(self, input, options=None):
        result = await self.complete(
            PROMPTS["task-model"]["RESEARCH"], input, 1000, options, kind="research"
        )
        v = result["value"]
        if (
            not obj(v)
            or not terms(v.get("queries"))
            or not isinstance(v.get("artist"), str)
            or len(v["artist"].encode("utf-16-le")) // 2 > 100
        ):
            outputInvalid(result)
        result["value"] = {
            "queries": [clean(v, 80) for v in v["queries"]],
            "artist": clean(v["artist"], 100),
            "artistId": v["artistId"] if validId(v.get("artistId")) else 0,
            "since": clean(v.get("since"), 40),
            "until": clean(v.get("until"), 40),
            "note": clean(v.get("note"), 160),
        }
        return result

    async def observe(self, input, options=None):
        result = await self.complete(
            PROMPTS["task-model"]["OBSERVE"], input, 700, options, kind="observe"
        )
        v = result["value"]
        if (
            not obj(v)
            or v.get("decision") not in ["done", "retry"]
            or not terms(v.get("queries"))
        ):
            outputInvalid(result)
        result["value"] = {
            "decision": v["decision"],
            "queries": [clean(s, 80) for s in v["queries"]],
            "note": clean(v.get("note"), 160),
        }
        return result

    async def compose(self, input, options=None):
        projected = rankingInput(input)
        try:
            result = await self.complete(
                PROMPTS["task-model"]["COMPOSE"], projected, 1200, options, kind="compose"
            )
        except ApiError as error:
            if error.code != "INVALID_TASK_MODEL":
                raise
            reasons = getattr(error, "taskDiagnostic", {}).get("reasonCounts") or {
                "invalidModelRanking": 1
            }
            return {
                "usage": getattr(error, "taskUsage", None),
                "modelMetadata": getattr(error, "taskModelMetadata", None),
                "diagnostics": {
                    "stage": "compose",
                    "outcome": "degraded",
                    "reasonCounts": reasons,
                },
                "value": {
                    "ids": [t["id"] for t in input["candidates"]],
                    "ranking": {
                        "status": "degraded",
                        "source": "catalog_order",
                        "scope": "catalog_order",
                        "rankedCount": 0,
                        "unrankedCount": len(input["candidates"]),
                        "reasonCounts": reasons,
                    },
                },
            }
        order = result["value"].get("order") if obj(result["value"]) else None
        reasons = {}
        seen = set()

        def add(key, n=1):
            reasons[key] = reasons.get(key, 0) + n

        if (
            not obj(result["value"])
            or len(result["value"]) != 1
            or not isinstance(order, list)
        ):
            add("invalidShape")
        else:
            if not order and input["candidates"]:
                add("emptyOrder")
            if len(order) > projected["priorityLimit"]:
                add("excessIndices", len(order) - projected["priorityLimit"])
            for index in order[:1000]:
                if not safe_integer(index):
                    add("invalidIndices")
                    if isinstance(index, str):
                        add("stringIndices")
                    continue
                if index < 0 or index >= len(input["candidates"]):
                    add("outOfRangeIndices")
                    continue
                if index in seen:
                    add("duplicateIndices")
                    continue
                seen.add(index)
        degraded = bool(reasons)
        priority = [] if degraded else order
        completed = (
            list(range(len(input["candidates"])))
            if degraded
            else [
                *priority,
                *(i for i in range(len(input["candidates"])) if i not in seen),
            ]
        )
        result.update(
            diagnostics={
                "stage": "compose",
                "outcome": "degraded" if degraded else "accepted",
                "reasonCounts": reasons,
            },
            value={
                "ids": [input["candidates"][int(i)]["id"] for i in completed],
                "ranking": {
                    "status": "degraded" if degraded else "model",
                    "source": "catalog_order"
                    if degraded
                    else "model_priority_then_catalog",
                    "scope": "catalog_order" if degraded else "priority_prefix",
                    "rankedCount": len(priority),
                    "unrankedCount": len(input["candidates"]) - len(priority),
                    "reasonCounts": reasons,
                },
            },
        )
        return result


def createTaskModel(options=None, **kwargs):
    return TaskModel(options, **kwargs)
