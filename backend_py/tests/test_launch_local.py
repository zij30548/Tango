"""启动器只探测隔离假 HTTP，不启动后台或接触真实配置与端口。"""
import asyncio
import hashlib
import json
from types import SimpleNamespace
import httpx
import pytest
from tango_backend import launch_local

NativeAsyncClient = httpx.AsyncClient
HEALTH = {'app': 'quiet-app-backend', 'runtimeLanguage': 'python', 'runtimeVersion': 2,
          'storage': 'sqlite', 'storageVersion': 1, 'rateLimiter': 'memory',
          'agentFeaturesVersion': 2, 'accountFeaturesVersion': 1, 'routingFeaturesVersion': 1,
          'modelRoutingVersion': 1,
          'lookupContextVersion': 1,
          'conversationClearVersion': 1,
          'musicIntentVersion': 1,
          'developmentMode': False, 'developmentSessionVersion': 1}

class CountingStream(httpx.AsyncByteStream):
    def __init__(self, chunks):
        self.chunks, self.readCount, self.closed = chunks, 0, False
    async def __aiter__(self):
        for chunk in self.chunks:
            self.readCount += 1
            yield chunk
    async def aclose(self):
        self.closed = True

@pytest.fixture
def probe(monkeypatch, tmp_path):
    monkeypatch.setattr(launch_local, 'PROJECT', tmp_path)
    monkeypatch.setenv('MUSICPLAYER_DEVELOPMENT_MODE', '0')
    monkeypatch.setenv('QUIET_STORAGE_DRIVER', 'sqlite')
    monkeypatch.setenv('NODE_ENV', 'development')
    monkeypatch.setenv('TANGO_ENV', 'development')
    monkeypatch.delenv('MUSICPLAYER_DEVELOPMENT_TOKEN_FILE', raising=False)
    calls, client_options = [], []
    responses = {'/health': lambda: httpx.Response(200, json=dict(HEALTH)), '/ready': lambda: httpx.Response(200, json={'ready': True})}
    def handle(request):
        calls.append(request.url.path)
        assert request.url.host == '127.0.0.1' and request.url.port == 19731
        return responses[request.url.path]()
    def client(**options):
        client_options.append(options)
        return NativeAsyncClient(transport=httpx.MockTransport(handle), **options)
    def forbidden_spawn(*args, **kwargs):
        pytest.fail('探测失败或复用现有后台时不能启动新进程')
    monkeypatch.setattr(launch_local.httpx, 'AsyncClient', client)
    monkeypatch.setattr(launch_local.subprocess, 'Popen', forbidden_spawn)
    return SimpleNamespace(responses=responses, calls=calls, options=client_options, root=tmp_path)

async def test_matching_python_service_is_reused_without_creating_files(probe):
    await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health', '/ready']
    assert probe.options[0] == {'timeout': 1.8, 'trust_env': False, 'follow_redirects': False}
    assert not (probe.root / 'backend_py').exists()

@pytest.mark.parametrize('patch,expected', [
    ({'app': 'unknown-service'}, '其它服务'),
    ({'runtimeLanguage': 'node'}, '旧后台'),
    ({'runtimeVersion': 1}, '旧后台'),
    ({'routingFeaturesVersion': 0}, '旧后台'),
    ({'modelRoutingVersion': 0}, '旧后台'),
    ({'modelRoutingVersion': None}, '旧后台'),
    ({'lookupContextVersion': 0}, '旧后台'),
    ({'lookupContextVersion': None}, '旧后台'),
    ({'lookupContextVersion': 2}, '旧后台'),
    ({'conversationClearVersion': 0}, '旧后台'),
    ({'conversationClearVersion': None}, '旧后台'),
    ({'conversationClearVersion': 2}, '旧后台'),
    ({'conversationClearVersion': '1'}, '旧后台'),
    ({'conversationClearVersion': True}, '旧后台'),
    ({'musicIntentVersion': None}, '旧后台'),
    ({'musicIntentVersion': 0}, '旧后台'),
    ({'musicIntentVersion': True}, '旧后台'),
    ({'storage': 'postgres'}, '存储模式不匹配'),
    ({'storageVersion': 0}, '存储模式不匹配'),
    ({'developmentMode': True}, '开发模式不匹配')])
async def test_existing_service_metadata_mismatch_never_spawns(probe, patch, expected):
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, **patch})
    with pytest.raises(ValueError, match=expected):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']
    assert not (probe.root / 'backend_py').exists()

async def test_postgres_requires_redis_metadata(probe, monkeypatch):
    monkeypatch.setenv('QUIET_STORAGE_DRIVER', 'postgres')
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'storage': 'postgres', 'rateLimiter': 'memory'})
    with pytest.raises(ValueError, match='存储模式不匹配'):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']

async def test_default_mysql_reuses_only_mysql_with_redis(probe, monkeypatch):
    monkeypatch.delenv('QUIET_STORAGE_DRIVER')
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'storage':'mysql', 'rateLimiter':'redis'})
    await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health', '/ready']
    probe.calls.clear()
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'storage':'mysql', 'rateLimiter':'memory'})
    with pytest.raises(ValueError, match='存储模式不匹配'):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']

