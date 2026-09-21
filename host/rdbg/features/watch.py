"""Watch feature — live UDP (thin wrapper around LiveSource)."""

from __future__ import annotations

import threading

from ..http.sse import SSEQueue, write_sse
from ..sources.live import LiveSource
from .base import Feature, send_json


_bus_lock = threading.Lock()
_bus = None
_bus_refs = 0
_subscribers = []


class _Fanout:
    """One LiveSource, many watch-page SSE queues."""

    def put(self, line):
        for q in list(_subscribers):
            try:
                q.put(line)
            except Exception:
                pass

    @property
    def sse(self):
        return self


_fanout = _Fanout()


def _watch_acquire(sse, config):
    global _bus, _bus_refs
    with _bus_lock:
        if sse not in _subscribers:
            _subscribers.append(sse)
            _bus_refs += 1
        if _bus is None:
            data_port = int(config.get("data_port") or 15001)
            peer_port = int(config.get("peer_port") or 15100)
            discover_port = int(config.get("discover_port") or 15999)
            src = LiveSource(data_port, peer_port, discover_port)
            src.shell = _fanout
            src.udp.on_output = _fanout.put
            src.start()
            _bus = src
        return _bus


def _watch_release(sse):
    global _bus, _bus_refs
    with _bus_lock:
        if sse not in _subscribers:
            return
        _subscribers.remove(sse)
        _bus_refs -= 1
        if _bus_refs <= 0 and _bus is not None:
            try:
                _bus.stop()
            except Exception:
                pass
            _bus = None
            _bus_refs = 0


class WatchFeature(Feature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    def __init__(self):
        super().__init__()
        self._source = None
        self.sse = SSEQueue()

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/watch")
        shell.route(p + "/events", self._handle_events)
        shell.route(p + "/select", self._handle_select)
        shell.route(p + "/img_subscribe", self._handle_img_subscribe)
        shell.route(p + "/state", self._handle_state)

    def on_start(self, config):
        self._source = _watch_acquire(self.sse, config)

    def on_stop(self):
        _watch_release(self.sse)
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
    """Kept for older call sites; live fanout uses _Fanout."""

    def __init__(self, sse, source):
        self.sse = sse
        self._source = source
