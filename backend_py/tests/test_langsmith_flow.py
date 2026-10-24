"""真实 ASGI/LangGraph 的隔离链路验证；不访问真实模型、曲库及用户存储。"""

import json
from unittest.mock import Mock

import httpx
import pytest
import requests

from evaluation import langsmith_demo as demo
from tango_backend.agent_tracing import AgentTracing


@pytest.fixture
def no_external_network(monkeypatch):
    """ASGI 直接调应用；任何额外 HTTP 请求都让用例立即失败。"""
    async def forbidden(*args, **kwargs):
        pytest.fail("隔离演示发起了真实外部 HTTP")
    def forbidden_sync(*args, **kwargs):
        pytest.fail("隔离演示发起了真实外部同步 HTTP")
    monkeypatch.setattr(httpx.AsyncHTTPTransport, "handle_async_request", forbidden)
    monkeypatch.setattr(requests.Session, "request", forbidden_sync)


@pytest.mark.asyncio
async def test_real_route_graph_tool_checks_preview_and_application_share_one_trace(no_external_network, monkeypatch):
    monkeypatch.setenv("LANGSMITH_TRACING", "true")
    monkeypatch.setenv("LANGCHAIN_TRACING_V2", "true")
    collector = demo.MemoryExporter()
    observer = AgentTracing(enabled=True, exporter=collector)
    try:
        result = await demo.execute_demo(observer)
        assert observer.flush(timeout=5)
    finally:
        assert observer.close(timeout=5)
    runs = demo.merged_runs(collector.events())
    roots = [row for row in runs if not row.get("parent_run_id")]
    assert len(roots) == 1
    assert len({row["trace_id"] for row in runs}) == 1
    assert {row["name"] for row in runs} >= {
        "agent.conversation", "conversation.route", "task.run", "task.entry", "task.understand",
        "task.research", "task.dispatch", "task.observe", "task.compose", "task.tool", "task.checks"}
    assert len([row for row in runs if row["name"] == "task.run"]) == 2
    ids = {row["id"] for row in runs}
    assert all(row.get("parent_run_id") in ids for row in runs if row.get("parent_run_id"))
    assert all("end_time" in row for row in runs)
    assert roots[0]["outputs"]["status"] == "preview"
    adopted = [row for row in runs if row["name"] == "task.adopt"]
    applied = [row for row in runs if row["name"] == "task.applied"]
    assert len(adopted) == len(applied) == 1
    assert adopted[0]["outputs"]["status"] == "awaiting_apply"
    assert applied[0]["outputs"]["status"] == "adopted"
    completion_version = roots[0]["outputs"]["state_version"]
    assert completion_version < adopted[0]["outputs"]["state_version"] < applied[0]["outputs"]["state_version"]
    terminal_root_updates = [event["payload"] for event in collector.events()
                             if event["operation"] == "patch" and event["payload"]["id"] == roots[0]["id"]]
    assert len(terminal_root_updates) == 1
    assert all(row["outputs"]["status"] == "preview" and row["outputs"]["state_version"] == completion_version
               for row in terminal_root_updates)
    assert terminal_root_updates[0]["outputs"]["checkpoint"] == "preview"
    assert result["routeStatus"] == "completed" and result["taskStatus"] == "adopted"
    assert result["trackCount"] == 5 and result["totalDurationMs"] == 1200000
    assert result["realModelRequests"] == result["realCatalogRequests"] == 0
    assert result["staleLeaseRejected"] and result["replayNoNewModelCallsOrUsage"]
    assert next(row for row in result["checks"] if row["key"] == "duration")["status"] == "satisfied"
    assert next(row for row in result["checks"] if row["key"] == "style")["status"] == "unverified"
    assert all(row["extra"]["metadata"]["fixture"] is True for row in runs)
    assert all(row["extra"]["metadata"]["upstream_mode"] == "scripted" for row in runs)
    encoded = json.dumps(runs, ensure_ascii=False)
    assert demo.INSTRUCTION not in encoded
    assert "Bearer " not in encoded and "leaseId" not in encoded and "coverUrl" not in encoded
    assert "http://" not in encoded and "https://" not in encoded
    assert observer.health()["failed"] == observer.health()["dropped"] == 0


