"""离线 PostgreSQL → MySQL 迁移：保留原库，只在空目标事务中导入并校验。"""
import argparse
import asyncio
from decimal import Decimal
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from urllib.parse import unquote, urlsplit
from uuid import uuid4

from .compat import now_ms, iso
from .mysql_infra import read_profile, private_write, clean_environment

# 按外键依赖顺序迁移全部业务表；固定数据库互斥行不属于业务状态。
TABLES = ('users', 'sessions', 'ai_usage', 'music_account_quota', 'music_registration_invites',
          'music_registration_invite_uses', 'music_tasks', 'music_task_requests', 'music_task_calls',
          'music_task_global_usage', 'music_task_account_leases', 'music_task_model_slots',
          'development_identities', 'music_conversations', 'music_conversation_clears', 'music_conversation_routes', 'music_conversation_route_attempts',
          'tango_storage_imports')
JSON_COLUMNS = {'snapshot_json', 'diagnostics_json', 'journal_json', 'input_json', 'result_json', 'counts_json',
                'model_selection_json', 'model_metadata_json', 'request_metadata_json', 'receipt_json'}
BOOL_COLUMNS = {'enabled', 'disabled', 'paid'}
MODEL_LEDGER_COLUMNS = {'model_selection_json', 'model_metadata_json'}


def migration_columns(table, source_columns, target_columns):
    """只允许已知可空审计扩展列缺失；旧源库不为迁移而发生任何写入。"""
    source, target = set(source_columns), set(target_columns)
    optional = MODEL_LEDGER_COLUMNS if table in {'music_task_calls', 'music_conversation_route_attempts'} else set()
    if table == 'music_task_requests':
        optional = {'request_metadata_json'}
    if table == 'music_conversations':
        optional = {'cleared_at'}
    if source - target or (target - source) - optional:
        _reject('COLUMN_SCHEMA_MISMATCH')
    return sorted(target)

class MigrationError(Exception):
    def __init__(self, code):
        super().__init__(code); self.code = code

def _reject(code):
    raise MigrationError(code)

def _json_load(value):
    def invalid(_): _reject('INVALID_SOURCE_JSON')
    return json.loads(value, parse_float=Decimal, parse_int=int, parse_constant=invalid) if isinstance(value, (str, bytes)) else value

def _number(value):
    if isinstance(value, int):
        sign, digits, exponent = int(value < 0), str(abs(value)), 0
    else:
        value = Decimal(str(value)) if isinstance(value, float) else value
        if not value.is_finite(): _reject('INVALID_NUMBER')
        t = value.as_tuple(); sign, digits, exponent = t.sign, ''.join(map(str, t.digits)), t.exponent
    if not digits or set(digits) <= {'0'}: return [0, '0', 0]
    while len(digits) > 1 and digits.endswith('0'):
        digits, exponent = digits[:-1], exponent + 1
    return [sign, digits, exponent]

def canonical(value):
    # 带类型标记，避免 JSON 对象伪装成数字摘要；不经 float 转换大整数或精确小数。
    if value is None: return ['null']
    if type(value) is bool: return ['boolean', value]
    if isinstance(value, (int, float, Decimal)): return ['number', *_number(value)]
    if isinstance(value, str): return ['string', value]
    if isinstance(value, list): return ['array', [canonical(v) for v in value]]
    if isinstance(value, dict): return ['object', [[k, canonical(value[k])] for k in sorted(value)]]
    _reject('UNSUPPORTED_COLUMN_TYPE')

def row_digest(row, columns):
    values = []
    for column in columns:
        value = row[column]
        if column in JSON_COLUMNS:
            normalized = ['sql_null'] if value is None else ['json', canonical(_json_load(value))]
        else:
            if column in BOOL_COLUMNS and type(value) is bool: value = int(value)
            normalized = canonical(value)
        values.append([column, normalized])
    return hashlib.sha256(json.dumps(values, ensure_ascii=False, separators=(',', ':')).encode('utf-8')).hexdigest()

