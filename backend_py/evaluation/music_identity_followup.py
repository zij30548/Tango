"""补充身份澄清诊断：独立范围授权、原预算剩余量、真实候选确认；默认仅离线准备。"""

import argparse
import asyncio
import hashlib
import importlib.util
import json
import logging
import os
from pathlib import Path
import re
import secrets
import unicodedata

from . import music_requirements_live as original
from .calibration import CalibrationError, encode, private_directory, read_json, write_new


PROJECT = original.PROJECT
BASE = PROJECT / ".local/music-identity-followup"
SOURCE_RUN = PROJECT / ".local/music-requirements-live/requirements-20261024T145819Z-39f07ba2"
SCOPE = "music_identity_followup_v1"
MAXIMUM = 40
PURPOSE = "music_identity_followup_diagnostic"
ENTRY = PROJECT / "scripts/check-music-identity-followup.py"
PROBE_SOURCE = PROJECT / "tests/music_identity_followup_probe.cpp"
PROBE_PROJECT = PROJECT / "tests/music_identity_followup_probe.pro"
QT_BINARY = PROJECT / ".local/music-identity-followup-probe/music_identity_followup_probe"
NETEASE_WRAPPER = PROJECT / "service/evaluation/music_identity_followup_netease.mjs"


class IdentityFollowupError(CalibrationError):
    """固定错误码不包含密钥、Cookie、用户库或供应商正文。"""


def canonical_plan():
    """只补诊断原首轮中实际遇到的身份追问，不改写原八题成绩。"""
    selection = {"kind": "select_verified_artist", "canonicalId": 7763,
                 "canonicalName": "G.E.M.邓紫棋", "requireCurrentCandidates": True}
    return {"schemaVersion": 1, "id": SCOPE, "purpose": PURPOSE, "requestedMaximum": MAXIMUM,
            "tasks": [
                {"id": "ID01", "title": "确认真实身份后展示歌手作品", "inputs": [
                    {"kind": "message", "text": "帮我找一些邓紫棋的歌曲"}, dict(selection)],
                 "expect": {"artistId": 7763, "autoPlay": False, "memoryWrite": False}},
                {"id": "ID02", "title": "三条件与多轮修改的身份澄清闭环", "inputs": [
                    {"kind": "message", "text": "我在散步，请你给我推荐邓紫棋的歌曲，给我形成一个30分钟的歌单。"},
                    dict(selection),
                    {"kind": "message", "text": "改成20分钟，歌手和场景都不变", "requirePreview": True}],
                 "expect": {"artistId": 7763, "scene": "散步", "minutesAfterSelection": [30, 20],
                            "sameTask": True, "autoPlay": False, "memoryWrite": False}},
            ], "canonicalIdentityBasis": "原首轮真实匿名曲库候选：必须本次再次出现相同ID和全名才能选择",
            "modelJudge": False, "automaticRetry": False, "overallAccuracy": None,
            "historicalScoreRewritten": False, "explicitScopeAuthorizationRequired": True}


def _identity_name(value):
    """仅规范大小写、Unicode、空白和艺名中的点；不以子串或别名制造全名匹配。"""
    return re.sub(r"[\s.]", "", unicodedata.normalize("NFKC", value).casefold())


def validate_identity_candidates(candidates):
    """返回实际显示序号；没有唯一的本次真实ID与全名配对即阻断，绝不默认第一项。"""
    if not isinstance(candidates, list) or not 1 <= len(candidates) <= 5:
        raise IdentityFollowupError("current_verified_identity_required")
    matches = [index + 1 for index, row in enumerate(candidates) if isinstance(row, dict)
               and type(row.get("id")) is int and row["id"] == 7763
               and isinstance(row.get("name"), str)
               and _identity_name(row["name"]) == _identity_name("G.E.M.邓紫棋")]
    if len(matches) != 1:
        raise IdentityFollowupError("current_verified_identity_required")
    return matches[0]


def validate_scope_authorization(document, manifestSeal, remaining=73):
    """新诊断须明确扩展范围；旧80次授权文字、无scope旧文件与超余量请求全部拒绝。"""
    statement = document.get("statement") if isinstance(document, dict) else None
    maximum = document.get("maximumProviderRequests") if isinstance(document, dict) else None
    if (not isinstance(document, dict) or document.get("scope") != SCOPE
            or document.get("manifestSeal") != manifestSeal or document.get("newAuthorization") is not True
            or document.get("bindingScopeOnly") is not True
            or document.get("historicalUnusedBudgetCarries") is not False
            or type(maximum) is not int or type(remaining) is not int or not 1 <= maximum <= min(MAXIMUM, remaining)
            or not isinstance(statement, str) or not statement.strip()
            or not ("身份澄清" in statement or SCOPE in statement)):
        raise IdentityFollowupError("explicit_identity_scope_authorization_required")
    return maximum


