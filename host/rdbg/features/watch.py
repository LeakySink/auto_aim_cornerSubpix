"""Watch feature — live UDP (thin wrapper around LiveSource)."""

from __future__ import annotations

import threading

from ..http.sse import SSEQueue, write_sse
from ..sources.live import LiveSource
from .base import Feature, send_json


class WatchFeature(Feature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    def __init__(self):
        super().__init__()
        self._source = None
        self.sse = SSEQueue()

    def attach(self, shell):
        shell.route("/api/watch/events", self._handle_events)
        shell.route("/api/watch/select", self._handle_select)
        shell.route("/api/watch/img_subscribe", self._handle_img_subscribe)
        shell.route("/api/watch/state", self._handle_state)

    def on_start(self, config):
        data_port = int(config.get("data_port") or 15001)
        peer_port = int(config.get("peer_port") or 15100)
        discover_port = int(config.get("discover_port") or 15999)
        self._source = LiveSource(data_port, peer_port, discover_port)
        # Wire SSE without attaching LiveSource pages to /
        self._source.shell = self  # duck-type: push via self.sse
        self._source.udp.on_output = lambda line: self.sse.put(line)
        # Monkey-patch push to our sse: LiveSource uses shell.sse
        # Provide compatible shell facade
        self._source.shell = _SseFacade(self.sse, self._source)
        self._source.start()

    def on_stop(self):
        if self._source:
            try:
                self._source.stop()
            except Exception:
                pass
            self._source = None

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            pass

    def _handle_events(self, handler):
        write_sse(handler, self.sse, on_connect=self._on_sse_connect)

    def _on_sse_connect(self):
        if self._source:
            self._source.push_state()

    def _handle_select(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "not running"}, code=409)
            return
        self._source._handle_select(handler)

    def _handle_img_subscribe(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "not running"}, code=409)
            return
        self._source._handle_img_subscribe(handler)

    def _handle_state(self, handler):
        st = self.status()
        if self._source:
            st["senders"] = self._source.live_senders()
            st["selected"] = self._source._selected
        send_json(handler, st)


class _SseFacade:
    """Minimal shell stand-in so LiveSource can push to our SSE queue."""

    def __init__(self, sse, source):
        self.sse = sse
        self._source = source
