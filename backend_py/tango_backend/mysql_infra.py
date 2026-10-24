"""只管理显式目录中的 MySQL 8.4 与 Redis；不操作系统默认库或 brew services。"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from urllib.parse import urlsplit

PROJECT = Path(__file__).resolve().parents[2]
DEFAULT_DIRECTORY = PROJECT / '.local/mysql-v1'


def clean_environment():
    return {key: os.environ[key] for key in ['PATH', 'HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'TMPDIR', 'SYSTEMROOT'] if key in os.environ}


def private_write(path, value, *, replace=False):
    path = Path(path)
    target = path.with_name(path.name + '.new-' + secrets.token_hex(8)) if replace else path
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'w') as file:
            file.write(value); file.flush(); os.fsync(file.fileno())
        if replace:
            os.replace(target, path)
    finally:
        if target != path:
            with contextlib.suppress(FileNotFoundError): target.unlink()


def private_read(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        details = os.fstat(fd)
        if not stat.S_ISREG(details.st_mode) or details.st_uid != os.getuid() or details.st_nlink != 1 or details.st_mode & 0o077:
            raise ValueError('基础设施配置必须为拥有者的私有普通文件。')
        if details.st_size > 1024 * 1024:
            raise ValueError('基础设施配置过大。')
        return os.read(fd, 1024 * 1024 + 1).decode('utf-8')
    finally:
        os.close(fd)


def read_profile(path):
    from dotenv import dotenv_values
    from io import StringIO
    return {key: value for key, value in dotenv_values(stream=StringIO(private_read(path)), interpolate=False).items() if value is not None}


def binary(name, mysql_bin=None):
    if name in ['mysqld', 'mysql', 'mysqladmin']:
        candidates = [Path(mysql_bin) / name] if mysql_bin else []
        candidates += [Path('/opt/homebrew/opt/mysql@8.4/bin') / name, Path('/usr/local/opt/mysql@8.4/bin') / name]
        found = shutil.which(name)
        if found: candidates.append(Path(found))
        for candidate in candidates:
            if candidate.is_file() and os.access(candidate, os.X_OK):
                result = subprocess.run([str(candidate), '--no-defaults', '--version'], capture_output=True, text=True, env=clean_environment(), timeout=10)
                if result.returncode == 0 and '8.4.' in result.stdout:
                    return str(candidate.resolve())
        raise RuntimeError('需要 MySQL 8.4 程序。')
    found = shutil.which(name)
    if not found: raise RuntimeError('缺少基础设施程序。')
    return found


def free_port():
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0)); return server.getsockname()[1]


def command(argv, *, input=None, timeout=60):
    result = subprocess.run(argv, input=input, capture_output=True, text=True, timeout=timeout, env=clean_environment())
    if result.returncode:
        raise RuntimeError('本机基础设施命令失败：' + Path(argv[0]).name)
    return result.stdout.strip()


def redis_exchange(url, *requests):
    u = urlsplit(url)
    if u.scheme != 'redis' or u.hostname != '127.0.0.1' or not u.password:
        raise ValueError('本机基础设施只接受有密码的回环 Redis。')
    values = [('AUTH', u.password), *requests]
    wire = b''
    for row in values:
        encoded = [str(value).encode() for value in row]
        wire += ('*' + str(len(encoded)) + '\r\n').encode() + b''.join(('$' + str(len(value)) + '\r\n').encode() + value + b'\r\n' for value in encoded)
    with socket.create_connection(('127.0.0.1', u.port or 6379), timeout=.5) as connection:
        connection.sendall(wire); result = b''
        # PING 后的响应足以证明认证；INFO 的 bulk 长度用于读取完整身份。
        while len(result) < 64 * 1024:
            part = connection.recv(8192)
            if not part: break
            result += part
            if any(row[0] == 'INFO' for row in requests):
                pieces = result.split(b'\r\n', 2)
                if len(pieces) >= 3 and pieces[1].startswith(b'$') and len(pieces[2]) >= int(pieces[1][1:]) + 2: break
            elif b'+PONG\r\n' in result or any(row[0] == 'SHUTDOWN' for row in requests): break
        if not result.startswith(b'+OK\r\n'):
            raise RuntimeError('Redis 身份验证失败。')
        return result


def redis_identity(url):
    result = redis_exchange(url, ('INFO', 'server')).decode('utf-8')
    data = dict(line.split(':', 1) for line in result.splitlines() if ':' in line and not line.startswith('#'))
    if not data.get('run_id') or not data.get('process_id'):
        raise RuntimeError('Redis 身份不可验证。')
    return {'runId': data['run_id'], 'pid': int(data['process_id'])}


def manifest(directory):
    directory = Path(directory).absolute()
    value = json.loads(private_read(directory / 'infra.json'))
    if value.get('schemaVersion') != 1 or value.get('directory') != str(directory) or value.get('mysqlVersion') != '8.4':
        raise ValueError('基础设施清单无效。')
    return value


def mysql_identity(directory, value):
    output = command([value['mysql'], '--defaults-file=' + str(Path(directory) / 'root-client.cnf'), '--batch', '--skip-column-names',
                      '--execute=SELECT @@datadir,@@port,@@server_uuid;'], timeout=5)
    rows = output.split('\t')
    if len(rows) != 3 or Path(rows[0]).resolve() != (Path(directory) / 'data').resolve() or int(rows[1]) != value['mysqlPort']:
        raise RuntimeError('MySQL 身份与拥有者目录不一致。')
    if value.get('mysqlServerUUID') and rows[2] != value['mysqlServerUUID']:
        raise RuntimeError('MySQL 实例已经更换。')
    return rows[2]


def _start_mysql(directory, value, bootstrap=False):
    output = os.open(Path(directory) / 'mysql-process.log', os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    try:
        argv = [value['mysqld'], '--defaults-file=' + str(Path(directory) / 'mysql.cnf')]
        if bootstrap: argv.append('--skip-networking=ON')
        child = subprocess.Popen(argv, env=clean_environment(), stdin=subprocess.DEVNULL, stdout=output, stderr=output, start_new_session=True)
    finally: os.close(output)
    for _ in range(200):
        if child.poll() is not None: raise RuntimeError('拥有者 MySQL 未能启动。')
        if bootstrap:
            try:
                command([value['mysql'], '--no-defaults', '--protocol=SOCKET', '--socket=' + value['mysqlSocket'], '--user=root', '--batch', '--execute=SELECT 1;'], timeout=1)
                return child
            except Exception: pass
        else:
            try: mysql_identity(directory, value); return child
            except Exception: pass
        time.sleep(.05)
    child.terminate()
    with contextlib.suppress(subprocess.TimeoutExpired): child.wait(timeout=5)
    raise RuntimeError('拥有者 MySQL 未就绪。')


def _start_redis(directory, value, profile):
    try:
        identity = redis_identity(profile['QUIET_REDIS_URL'])
        if value.get('redisRunId') and identity['runId'] != value['redisRunId']:
            raise RuntimeError('Redis 实例已经更换。')
        return identity
    except (OSError, ConnectionError): pass
    output = os.open(Path(directory) / 'redis-process.log', os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    try:
        child = subprocess.Popen([value['redisServer'], str(Path(directory) / 'redis.conf')], env=clean_environment(), stdin=subprocess.DEVNULL, stdout=output, stderr=output, start_new_session=True)
    finally: os.close(output)
    for _ in range(100):
        if child.poll() is not None: raise RuntimeError('拥有者 Redis 未能启动。')
        try: return redis_identity(profile['QUIET_REDIS_URL'])
        except Exception: time.sleep(.05)
    child.terminate()
    with contextlib.suppress(subprocess.TimeoutExpired): child.wait(timeout=5)
    raise RuntimeError('拥有者 Redis 未就绪。')


def start(directory=DEFAULT_DIRECTORY, *, mysql_bin=None, redis_env=None, namespace=None, ephemeral=False):
    directory = Path(directory).absolute()
    os.umask(0o077)
    if (directory / 'infra.json').exists():
        value, profile = manifest(directory), read_profile(directory / 'storage.env')
        try: mysql_identity(directory, value)
        except Exception: _start_mysql(directory, value)
        if value['redisOwned']:
            identity = _start_redis(directory, value, profile)
            value.update(redisRunId=identity['runId'], redisPid=identity['pid'])
        else: redis_identity(profile['QUIET_REDIS_URL'])
        private_write(directory / 'infra.json', json.dumps(value, indent=2) + '\n', replace=True)
        return {'ok': True, 'directory': str(directory), 'mysqlPort': value['mysqlPort'], 'redisOwned': value['redisOwned']}
    if directory.exists() and any(directory.iterdir()):
        raise ValueError('新基础设施目录必须为空，不能覆盖已有数据。')
    directory.mkdir(parents=True, exist_ok=True, mode=0o700); directory.chmod(0o700)
    for part in ['data', 'socket', 'redis']: (directory / part).mkdir(mode=0o700)
    mysqld, mysql = binary('mysqld', mysql_bin), binary('mysql', mysql_bin)
    mysql_port = free_port(); mysql_socket = str(directory / 'socket/mysql.sock')
    if len(mysql_socket.encode()) > 100: raise ValueError('MySQL socket 路径过长，请使用短的私有目录。')
    mysql_password, root_password = secrets.token_hex(32), secrets.token_hex(32)
    value = {'schemaVersion': 1, 'directory': str(directory), 'mysqlVersion': '8.4', 'mysqld': mysqld, 'mysql': mysql,
             'mysqlPort': mysql_port, 'mysqlSocket': mysql_socket, 'redisOwned': redis_env is None, 'namespace': namespace or 'tango:mysql_v1', 'testOnlyPrivileges': bool(ephemeral)}
    cnf = f'''[mysqld]\nbasedir={Path(mysqld).parent.parent}\ndatadir={directory / 'data'}\nsocket={mysql_socket}\npid-file={directory / 'mysql.pid'}\nport={mysql_port}\nbind-address=127.0.0.1\nskip-name-resolve=ON\nmysqlx=0\nlocal-infile=0\nsecure-file-priv=NULL\ncharacter-set-server=utf8mb4\ncollation-server=utf8mb4_0900_bin\ndefault-time-zone=+00:00\ninnodb-buffer-pool-size=64M\nmax-connections=100\nlog-error={directory / 'mysql.log'}\n'''
    private_write(directory / 'mysql.cnf', cnf)
    command([mysqld, '--no-defaults', '--initialize-insecure', '--datadir=' + str(directory / 'data'), '--basedir=' + str(Path(mysqld).parent.parent)], timeout=120)
    bootstrap = _start_mysql(directory, value, bootstrap=True)
    try:
        sql = f"ALTER USER 'root'@'localhost' IDENTIFIED BY '{root_password}';\nCREATE DATABASE tango CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;\nCREATE USER 'tango'@'127.0.0.1' IDENTIFIED BY '{mysql_password}';\nGRANT ALL PRIVILEGES ON tango.* TO 'tango'@'127.0.0.1';\n"
        if ephemeral: sql += "GRANT ALL PRIVILEGES ON *.* TO 'tango'@'127.0.0.1';\n"
        command([mysql, '--no-defaults', '--protocol=SOCKET', '--socket=' + mysql_socket, '--user=root', '--batch'], input=sql)
    finally:
        bootstrap.terminate()
        try: bootstrap.wait(timeout=15)
        except subprocess.TimeoutExpired: bootstrap.kill(); bootstrap.wait(timeout=5)
    private_write(directory / 'root-client.cnf', f'[client]\nuser=root\npassword={root_password}\nprotocol=SOCKET\nsocket={mysql_socket}\n')
    profile = {'QUIET_STORAGE_DRIVER': 'mysql', 'QUIET_DATABASE_URL': f'mysql://tango:{mysql_password}@127.0.0.1:{mysql_port}/tango'}
    if redis_env:
        supplied = read_profile(Path(redis_env).absolute())
        profile.update(QUIET_REDIS_URL=supplied['QUIET_REDIS_URL'], QUIET_REDIS_NAMESPACE=namespace or supplied['QUIET_REDIS_NAMESPACE'])
        redis_identity(profile['QUIET_REDIS_URL'])
    else:
        redis_password, redis_port = secrets.token_hex(32), free_port()
        while redis_port == mysql_port: redis_port = free_port()
        value.update(redisPort=redis_port, redisServer=binary('redis-server'))
        profile.update(QUIET_REDIS_URL=f'redis://:{redis_password}@127.0.0.1:{redis_port}/0', QUIET_REDIS_NAMESPACE=namespace or 'tango:mysql_v1')
        private_write(directory / 'redis.conf', f'bind 127.0.0.1\nprotected-mode yes\nport {redis_port}\nrequirepass {redis_password}\ndaemonize no\nsave ""\nappendonly {"no" if ephemeral else "yes"}\ndir {directory / "redis"}\nmaxmemory 64mb\nmaxmemory-policy noeviction\n')
    if not all(c.isalnum() or c in ':_-' for c in profile['QUIET_REDIS_NAMESPACE']): raise ValueError('Redis namespace 无效。')
    value['namespace'] = profile['QUIET_REDIS_NAMESPACE']
    private_write(directory / 'storage.env', ''.join(key + '=' + val + '\n' for key, val in profile.items()))
    # 先保存私有拥有者清单，后续失败也能用明确目录安全关闭本实例。
    private_write(directory / 'infra.json', json.dumps(value, indent=2) + '\n')
    value['mysqlPid'] = _start_mysql(directory, value).pid
    value['mysqlServerUUID'] = mysql_identity(directory, value)
    if value['redisOwned']:
        identity = _start_redis(directory, value, profile)
        value.update(redisRunId=identity['runId'], redisPid=identity['pid'])
    private_write(directory / 'infra.json', json.dumps(value, indent=2) + '\n', replace=True)
    return {'ok': True, 'directory': str(directory), 'mysqlPort': mysql_port, 'redisOwned': value['redisOwned']}


def status(directory=DEFAULT_DIRECTORY):
    directory = Path(directory).absolute(); value = manifest(directory); profile = read_profile(directory / 'storage.env')
    try: mysql_identity(directory, value); mysql = True
    except Exception: mysql = False
    try:
        identity = redis_identity(profile['QUIET_REDIS_URL'])
        redis = not value['redisOwned'] or identity['runId'] == value.get('redisRunId')
    except Exception: redis = False
    return {'ok': mysql and redis, 'directory': str(directory), 'mysql': mysql, 'redis': redis, 'redisOwned': value['redisOwned']}


def stop(directory=DEFAULT_DIRECTORY):
    directory = Path(directory).absolute(); value = manifest(directory); profile = read_profile(directory / 'storage.env')
    try: mysql_identity(directory, value)
    except Exception:
        if (directory / 'mysql.pid').exists(): raise RuntimeError('不能验证现有 MySQL 身份，拒绝停止进程。')
    else:
        command([binary('mysqladmin', str(Path(value['mysql']).parent)), '--defaults-file=' + str(directory / 'root-client.cnf'), 'shutdown'], timeout=20)
    if value['redisOwned']:
        try: identity = redis_identity(profile['QUIET_REDIS_URL'])
        except (OSError, ConnectionError): identity = None
        if identity:
            if identity['runId'] != value.get('redisRunId') or identity['pid'] != value.get('redisPid'):
                raise RuntimeError('Redis 身份不匹配，拒绝停止进程。')
            # SHUTDOWN 可能在 AUTH 回复刷出前关闭连接；先前已核验实例身份，
            # 以随后无法连接为准，不能把正常断连当成停机失败。
            with contextlib.suppress(OSError, RuntimeError):
                redis_exchange(profile['QUIET_REDIS_URL'], ('SHUTDOWN',))
            for _ in range(50):
                try: redis_identity(profile['QUIET_REDIS_URL'])
                except (OSError, ConnectionError): break
                time.sleep(.05)
            else: raise RuntimeError('拥有者 Redis 未完成停止。')
    return {'ok': True, 'directory': str(directory), 'mysql': False, 'redisOwned': value['redisOwned']}


def main(argv=None):
    parser = argparse.ArgumentParser(description='管理独立拥有的 MySQL 8.4 与本机 Redis；绝不操作默认库。')
    parser.add_argument('action', choices=['start', 'status', 'stop'])
    parser.add_argument('--directory', default=str(DEFAULT_DIRECTORY))
    parser.add_argument('--mysql-bin')
    parser.add_argument('--redis-env', help='明确复用这个私有 profile 中的 Redis；不会停止它。')
    parser.add_argument('--namespace')
    args = parser.parse_args(argv)
    try:
        result = start(args.directory, mysql_bin=args.mysql_bin, redis_env=args.redis_env, namespace=args.namespace) if args.action == 'start' else status(args.directory) if args.action == 'status' else stop(args.directory)
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result['ok'] else 1
    except Exception:
        print(json.dumps({'ok': False, 'error': 'MYSQL_INFRA_FAILED'}, ensure_ascii=False), file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