def source_evidence():
    """只读已经停止的原场次及原调用账本，余量包括失败和未知消耗，不访问真实业务库。"""
    source = SOURCE_RUN.absolute()
    if source.is_symlink() or not source.is_dir() or source.resolve() != source:
        raise IdentityFollowupError("original_completed_run_required")
    report, manifest = read_json(source / "report.json"), read_json(source / "manifest.json")
    authorization = read_json(source / "authorization.json")
    events = [json.loads(line) for line in (source / "provider-requests.jsonl").read_text().splitlines() if line.strip()]
    catalog = [json.loads(line) for line in (source / "catalog-requests.jsonl").read_text().splitlines() if line.strip()]
    canonical_present = any(row.get("path") == "/assistant/artists" and row.get("status") == 200
                            and row.get("delivered") is True and any(
                                type(candidate.get("id")) is int and candidate["id"] == 7763
                                and isinstance(candidate.get("name"), str)
                                and _identity_name(candidate["name"]) == _identity_name("G.E.M.邓紫棋")
                                for candidate in (row.get("metadata") or {}).get("artists", []) if isinstance(candidate, dict))
                            for row in catalog)
    numbers = [row.get("number") for row in events if row.get("event") == "started"]
    maximum, used = report.get("maximumProviderRequests"), report.get("requests", {}).get("started")
    unsigned = {key: value for key, value in manifest.items() if key != "seal"}
    if (report.get("status") != "stopped" or report.get("sourceUnchangedAtShutdown") is not True
            or report.get("purpose") != "actual_qt_music_requirements_acceptance"
            or report.get("manifestSeal") != manifest.get("seal")
            or authorization.get("manifestSeal") != manifest.get("seal")
            or manifest.get("seal") != hashlib.sha256(encode(unsigned).encode()).hexdigest()
            or original.digest(source / "authorization.json") != report.get("authorizationDigest")
            or authorization.get("newAuthorization") is not True or authorization.get("maximumProviderRequests") != 80
            or type(maximum) is not int or maximum != 80 or type(used) is not int or not 0 <= used <= maximum
            or numbers != list(range(1, used + 1)) or not canonical_present):
        raise IdentityFollowupError("original_budget_evidence_mismatch")
    return {"runId": source.name, "manifestSeal": manifest["seal"], "originalMaximum": maximum,
            "previousProviderRequests": used, "remaining": maximum - used,
            "reportSha256": original.digest(source / "report.json"),
            "providerLedgerSha256": original.digest(source / "provider-requests.jsonl"),
            "catalogEvidenceSha256": original.digest(source / "catalog-requests.jsonl"),
            "canonicalIdentity": {"id": 7763, "name": "G.E.M.邓紫棋"}}


def _runtime():
    """以独立模块实例复用既有运行时，原模块、原源码与原成绩保持不变。"""
    spec = importlib.util.spec_from_file_location("tango_identity_followup_runtime", original.__file__)
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    for key, value in {"BASE": BASE, "MAXIMUM": MAXIMUM, "QT_BINARY": QT_BINARY,
                       "NETEASE_WRAPPER": NETEASE_WRAPPER, "PROBE_SOURCE": PROBE_SOURCE,
                       "PROBE_PROJECT": PROBE_PROJECT, "ENTRY": ENTRY}.items():
        setattr(runtime, key, value)
    runtime.canonical_plan = canonical_plan
    runtime.verify = verify
    runtime.frozen_sources = frozen_sources
    return runtime


def frozen_sources():
    """追加新入口、探针及旧公共助手的源码与依赖锁，未读取配置和用户数据。"""
    result = original.source_hashes(PROJECT)
    paths = [Path(__file__), Path(original.__file__), ENTRY, PROBE_SOURCE, PROBE_PROJECT, QT_BINARY, NETEASE_WRAPPER,
             PROJECT / "tests/music_requirements_live_probe.cpp", PROJECT / "tests/routing_fixtures.h",
             PROJECT / "tests/music_identity_followup_helpers.h",
             PROJECT / "tests/music_identity_followup_flow_test.cpp", PROJECT / "tests/music_identity_followup_flow_test.pro",
             PROJECT / "service/package.json", PROJECT / "service/package-lock.json"]
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise IdentityFollowupError("unsafe_or_missing_identity_source")
        result[path.relative_to(PROJECT).as_posix()] = original.digest(path)
    return result


