#!/usr/bin/env python3
"""离线检查 Harness 仓库契约；不导入产品代码、不读取私密配置、不启动服务。

这里只验证声明、导航和明确的源码依赖边界，不等于安全沙箱、完整秘密
扫描或 Agent 效果评测。新增外部 HTTP 适配器时必须审查并修改白名单。
"""

from __future__ import annotations

import argparse
import ast
from datetime import date
import json
from pathlib import Path, PurePosixPath
import re
import sys
from urllib.parse import unquote, urlsplit


PRINCIPLES = frozenset({
    "context_memory", "tool_permissions", "execution_state",
    "evaluation_feedback", "observability_recovery",
})
MANIFEST = "docs/HARNESS_STATUS.json"
POLICY = "docs/HARNESS_ENGINEERING.md"
MAX_DOCUMENT_BYTES = 256 * 1024
HTTP_IMPORTS = frozenset({"httpx", "requests", "aiohttp", "urllib3", "urllib.request", "http.client"})
HTTP_ADAPTERS = frozenset({"model_transport.py", "launch_local.py", "agent_tracing.py"})
BACKEND_CONFIGURATION = frozenset({"QWEN_API_KEY", "QUIET_DATABASE_URL", "QUIET_REDIS_URL", "backend_py/.env"})
STARTUP_FILES = (
    "scripts/ensure-app-backend.sh", "scripts/start-app-backend.sh",
    "scripts/run-macos.sh", ".vscode/tasks.json", ".vscode/launch.json",
)
PRIVATE_COMPONENTS = frozenset({".git", ".local", ".data", ".venv", "node_modules", "__pycache__"})
SOURCE_SUFFIXES = frozenset({".cpp", ".h", ".hpp", ".pro", ".pri"})


class ContractError(ValueError):
    pass


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"JSON 字段重复：{key}")
        result[key] = value
    return result


def _fields(value, expected, location):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise ContractError(f"{location} 字段必须为：{', '.join(sorted(expected))}")


def _strings(value, location, *, nonempty=False):
    if not isinstance(value, list) or any(not isinstance(x, str) or not x.strip() for x in value):
        raise ContractError(f"{location} 必须是非空字符串组成的数组")
    if nonempty and not value:
        raise ContractError(f"{location} 不能为空")
    if len(value) != len(set(value)):
        raise ContractError(f"{location} 不能重复")
    return value


def _safe_file(root, relative, *, base=None):
    if not isinstance(relative, str) or not relative or "\\" in relative or "\x00" in relative:
        raise ContractError("文件引用必须为仓库内相对路径")
    path = PurePosixPath(relative)
    if path.is_absolute() or (base is None and any(p in {".", ".."} for p in relative.split("/"))):
        raise ContractError(f"文件引用不可越界：{relative}")
    if any(p in PRIVATE_COMPONENTS or p == ".env" or p.startswith(".env.") and p != ".env.example" for p in path.parts):
        raise ContractError(f"文件引用不可读取私有路径：{relative}")
    if path.suffix.lower() in {".sqlite", ".sqlite3", ".db", ".dump", ".log"}:
        raise ContractError(f"文件引用不可读取用户数据：{relative}")
    candidate = (base or root) / relative
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError, RuntimeError):
        raise ContractError(f"文件不存在或越界：{relative}") from None
    current = root
    for part in candidate.relative_to(root).parts:
        current = current / part
        if current.is_symlink():
            raise ContractError(f"文件引用不可使用符号链接：{relative}")
    if not resolved.is_file():
        raise ContractError(f"引用必须指向真实文件：{relative}")
    return resolved


def _read(path, *, maximum=MAX_DOCUMENT_BYTES):
    if path.stat().st_size > maximum:
        raise ContractError(f"文件超过检查大小上限：{path.name}")
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        raise ContractError(f"文件不可按 UTF-8 读取：{path.name}") from None


