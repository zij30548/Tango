"""音乐业务Skill登记；实际执行仍是已注册能力和设备桥接，不宣称MCP或动态多Agent。"""

from .compat import clone

MUSIC_SKILLS = {
    "song.lookup": {"version": 1, "capability": "music.lookup", "execution": "device_bridge", "executor": "device",
                    "permission": "catalog.read", "timeoutMs": 15000, "output": "song_candidates",
                    "tools": ["artist.resolve", "song.search"], "effects": "read_only_unless_current_explicit_playback", "maximumCalls": 2},
    "artist.catalog": {"version": 1, "capability": "music.artist_catalog", "execution": "device_bridge", "executor": "device",
                       "permission": "catalog.read", "timeoutMs": 15000, "output": "artist_tracks",
                       "tools": ["artist.resolve", "artist.tracks"], "effects": "read_only", "maximumCalls": 2},
    "playlist.find": {"version": 1, "capability": "music.playlist_search", "execution": "device_bridge", "executor": "device",
                      "permission": "catalog.read", "timeoutMs": 15000, "output": "playlist_cards",
                      "tools": ["playlist.search", "playlist.tracks"], "effects": "read_only", "maximumCalls": 2},
    "scene.playlist": {"version": 1, "capability": "music.task", "execution": "bounded_langgraph_and_device_bridge", "executor": "task",
                       "permission": "owned_account_and_device", "stepTimeoutMs": 45000, "output": "task_preview",
                       "tools": ["artist.resolve", "artist.tracks", "playlist.search", "playlist.tracks", "playlist.compose", "constraints.validate"],
                       "effects": "preview_requires_explicit_adoption", "budget": "existing_task_and_account_budget"},
    "release.query": {"version": 1, "capability": "music.task", "execution": "bounded_langgraph_and_device_bridge", "executor": "task",
                      "permission": "owned_account_and_device", "stepTimeoutMs": 45000, "output": "task_preview",
                      "tools": ["artist.resolve", "releases.research", "constraints.validate"],
                      "effects": "preview_requires_explicit_adoption", "budget": "existing_task_and_account_budget"},
}


def musicSkills():
    """返回独立副本，调用方不能修改运行中的注册描述。"""
    return {"version": 1, "runtime": "bounded_music_workflow", "mcp": False, "dynamicMultiAgent": False,
            "skills": clone(MUSIC_SKILLS)}


def routedMusicSkills(capability, intents):
    """正式路由验证和追踪读取同一绑定，不能注册一个从未接到执行器的Skill名称。"""
    requested = {"song.lookup": "song_lookup", "artist.catalog": "artist_catalog", "playlist.find": "playlist_search",
                 "scene.playlist": "scene_research", "release.query": "release_research"}
    return [name for name, skill in MUSIC_SKILLS.items() if skill["capability"] == capability and requested[name] in intents]
