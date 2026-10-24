"""锁定/解除锁定后的权威检查刷新；隔离曲库与模型，不把陈旧文案当事实。"""

import pytest

from tango_backend.compat import clone
from tango_backend.errors import ApiError
from tango_backend.task_schema import previewLocksCheck, refreshPreviewLocksCheck
from test_task_runtime_migration import body, f, finish


def lock_check(task):
    """每份预览只允许一个当前锁定检查。"""
    checks = [check for check in task["preview"]["checks"] if check["key"] == "locks"]
    assert len(checks) == 1
    return checks[0]


async def test_lock_unlock_recomputes_actual_preview_check_and_persists_without_research(f):
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body())
    first, _ = await finish(runtime, created, f.user, f.time[0])
    ids = [track["id"] for track in first["preview"]["tracks"]]
    assert len(ids) >= 4
    calls = len(f.model.calls)
    unchanged = [check for check in first["preview"]["checks"] if check["key"] != "locks"]
    # 锁定非首部曲目不应重排当前歌单，也不能因为不在前缀就误报本轮锁定失败。
    request = body(action="lock", expectedRevision=first["revision"], lockedIds=[ids[1], ids[3]])
    locked = await runtime.action(first["id"], f.user, request)
    assert locked["lockedIds"] == [ids[1], ids[3]]
    assert locked["preview"]["lockedIds"] == [ids[1], ids[3]]
    assert [track["id"] for track in locked["preview"]["tracks"]] == ids
    check = lock_check(locked)
    assert check["status"] == "satisfied" and "2 首" in check["text"]
    assert check["detail"] == check["text"]
    assert [check for check in locked["preview"]["checks"] if check["key"] != "locks"] == unchanged
    assert lock_check(await runtime.get(first["id"], f.user)) == check
    replay = await runtime.action(first["id"], f.user, request)
    assert replay["revision"] == locked["revision"] and lock_check(replay) == check
    unlocked = await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=locked["revision"], lockedIds=[]))
    assert unlocked["lockedIds"] == [] and unlocked["preview"]["lockedIds"] == []
    assert lock_check(unlocked)["status"] == "satisfied" and "0 首" in lock_check(unlocked)["text"]
    assert lock_check(await runtime.get(first["id"], f.user)) == lock_check(unlocked)
    assert [track["id"] for track in unlocked["preview"]["tracks"]] == ids
    assert len(f.model.calls) == calls


async def test_invalid_or_stale_lock_cannot_refresh_checks_or_change_authoritative_preview(f):
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body())
    first, _ = await finish(runtime, created, f.user, f.time[0])
    ids = [track["id"] for track in first["preview"]["tracks"]]
    with pytest.raises(ApiError) as error:
        await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=first["revision"], lockedIds=[987654321]))
    assert error.value.code == "INVALID_LOCK"
    assert (await runtime.get(first["id"], f.user))["preview"] == first["preview"]
    locked = await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=first["revision"], lockedIds=ids[:2]))
    with pytest.raises(ApiError) as error:
        await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=first["revision"], lockedIds=[]))
    assert error.value.code == "TASK_REVISION_CONFLICT"
    current = await runtime.get(first["id"], f.user)
    assert current["lockedIds"] == ids[:2] and lock_check(current) == lock_check(locked)


async def test_undo_refreshes_locks_from_restored_actual_tracks_even_for_old_stale_check(f):
    runtime = await f.runtime(strategy="fixed")
    created = await runtime.create(f.user, body())
    first, _ = await finish(runtime, created, f.user, f.time[0])
    ids = [track["id"] for track in first["preview"]["tracks"]]
    locked = await runtime.action(first["id"], f.user, body(action="lock", expectedRevision=first["revision"], lockedIds=ids[:3]))
    turn = await runtime.action(first["id"], f.user, body(action="turn", expectedRevision=locked["revision"], instruction="保留锁定曲目，后半段轻快一点"))
    revised, _ = await finish(runtime, turn, f.user, f.time[0], label="读书 R&B 轻快", offset=100)
    stored = await runtime.store.load(first["id"], f.user)
    # 仅在拥有者临时库注入旧版本陈旧检查；撤回不应继续宣传“锁0首”。
    previous = stored["versions"][-1]["preview"]
    previous["checks"] = [
        {**check, "text": "保留 0 首锁定歌曲及顺序。", "detail": "保留 0 首锁定歌曲及顺序。"}
        if check["key"] == "locks" else clone(check) for check in previous["checks"]
    ]
    await runtime.store.save(stored)
    calls = len(f.model.calls)
    undone = await runtime.action(first["id"], f.user, body(action="undo", expectedRevision=revised["revision"]))
    assert undone["lockedIds"] == ids[:3]
    assert [track["id"] for track in undone["preview"]["tracks"]] == ids
    assert lock_check(undone)["status"] == "satisfied" and "3 首" in lock_check(undone)["text"]
    assert lock_check(undone)["detail"] == lock_check(undone)["text"]
    assert len(f.model.calls) == calls


@pytest.mark.parametrize("tracks,locked,expected,status", [
    ([1, 2, 3, 4], [2, 4], [1, 2, 3, 4], "satisfied"),
    ([1, 2, 3, 4], [], [1, 2, 3, 4], "satisfied"),
    ([1, 2, 3, 4], [9], [1, 2, 3, 4], "unmet"),
    ([1, 2, 3, 4], [2, 2], [1, 2, 3, 4], "unmet"),
    ([1, 4, 3, 2], [2, 4], [1, 2, 3, 4], "unmet"),
    ([1, 2, 2, 3, 4], [2, 4], [1, 2, 3, 4], "unmet"),
    ([1, 2, 3, 4], [2, 4], [1, 2, 2, 3, 4], "unmet"),
])
def test_lock_check_checks_actual_identity_uniqueness_and_relative_order(tracks, locked, expected, status):
    row = lambda ids: [{"id": value} for value in ids]
    check = previewLocksCheck(row(tracks), locked, row(expected))
    assert check["status"] == status and f"{len(locked)} 首" in check["text"]


def test_recomposed_preview_keeps_stronger_prefix_rule_than_current_lock_selection():
    tracks = [{"id": value} for value in [1, 2, 3, 4]]
    assert previewLocksCheck(tracks, [2, 4], tracks)["status"] == "satisfied"
    assert previewLocksCheck(tracks, [2, 4], tracks, requirePrefix=True)["status"] == "unmet"
    updated = [{"id": value} for value in [2, 4, 1, 3]]
    assert previewLocksCheck(updated, [2, 4], tracks, requirePrefix=True)["status"] == "satisfied"


def test_refresh_of_missing_locked_id_does_not_turn_invalid_snapshot_into_success():
    task = {"lockedIds": [9], "preview": {"tracks": [{"id": 1}], "checks": [
        {"key": "locks", "status": "satisfied", "text": "旧检查", "detail": "旧检查"},
        {"key": "style", "status": "unverified", "text": "未验证听感"},
    ]}}
    refreshPreviewLocksCheck(task)
    assert lock_check(task)["status"] == "unmet" and "1 首" in lock_check(task)["text"]
    assert task["preview"]["checks"][1] == {"key": "style", "status": "unverified", "text": "未验证听感"}
