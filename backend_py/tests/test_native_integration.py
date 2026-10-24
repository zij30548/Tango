"""真实 Qt C++ 客户端 → Python HTTP → LangGraph → 假曲库结果的贯通测试。"""
import os
from pathlib import Path
import subprocess
import pytest
from helpers.process_fixture import backend,provider,PROJECT
from helpers.mysql_fixture import mysql_database

@pytest.fixture(params=['sqlite', 'mysql'])
async def storage_extra(request):
    if request.param == 'sqlite':
        yield {}
    else:
        async with mysql_database() as url:
            yield {'QUIET_STORAGE_DRIVER': 'mysql', 'QUIET_DATABASE_URL': url,
                   'QUIET_REDIS_URL': os.environ['TANGO_TEST_REDIS_URL'],
                   'QUIET_REDIS_NAMESPACE': 'tango:mysql_qt:' + __import__('uuid').uuid4().hex}

def test_native_qt_clients_connect_to_python_backend(tmp_path, storage_extra):
    qmake=Path('/opt/homebrew/opt/qt@5/bin/qmake')
    if not qmake.is_file():pytest.skip('本机没有 Qt5 qmake；需要单独执行原生集成验收')
    build=PROJECT/'build-macos-tests/python_backend_integration_test';build.mkdir(parents=True,exist_ok=True)
    for command in [[str(qmake),str(PROJECT/'tests/python_backend_integration_test.pro')],['make','-j4']]:
        result=subprocess.run(command,cwd=build,capture_output=True,text=True,timeout=120)
        assert result.returncode==0,result.stdout[-4000:]+result.stderr[-4000:]
    with provider() as (url,calls):
        with backend(tmp_path,url,storage_extra) as (client,proc,port):
            env={'PATH':os.environ.get('PATH','/usr/bin:/bin'),'MUSICPLAYER_BACKEND_URL':str(client.base_url).rstrip('/'),'MUSICPLAYER_DEVELOPMENT_MODE':'0','MUSICPLAYER_DATA_DIR':str(tmp_path/'qt-data'),'QT_QPA_PLATFORM':'offscreen'}
            result=subprocess.run([str(build/'python_backend_integration_test'),'-o','-,txt'],env=env,capture_output=True,text=True,timeout=45)
            (tmp_path/'qt-result.txt').write_text(result.stdout+result.stderr)
            (build/'last-python-qt-result.txt').write_text(result.stdout+result.stderr)
            assert result.returncode==0,result.stdout+result.stderr
            assert len(calls)>=3
