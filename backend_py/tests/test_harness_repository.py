"""门禁本身也要验证失败路径；这些测试不调用模型、数据库或本机服务。"""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


PROJECT = Path(__file__).resolve().parents[2]
SCRIPT = PROJECT / "scripts/check-harness.py"
SPEC = importlib.util.spec_from_file_location("tango_harness_check", SCRIPT)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


@pytest.fixture
def repository(tmp_path):
    def write(relative, content):
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    write("backend_py/tango_backend/example.py", "from urllib.parse import urlsplit\n")
    write("backend_py/tests/test_example.py", "def test_example():\n    assert True\n")
    write("mainwindow.cpp", "int main() { return 0; }\n")
    write("docs/HARNESS_ENGINEERING.md", "# Harness\n[路线图](AGENT_UPGRADE_ROADMAP.md)\n")
    write("docs/AGENT_UPGRADE_ROADMAP.md", "# 路线图\n")
    write("docs/exec-plans/active/mcp-tools.md", "# 未完成 MCP\n")
    write("docs/DEVELOPMENT_PROGRESS.md", "# 进展\n")
    write("AGENTS.md", "# 导航\n" + "\n".join([
        "[约定](docs/HARNESS_ENGINEERING.md)", "[能力状态](docs/HARNESS_STATUS.json)",
        "[路线图](docs/AGENT_UPGRADE_ROADMAP.md)", "[进展](docs/DEVELOPMENT_PROGRESS.md)",
        "[当前计划](docs/exec-plans/active/mcp-tools.md)",
    ]))
    for relative in gate.STARTUP_FILES:
        write(relative, "exec python -m tango_backend.server\n" if relative.endswith(".sh") else "{}\n")
    manifest = {
        "schemaVersion": 1,
        "reviewedOn": "2026-10-21",
        "scope": "development-policy-and-current-agent-boundaries",
        "principles": [
            {"id": ident, "title": ident, "status": "partial",
             "implemented": ["隔离夹具中的既有实现"],
             "evidence": ["backend_py/tango_backend/example.py"],
             "tests": ["backend_py/tests/test_example.py"],
             "missing": ["尚未验收的下一阶段能力"]}
            for ident in sorted(gate.PRINCIPLES)
        ],
        "development": {
            "policy": "docs/HARNESS_ENGINEERING.md",
            "roadmap": "docs/AGENT_UPGRADE_ROADMAP.md",
            "activePlan": "docs/exec-plans/active/mcp-tools.md",
            "progress": "docs/DEVELOPMENT_PROGRESS.md",
            "checkCommand": "python3 scripts/check-harness.py",
        },
    }
    write(gate.MANIFEST, json.dumps(manifest, ensure_ascii=False))
    return tmp_path, manifest, write


def save(repository, manifest=None):
    root, original, write = repository
    write(gate.MANIFEST, json.dumps(original if manifest is None else manifest, ensure_ascii=False))
    return gate.validate_repository(root)


def test_honest_partial_capabilities_pass_without_claiming_completion(repository):
    result = save(repository)
    assert result["ok"], result["errors"]
    assert result["checks"] == {"manifest": 5, "navigation": 6, "architecture": (1, 1)}


def test_pending_is_valid_when_no_implementation_is_claimed(repository):
    repository[1]["principles"][0].update(status="pending", implemented=[], evidence=[], tests=[])
    assert save(repository)["ok"]


def test_complete_requires_an_empty_missing_list_and_linked_evidence(repository):
    repository[1]["principles"][0].update(status="complete", missing=[])
    assert save(repository)["ok"]


@pytest.mark.parametrize("change", [
    lambda m: m.pop("scope"),
    lambda m: m.update(schemaVersion=True),
    lambda m: m.update(reviewedOn="2026-02-30"),
    lambda m: m.update(scope="all-capabilities-are-finished"),
    lambda m: m["principles"].pop(),
    lambda m: m["principles"][0].update(id=m["principles"][1]["id"]),
    lambda m: m["principles"][0].update(status={"invalid": "type"}),
    lambda m: m["principles"][0].update(status="pending"),
    lambda m: m["principles"][0].update(status="complete"),
    lambda m: m["principles"][0].update(missing=[]),
    lambda m: m["principles"][0].update(evidence=[]),
    lambda m: m["principles"][0].update(tests=["backend_py/tango_backend/example.py"]),
    lambda m: m["principles"][0].update(tests=["backend_py/tests/test_example.py"] * 2),
    lambda m: m["development"].update(checkCommand="true"),
])
def test_invalid_or_inflated_declarations_fail(repository, change):
    manifest = deepcopy(repository[1])
    change(manifest)
    result = save(repository, manifest)
    assert not result["ok"]
    assert any(error.startswith("manifest:") for error in result["errors"])


@pytest.mark.parametrize("reference", [
    "/etc/passwd", "../outside.py", "docs/../../outside.py",
    "docs/./HARNESS_ENGINEERING.md", "docs\\HARNESS_ENGINEERING.md",
    "docs/missing.md", "backend_py/.env", ".local/mysql-v1/storage.env",
    "docs/user.sqlite", "docs/private.log", ".git/config",
])
def test_manifest_references_cannot_escape_or_read_private_data(repository, reference):
    repository[1]["principles"][0]["evidence"] = [reference]
    assert not save(repository)["ok"]