def prepare():
    """只冻结两题与已核余量，不读.env、不授权、不启动网络、模型或曲库。"""
    base = private_directory(BASE)
    evidence = source_evidence()
    run = base / ("identity-" + original.datetime.now(original.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
                  + "-" + secrets.token_hex(4))
    run.mkdir(mode=0o700)
    plan = canonical_plan()
    manifest = {"schemaVersion": 1, "purpose": PURPOSE, "scope": SCOPE, "authorization": None,
                "freshAuthorizationRequired": True, "maximumProviderRequests": min(MAXIMUM, evidence["remaining"]),
                "historicalUnusedBudgetCarries": False, "automaticRetry": False, "modelJudge": False,
                "sourceEvidence": evidence, "sources": frozen_sources(), "preparedAt": original.utc(),
                "planDigest": hashlib.sha256(encode(plan).encode()).hexdigest(),
                "limitations": ["补充已暴露身份澄清的诊断，不改原八题成绩或声称总体准确率", "真实运行仍须新scope授权，失败和未知计数",
                                "本次读取当前真实候选ID和全名，缺失或不唯一即阻断", "匿名曲库不证明会员权益或听感", "Cloud关闭且禁止自动重跑"]}
    manifest["seal"] = hashlib.sha256(encode(manifest).encode()).hexdigest()
    write_new(run / "manifest.json", manifest)
    write_new(run / "plan.json", plan)
    return run


def verify(path, *, require_authorization=False):
    """授权前后均核验冻结版本、原账本和当前scope；授权文件不能移到另一场次复用。"""
    run = private_directory(path)
    if run.parent != BASE or not run.name.startswith("identity-"):
        raise IdentityFollowupError("invalid_owned_identity_directory")
    manifest = read_json(run / "manifest.json")
    unsigned = {key: value for key, value in manifest.items() if key != "seal"}
    if (manifest.get("seal") != hashlib.sha256(encode(unsigned).encode()).hexdigest()
            or manifest.get("scope") != SCOPE or manifest.get("purpose") != PURPOSE
            or manifest.get("sources") != frozen_sources() or manifest.get("sourceEvidence") != source_evidence()
            or read_json(run / "plan.json") != canonical_plan()
            or manifest.get("planDigest") != hashlib.sha256(encode(canonical_plan()).encode()).hexdigest()):
        raise IdentityFollowupError("frozen_identity_protocol_changed")
    if require_authorization:
        if not (run / "authorization.json").is_file():
            raise IdentityFollowupError("explicit_identity_scope_authorization_required")
        maximum = validate_scope_authorization(read_json(run / "authorization.json"), manifest["seal"],
                                               manifest["sourceEvidence"]["remaining"])
        manifest = {**manifest, "authorizedMaximum": maximum,
                    "authorizationDigest": original.digest(run / "authorization.json")}
    return run, manifest


def authorize(path, maximum, statement, scope):
    """只记录用户针对身份澄清的新范围许可；旧80次语句不能替代本次许可。"""
    run, manifest = verify(path)
    document = {"schemaVersion": 1, "scope": scope, "manifestSeal": manifest["seal"],
                "maximumProviderRequests": maximum, "statement": statement, "newAuthorization": True,
                "bindingScopeOnly": True, "historicalUnusedBudgetCarries": False,
                "sourceRunId": manifest["sourceEvidence"]["runId"], "authorizedAt": original.utc()}
    validate_scope_authorization(document, manifest["seal"], manifest["sourceEvidence"]["remaining"])
    if (run / "execution-lock.json").exists() or (BASE / "scope-execution-lock.json").exists():
        raise IdentityFollowupError("identity_scope_already_executed")
    write_new(run / "authorization.json", document)
    return {"status": "scope_authorized_no_calls", "scope": SCOPE, "maximumProviderRequests": maximum}


async def serve(path):
    """新scope最多执行一次；已有原模型传输与正式账号、任务预算保持生效。"""
    run, manifest = verify(path, require_authorization=True)
    write_new(BASE / "scope-execution-lock.json", {"runId": run.name, "manifestSeal": manifest["seal"],
              "scope": SCOPE, "maximumProviderRequests": manifest["authorizedMaximum"], "startedAt": original.utc()})
    return await _runtime().serve(run)


def main(argv=None):
    """显式区分准备、范围授权和运行，默认没有任何真实调用。"""
    os.umask(0o077)
    logging.disable(logging.CRITICAL)
    parser = argparse.ArgumentParser(description="两题身份澄清诊断；最多40次且须新scope授权")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("plan")
    commands.add_parser("prepare")
    for name in ("serve", "launch-catalog", "launch-probe", "authorize"):
        command = commands.add_parser(name)
        command.add_argument("--run", type=Path, required=True)
        if name == "authorize":
            command.add_argument("--maximum", type=int, required=True)
            command.add_argument("--scope", choices=[SCOPE], required=True)
            command.add_argument("--statement", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            print(encode(canonical_plan()))
        elif args.command == "prepare":
            print(encode({"status": "prepared_no_calls", "runDirectory": str(prepare())}))
        elif args.command == "authorize":
            print(encode(authorize(args.run, args.maximum, args.statement, args.scope)))
        elif args.command == "serve":
            return asyncio.run(serve(args.run))
        else:
            runtime = _runtime()
            value = runtime.launch_catalog(args.run) if args.command == "launch-catalog" else runtime.launch_qt(args.run)
            print(encode(value))
        return 0
    except BaseException as error:
        code = str(error) if isinstance(error, CalibrationError) else "IDENTITY_FOLLOWUP_FAILED"
        print(encode({"status": "failed", "errorCode": code if re.fullmatch(r"[A-Za-z_]{1,100}", code) else "IDENTITY_FOLLOWUP_FAILED"}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
