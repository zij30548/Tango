"""独立持有的假模型HTTP和Python后台进程，完全隔离真实配置与用户数据。"""
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
import httpx

PROJECT=Path(__file__).resolve().parents[3]

def free_port():
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));return sock.getsockname()[1]

def brief():return {'kind':'research','title':'阅读时刻','scene':'读书','styles':[],'queries':['读书'],'artist':'','artistId':0,'requiredArtist':'','artistOnly':False,'since':'','until':'','targetMinutes':20,'excludeLive':True,'avoidPrevious':False,'excludedArtists':[],'requireReleases':False,'retainFirst':0,'removeIds':[],'question':'','adjustment':''}

@contextmanager
def provider():
    calls=[]
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_POST(self):
            data=json.loads(self.rfile.read(int(self.headers['content-length'])));calls.append(data);prompt=data['messages'][0]['content'];input=json.loads(data['messages'][1]['content'])
            # 路由角色由独立输入结构识别，不依赖提示词中文称谓。
            if {'conversationId','messageId','task'} <= set(input):
                value={'intents':['scene_research'],'slots':{'artist':'','targetMinutes':20},'actions':[{'capability':'music.task','source':input['instruction'],'dependsOn':[],'parameters':{'mode':'new'}}],'missingSlots':[]}
            elif '职责是理解首次完整任务' in prompt:value=brief()
            elif '本轮修改解析器' in prompt:value={'kind':'edit','changes':{},'question':''}
            elif '研究规划器' in prompt:value={'queries':['读书'],'artist':'','artistId':0,'since':'','until':'','note':''}
            elif '研究结果观察器' in prompt:value={'decision':'done','queries':[],'note':'候选足够'}
            elif '优先候选前缀' in prompt:value={'order':list(range(min(5,len(input['candidates']))))}
            else:value={'kind':'clarify','question':'测试上游未识别此请求'}
            body=json.dumps({'choices':[{'finish_reason':'stop','message':{'content':json.dumps(value,ensure_ascii=False)}}],'usage':{'prompt_tokens':3,'completion_tokens':2}},ensure_ascii=False).encode()
            self.send_response(200);self.send_header('content-type','application/json');self.send_header('content-length',str(len(body)));self.end_headers();self.wfile.write(body)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:yield f'http://127.0.0.1:{server.server_port}/v1',calls
    finally:server.shutdown();server.server_close();thread.join(timeout=2)

@contextmanager
def backend(directory,provider_url,extra=None):
    directory=Path(directory);port=free_port();log=directory/'process.log';directory.mkdir(exist_ok=True,parents=True)
    # 不继承宿主QWEN、PG、Redis或追踪凭据。
    env={'PATH':os.environ.get('PATH','/usr/bin:/bin'),'LANG':'en_US.UTF-8','QUIET_BACKEND_HOST':'127.0.0.1','QUIET_BACKEND_PORT':str(port),'QUIET_STORAGE_DRIVER':'sqlite','QUIET_DB_PATH':str(directory/'accounts.sqlite'),'QUIET_REGISTRATION_MODE':'open','QWEN_API_KEY':'owned-python-fixture-key','QWEN_BASE_URL':provider_url,'QWEN_MODEL':'qwen-python-fixture',**(extra or {})}
    with log.open('ab') as output:
        proc=subprocess.Popen([sys.executable,'-m','tango_backend.server','--no-env'],cwd=PROJECT/'backend_py',env=env,stdout=output,stderr=output)
        try:
            origin=f'http://127.0.0.1:{port}'
            with httpx.Client(base_url=origin,timeout=.5,trust_env=False) as client:
                for _ in range(120):
                    if proc.poll() is not None:raise AssertionError('隔离后台启动失败: '+log.read_text()[-5000:])
                    try:
                        if client.get('/ready').status_code==200:break
                    except httpx.HTTPError:pass
                    time.sleep(.05)
                else:raise AssertionError('隔离后台未就绪: '+log.read_text()[-5000:])
                yield client,proc,port
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=8)
                except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=2)