async def test_active_mysql_does_not_reuse_old_postgres_service(probe, monkeypatch):
    monkeypatch.delenv('QUIET_STORAGE_DRIVER')
    directory = probe.root / '.local/mysql-v1'
    directory.mkdir(parents=True)
    marker = directory / 'active.json'
    marker.write_text('{"driver":"mysql","version":1}'); marker.chmod(0o600)
    (directory / 'storage.env').write_text('fixture-placeholder\n')
    legacy = probe.root / '.local/storage-v1/storage.env'
    legacy.parent.mkdir(parents=True);legacy.write_text('legacy-placeholder\n')
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'storage':'postgres', 'rateLimiter':'redis'})
    with pytest.raises(ValueError, match='存储模式不匹配'):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']

async def test_development_fingerprint_must_match_private_fixture_token(probe, monkeypatch):
    token, path = '1' * 64, probe.root / 'fixture.token'
    path.write_text(token + '\n'); path.chmod(0o600)
    monkeypatch.setenv('MUSICPLAYER_DEVELOPMENT_MODE', '1')
    monkeypatch.setenv('MUSICPLAYER_DEVELOPMENT_TOKEN_FILE', str(path))
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'developmentMode': True, 'developmentTokenFingerprint': '0' * 64})
    with pytest.raises(ValueError, match='开发模式不匹配'):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']
    probe.calls.clear()
    probe.responses['/health'] = lambda: httpx.Response(200, json={**HEALTH, 'developmentMode': True, 'developmentTokenFingerprint': hashlib.sha256(token.encode()).hexdigest()})
    await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health', '/ready']

@pytest.mark.parametrize('route', ['/health', '/ready'])
async def test_oversized_content_length_is_rejected_before_reading_body(probe, route):
    stream = CountingStream([b'x' * 1024] * 100)
    probe.responses[route] = lambda: httpx.Response(200, headers={'content-length': '102400'}, stream=stream)
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert stream.readCount == 0 and stream.closed
    assert not (probe.root / 'backend_py').exists()

@pytest.mark.parametrize('route', ['/health', '/ready'])
async def test_chunked_response_stops_at_byte_limit_instead_of_reading_everything(probe, route):
    prefix = json.dumps(HEALTH if route == '/health' else {'ready': True}).encode()
    body = prefix + b' ' * (4096 - len(prefix))
    stream = CountingStream([body[n:n + 1024] for n in range(0, 4096, 1024)] + [b' '] * 100)
    probe.responses[route] = lambda: httpx.Response(200, stream=stream)
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert stream.closed and stream.readCount < len(stream.chunks)

async def test_limit_is_utf8_bytes_and_4096_byte_boundary_is_accepted(probe):
    body = json.dumps({**HEALTH, 'padding': '歌' * 1500}, ensure_ascii=False).encode()
    assert len(body.decode()) < 4096 < len(body)
    probe.responses['/health'] = lambda: httpx.Response(200, stream=CountingStream([body]))
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    for route, value in [('/health', HEALTH), ('/ready', {'ready': True})]:
        prefix = json.dumps(value).encode()
        exact = prefix + b' ' * (4096 - len(prefix))
        probe.responses[route] = lambda exact=exact: httpx.Response(200, content=exact)
    await launch_local.launch('http://127.0.0.1:19731')

@pytest.mark.parametrize('route,status', [('/health', 302), ('/health', 500), ('/ready', 302), ('/ready', 503)])
async def test_redirect_or_error_status_is_not_accepted_or_followed(probe, route, status):
    probe.responses[route] = lambda: httpx.Response(status, headers={'location': '/must-not-follow'}, json=HEALTH if route == '/health' else {'ready': True})
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert '/must-not-follow' not in probe.calls
    assert len(probe.calls) == (1 if route == '/health' else 2)

@pytest.mark.parametrize('content', [b'[]', b'null', b'{invalid', b'{"x":NaN}'])
async def test_health_must_be_a_strict_json_object(probe, content):
    probe.responses['/health'] = lambda: httpx.Response(200, content=content)
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert probe.calls == ['/health']

async def test_total_probe_timeout_closes_stalled_stream_without_spawn(probe, monkeypatch):
    monkeypatch.setattr(launch_local, 'PROBE_TIMEOUT', .02)
    class Stalled(httpx.AsyncByteStream):
        closed = False
        async def __aiter__(self):
            await asyncio.sleep(60)
            yield b'{}'
        async def aclose(self):
            self.closed = True
    stream = Stalled()
    probe.responses['/health'] = lambda: httpx.Response(200, stream=stream)
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert stream.closed and probe.calls == ['/health']

async def test_compressed_probe_is_rejected_before_decompression(probe):
    stream = CountingStream([b'not-even-valid-gzip'])
    probe.responses['/health'] = lambda: httpx.Response(200, headers={'content-encoding': 'gzip'}, stream=stream)
    with pytest.raises(ValueError):
        await launch_local.launch('http://127.0.0.1:19731')
    assert stream.readCount == 0 and stream.closed
