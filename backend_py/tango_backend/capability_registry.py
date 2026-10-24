"""版本化应用能力；尚不是 MCP 工具或自主 Agent 注册表。"""

from copy import deepcopy

CAPABILITY_REGISTRY_VERSION = 4
CAPABILITY_REGISTRY = {
    "music.artist_catalog": {
        "name": "music.artist_catalog", "version": 1,
        "description": "核验歌手身份后浏览真实作品，搜索结果不自动播放",
        "inputSchema": {"type": "object", "required": ["artist", "limit"], "additionalProperties": False,
                        "properties": {"artist": {"type": "string", "minLength": 1, "maxLength": 100},
                                       "limit": {"type": "integer", "minimum": 1, "maximum": 30}}},
        "output": "device_artist_catalog", "permission": "current_turn_explicit_artist_lookup",
        "confirmation": "read_only_no_playback_or_memory", "timeoutMs": 15000, "modelBudget": 0,
        "dependencies": [], "acceptance": ["artist_id_verified", "alias_resolved_from_catalog", "search_does_not_play"],
    },
    "music.playlist_search": {
        "name": "music.playlist_search", "version": 1,
        "description": "只读查找已有歌单，由用户选择查看曲目，不替代按约束生成歌单",
        "inputSchema": {"type": "object", "required": ["keyword", "limit"], "additionalProperties": False,
                        "properties": {"keyword": {"type": "string", "minLength": 1, "maxLength": 80},
                                       "limit": {"type": "integer", "minimum": 1, "maximum": 5}}},
        "output": "device_playlist_search", "permission": "current_turn_explicit_playlist_lookup",
        "confirmation": "read_only_no_playback_or_memory", "timeoutMs": 15000, "modelBudget": 0,
        "dependencies": [], "acceptance": ["real_playlist_id_only", "search_does_not_play", "duration_request_uses_composition"],
    },
    "music.task": {
        "name": "music.task",
        "version": 1,
        "description": "找歌、新作核验和当前歌单修改，合并为一个音乐任务",
        "inputSchema": {
            "type": "object",
            "required": ["mode"],
            "additionalProperties": False,
            "properties": {
                "mode": {"enum": ["new", "continue"]},
                "taskId": {
                    "type": "string",
                    "description": "Server-bound for continue only",
                },
                "expectedRevision": {"type": "integer", "minimum": 1},
            },
        },
        "output": "task_preview",
        "permission": "owned_account_and_device",
        "confirmation": "adoption_before_playback",
        "timeoutMs": 15000,
        "modelBudget": "existing_task_budget",
        "dependencies": ["memory.propose:confirmed", "task.control:accepted"],
        "acceptance": [
            "scene_and_releases_share_one_task",
            "new_topic_does_not_revise_old_task",
            "preview_does_not_play",
        ],
    },
    "music.lookup": {
        "name": "music.lookup",
        "version": 1,
        "description": "按歌名、可选歌手与明确版本查歌；搜索不播放，当前明确播放指令须唯一匹配并核验版本",
        "inputSchema": {
            "type": "object", "required": ["artist", "title", "play"],
            "additionalProperties": False,
            "properties": {
                "artist": {"type": "string", "maxLength": 100},
                "title": {"type": "string", "minLength": 1, "maxLength": 200},
                "play": {"type": "boolean"},
                "version": {"enum": ["studio", "live"], "description": "只在当前指令明确指定录音室版或现场版时提供；未知版本不能冒充已核验"},
            },
        },
        "output": "device_song_search",
        "permission": "current_turn_explicit_lookup_or_playback",
        "confirmation": "ambiguous_match_requires_selection",
        "timeoutMs": 15000,
        "modelBudget": 0,
        "dependencies": [],
        "acceptance": ["search_does_not_play", "play_requires_verified_exact_match", "model_cannot_provide_track_id_or_url"],
    },
    "music.refine": {
        "name": "music.refine",
        "version": 1,
        "description": "在本会话相邻查找候选中按序号、已知版本或歌手只读筛选，不播放、不写偏好",
        "inputSchema": {
            "type": "object", "required": ["candidateSetId", "originMessageId", "originActionId", "operation"],
            "additionalProperties": False,
            "properties": {
                "candidateSetId": {"type": "string", "minLength": 8, "maxLength": 128},
                "originMessageId": {"type": "string", "minLength": 8, "maxLength": 128},
                "originActionId": {"type": "string", "minLength": 8, "maxLength": 128},
                "operation": {"enum": ["select", "version", "exclude_version", "artist", "other_artist"]},
                "index": {"type": "integer", "minimum": 1, "maximum": 12},
                "version": {"enum": ["studio", "live"]},
                "artist": {"type": "string", "minLength": 1, "maxLength": 200},
            },
        },
        "output": "device_readonly_candidate_selection",
        "permission": "owned_adjacent_lookup_context_and_current_turn_selector",
        "confirmation": "read_only_no_playback_or_memory",
        "timeoutMs": 0,
        "modelBudget": 0,
        "dependencies": [],
        "acceptance": ["current_visible_index_only", "version_requires_positive_metadata", "stale_context_cannot_execute", "candidate_text_cannot_authorize_side_effects"],
    },
    "memory.propose": {
        "name": "memory.propose",
        "version": 1,
        "description": "提出当前账号本机偏好变更，确认后才保存",
        "inputSchema": {
            "type": "object",
            "required": ["operation", "preference"],
            "additionalProperties": False,
            "properties": {
                "operation": {"enum": ["set", "clear"]},
                "preference": {"type": "string", "maxLength": 500},
            },
        },
        "output": "local_confirmation",
        "permission": "current_turn_explicit_permission",
        "confirmation": "explicit_local_confirmation",
        "timeoutMs": 0,
        "modelBudget": 0,
        "dependencies": [],
        "acceptance": [
            "quoted_or_negated_command_rejected",
            "cancel_keeps_memory",
            "confirmation_is_account_scoped",
        ],
    },
    "task.control": {
        "name": "task.control",
        "version": 1,
        "description": "暂停、取消、恢复或撤回当前音乐任务",
        "inputSchema": {
            "type": "object",
            "required": ["action", "taskId", "expectedRevision"],
            "additionalProperties": False,
            "properties": {
                "action": {"enum": ["pause", "cancel", "resume", "undo"]},
                "taskId": {"type": "string"},
                "expectedRevision": {"type": "integer", "minimum": 1},
            },
        },
        "output": "task_state",
        "permission": "owned_task_and_revision",
        "confirmation": "current_turn_request",
        "timeoutMs": 15000,
        "modelBudget": "resume_uses_existing_task_budget",
        "dependencies": [],
        "acceptance": [
            "stale_target_rejected",
            "control_does_not_create_research",
            "request_id_is_idempotent",
        ],
    },
    "assistant.reply": {
        "name": "assistant.reply",
        "version": 1,
        "description": "版本化产品帮助、依据已有证据的解释或必要追问",
        "inputSchema": {
            "type": "object",
            "required": ["kind", "text"],
            "additionalProperties": False,
            "properties": {
                "kind": {"enum": ["help", "explain", "history", "clarify", "unsupported"]},
                "text": {"type": "string", "maxLength": 1200},
                "trackIndex": {"type": "integer", "minimum": 0, "maximum": 30},
            },
        },
        "output": "conversation_answer",
        "permission": "owned_conversation_context",
        "confirmation": "none",
        "timeoutMs": 0,
        "modelBudget": 0,
        "dependencies": [],
        "acceptance": [
            "help_has_no_catalog_call",
            "explanation_uses_existing_evidence",
            "answer_preserves_preview",
        ],
    },
}


def capabilities():
    return deepcopy(
        {
            "version": CAPABILITY_REGISTRY_VERSION,
            "capabilities": list(CAPABILITY_REGISTRY.values()),
        }
    )