def table_digest(rows, columns):
    hashes = sorted(row_digest(row, columns) for row in rows)
    return hashlib.sha256(('\n'.join(hashes)).encode('ascii')).hexdigest()

def _json_text(value):
    if value is None: return 'null'
    if type(value) is bool: return 'true' if value else 'false'
    if isinstance(value, str): return json.dumps(value, ensure_ascii=False)
    if isinstance(value, int): return str(value)
    if isinstance(value, Decimal):
        if not value.is_finite(): _reject('INVALID_SOURCE_JSON')
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value): _reject('INVALID_SOURCE_JSON')
        return repr(value)
    if isinstance(value, list): return '[' + ','.join(_json_text(v) for v in value) + ']'
    if isinstance(value, dict): return '{' + ','.join(json.dumps(k, ensure_ascii=False) + ':' + _json_text(v) for k, v in value.items()) + '}'
    _reject('INVALID_SOURCE_JSON')

def _insert_value(column, value):
    if column in JSON_COLUMNS and value is not None:
        _json_load(value)
        return value if isinstance(value, str) else _json_text(value)
    return int(value) if column in BOOL_COLUMNS and type(value) is bool else value

def _identifier(value):
    if not isinstance(value, str) or not re.fullmatch('[a-z][a-z0-9_]*', value): _reject('UNSUPPORTED_SCHEMA')
    return value

async def _tables(db, dialect):
    sql = "SELECT table_name FROM information_schema.tables WHERE table_schema=" + ('current_schema()' if dialect == 'postgres' else 'DATABASE()') + " AND table_type='BASE TABLE'"
    rows = await db.fetchall(sql)
    return {row.get('table_name', row.get('TABLE_NAME')) for row in rows}

async def _columns(db, table, dialect):
    sql = 'SELECT column_name FROM information_schema.columns WHERE table_schema=' + ('current_schema()' if dialect == 'postgres' else 'DATABASE()') + ' AND table_name=? ORDER BY ordinal_position'
    rows = await db.fetchall(sql, (table,))
    return [row.get('column_name', row.get('COLUMN_NAME')) for row in rows]

async def _source_safe(db, existing, now):
    checks = [
        ('music_tasks', "status IN ('planning','researching','composing')", ()),
        ('music_task_calls', "status='reserved'", ()),
        ('music_conversation_routes', "status='running'", ()),
        ('music_conversation_route_attempts', "status='reserved'", ()),
        ('music_tasks', 'lease_until>? AND owner_id IS NOT NULL', (now,)),
        ('music_task_account_leases', 'lease_until>?', (now,)),
        ('music_task_model_slots', 'lease_until>?', (now,)),
    ]
    for table, where, args in checks:
        if table in existing and await db.fetchone(f'SELECT 1 FROM "{table}" WHERE {where} LIMIT 1', args):
            _reject('SOURCE_ACTIVE_OR_UNCERTAIN')

async def _target_empty(db, existing):
    if set(TABLES) - existing: _reject('TARGET_SCHEMA_INCOMPLETE')
    unknown = existing - set(TABLES) - {'music_storage_locks'}
    if unknown: _reject('TARGET_SCHEMA_UNSUPPORTED')
    for table in TABLES:
        if int((await db.fetchone(f'SELECT COUNT(*) AS n FROM `{table}`'))['n']) != 0:
            _reject('TARGET_NOT_EMPTY')

def _pg_dump_binary():
    for candidate in [shutil.which('pg_dump'), '/opt/homebrew/opt/postgresql@17/bin/pg_dump', '/usr/local/opt/postgresql@17/bin/pg_dump']:
        if candidate and Path(candidate).is_file(): return str(candidate)
    _reject('BACKUP_TOOL_MISSING')

