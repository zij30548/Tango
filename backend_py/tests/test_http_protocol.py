"""真实 TCP 边界：未完成的请求头不能长期占连接，完成请求不被误切断。"""
import asyncio
import uvicorn
from uvicorn.server import ServerState
from tango_backend.http_protocol import BoundedH11Protocol

async def app(scope,receive,send):
    await send({'type':'http.response.start','status':200,'headers':[(b'content-length',b'2')]})
    await send({'type':'http.response.body','body':b'ok'})

async def test_initial_and_keepalive_slow_headers_are_closed():
    class Protocol(BoundedH11Protocol):header_timeout=.05
    config=uvicorn.Config(app,access_log=False,log_level='critical',lifespan='off');state=ServerState()
    loop=asyncio.get_running_loop();server=await loop.create_server(lambda:Protocol(config,state,{}),'127.0.0.1',0)
    port=server.sockets[0].getsockname()[1]
    try:
        reader,writer=await asyncio.open_connection('127.0.0.1',port)
        writer.write(b'GET /health HTTP/1.1\r\nHost:');await writer.drain()
        assert await asyncio.wait_for(reader.read(),.5)==b'';writer.close();await writer.wait_closed()
        reader,writer=await asyncio.open_connection('127.0.0.1',port)
        writer.write(b'GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n');await writer.drain()
        header=await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'),.5);assert b'200 OK' in header
        assert await reader.readexactly(2)==b'ok'
        writer.write(b'GET /next HTTP/1.1\r\nHost:');await writer.drain()
        assert await asyncio.wait_for(reader.read(),.5)==b'';writer.close();await writer.wait_closed()
    finally:server.close();await server.wait_closed()

async def test_per_connection_request_cap():
    class Protocol(BoundedH11Protocol):max_requests=2
    config=uvicorn.Config(app,access_log=False,log_level='critical',lifespan='off');state=ServerState()
    loop=asyncio.get_running_loop();server=await loop.create_server(lambda:Protocol(config,state,{}),'127.0.0.1',0)
    try:
        reader,writer=await asyncio.open_connection('127.0.0.1',server.sockets[0].getsockname()[1])
        for _ in range(2):
            writer.write(b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n');await writer.drain()
            assert b'200 OK' in await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'),.5)
            assert await reader.readexactly(2)==b'ok'
        assert await asyncio.wait_for(reader.read(),.5)==b''
        writer.close();await writer.wait_closed()
    finally:server.close();await server.wait_closed()
