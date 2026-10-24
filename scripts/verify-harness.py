#!/usr/bin/env python3
"""Harness 统一验收入口：只运行隔离测试，不加载应用配置或安装依赖。"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time


PROJECT = Path(__file__).resolve().parents[1]
QUICK_TESTS = (
    "tests/test_calibration.py",
    "tests/test_harness_repository.py",
    "tests/test_verify_harness.py",
    "tests/test_models.py",
    "tests/test_conversation_routing.py",
    "tests/test_chat_clear.py",
    "tests/test_music_requirements.py",
    "tests/test_music_requirements_live.py",
    "tests/test_music_identity_followup.py",
    "tests/test_chat_repair_routing.py",
    "tests/test_request_understanding.py",
    "tests/test_demo_lookup_intent.py",
    "tests/test_demo_release_intent.py",
    "tests/test_demo_request_control.py",
    "tests/test_demo_route_contract.py",
    "tests/test_demo_stability.py",
    "tests/test_preview_lock_checks.py",
    "tests/test_demo_followup_boundaries.py",
    "tests/test_diagnostics.py",
    "tests/test_agent_tracing.py",
    "tests/test_conversation_tracing.py",
    "tests/test_langsmith_flow.py",
    "tests/test_task_tracing.py",
    "tests/test_task_request_receipts.py",
    "tests/test_independent_evaluation.py",
    "tests/test_independent_server.py",
    "tests/test_independent_scoring.py",
    "tests/test_repaired_quality_runner.py",
    "tests/test_repaired_quality_protocol.py",
    "tests/test_repaired_quality_scoring.py",
    "tests/test_repaired_quality_scoring_audit.py",
    "tests/test_repaired_quality_metrics.py",
    "tests/test_repaired_quality_review.py",
    "tests/test_agent_route_repairs.py",
    "tests/test_route_residual_contracts.py",
    "tests/test_lookup_context.py",
    "tests/test_lookup_regression.py",
    "tests/test_agent_repair_regression.py",
    "tests/test_track_count_contract.py",
    "tests/test_app.py",
    "tests/test_http_protocol.py",
    "tests/test_launch_local.py",
    "tests/test_storage_profile.py",
    "tests/test_model_profiles.py",
    "tests/test_multi_model_runtime.py",
    "tests/test_multi_model_storage.py",
    "tests/test_multi_model_server.py",
)
NATIVE_TARGETS = ("hosted_flow_test", "music_task_flow_test", "routing_flow_test", "lookup_context_flow_test", "chat_workspace_flow_test", "music_intent_flow_test")
ENVIRONMENT_KEYS = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TMPDIR", "SYSTEMROOT")
SCOPES = {
    "quick": "离线契约、模拟模型/曲库、临时 SQLite 与本机 HTTP；不覆盖 MySQL、Qt 音频或真实模型效果。",
    "mysql": "独立临时 MySQL/Redis 全回归（迁移用例另建 PG）；不读取应用库，不评价真实模型/曲库/音频。",
    "pg": "独立临时 PostgreSQL/Redis 兼容回归；不能与 MySQL 测试数量相加。",
    "native": "macOS 原生 Qt 六套流程顺序运行，含统一音乐需求、艺人身份、场景歌单、聊天点播清除及持久恢复；模拟模型/曲库、临时 SQLite 和生成 WAV，不是会员音乐验收。",
}


class HarnessError(Exception):
    """只携带固定原因码，避免把底层异常中的路径或凭据带进报告。"""


def clean_environment(source=None):
    source = os.environ if source is None else source
    values = {key: source[key] for key in ENVIRONMENT_KEYS if key in source}
    values.update(PYTHONNOUSERSITE="1", PYTHONUNBUFFERED="1", PYTEST_DISABLE_PLUGIN_AUTOLOAD="1")
    return values


def private_directory(path):
    if path.is_symlink():
        raise HarnessError("private_run_directory_symlink_rejected")
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    details = path.lstat()
    if not stat.S_ISDIR(details.st_mode) or details.st_uid != os.getuid() or details.st_mode & 0o077:
        raise HarnessError("private_run_directory_required")


def reserve_report(path):
    """先占用全新报告，测试失败也保存；从不覆盖已有文件或跟随目标符号链接。"""
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    return os.fdopen(os.open(path, flags, 0o600), "w", encoding="utf-8")


def require_file(path, *, executable=False):
    if not path.is_file() or (executable and not os.access(path, os.X_OK)):
        raise HarnessError("required_file_or_executable_missing")
    return str(path)


def find_program(name, environment):
    program = shutil.which(name, path=environment.get("PATH", os.defpath))
    if not program:
        raise HarnessError("required_program_missing:" + name)
    return program


def command_plan(project, tier, run_directory, environment, platform=None):
    """命令来自固定清单，不接受自定义 shell、模型端点或数据库地址。"""
    python = require_file(project / "backend_py/.venv/bin/python", executable=True)
    checker = require_file(project / "scripts/check-harness.py")
    plan = [{"id": "repository", "args": [sys.executable, checker], "cwd": project, "timeout": 60}]
    if tier == "quick":
        node = find_program("node", environment)
        for name in QUICK_TESTS:
            require_file(project / "backend_py" / name)
        node_tests = sorted((project / "service/test").glob("*.test.mjs"))
        if not node_tests:
            raise HarnessError("service_test_files_missing")
        node_tests.append(project / "scripts/development-env.test.mjs")
        for path in node_tests:
            require_file(path)
        plan.extend([
            {"id": "node_version", "args": [node, "--eval", "process.exit(Number(process.versions.node.split('.')[0]) >= 20 ? 0 : 1)"], "cwd": project, "timeout": 15},
            {"id": "python_offline", "args": [python, "-m", "pytest", "-p", "asyncio", *QUICK_TESTS, "-q", "-ra"], "cwd": project / "backend_py", "timeout": 300},
            {"id": "service_offline", "args": [node, "--test", *map(str, node_tests)], "cwd": project, "timeout": 120},
        ])
    elif tier in ("mysql", "pg"):
        runner = project / "backend_py/tests" / ("run_mysql_suite.py" if tier == "mysql" else "run_storage_suite.py")
        require_file(runner)
        plan.append({"id": tier + "_isolated", "args": [python, str(runner), "--log", str(run_directory / (tier + "-pytest.log")), "--", "tests", "-q", "-ra", "-p", "asyncio"], "cwd": project / "backend_py", "timeout": 1200})
    else:
        if (sys.platform if platform is None else platform) != "darwin":
            raise HarnessError("native_requires_macos_not_offscreen")
        qmake = next((candidate for candidate in (Path("/opt/homebrew/opt/qt@5/bin/qmake"), Path("/usr/local/opt/qt@5/bin/qmake")) if candidate.is_file() and os.access(candidate, os.X_OK)), None)
        if qmake is None:
            raise HarnessError("native_requires_qt5_qmake")
        make = find_program("make", environment)
        for target in NATIVE_TARGETS:
            source = project / "tests" / (target + ".pro")
            require_file(source)
            build = run_directory / target
            build.mkdir(mode=0o700)
            plan.extend([
                {"id": target + "_configure", "args": [str(qmake), str(source)], "cwd": build, "timeout": 60},
                {"id": target + "_build", "args": [make, "-j4"], "cwd": build, "timeout": 300},
                {"id": target + "_run", "args": [str(build / target), "-v1"], "cwd": build, "timeout": 120},
            ])
    return plan


def _signal_group(group, value):
    try:
        os.killpg(group, value)
        return True
    except ProcessLookupError:
        return False


def _wait_group(group, timeout):
    deadline = time.monotonic() + timeout
    while _signal_group(group, 0):
        if time.monotonic() >= deadline:
            return False
        time.sleep(.05)
    return True


def interrupt_process(process, *, process_group=None):
    # 仅处理 Popen(start_new_session=True) 刚创建的拥有者进程组。
    # Qt 父进程退出后仍清理该组的测试后台，不扫描端口或其他系统进程。
    if process_group is not None:
        if process_group <= 0 or process_group == os.getpgrp():
            raise HarnessError("invalid_owned_process_group")
        _signal_group(process_group, signal.SIGINT)
    elif process.poll() is None:
        process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        if process_group is None:
            process.kill()
    if process_group is not None:
        if not _wait_group(process_group, .5):
            _signal_group(process_group, signal.SIGTERM)
            if not _wait_group(process_group, .5):
                _signal_group(process_group, signal.SIGKILL)
    process.wait(timeout=5)


def execute(command, environment, log_path):
    started = time.monotonic()
    code, reason = 1, "command_start_failed"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(log_path, flags, 0o600), "wb") as output:
        try:
            process = subprocess.Popen(command["args"], cwd=command["cwd"], env=environment,
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=os.name == "posix")
        except OSError:
            return {"exitCode": code, "reason": reason, "seconds": round(time.monotonic() - started, 3)}
        try:
            code = process.wait(timeout=command["timeout"])
            reason = "passed" if code == 0 else "command_failed"
        except subprocess.TimeoutExpired:
            interrupt_process(process, process_group=process.pid if os.name == "posix" else None)
            code, reason = 124, "command_timeout"
        except KeyboardInterrupt:
            interrupt_process(process, process_group=process.pid if os.name == "posix" else None)
            code, reason = 130, "interrupted"
    return {"exitCode": code, "reason": reason, "seconds": round(time.monotonic() - started, 3)}


def run(argv=None, *, project=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("positional_tier", metavar="tier", choices=tuple(SCOPES), nargs="?", help="兼容位置参数；默认 quick。")
    parser.add_argument("--tier", dest="option_tier", choices=tuple(SCOPES), help="quick / mysql / pg / native；native 必须使用 macOS 原生音频。")
    parser.add_argument("--report", type=Path, help="全新 JSON 路径；默认在 .local/harness-runs/ 私有目录，不覆盖旧结果。")
    args = parser.parse_args(argv)
    if args.positional_tier and args.option_tier and args.positional_tier != args.option_tier:
        parser.error("位置参数与 --tier 冲突；请明确选择同一个验收层级。")
    args.tier = args.option_tier or args.positional_tier or "quick"
    project = PROJECT if project is None else Path(project)
    environment = clean_environment()
    report = {"schemaVersion": 1, "tier": args.tier, "scope": SCOPES[args.tier], "startedAt": datetime.now(timezone.utc).isoformat(), "status": "blocked", "reason": "preflight_failed", "commands": []}
    report_file = None
    result = 1
    try:
        # 默认输出必须留在本项目；先拒绝 .local 链接，避免 mkdir 穿过它建目录。
        if (project / ".local").is_symlink():
            raise HarnessError("private_run_directory_symlink_rejected")
        base = project / ".local/harness-runs"
        private_directory(base)
        run_directory = Path(tempfile.mkdtemp(prefix=datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S-"), dir=base))
        report_path = (args.report.expanduser().absolute() if args.report else run_directory / "report.json")
        report_file = reserve_report(report_path)
        plan = command_plan(project, args.tier, run_directory, environment)
        report.update(status="running", reason="running")
        for command in plan:
            print("Harness: " + command["id"], flush=True)
            outcome = execute(command, environment, run_directory / (command["id"] + ".log"))
            report["commands"].append({"id": command["id"], "command": command["args"], "cwd": str(command["cwd"]), **outcome})
            if outcome["exitCode"] != 0:
                result = outcome["exitCode"] if outcome["exitCode"] > 0 else 1
                report.update(status="failed", reason=outcome["reason"])
                break
        else:
            result = 0
            report.update(status="passed", reason="selected_tier_completed")
        print("Harness: " + report["status"] + "；范围：" + report["scope"], flush=True)
        print("私有报告：" + str(report_path), flush=True)
    except HarnessError as error:
        report["reason"] = str(error)
        print("Harness 未执行完成：" + str(error), file=sys.stderr)
    except (OSError, ValueError):
        report["reason"] = "private_report_or_run_setup_failed"
        print("Harness 无法创建私有运行目录或全新报告；已有结果未覆盖。", file=sys.stderr)
    except KeyboardInterrupt:
        result = 130
        report.update(status="failed", reason="interrupted")
    finally:
        if report_file:
            report["finishedAt"] = datetime.now(timezone.utc).isoformat()
            with report_file:
                json.dump(report, report_file, ensure_ascii=False, indent=2)
                report_file.write("\n")
                report_file.flush()
                os.fsync(report_file.fileno())
    return result


if __name__ == "__main__":
    raise SystemExit(run())