def _manifest(root):
    try:
        value = json.loads(_read(_safe_file(root, MANIFEST)), object_pairs_hook=_object)
    except json.JSONDecodeError:
        raise ContractError("HARNESS_STATUS.json 不是有效 JSON") from None
    _fields(value, {"schemaVersion", "reviewedOn", "scope", "principles", "development"}, MANIFEST)
    if type(value["schemaVersion"]) is not int or value["schemaVersion"] != 1:
        raise ContractError("schemaVersion 必须为整数 1")
    try:
        reviewed = value["reviewedOn"]
        if not isinstance(reviewed, str) or date.fromisoformat(reviewed).isoformat() != reviewed:
            raise ValueError
    except (ValueError, TypeError):
        raise ContractError("reviewedOn 必须为 YYYY-MM-DD 日期") from None
    if value["scope"] != "development-policy-and-current-agent-boundaries":
        raise ContractError("scope 必须明确区分开发约定与当前 Agent 能力")
    rows = value["principles"]
    if not isinstance(rows, list) or len(rows) != len(PRINCIPLES):
        raise ContractError("必须完整登记五类 Harness 原则")
    found = set()
    for row in rows:
        _fields(row, {"id", "title", "status", "implemented", "evidence", "tests", "missing"}, "principle")
        ident = row["id"]
        if not isinstance(ident, str) or ident not in PRINCIPLES or ident in found:
            raise ContractError("原则 id 未知或重复")
        found.add(ident)
        if not isinstance(row["title"], str) or not row["title"].strip():
            raise ContractError(f"{ident}.title 不能为空")
        status = row["status"]
        if not isinstance(status, str) or status not in {"pending", "partial", "complete"}:
            raise ContractError(f"{ident}.status 必须为 pending / partial / complete")
        for key in ("implemented", "evidence", "tests", "missing"):
            _strings(row[key], f"{ident}.{key}")
        if status == "pending" and row["implemented"]:
            raise ContractError(f"{ident} 为 pending 时不能声称已有实现")
        if status != "pending" and any(not row[key] for key in ("implemented", "evidence", "tests")):
            raise ContractError(f"{ident} 已有能力必须同时登记实现、证据和测试")
        if status == "complete" and row["missing"]:
            raise ContractError(f"{ident} 仍有待验收项，不能声明 complete")
        if status != "complete" and not row["missing"]:
            raise ContractError(f"{ident} 未完成时必须说明待验收项")
        for reference in row["evidence"]:
            _safe_file(root, reference)
        for reference in row["tests"]:
            test = _safe_file(root, reference)
            parts = test.relative_to(root).parts
            if parts[0] not in {"tests", "backend_py", "scripts"} or (
                parts[0] == "backend_py" and (len(parts) < 3 or parts[1] != "tests")
            ) or test.suffix not in {".py", ".cpp", ".mjs"} or (
                parts[0] == "scripts" and not test.name.endswith(".test.mjs")
            ):
                raise ContractError(f"测试必须引用当前测试源码：{reference}")
    development = value["development"]
    _fields(development, {"policy", "roadmap", "activePlan", "progress", "checkCommand"}, "development")
    if development["policy"] != POLICY or development["checkCommand"] != "python3 scripts/check-harness.py":
        raise ContractError("development 的 policy 或 checkCommand 不是约定入口")
    for key in ("policy", "roadmap", "activePlan", "progress"):
        _safe_file(root, development[key])
    return value


def _navigation(root):
    manifest = _manifest(root)
    required = {MANIFEST, POLICY, *(manifest["development"][key] for key in ("roadmap", "activePlan", "progress"))}
    agents_targets = set()
    count = 0
    for relative in ("AGENTS.md", POLICY):
        document = _safe_file(root, relative)
        content = _read(document, maximum=10000 if relative == "AGENTS.md" else MAX_DOCUMENT_BYTES)
        if relative == "AGENTS.md" and len(content.splitlines()) > 120:
            raise ContractError("AGENTS.md 超过 120 行；详细说明应放到导航文档")
        # 只检查显式 Markdown 链接；不把代码样例中的路径当行为证据。
        content = re.sub(r"```.*?```", "", content, flags=re.DOTALL)
        for match in re.finditer(r"!?\[[^\]\n]*\]\((<[^>\n]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)", content):
            target = match.group(1).strip("<>")
            try:
                parsed = urlsplit(target)
            except ValueError:
                raise ContractError(f"Markdown 链接格式无效：{relative}") from None
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target_file = _safe_file(root, unquote(parsed.path), base=document.parent)
            if relative == "AGENTS.md":
                agents_targets.add(target_file.relative_to(root).as_posix())
            count += 1
    if required - agents_targets:
        raise ContractError("AGENTS.md 缺少必要导航：" + ", ".join(sorted(required - agents_targets)))
    return count