def test_cloud_configuration_only_passes_explicit_langsmith_fields(tmp_path, monkeypatch):
    path = tmp_path / ".env"
    path.write_text("LANGSMITH_API_KEY=fixture-tracing-key\nLANGSMITH_PROJECT=tango-fixture\nQUIET_MODEL_API_KEY=fixture-model-key\nDATABASE_URL=mysql://private-database\n")
    monkeypatch.setenv("LANGSMITH_API_KEY", "ambient-key-must-not-win")
    config = demo.cloud_configuration(path, "eu")
    assert config == {"LANGSMITH_API_KEY": "fixture-tracing-key", "LANGSMITH_PROJECT": "tango-fixture",
                      "LANGSMITH_ENDPOINT": "https://eu.api.smith.langchain.com", "QUIET_LANGSMITH_ENABLED": "1"}
    assert "fixture-model-key" not in json.dumps(config) and "mysql://" not in json.dumps(config)


def test_cloud_configuration_does_not_expand_model_key_interpolation(tmp_path):
    path = tmp_path / ".env"
    path.write_text("QUIET_MODEL_API_KEY=fixture-private-model-key\nLANGSMITH_API_KEY=${QUIET_MODEL_API_KEY}\n")
    with pytest.raises(demo.DemoError, match="DEMO_LANGSMITH_CONFIGURATION_INVALID"):
        demo.cloud_configuration(path)


@pytest.mark.parametrize("kind", ["missing", "symlink", "no_key"])
def test_cloud_requires_local_explicit_configuration(tmp_path, kind):
    path = tmp_path / ".env"
    if kind == "no_key":
        path.write_text("QUIET_MODEL_API_KEY=private-model-key\n")
    if kind == "symlink":
        target = tmp_path / "target"
        target.write_text("LANGSMITH_API_KEY=fixture-key\n")
        path.symlink_to(target)
    with pytest.raises(demo.DemoError, match="DEMO_LANGSMITH_CONFIGURATION_REQUIRED"):
        demo.cloud_configuration(path)


def test_cloud_and_local_receive_same_fixture_marked_summary_without_hiding_failure():
    collector = demo.MemoryExporter()
    cloud = Mock()
    exporter = demo.DemoCloudExporter(collector, cloud)
    exporter.export("create", {"id": "fixture-id", "inputs": {}})
    recorded = collector.events()[0]
    assert cloud.export.call_args.args == (recorded["operation"], recorded["payload"])
    assert recorded["payload"]["extra"]["metadata"]["fixture"] is True
    cloud.export.side_effect = RuntimeError("fixture-cloud-failure")
    with pytest.raises(RuntimeError):
        exporter.export("patch", {"id": "fixture-id", "outputs": {}})
    assert len(collector.events()) == 2


def test_batch_export_forwards_same_fixture_events_and_preserves_local_evidence_on_cloud_failure():
    collector = demo.MemoryExporter()
    cloud = Mock()
    exporter = demo.DemoCloudExporter(collector, cloud)
    batch = [("create", {"id": "fixture-id", "inputs": {}}),
             ("patch", {"id": "fixture-id", "outputs": {"status": "completed"}})]
    exporter.export_many(batch)
    recorded = collector.events()
    cloud.export_many.assert_called_once_with([(event["operation"], event["payload"]) for event in recorded])
    cloud.export.assert_not_called()
    assert all(event["payload"]["extra"]["metadata"] == demo.FIXTURE_METADATA for event in recorded)
    assert all("extra" not in payload for operation, payload in batch)
    cloud.export_many.side_effect = RuntimeError("fixture-cloud-batch-failure")
    with pytest.raises(RuntimeError):
        exporter.export_many(batch)
    assert len(collector.events()) == 4