def backup_postgres(url, directory):
    directory = Path(directory).absolute(); directory.mkdir(parents=True, exist_ok=True, mode=0o700); directory.chmod(0o700)
    path = directory / ('postgres-before-mysql-' + str(uuid4()) + '.dump')
    partial = path.with_suffix('.partial')
    fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600); os.close(fd)
    u = urlsplit(url)
    env = {**clean_environment(), 'PGPASSWORD': unquote(u.password or ''), 'PGCONNECT_TIMEOUT': '5'}
    argv = [_pg_dump_binary(), '--no-password', '--host=' + (u.hostname or '127.0.0.1'), '--port=' + str(u.port or 5432),
            '--username=' + unquote(u.username or ''), '--dbname=' + unquote(u.path.lstrip('/')), '--format=custom', '--file=' + str(partial)]
    result = subprocess.run(argv, env=env, capture_output=True, timeout=300)
    if result.returncode or not partial.stat().st_size: _reject('BACKUP_FAILED')
    os.replace(partial, path); path.chmod(0o600)
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''): checksum.update(chunk)
    return {'path': str(path), 'sha256': checksum.hexdigest(), 'bytes': path.stat().st_size}

async def migrate(source_env, target_env, *, execute=False, backup_directory=None, report=None):
    from .storage_db import Database
    from .mysql_store import MySQLStore
    source_path, target_path = Path(source_env).absolute(), Path(target_env).absolute()
    source_config, target_config = read_profile(source_path), read_profile(target_path)
    source_url, target_url = source_config.get('QUIET_DATABASE_URL', ''), target_config.get('QUIET_DATABASE_URL', '')
    if source_config.get('QUIET_STORAGE_DRIVER') != 'postgres' or urlsplit(source_url).scheme not in ['postgres', 'postgresql']:
        _reject('SOURCE_MUST_BE_POSTGRES')
    if target_config.get('QUIET_STORAGE_DRIVER') != 'mysql' or urlsplit(target_url).scheme != 'mysql':
        _reject('TARGET_MUST_BE_MYSQL')
    report_path = Path(report).absolute() if report else None
    if report_path:
        # 先独占占位，已有或不可写的报告路径不能在业务提交后才让迁移报错。
        try:
            private_write(report_path, json.dumps({'ok': False, 'status': 'pending', 'profileSwitched': False}) + '\n')
        except OSError:
            _reject('REPORT_UNAVAILABLE')
    source, target = Database(connectionString=source_url), MySQLStore(connectionString=target_url)
    result = {'ok': False, 'executed': bool(execute), 'verified': False, 'profileSwitched': False, 'sourceRetained': True}
    try:
        await source.initialize(); await target.initialize()
        existing = await _tables(source, 'postgres')
        if existing - set(TABLES) - {'music_storage_locks'}: _reject('SOURCE_SCHEMA_UNSUPPORTED')
        if 'users' not in existing: _reject('SOURCE_SCHEMA_INCOMPLETE')
        async with source.transaction():
            # 只持有源表 SHARE 锁，绝不更新 PG 数据；停机检查与整个导出期间阻止写入竞态。
            await source.execute('SET TRANSACTION ISOLATION LEVEL REPEATABLE READ')
            await source.execute("SET LOCAL lock_timeout='5s'")
            if existing:
                await source.execute('LOCK TABLE ' + ','.join('"' + _identifier(t) + '"' for t in sorted(existing)) + ' IN SHARE MODE')
            # 加锁后才进行首个快照读取，避免锁前快照漏掉等待锁期间已提交的数据。
            if await _tables(source, 'postgres') != existing: _reject('SOURCE_SCHEMA_CHANGED')
            await _source_safe(source, existing, now_ms())
            async with target.db.transaction():
                await target.db.lock_key('storage:postgres_to_mysql_migration')
                await _target_empty(target.db, await _tables(target.db, 'mysql'))
                modes = (await target.db.fetchone('SELECT @@SESSION.sql_mode AS mode'))['mode'].split(',')
                if not any(mode in modes for mode in ['STRICT_TRANS_TABLES', 'STRICT_ALL_TABLES']): _reject('TARGET_REQUIRES_STRICT_MODE')
                engines = await target.db.fetchall("SELECT engine FROM information_schema.tables WHERE table_schema=DATABASE() AND table_type='BASE TABLE'")
                if any(row.get('engine', row.get('ENGINE')) != 'InnoDB' for row in engines): _reject('TARGET_REQUIRES_INNODB')
                rows, columns, summaries = {}, {}, {}
                for table in TABLES:
                    target_columns = await _columns(target.db, table, 'mysql')
                    if table in existing:
                        source_columns = await _columns(source, table, 'postgres')
                        columns[table] = migration_columns(table, source_columns, target_columns)
                        rows[table] = await source.fetchall(f'SELECT * FROM "{table}"')
                        # 可空新列只投影为未知，不修改源表，不把当前配置反填到历史尝试。
                        rows[table] = [{column: row.get(column) for column in columns[table]} for row in rows[table]]
                    else:
                        columns[table], rows[table] = sorted(target_columns), []
                    summaries[table] = {'count': len(rows[table]), 'sha256': table_digest(rows[table], columns[table])}
                if execute:
                    # 备份先于任何目标业务写入，凭据只经环境传给 pg_dump，不进入 argv 或日志。
                    result['backup'] = await asyncio.to_thread(backup_postgres, source_url, backup_directory or target_path.parent / 'backups')
                    for table in TABLES:
                        names = columns[table]
                        sql = f'INSERT INTO `{table}` (' + ','.join('`' + _identifier(c) + '`' for c in names) + ') VALUES (' + ','.join('?' for _ in names) + ')'
                        for row in rows[table]:
                            await target.db.execute(sql, [_insert_value(column, row[column]) for column in names])
                    for table in TABLES:
                        copied = await target.db.fetchall(f'SELECT * FROM `{table}`')
                        if len(copied) != summaries[table]['count'] or table_digest(copied, columns[table]) != summaries[table]['sha256']:
                            _reject('ROW_DIGEST_MISMATCH')
                result.update(ok=True, verified=True, tables=summaries, at=iso())
        # 直到外层 MySQL 提交成功，才输出成功；报告不包含任何用户行或凭据。
        if report_path:
            try:
                result['reportWritten'] = True
                private_write(report_path, json.dumps(result, ensure_ascii=False, indent=2) + '\n', replace=True)
            except OSError:
                # 数据已提交，不能把落盘报告失败伪装成整笔迁移回滚；调用方仍需人工核对。
                result.update(reportWritten=False, reportWarning='COMMITTED_REPORT_WRITE_FAILED')
        return result
    finally:
        await source.close(); await target.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description='离线迁移 PG 到全空 MySQL；必须先停全部后台，不切换 active profile。')
    parser.add_argument('--source-env', required=True)
    parser.add_argument('--target-env', required=True)
    parser.add_argument('--execute', action='store_true', help='默认只核验；执行时先保存私有 PG 备份，再在目标事务中导入并逐表验摘要。')
    parser.add_argument('--backup-directory')
    parser.add_argument('--report')
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        result = asyncio.run(migrate(args.source_env, args.target_env, execute=args.execute, backup_directory=args.backup_directory, report=args.report))
        print(json.dumps({'ok': True, 'executed': result['executed'], 'verified': result['verified'], 'profileSwitched': False,
                          'tables': {table: values['count'] for table, values in result['tables'].items()}, **({'backup': result['backup']['path']} if result.get('backup') else {}),
                          **({'reportWarning': result['reportWarning']} if result.get('reportWarning') else {})}, ensure_ascii=False))
        return 0
    except Exception as error:
        print(json.dumps({'ok': False, 'error': error.code if isinstance(error, MigrationError) else 'MIGRATION_FAILED', 'profileSwitched': False}, ensure_ascii=False), file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