@pytest.mark.parametrize("outside", [False, True])
def test_symbolic_evidence_is_rejected_even_for_existing_target(repository, tmp_path, outside):
    root, manifest, write = repository
    target = write("docs/real.md", "# 实际文件\n")
    if outside:
        target = tmp_path.parent / (tmp_path.name + "-outside.md")
        target.write_text("outside", encoding="utf-8")
    link = root / "docs/link.md"
    link.symlink_to(target)
    manifest["principles"][0]["evidence"] = ["docs/link.md"]
    assert not save(repository)["ok"]


def test_duplicate_json_fields_are_rejected(repository):
    root, _, write = repository
    write(gate.MANIFEST, '{"schemaVersion":1,"schemaVersion":1}')
    result = gate.validate_repository(root)
    assert not result["ok"]
    assert any("字段重复" in item for item in result["errors"])


@pytest.mark.parametrize("document", ["AGENTS.md", "docs/HARNESS_ENGINEERING.md"])
def test_broken_local_navigation_fails(repository, document):
    repository[2](document, "# 导航\n[不存在](missing.md)\n")
    result = save(repository)
    assert not result["ok"]
    assert any(error.startswith("navigation:") for error in result["errors"])


def test_agents_size_is_bounded_and_external_links_are_not_fetched(repository):
    root, _, write = repository
    navigation = (root / "AGENTS.md").read_text(encoding="utf-8")
    write("AGENTS.md", "[外站](https://example.invalid/never-fetch)\n" + "行\n" * 120)
    assert not save(repository)["ok"]
    write("AGENTS.md", navigation + "\n[外站](https://example.invalid/never-fetch)\n")
    assert save(repository)["ok"]


def test_agents_must_keep_the_required_policy_status_and_plan_navigation(repository):
    repository[2]("AGENTS.md", "# 空导航\n")
    result = save(repository)
    assert not result["ok"]
    assert any("缺少必要导航" in error for error in result["errors"])


def test_agents_utf8_byte_budget_is_enforced_independently_of_line_count(repository):
    repository[2]("AGENTS.md", "中" * 4000)
    assert not save(repository)["ok"]


@pytest.mark.parametrize("source", [
    "import tests.test_example\n", "from tango_backend.tests import fixture\n",
    "from .tests import fixture\n", "from backend import server\n",
    "import importlib\nimportlib.import_module('backend.server')\n",
    "__import__('tests.fixture')\n",
    "import httpx\n", "from urllib import request\n", "import http.client\n",
])
def test_product_import_boundaries_fail(repository, source):
    repository[2]("backend_py/tango_backend/example.py", source)
    result = save(repository)
    assert not result["ok"]
    assert any(error.startswith("architecture:") for error in result["errors"])


def test_reviewed_http_adapters_are_explicitly_allowed(repository):
    repository[2]("backend_py/tango_backend/model_transport.py", "import httpx\n")
    repository[2]("backend_py/tango_backend/launch_local.py", "import httpx\n")
    assert save(repository)["ok"]


def test_qt_product_cannot_depend_on_backend_credentials_but_tests_can(repository):
    _, _, write = repository
    write("tests/secret_isolation_test.cpp", 'const char* key = "QWEN_API_KEY";\n')
    assert save(repository)["ok"]
    write("mainwindow.cpp", 'const char* key = "QWEN_API_KEY";\n')
    assert not save(repository)["ok"]


@pytest.mark.parametrize("entry", [
    'node "$project_dir/backend/server.mjs"\n',
    'npm --prefix "$project_dir/backend" start\n',
])
def test_startup_cannot_restore_retired_ai_backend(repository, entry):
    repository[2]("scripts/start-app-backend.sh", entry)
    assert not save(repository)["ok"]


def test_cli_returns_failure_and_machine_readable_reason(repository):
    root = repository[0]
    repository[2]("AGENTS.md", "[失效链接](missing.md)\n")
    completed = subprocess.run([sys.executable, str(SCRIPT), "--root", str(root), "--json"],
                               capture_output=True, text=True, timeout=10)
    assert completed.returncode == 1
    result = json.loads(completed.stdout)
    assert not result["ok"]
    assert "navigation" in result["errors"][0]


def test_gate_never_opens_private_configuration_or_user_database(repository, monkeypatch):
    _, _, write = repository
    write("backend_py/.env", "DO_NOT_READ=fixture\n").chmod(0)
    write(".local/mysql-v1/storage.env", "DO_NOT_READ=fixture\n").chmod(0)
    write("backend_py/.data/quiet.sqlite", "DO_NOT_READ").chmod(0)
    original_read = Path.read_text
    def guarded_read(path, *args, **kwargs):
        assert path.name != ".env" and path.name != "storage.env" and path.suffix != ".sqlite"
        return original_read(path, *args, **kwargs)
    monkeypatch.setattr(Path, "read_text", guarded_read)
    assert save(repository)["ok"]
