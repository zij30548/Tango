"""验收入口的隔离、失败留证和原生平台边界；不启动真实应用或数据库。"""

import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest


PROJECT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("verify_harness", PROJECT / "scripts/verify-harness.py")
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


def fixture_project(directory):
    for name in ("backend_py/.venv/bin/python", "scripts/check-harness.py", "scripts/development-env.test.mjs", "service/test/fixture.test.mjs", *["backend_py/" + name for name in verify.QUICK_TESTS]):
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture\n")
    (directory / "backend_py/.venv/bin/python").chmod(0o700)
    return directory


def report_from(directory):
    return json.loads((directory / "result.json").read_text())


def test_child_environment_drops_provider_db_proxy_and_injection_options():
    source = {"PATH": "/usr/bin", "HOME": "/fixture", "LANG": "C.UTF-8", "QWEN_API_KEY": "must-not-inherit", "QUIET_DATABASE_URL": "must-not-inherit", "TANGO_MYSQL_TEST_URL": "must-not-inherit", "HTTPS_PROXY": "must-not-inherit", "NODE_OPTIONS": "must-not-inherit", "PYTHONPATH": "must-not-inherit", "PYTEST_ADDOPTS": "must-not-inherit", "QT_QPA_PLATFORM": "offscreen"}
    actual = verify.clean_environment(source)
    assert actual == {"PATH": "/usr/bin", "HOME": "/fixture", "LANG": "C.UTF-8", "PYTHONNOUSERSITE": "1", "PYTHONUNBUFFERED": "1", "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"}
    assert source["QWEN_API_KEY"] == "must-not-inherit"


def test_quick_plan_is_explicit_offline_list_and_has_no_app_or_installer(tmp_path, monkeypatch):
    project = fixture_project(tmp_path)
    monkeypatch.setattr(verify, "find_program", lambda *args: "/fixture/node")
    plan = verify.command_plan(project, "quick", tmp_path, {})
    assert [item["id"] for item in plan] == ["repository", "node_version", "python_offline", "service_offline"]
    args = plan[2]["args"]
    assert args[1:5] == ["-m", "pytest", "-p", "asyncio"]
    assert [arg for arg in args if arg.startswith("tests/")] == list(verify.QUICK_TESTS)
    assert plan[2]["cwd"] == project / "backend_py"
    assert plan[3]["args"][1] == "--test"
    assert not any("pip" == arg or "uv" == arg or "tango_backend.server" in arg for item in plan for arg in item["args"])


@pytest.mark.parametrize("tier,runner", [("mysql", "run_mysql_suite.py"), ("pg", "run_storage_suite.py")])
def test_storage_tiers_use_owned_runner_and_private_new_log(tmp_path, tier, runner):
    project = fixture_project(tmp_path)
    (project / "backend_py/tests" / runner).write_text("fixture\n")
    plan = verify.command_plan(project, tier, tmp_path / "private-run", {})
    args = plan[1]["args"]
    assert Path(args[1]).name == runner
    assert args[2:4] == ["--log", str(tmp_path / "private-run" / (tier + "-pytest.log"))]
    assert args[4:] == ["--", "tests", "-q", "-ra", "-p", "asyncio"]
    assert not any(".env" in arg or "storage.env" in arg for arg in args)


def test_native_tier_rejects_linux_instead_of_passing_offscreen(tmp_path):
    project = fixture_project(tmp_path)
    with pytest.raises(verify.HarnessError, match="native_requires_macos_not_offscreen"):
        verify.command_plan(project, "native", tmp_path, {}, platform="linux")


def test_missing_node_fails_instead_of_silent_skip(monkeypatch):
    monkeypatch.setattr(verify.shutil, "which", lambda *args, **kwargs: None)
    with pytest.raises(verify.HarnessError, match="required_program_missing:node"):
        verify.find_program("node", {"PATH": "/fixture-empty"})


def test_first_failure_is_recorded_once_and_stops_later_commands(tmp_path, monkeypatch):
    project = fixture_project(tmp_path)
    monkeypatch.setattr(verify, "find_program", lambda *args: "/fixture/node")
    calls = []
    def failed(command, environment, log):
        calls.append(command["id"])
        return {"exitCode": 7, "reason": "command_failed", "seconds": .01}
    monkeypatch.setattr(verify, "execute", failed)
    assert verify.run(["quick", "--report", str(tmp_path / "result.json")], project=project) == 7
    report = report_from(tmp_path)
    assert report["status"] == "failed" and report["reason"] == "command_failed"
    assert calls == ["repository"]
    assert len(report["commands"]) == 1 and report["commands"][0]["exitCode"] == 7


