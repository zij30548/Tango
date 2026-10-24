"""补齐原 Node 服务的慢请求头与每连接请求数保护。"""

import h11
from uvicorn.protocols.http.h11_impl import H11Protocol


class BoundedH11Protocol(H11Protocol):
    header_timeout = 10
    max_requests = 100

    def connection_made(self, transport):
        self._header_timer = None
        self._completed_requests = 0
        super().connection_made(transport)
        self._arm_headers()

    def _arm_headers(self):
        self._cancel_headers()
        self._header_timer = self.loop.call_later(
            self.header_timeout, self._headers_expired
        )

    def _cancel_headers(self):
        if self._header_timer:
            self._header_timer.cancel()
            self._header_timer = None

    def _headers_expired(self):
        self._header_timer = None
        self.transport.close()

    def data_received(self, data):
        super().data_received(data)
        # 请求头收齐后，正文期限由readBody独立控制；长时间SSE不受头部期限影响。
        if (
            self.conn.their_state in (h11.SEND_BODY, h11.DONE)
            and self.cycle
            and not self.cycle.response_complete
        ):
            self._cancel_headers()

    def on_response_complete(self):
        super().on_response_complete()
        self._completed_requests += 1
        if self._completed_requests >= self.max_requests:
            self.transport.close()
        elif self.conn.our_state is not h11.MUST_CLOSE:
            self._arm_headers()

    def connection_lost(self, exc):
        self._cancel_headers()
        super().connection_lost(exc)
