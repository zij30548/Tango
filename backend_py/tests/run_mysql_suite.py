"""全新拥有者 MySQL/Redis 测试运行器；不加载真实后台 env 或既有用户库。"""
import argparse
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BACKEND = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND))
from tango_backend.mysql_infra import start, stop, read_profile, clean_environment


def run(argv=None):
    parser = argparse.ArgumentParser(description='独立临时 MySQL 8.4 与 Redis 全回归，结束自动停止并清理。')
    parser.add_argument('--log')
    parser.add_argument('pytest_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    extra = args.pytest_args[1:] if args.pytest_args[:1] == ['--'] else args.pytest_args
    selected = extra or ['tests', '-q']
    directory = Path(tempfile.mkdtemp(prefix='tgmy-', dir='/tmp'))
    directory.chmod(0o700)
    process = log = None
    try:
        start(directory, ephemeral=True, namespace='tango:mysql_fixture')
        profile = read_profile(directory / 'storage.env')
        mysql_url, redis_url = profile['QUIET_DATABASE_URL'], profile['QUIET_REDIS_URL']
        env = {**clean_environment(), 'PYTHONPATH': str(BACKEND), 'PYTHONUNBUFFERED': '1',
               'TANGO_MYSQL_TEST_URL': mysql_url, 'TANGO_TEST_REDIS_URL': redis_url, 'TANGO_REDIS_TEST_URL': redis_url}
        secrets = [mysql_url, redis_url, mysql_url.split(':', 2)[2].split('@', 1)[0], redis_url.split(':', 2)[2].split('@', 1)[0]]
        if args.log:
            path = Path(args.log).absolute(); path.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            log = os.fdopen(fd, 'w')
        print('已启动独立临时 MySQL 8.4 与 Redis；不使用当前应用服务，结束后清理。', flush=True)
        process = subprocess.Popen([sys.executable, '-m', 'pytest', *selected], cwd=BACKEND, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in process.stdout:
            for secret in secrets:
                if secret: line = line.replace(secret, '[临时凭据已隐藏]')
            sys.stdout.write(line); sys.stdout.flush()
            if log: log.write(line); log.flush()
        return process.wait()
    finally:
        if process and process.poll() is None:
            process.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired): process.wait(timeout=5)
            if process.poll() is None: process.kill(); process.wait(timeout=2)
        if log: log.close()
        if (directory / 'infra.json').exists():
            stop(directory)
        shutil.rmtree(directory)

if __name__ == '__main__':
    try:
        raise SystemExit(run())
    except KeyboardInterrupt:
        print('测试已中止；拥有者临时服务已清理。', file=sys.stderr)
        raise SystemExit(130)
    except Exception:
        print('MySQL 测试运行器失败；请检查 MySQL 8.4/Redis 程序及全新的日志路径。', file=sys.stderr)
        raise SystemExit(1)
