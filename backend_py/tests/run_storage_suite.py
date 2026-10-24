"""独立启动临时 PG/Redis 跑迁移回归；不读取应用配置或已有数据库。

用法：backend_py/.venv/bin/python backend_py/tests/run_storage_suite.py --log /tmp/tango-python-full.log
可在 -- 后追加 pytest 参数，例如 -- tests/test_storage.py -q。
"""
import argparse
import contextlib
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PROJECT = Path(__file__).resolve().parents[2]
BACKEND = PROJECT / 'backend_py'


def private_write(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as file:
        file.write(value)
        file.flush()
        os.fsync(file.fileno())


def binary(name):
    found = shutil.which(name)
    if found:
        return found
    brew = shutil.which('brew')
    if brew:
        for formula in ['postgresql@17', 'postgresql@16', 'postgresql@18']:
            result = subprocess.run([brew, '--prefix', formula], capture_output=True, text=True, timeout=10)
            candidate = Path(result.stdout.strip()) / 'bin' / name
            if result.returncode == 0 and candidate.is_file():
                return str(candidate)
    raise RuntimeError('缺少测试服务程序：' + name)


def free_port():
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        return server.getsockname()[1]


def clean_environment():
    # 只传基础执行环境，不能继承真实服务密钥、数据库地址或应用配置。
    keep = ['PATH', 'HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'SYSTEMROOT', 'TMPDIR', 'QT_PLUGIN_PATH']
    return {key: os.environ[key] for key in keep if key in os.environ}


def command(argv, env, timeout=60):
    result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        # 服务日志留在拥有者临时目录，失败时只返回固定阶段，不打印配置内容。
        raise RuntimeError('测试基础设施启动失败：' + Path(argv[0]).name)
    return result.stdout


def redis_ready(port, password):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=.3) as connection:
            def request(*values):
                return ('*' + str(len(values)) + '\r\n' + ''.join('$' + str(len(v)) + '\r\n' + v + '\r\n' for v in values)).encode('ascii')
            connection.sendall(request('AUTH', password) + request('PING'))
            received = b''
            while b'+PONG\r\n' not in received and len(received) < 1024:
                part = connection.recv(1024)
                if not part:
                    break
                received += part
            return b'+PONG\r\n' in received
    except OSError:
        return False


def run(argv=None):
    parser = argparse.ArgumentParser(description='只用自建临时 PostgreSQL 和 Redis 执行 Python 全回归。')
    parser.add_argument('--log', help='保存已脱敏的 pytest 输出；不写入连接凭据。')
    parser.add_argument('pytest_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    extra = args.pytest_args[1:] if args.pytest_args[:1] == ['--'] else args.pytest_args
    selected = extra or ['tests', '-q']
    directory = Path(tempfile.mkdtemp(prefix='tgpy-', dir='/tmp'))
    os.chmod(directory, 0o700)
    env = clean_environment()
    redis = log_file = None
    pg_data, pg_ctl = directory / 'postgres', None
    pg_password, redis_password = secrets.token_hex(32), secrets.token_hex(32)
    pg_url = redis_url = ''
    try:
        pg_ctl, initdb, createdb, redis_server = [binary(name) for name in ['pg_ctl', 'initdb', 'createdb', 'redis-server']]
        pg_port, redis_port = free_port(), free_port()
        while pg_port == redis_port:
            redis_port = free_port()
        socket_dir = directory / 'socket'
        socket_dir.mkdir(mode=0o700)
        password_file, pgpass = directory / 'pg-password', directory / 'pgpass'
        private_write(password_file, pg_password + '\n')
        private_write(pgpass, f'127.0.0.1:{pg_port}:*:tango:{pg_password}\n')
        command([initdb, '-D', str(pg_data), '--username=tango', '--auth-local=scram-sha-256', '--auth-host=scram-sha-256',
            '--encoding=UTF8', '--locale=C', '--pwfile=' + str(password_file)], env)
        with (pg_data / 'postgresql.conf').open('a') as file:
            file.write(f"\nlisten_addresses='127.0.0.1'\nport={pg_port}\nunix_socket_directories='{socket_dir}'\nshared_buffers='32MB'\nmax_connections=80\npassword_encryption='scram-sha-256'\n")
        command([pg_ctl, '-D', str(pg_data), '-l', str(directory / 'postgres.log'), '-w', 'start'], env)
        command([createdb, '-h', '127.0.0.1', '-p', str(pg_port), '-U', 'tango', 'tango'], {**env, 'PGPASSFILE': str(pgpass)})
        redis_config = directory / 'redis.conf'
        private_write(redis_config, f"bind 127.0.0.1\nprotected-mode yes\nport {redis_port}\nrequirepass {redis_password}\ndaemonize no\nsave \"\"\nappendonly no\ndir {directory}\nmaxmemory 64mb\nmaxmemory-policy noeviction\n")
        with (directory / 'redis.log').open('wb') as file:
            redis = subprocess.Popen([redis_server, str(redis_config)], env=env, stdout=file, stderr=subprocess.STDOUT)
        for _ in range(50):
            if redis.poll() is not None:
                raise RuntimeError('测试基础设施启动失败：redis-server')
            if redis_ready(redis_port, redis_password):
                break
            time.sleep(.05)
        else:
            raise RuntimeError('临时 Redis 未就绪')
        pg_url = f'postgresql://tango:{pg_password}@127.0.0.1:{pg_port}/tango'
        redis_url = f'redis://:{redis_password}@127.0.0.1:{redis_port}/0'
        child_env = {**env, 'PYTHONPATH': str(BACKEND), 'PYTHONUNBUFFERED': '1',
            'TANGO_TEST_DATABASE_URL': pg_url, 'TANGO_TEST_REDIS_URL': redis_url,
            'TANGO_POSTGRES_TEST_URL': pg_url, 'TANGO_REDIS_TEST_URL': redis_url,
            'TANGO_PYTHON_CONVERSATION_PG_URL': pg_url}
        if args.log:
            target = Path(args.log).absolute()
            target.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            log_file = os.fdopen(fd, 'w')
        print('已启动独立临时 PostgreSQL 和 Redis；测试结束后自动清理。', flush=True)
        process = subprocess.Popen([sys.executable, '-m', 'pytest', *selected], cwd=BACKEND, env=child_env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        try:
            for line in process.stdout:
                redacted = line.replace(pg_url, '[临时 PostgreSQL]').replace(redis_url, '[临时 Redis]').replace(pg_password, '[已隐藏]').replace(redis_password, '[已隐藏]')
                sys.stdout.write(redacted)
                sys.stdout.flush()
                if log_file:
                    log_file.write(redacted)
                    log_file.flush()
            return process.wait()
        except BaseException:
            process.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=5)
            if process.poll() is None:
                process.kill()
                process.wait()
            raise
    finally:
        if log_file:
            log_file.close()
        if redis is not None and redis.poll() is None:
            redis.terminate()
            try:
                redis.wait(timeout=5)
            except subprocess.TimeoutExpired:
                redis.kill()
                redis.wait()
        if pg_ctl and (pg_data / 'postmaster.pid').is_file():
            with contextlib.suppress(Exception):
                command([pg_ctl, '-D', str(pg_data), '-m', 'fast', '-w', 'stop'], env, timeout=15)
        shutil.rmtree(directory)


if __name__ == '__main__':
    try:
        sys.exit(run())
    except KeyboardInterrupt:
        sys.stderr.write('测试已中止，临时服务已清理。\n')
        sys.exit(130)
    except Exception:
        sys.stderr.write('临时测试运行器失败；请检查本机 PostgreSQL/Redis 程序与全新的日志路径。\n')
        sys.exit(1)