def test_missing_dependency_has_private_blocked_report_and_never_executes(tmp_path, monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("缺依赖不能自动安装或启动任何服务")
    monkeypatch.setattr(verify, "execute", forbidden)
    assert verify.run(["--report", str(tmp_path / "result.json")], project=tmp_path) == 1
    report = report_from(tmp_path)
    assert report["status"] == "blocked" and report["commands"] == []
    assert report["reason"] == "required_file_or_executable_missing"
    assert (tmp_path / "result.json").stat().st_mode & 0o777 == 0o600
    assert (tmp_path / ".local/harness-runs").stat().st_mode & 0o777 == 0o700


@pytest.mark.parametrize("kind", ["file", "symlink"])
def test_existing_report_or_symlink_never_overwritten_or_executed(tmp_path, monkeypatch, kind):
    target = tmp_path / "result.json"
    original = tmp_path / "original.json"
    original.write_text("keep-existing-result\n")
    if kind == "file":
        target.write_text("keep-existing-result\n")
    else:
        target.symlink_to(original)
    def forbidden(*args, **kwargs):
        pytest.fail("报告冲突必须先于测试执行")
    monkeypatch.setattr(verify, "execute", forbidden)
    assert verify.run(["--report", str(target)], project=tmp_path) == 1
    assert target.read_text() == original.read_text() == "keep-existing-result\n"


def test_private_run_base_rejects_public_permissions_and_symlink(tmp_path):
    directory = tmp_path / "public"
    directory.mkdir(mode=0o755)
    directory.chmod(0o755)
    with pytest.raises(verify.HarnessError):
        verify.private_directory(directory)
    link = tmp_path / "linked"
    link.symlink_to(directory, target_is_directory=True)
    with pytest.raises(verify.HarnessError):
        verify.private_directory(link)


@pytest.mark.parametrize("tier_args,expected", [([], "quick"), (["--tier", "quick"], "quick"), (["mysql"], "mysql"), (["--tier", "mysql"], "mysql"), (["--tier", "native"], "native"), (["quick", "--tier", "quick"], "quick")])
def test_actual_cli_accepts_option_position_and_default_without_choosing_wrong_tier(tmp_path, monkeypatch, tier_args, expected):
    seen = []
    def plan(project, tier, directory, environment):
        seen.append(tier)
        return []
    monkeypatch.setattr(verify, "command_plan", plan)
    assert verify.run([*tier_args, "--report", str(tmp_path / "result.json")], project=tmp_path) == 0
    assert seen == [expected] and report_from(tmp_path)["tier"] == expected


def test_actual_cli_rejects_conflicting_tiers_before_creating_report(tmp_path):
    with pytest.raises(SystemExit) as error:
        verify.run(["quick", "--tier", "mysql", "--report", str(tmp_path / "result.json")], project=tmp_path)
    assert error.value.code == 2
    assert not (tmp_path / "result.json").exists() and not (tmp_path / ".local").exists()


@pytest.mark.parametrize("component", [".local", "harness-runs"])
def test_default_run_directory_rejects_local_ancestor_symlink_without_writing_outside(tmp_path, monkeypatch, component):
    project = tmp_path / "project"
    project.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir(mode=0o700)
    if component == ".local":
        (project / ".local").symlink_to(outside, target_is_directory=True)
    else:
        (project / ".local").mkdir(mode=0o700)
        (project / ".local/harness-runs").symlink_to(outside, target_is_directory=True)
    def forbidden(*args, **kwargs):
        pytest.fail("默认私有目录包含符号链接时不能执行命令")
    monkeypatch.setattr(verify, "execute", forbidden)
    assert verify.run([], project=project) == 1
    assert list(outside.iterdir()) == []


def test_real_child_has_clean_environment_and_output_stays_in_private_log(tmp_path, monkeypatch):
    monkeypatch.setenv("QWEN_API_KEY", "never-copy-this-value")
    monkeypatch.setenv("QUIET_DATABASE_URL", "never-copy-this-database")
    monkeypatch.setenv("LANG", "harness-fixture-output-only")
    command = {"id": "owned_fixture", "args": [sys.executable, "-c", "import os; assert 'QWEN_API_KEY' not in os.environ; assert 'QUIET_DATABASE_URL' not in os.environ; print(os.environ['LANG'])"], "cwd": tmp_path, "timeout": 10}
    monkeypatch.setattr(verify, "command_plan", lambda *args: [command])
    assert verify.run(["--report", str(tmp_path / "result.json")], project=tmp_path) == 0
    report_text = (tmp_path / "result.json").read_text()
    assert "harness-fixture-output-only" not in report_text and "never-copy-this-value" not in report_text
    assert report_from(tmp_path)["status"] == "passed"
    logs = list((tmp_path / ".local/harness-runs").glob("*/owned_fixture.log"))
    assert len(logs) == 1 and logs[0].stat().st_mode & 0o777 == 0o600
    assert "harness-fixture-output-only" in logs[0].read_text()


def test_command_start_error_never_serializes_raw_exception(tmp_path, monkeypatch):
    def fail(*args, **kwargs):
        raise OSError("fixture-secret-must-not-appear")
    monkeypatch.setattr(verify.subprocess, "Popen", fail)
    outcome = verify.execute({"args": ["missing"], "cwd": tmp_path, "timeout": 1}, {}, tmp_path / "result.log")
    assert outcome["exitCode"] == 1 and outcome["reason"] == "command_start_failed"
    assert "fixture-secret" not in json.dumps(outcome)


def test_timeout_signals_owned_runner_for_finally_cleanup(tmp_path, monkeypatch):
    class Process:
        def __init__(self):
            self.signals, self.waits = [], 0
            self.pid = 123456789
        def wait(self, timeout):
            self.waits += 1
            if self.waits == 1:
                raise subprocess.TimeoutExpired("fixture", timeout)
            return 130
        def poll(self):
            return None
        def send_signal(self, value):
            self.signals.append(value)
    process = Process()
    group_signals = []
    monkeypatch.setattr(verify, "_signal_group", lambda group, value: group_signals.append((group, value)) or True)
    monkeypatch.setattr(verify, "_wait_group", lambda group, timeout: True)
    monkeypatch.setattr(verify.subprocess, "Popen", lambda *args, **kwargs: process)
    outcome = verify.execute({"args": ["fixture"], "cwd": tmp_path, "timeout": 1}, {}, tmp_path / "result.log")
    assert outcome["exitCode"] == 124 and outcome["reason"] == "command_timeout"
    if os.name == "posix":
        assert group_signals == [(process.pid, signal.SIGINT)] and process.signals == []
    else:
        assert process.signals == [signal.SIGINT]
    assert process.waits == 3


def test_cancel_never_signals_the_harness_own_group():
    if os.name != "posix":
        pytest.skip("进程组仅用于 POSIX")
    with pytest.raises(verify.HarnessError, match="invalid_owned_process_group"):
        verify.interrupt_process(None, process_group=os.getpgrp())


@pytest.mark.skipif(os.name != "posix", reason="POSIX 拥有者进程组取消")
@pytest.mark.parametrize("trigger,expected", [("timeout", 124), ("interrupt", 130)])
def test_real_cancel_removes_stubborn_grandchild_after_parent_exits(tmp_path, monkeypatch, trigger, expected):
    marker = tmp_path / "owned-grandchild.pid"
    # 模拟 Qt 父退出而 Python 后台忽略温和信号；仅写本测试的临时 PID。
    grandchild = "import os,signal,time; from pathlib import Path; signal.signal(signal.SIGINT,signal.SIG_IGN); signal.signal(signal.SIGTERM,signal.SIG_IGN); Path(" + repr(str(marker)) + ").write_text(str(os.getpid())); time.sleep(60)"
    parent = "import subprocess,sys,time; subprocess.Popen([sys.executable,'-c'," + repr(grandchild) + "]); time.sleep(60)"
    real_popen = verify.subprocess.Popen
    children = []
    def spawn(*args, **kwargs):
        process = real_popen(*args, **kwargs)
        children.append(process)
        if trigger == "interrupt":
            real_wait = process.wait
            first = True
            def wait(timeout):
                nonlocal first
                if first:
                    first = False
                    deadline = time.monotonic() + 5
                    while not marker.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    raise KeyboardInterrupt
                return real_wait(timeout=timeout)
            process.wait = wait
        return process
    monkeypatch.setattr(verify.subprocess, "Popen", spawn)
    def running(pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        # 部分 Linux 容器 PID 1 延迟回收孤儿；僵尸已不再执行后台代码。
        proc_stat = Path("/proc") / str(pid) / "stat"
        if proc_stat.is_file() and proc_stat.read_text().split(")", 1)[1].split()[0] == "Z":
            return False
        return True
    try:
        outcome = verify.execute({"args": [sys.executable, "-c", parent], "cwd": tmp_path, "timeout": 1.5}, verify.clean_environment(), tmp_path / "cancel.log")
        assert outcome["exitCode"] == expected
        assert marker.is_file(), "隔离后台须实际启动后再验证清理"
        pid = int(marker.read_text())
        deadline = time.monotonic() + 2
        while running(pid) and time.monotonic() < deadline:
            time.sleep(.02)
        assert not running(pid), "Qt 类父进程退出后，其拥有者进程组不可遗留后台"
        assert children[0].poll() is not None
    finally:
        # 即使断言失败，也只终止本测试记录的拥有者进程组。
        for process in children:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)


def test_ci_only_runs_quick_without_provider_secrets_or_write_permissions():
    workflow = (PROJECT / ".github/workflows/harness.yml").read_text()
    assert "contents: read" in workflow and "persist-credentials: false" in workflow
    assert "python scripts/verify-harness.py --tier quick" in workflow
    assert "pull_request_target" not in workflow and "secrets." not in workflow
    assert "uv==0.12.7" in workflow and "--frozen" in workflow
    assert "verify-harness.py --tier mysql" not in workflow and "verify-harness.py --tier native" not in workflow
