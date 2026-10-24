"""拥有者临时MySQL上的统一需求与身份验收，不接触应用库或外部服务。"""

from test_mysql_task_runtime import f
import test_music_requirements as music


async def test_mysql_real_runtime_preserves_artist_scene_duration_and_followups(f):
    await music.test_actual_runtime_three_constraints_alias_ids_followups_and_no_plan_model(f)


async def test_mysql_real_runtime_rejects_wrong_identity_and_keeps_scene_unknown(f):
    await music.test_runtime_unverified_scene_is_honest_and_identity_failure_does_not_commit(f)


async def test_mysql_model_patch_keeps_current_requirement_and_undo_version(f):
    await music.test_runtime_model_patch_updates_current_requirement_sources_and_undo(f)