def test_html_escapes_collected_text_and_does_not_embed_remote_resources(tmp_path):
    report = {"runs": [{"id": "fixture", "name": "<script>alert(1)</script>", "inputs": {"unsafe": "<img src=x>"}}]}
    rendered = demo.render_html(report)
    assert "<script>" not in rendered and "<img" not in rendered
    assert "&lt;script&gt;" in rendered and "&lt;img" in rendered
    demo.write_artifacts(tmp_path / "private", report)
    assert json.loads((tmp_path / "private" / "report.json").read_text()) == report
    assert (tmp_path / "private" / "trace.html").stat().st_mode & 0o077 == 0
    with pytest.raises(FileExistsError):
        demo.write_artifacts(tmp_path / "private", report)


def test_report_rejects_symlink_output(tmp_path):
    target = tmp_path / "target"
    target.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(target, target_is_directory=True)
    with pytest.raises(demo.DemoError, match="DEMO_UNSAFE_OUTPUT"):
        demo.write_artifacts(alias, {"runs": []})


def test_default_cli_never_reads_cloud_config_or_constructs_cloud_exporter(tmp_path, monkeypatch, capsys, no_external_network):
    monkeypatch.setenv("QUIET_LANGSMITH_ENABLED", "1")
    monkeypatch.setenv("LANGSMITH_TRACING", "true")
    monkeypatch.setenv("LANGSMITH_API_KEY", "fixture-key-never-send")
    monkeypatch.setattr(demo, "cloud_configuration", lambda *a, **kw: pytest.fail("默认演示读取了私有云配置"))
    monkeypatch.setattr(demo.LangSmithExporter, "from_env", lambda *a, **kw: pytest.fail("默认演示构造了云发送器"))
    assert demo.main(["--output", str(tmp_path / "private")]) == 0
    report = json.loads((tmp_path / "private" / "report.json").read_text())
    assert report["uploaded"] is False and report["uploadRequested"] is False
    assert report["cloudReadbackVerified"] is False
    assert "fixture-key-never-send" not in capsys.readouterr().out


@pytest.mark.parametrize("cloud_failure", [False, True])
def test_explicit_upload_passes_fixture_summary_and_never_claims_failed_export_succeeded(
    tmp_path, monkeypatch, capsys, no_external_network, cloud_failure
):
    cloud = Mock()
    # 本例保留单项出口兼容路径；批量转发另有严格用例，不让 Mock 自动补方法掩盖失败。
    cloud.export_many = None
    if cloud_failure:
        cloud.export.side_effect = RuntimeError("fixture-cloud-failure")
    seen = []
    def config(path, region):
        seen.append((path.name, region))
        return {"QUIET_LANGSMITH_ENABLED": "1", "LANGSMITH_API_KEY": "fixture-cloud-key",
                "LANGSMITH_ENDPOINT": demo.REGIONS[region][0], "LANGSMITH_PROJECT": "tango-fixture"}
    monkeypatch.setattr(demo, "cloud_configuration", config)
    monkeypatch.setattr(demo.LangSmithExporter, "from_env", lambda env: cloud)
    exit_code = demo.main(["--upload", "--region", "eu", "--output", str(tmp_path / "private")])
    report = json.loads((tmp_path / "private" / "report.json").read_text())
    assert exit_code == (1 if cloud_failure else 0)
    assert seen == [(".env", "eu")]
    assert report["uploadRequested"] and report["uploaded"] is (not cloud_failure)
    assert report["cloudReadbackVerified"] is False
    assert report["langsmithConsole"] == "https://eu.smith.langchain.com"
    assert len(report["traceIds"]) == 1 and report["runs"]
    # 兼容出口遇到首项失败就中止该批；worker 按整批事件记失败，不自动重发剩余项。
    expected_cloud_calls = report["exportHealth"]["exportBatches"] if cloud_failure else len(report["events"])
    assert cloud.export.call_count == expected_cloud_calls
    assert all(call.args[1]["extra"]["metadata"]["fixture"] is True for call in cloud.export.call_args_list)
    assert report["exportHealth"]["failed"] == (len(report["events"]) if cloud_failure else 0)
    assert "fixture-cloud-key" not in capsys.readouterr().out
