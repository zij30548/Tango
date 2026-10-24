"""路由模型的统一输出契约；结构约束不能代替来源、权限和任务版本校验。"""

import json
import re

from .compat import clone


ROUTE_OUTPUT_CONTRACT = "tango_music_route_v3"
ROUTE_INTENTS = (
    "scene_research", "release_research", "song_lookup", "artist_catalog", "playlist_search", "conversation_history",
    "explain_result", "preference_proposal", "preference_deletion", "task_control",
    "clarification", "help", "unsupported",
)


def _object(properties, required=None):
    return {"type": "object", "properties": properties,
            "required": list(properties) if required is None else required, "additionalProperties": False}


def _string(maximum, minimum=0):
    return {"type": "string", "minLength": minimum, "maxLength": maximum}


def _action(capability, parameters):
    return _object({"capability": {"const": capability}, "source": _string(1000, 1),
                    "dependsOn": {"type": "array", "items": {"type": "integer", "minimum": 0, "maximum": 3}, "maxItems": 3},
                    "parameters": parameters})


ROUTE_OUTPUT_SCHEMA = _object({
    "intents": {"type": "array", "items": {"type": "string", "enum": list(ROUTE_INTENTS)}, "minItems": 1, "maxItems": len(ROUTE_INTENTS)},
    "slots": _object({"artist": _string(100), "targetMinutes": {"type": "integer", "minimum": 0, "maximum": 240},
                      "targetTrackCount": {"type": "integer", "minimum": 0, "maximum": 30}}, ["artist", "targetMinutes"]),
    "actions": {"type": "array", "minItems": 1, "maxItems": 4, "items": {"anyOf": [
        _action("music.task", _object({"mode": {"enum": ["new", "continue"]}})),
        _action("music.artist_catalog", _object({"artist": _string(100, 1), "limit": {"type": "integer", "minimum": 1, "maximum": 30}})),
        _action("music.playlist_search", _object({"keyword": _string(80, 1), "limit": {"type": "integer", "minimum": 1, "maximum": 5}})),
        _action("music.lookup", _object({"artist": _string(100), "title": _string(200, 1), "play": {"type": "boolean"},
                                        "version": {"enum": ["studio", "live"]}}, ["artist", "title", "play"])),
        _action("memory.propose", _object({"operation": {"enum": ["set", "clear"]}, "preference": _string(500)})),
        _action("task.control", _object({"action": {"enum": ["pause", "cancel", "resume", "undo"]}})),
        _action("assistant.reply", _object({"kind": {"enum": ["help", "explain", "history", "unsupported"]},
                                           "text": _string(1200), "trackIndex": {"type": "integer", "minimum": 0, "maximum": 30}}, ["kind", "text"])),
        _action("assistant.reply", _object({"kind": {"const": "clarify"},
                                           "text": _string(1200, 1)}, ["kind", "text"])),
    ]}},
    "missingSlots": {"type": "array", "maxItems": 1, "items": _object({
        "intent": {"enum": list(ROUTE_INTENTS)},
        "slot": {"enum": ["artist", "reference", "task", "scope", "conflict", "support", "duration"]},
        "question": _string(300, 1),
    })},
})


def routeResponseFormat(model, json_mode=True):
    """仅向文档列明支持的千问型号发送Schema，其余型号继续使用既有JSON模式。"""
    known = isinstance(model, str) and re.fullmatch(
        r"(?:qwen3\.7-(?:flash|plus|max)|qwen3\.8-(?:flash|max))(?:-(?:latest|\d{4}-\d{2}-\d{2}))?", model,
    )
    if not known or not json_mode:
        return None
    return {"type": "json_schema", "json_schema": {"name": ROUTE_OUTPUT_CONTRACT, "strict": True,
                                                    "schema": clone(ROUTE_OUTPUT_SCHEMA)}}


def routeSchemaPrompt():
    """提示词与HTTP共用相同机器契约，避免手写字段说明互相矛盾。"""
    return json.dumps(ROUTE_OUTPUT_SCHEMA, ensure_ascii=False, separators=(",", ":"))


def routeStructureDiagnostic(value):
    """只记录缺字段、额外字段和类型错误的数量，不输出提案值或未知键名。"""
    counts = {"invalidRoot": 0, "missingRootFields": 0, "extraRootFields": 0,
              "missingSlotFields": 0, "extraSlotFields": 0, "invalidSlotTypes": 0,
              "invalidActionShapes": 0}
    if not isinstance(value, dict):
        counts["invalidRoot"] = 1
        return counts
    counts["missingRootFields"] = len({"intents", "actions", "missingSlots"} - set(value))
    counts["extraRootFields"] = len(set(value) - {"intents", "slots", "actions", "missingSlots"})
    slots = value.get("slots", {"artist": "", "targetMinutes": 0})
    if not isinstance(slots, dict):
        counts["invalidSlotTypes"] = 1
    else:
        counts["missingSlotFields"] = len({"artist", "targetMinutes"} - set(slots))
        counts["extraSlotFields"] = len(set(slots) - {"artist", "targetMinutes", "targetTrackCount"})
        counts["invalidSlotTypes"] = sum(
            (type(v) is not str if k == "artist" else type(v) is not int)
            for k, v in slots.items() if k in {"artist", "targetMinutes", "targetTrackCount"}
        )
    actions = value.get("actions")
    if isinstance(actions, list):
        counts["invalidActionShapes"] = sum(not isinstance(v, dict) or set(v) != {"capability", "source", "dependsOn", "parameters"} for v in actions)
    return {key: min(100, value) for key, value in counts.items()}