def _imported_modules(node):
    if isinstance(node, ast.Import):
        return [alias.name for alias in node.names]
    if isinstance(node, ast.ImportFrom):
        module = node.module or ""
        return [module, *(f"{module}.{a.name}" if module else a.name for a in node.names)]
    if isinstance(node, ast.Call) and node.args and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str):
        if isinstance(node.func, ast.Name) and node.func.id == "__import__":
            return [node.args[0].value]
        if isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name) and node.func.value.id == "importlib" and node.func.attr == "import_module":
            return [node.args[0].value]
    return []


def _architecture(root):
    package = root / "backend_py/tango_backend"
    if not package.is_dir() or package.is_symlink():
        raise ContractError("缺少当前 Python 产品包 backend_py/tango_backend")
    python_count = 0
    for candidate in sorted(package.rglob("*.py")):
        source = _safe_file(root, candidate.relative_to(root).as_posix())
        relative = source.relative_to(package).as_posix()
        try:
            tree = ast.parse(_read(source), filename=relative)
        except SyntaxError:
            raise ContractError(f"Python 产品源码语法错误：{relative}") from None
        for node in ast.walk(tree):
            for module in _imported_modules(node):
                first = module.split(".")[0]
                if first == "backend" or any(part in {"tests", "test"} for part in module.split(".")):
                    raise ContractError(f"产品代码不可反向导入测试或旧后台：{relative}:{node.lineno}")
                if any(module == name or module.startswith(name + ".") for name in HTTP_IMPORTS) and relative not in HTTP_ADAPTERS:
                    raise ContractError(f"新增 HTTP 依赖必须经过适配器审查：{relative}:{node.lineno}")
        python_count += 1
    qt_count = 0
    qt_sources = [path for path in root.iterdir() if path.suffix in SOURCE_SUFFIXES]
    for folder in (root / "src", root / "include"):
        if folder.is_dir() and not folder.is_symlink():
            qt_sources.extend(path for path in folder.rglob("*") if path.suffix in SOURCE_SUFFIXES)
    for candidate in sorted(qt_sources):
        source = _safe_file(root, candidate.relative_to(root).as_posix())
        text = _read(source)
        if any(marker in text for marker in BACKEND_CONFIGURATION):
            raise ContractError(f"Qt 产品源码不可依赖后台凭据配置：{source.relative_to(root)}")
        qt_count += 1
    retired = re.compile(r"(?<![A-Za-z0-9_])backend/(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.(?:mjs|cjs|js|json)\b")
    startup_files = set(STARTUP_FILES)
    startup_files.update(path.relative_to(root).as_posix() for path in (root / "scripts").glob("*.sh"))
    startup_files.update(path.relative_to(root).as_posix() for path in root.glob("*.command"))
    for relative in sorted(startup_files):
        text = _read(_safe_file(root, relative))
        if retired.search(text) or re.search(r"(?:--prefix|--cwd|cd)\s+[\"']?(?:\$\{?project_dir\}?/)?backend[\"']?(?:\s|$)", text):
            raise ContractError(f"启动入口不可恢复旧 JavaScript AI 后台：{relative}")
    if (root / "backend").exists():
        raise ContractError("旧 backend/ 不可重新成为当前源码入口；第三方网易云适配保留在 service/")
    return python_count, qt_count


def validate_repository(root):
    """只读检查；partial/pending 的诚实登记允许通过，不代表产品已完成。"""
    root = Path(root).resolve()
    errors = []
    checks = {}
    for name, operation in (
        ("manifest", lambda: len(_manifest(root)["principles"])),
        ("navigation", lambda: _navigation(root)),
        ("architecture", lambda: _architecture(root)),
    ):
        try:
            checks[name] = operation()
        except (ContractError, OSError) as error:
            errors.append(f"{name}: {error}")
    return {"ok": not errors, "checks": checks, "errors": errors,
            "scope": "repository-contracts-only; no model calls, private configuration, databases or services"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true", help="输出机器可读的门禁结果")
    args = parser.parse_args(argv)
    result = validate_repository(args.root)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif result["ok"]:
        print("Harness 仓库契约检查通过；能力缺口仍以 HARNESS_STATUS.json 为准。")
    else:
        print("Harness 仓库契约检查失败：", file=sys.stderr)
        for error in result["errors"]:
            print(f"- {error}", file=sys.stderr)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
