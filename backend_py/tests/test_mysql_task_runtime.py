"""真实临时 MySQL 上验证 LangGraph、租约、账本与恢复；模型和曲库均为离线夹具。"""

import asyncio
import os
import uuid
from types import SimpleNamespace
from urllib.parse import urlsplit, urlunsplit, unquote

import pytest

from tango_backend.account_quota import AccountPolicyStore
from tango_backend.compat import clone, iso, now_ms
from tango_backend.errors import ApiError
from tango_backend.task_runtime import MusicTaskRuntime
from tango_backend.task_store import TaskStore
import test_task_runtime_migration as migration


async def test_mysql_completed_preview_pause_restart_resume_never_schedules_model(f):
    await migration.test_completed_preview_pause_restart_resume_never_schedules_model(f)


@pytest.fixture
async def f():
    url = os.environ.get("TANGO_MYSQL_TEST_URL")
    if not url:
        pytest.skip("须通过独立 runner 显式提供临时 MySQL，禁止使用真实数据库")
    import asyncmy
    from tango_backend.store import MysqlStore

    parsed = urlsplit(url)
    # 只接受 runner 的本机临时入口，逐例建库，绝不清理调用方给出的库。
    assert parsed.hostname in ("127.0.0.1", "localhost", "::1")
    admin = await asyncmy.connect(
        host=parsed.hostname, port=parsed.port or 3306,
        user=unquote(parsed.username or ""), password=unquote(parsed.password or ""),
        autocommit=True, charset="utf8mb4",
    )
    database = "tango_task_" + uuid.uuid4().hex
    async with admin.cursor() as cursor:
        await cursor.execute("CREATE DATABASE `" + database + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
    target = urlunsplit((parsed.scheme, parsed.netloc, "/" + database, "", ""))
    stores, runtimes = [], []
    time = [now_ms()]
    model = migration.Model()

    async def new_store():
        store = MysqlStore(connectionString=target)
        stores.append(store)
        await store.initialize()
        return store

    async def add_user(store, label="fixture"):
        user = str(uuid.uuid4())
        await store.db.execute(
            "INSERT INTO users(id,username,username_key,password_salt,password_hash,created_at) VALUES(?,?,?,?,?,?)",
            [user, label + user[:8], label + user[:8], "fixture-salt", "fixture-hash", time[0]],
        )
        return user

    async def runtime(**options):
        account = options.pop("store", store)
        value = MusicTaskRuntime(store=account, model=options.pop("model", model), now=lambda: time[0], **options)
        runtimes.append(value)
        await value.initialize()
        return value

    try:
        store = await new_store()
        user = await add_user(store)
        store.accountPolicy = AccountPolicyStore(store)
        yield SimpleNamespace(store=store, user=user, model=model, runtime=runtime,
                              new_store=new_store, add_user=add_user, time=time, postgres=True)
    finally:
        if model.gate:
            model.gate.set()
        for value in runtimes:
            await value.close()
        for value in stores:
            await value.close()
        async with admin.cursor() as cursor:
            await cursor.execute("DROP DATABASE `" + database + "`")
        await admin.ensure_closed()


@pytest.mark.parametrize("strategy", ["fixed", "single_agent", "dual_agent"])
async def test_mysql_complete_lock_turn_undo_adopt(f, strategy):
    await migration.test_actual_langgraph_complete_lock_turn_undo_and_adopt(f, strategy)


async def test_mysql_cancel_retains_inflight_slot_and_late_completion_cannot_overwrite(f):
    await migration.test_cancel_keeps_paid_slot_until_model_settles_and_rejects_old_completion(f)


async def test_mysql_task_budget_is_not_reset_by_resume(f):
    await migration.test_budget_reservation_failure_and_explicit_resume_do_not_reset_total(f)


async def test_mysql_tool_identity_revision_replay_and_idempotency_guards(f):
    await migration.test_tool_lease_result_injection_and_idempotency_guards(f)


async def test_mysql_restore_is_observation_only_and_tool_timeout_pauses(f):
    await migration.test_tool_timeout_and_restart_are_observation_only_until_resume(f)


async def test_mysql_invalid_observer_degrades_and_provider_failure_is_explicit(f):
    await migration.test_invalid_observer_is_explicitly_degraded_but_provider_errors_fail(f)


async def test_mysql_concurrent_scene_release_handoff_and_artist_evidence(f):
    await migration.test_release_branches_concurrent_handoff_and_verified_artist(f)


@pytest.mark.parametrize('strategy',['fixed','single_agent','dual_agent'])
async def test_mysql_pure_releases_never_open_generic_scene_lane(f,strategy):
    await migration.test_pure_release_query_never_opens_generic_scene_lane(f,strategy)


async def test_mysql_validated_release_intent_controls_scene_lane(f):
    await migration.test_validated_release_intent_overrides_model_generated_scene(f)


@pytest.mark.parametrize('release_first',[False,True])
async def test_mysql_parallel_scene_does_not_bypass_artist_clarification(f,release_first):
    await migration.test_parallel_scene_result_never_bypasses_pending_artist_choice(f,release_first)


async def test_mysql_release_tracks_require_verified_artist(f):
    await migration.test_release_tracks_require_catalog_verified_identity(f)


@pytest.mark.parametrize('require_choice',[False,True])
async def test_mysql_verified_alias_keeps_artist_only_releases(f,require_choice):
    await migration.test_catalog_resolved_alias_preserves_artist_only_release_tracks(f,require_choice)


async def test_mysql_pure_release_restore_rejects_stale_scene_candidates(f):
    await migration.test_pure_release_restore_ignores_stale_scene_candidates(f)


async def test_mysql_out_of_range_releases_are_not_successful_empty_previews(f):
    await migration.test_out_of_range_release_does_not_report_empty_preview_as_success(f)


async def test_mysql_cross_worker_provider_exclusion_and_lease_recovery(f):
    await migration.test_pg_two_workers_do_not_overlap_provider_or_recover_fresh_lease(f)


async def test_mysql_unknown_attempt_remains_charged_and_never_replays_automatically(f):
    await migration.test_restart_preserves_unknown_call_charge_and_never_replays_without_resume(f)


async def test_mysql_artist_choice_cannot_inject_resolved_catalog_identity(f):
    await migration.test_artist_choice_is_catalog_bound_and_mismatched_identity_cannot_enter_evidence(f)


async def test_mysql_matches_same_input_frozen_js_workflow(f):
    await migration.test_same_input_full_workflow_matches_frozen_js_langgraph_runtime(f)


async def test_mysql_old_owner_fence_cannot_write_heartbeat_release_or_spend(f):
    runtime = await f.runtime(workerId="fence-one")
    runtime.schedule = lambda _: None
    created = await runtime.create(f.user, migration.body())
    task = await runtime.store.load(created["id"], f.user)
    first = await runtime.store.claimTask(task["id"], f.user, workerId="fence-one", now=f.time[0], leaseMs=1000)
    # 当前租约可在同一毫秒续约，不能被 MySQL changed-row 计数误判失效。
    assert await runtime.store.heartbeatTask(task["id"], first, now=f.time[0], leaseMs=1000)
    other = TaskStore(await f.new_store())
    f.time[0] += 1001
    second = await other.claimTask(task["id"], f.user, workerId="fence-two", now=f.time[0], leaseMs=1000)
    assert second["epoch"] > first["epoch"] and second["token"] != first["token"]
    assert not await runtime.store.heartbeatTask(task["id"], first, now=f.time[0])
    assert not await runtime.store.releaseTask(task["id"], first)
    changed = clone(task)
    changed["stateVersion"] += 1
    with pytest.raises(ApiError) as failure:
        await runtime.store.save(changed, expectedStateVersion=task["stateVersion"], claim=first, now=f.time[0])
    assert failure.value.code == "TASK_CLAIMED"
    with pytest.raises(ApiError) as failure:
        await runtime.store.reserveCall(task, "understand", f.time[0], f.store, 100, 2000, claim=first)
    assert failure.value.code == "TASK_CLAIMED"
    assert await other.isTaskClaimCurrent(task["id"], second, f.time[0])
    assert await runtime.store.getGlobalDailyUsage(iso(f.time[0])[:10]) == 0
    await other.releaseTask(task["id"], second)


async def test_mysql_global_concurrency_is_shared_across_workers_and_legacy(f):
    first = TaskStore(f.store)
    second = TaskStore(await f.new_store())
    other_user = await f.add_user(f.store, "second")
    results = await asyncio.gather(
        first.acquireLegacy(f.user, workerId="slot-one", now=f.time[0], globalConcurrency=1),
        second.acquireLegacy(other_user, workerId="slot-two", now=f.time[0], globalConcurrency=1),
        return_exceptions=True,
    )
    claims = [value for value in results if isinstance(value, dict)]
    errors = [value for value in results if isinstance(value, ApiError)]
    assert len(claims) == 1 and len(errors) == 1 and errors[0].code == "AI_BUSY"
    owner = first if claims[0]["userId"] == f.user else second
    assert await owner.heartbeatLegacy(claims[0], now=f.time[0])
    assert await owner.releaseLegacy(claims[0])
    assert not await owner.releaseLegacy(claims[0])
    assert (await f.store.db.fetchone("SELECT count(*) AS n FROM music_task_model_slots"))["n"] == 0
    assert (await f.store.db.fetchone("SELECT count(*) AS n FROM music_task_account_leases"))["n"] == 0


async def test_mysql_global_budget_race_and_account_rejection_rollback(f):
    first = TaskStore(f.store)
    second = TaskStore(await f.new_store())
    other_user = await f.add_user(f.store, "budget")
    claim1 = await first.acquireLegacy(f.user, workerId="budget-one", now=f.time[0])
    claim2 = await second.acquireLegacy(other_user, workerId="budget-two", now=f.time[0])
    results = await asyncio.gather(
        first.reserveLegacyUsage(f.user, f.time[0], f.store, 100, 1, claim=claim1),
        second.reserveLegacyUsage(other_user, f.time[0], second.accountStore, 100, 1, claim=claim2),
        return_exceptions=True,
    )
    assert sum(value is None for value in results) == 1
    assert [value.code for value in results if isinstance(value, ApiError)] == ["AI_GLOBAL_DAILY_LIMIT"]
    assert await first.getGlobalDailyUsage(iso(f.time[0])[:10]) == 1
    assert (await f.store.db.fetchone("SELECT SUM(request_count) AS n FROM ai_usage"))["n"] == 1
    # 全站计数先预留，随后账户总额拒绝，整个事务必须恢复原值。
    await f.store.accountPolicy.adminset(f.user, {"dailyLimit": 0, "totalLimit": 0, "enabled": True}, now=f.time[0])
    with pytest.raises(ApiError) as failure:
        await first.reserveLegacyUsage(f.user, f.time[0], f.store, 100, 100, claim=claim1)
    assert failure.value.code == "AI_TOTAL_LIMIT"
    assert await first.getGlobalDailyUsage(iso(f.time[0])[:10]) == 1
    assert (await f.store.db.fetchone("SELECT SUM(request_count) AS n FROM ai_usage"))["n"] == 1
    await first.releaseLegacy(claim1)
    await second.releaseLegacy(claim2)


async def test_mysql_checkpoint_cas_and_stranded_json_recovery_are_atomic(f):
    runtime = await f.runtime()
    runtime.schedule = lambda _: None
    created = await runtime.create(f.user, migration.body())
    task = await runtime.store.load(created["id"], f.user)
    changed = clone(task)
    changed.update(status="waiting_tools", pendingSteps=[], stateVersion=task["stateVersion"] + 1)
    await runtime.store.save(changed, expectedStateVersion=task["stateVersion"], now=f.time[0])
    with pytest.raises(ApiError) as failure:
        await runtime.store.save(changed, expectedStateVersion=task["stateVersion"], now=f.time[0])
    assert failure.value.code == "TASK_WRITE_CONFLICT"
    assert not await runtime.store.interrupted(now=f.time[0], graceMs=30000)
    f.time[0] += 30001
    rows = await runtime.store.interrupted(now=f.time[0], graceMs=30000)
    assert [row["id"] for row in rows] == [created["id"]]
    first, second = await asyncio.gather(
        runtime.store.recoverInterrupted(rows[0], now=f.time[0]),
        TaskStore(await f.new_store()).recoverInterrupted(rows[0], now=f.time[0]),
    )
    assert (first is None) != (second is None)
    restored = await runtime.store.load(created["id"], f.user)
    assert restored["status"] == "paused" and restored["stateVersion"] == changed["stateVersion"] + 1
    assert f.model.calls == []


async def test_mysql_account_daily_total_and_disable_are_enforced_before_paid_attempt(f):
    first = TaskStore(f.store)
    second = TaskStore(await f.new_store())
    await f.store.accountPolicy.adminset(f.user, {"dailyLimit": 1, "totalLimit": 2, "enabled": True}, now=f.time[0])
    claim = await first.acquireLegacy(f.user, workerId="quota-worker", now=f.time[0], leaseMs=4 * 86400000)
    outcomes = await asyncio.gather(
        first.reserveLegacyUsage(f.user, f.time[0], f.store, 100, 100, claim=claim),
        second.reserveLegacyUsage(f.user, f.time[0], second.accountStore, 100, 100, claim=claim),
        return_exceptions=True,
    )
    assert sum(value is None for value in outcomes) == 1
    assert [value.code for value in outcomes if isinstance(value, ApiError)] == ["AI_DAILY_LIMIT"]
    assert await first.getGlobalDailyUsage(iso(f.time[0])[:10]) == 1
    f.time[0] += 86400000
    await second.reserveLegacyUsage(f.user, f.time[0], second.accountStore, 100, 100, claim=claim)
    f.time[0] += 86400000
    with pytest.raises(ApiError) as failure:
        await first.reserveLegacyUsage(f.user, f.time[0], f.store, 100, 100, claim=claim)
    assert failure.value.code == "AI_TOTAL_LIMIT"
    assert await first.getGlobalDailyUsage(iso(f.time[0])[:10]) == 0
    await f.store.accountPolicy.adminset(f.user, {"dailyLimit": 100, "totalLimit": 100, "enabled": False}, now=f.time[0])
    with pytest.raises(ApiError) as failure:
        await first.reserveLegacyUsage(f.user, f.time[0], f.store, 100, 100, claim=claim)
    assert failure.value.code == "AI_ACCOUNT_DISABLED"
    assert await first.getGlobalDailyUsage(iso(f.time[0])[:10]) == 0
    assert (await first.getAccountUsage(f.user, f.time[0]))["total"]["used"] == 2
    await first.releaseLegacy(claim)
