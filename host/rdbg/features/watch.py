"""Watch feature — one thread per page, one UDP port per robot."""

from __future__ import annotations

import threading

from ..http.sse import SSEQueue, write_sse
from ..sources.fleet import fleet
from .base import Feature, json_body, send_json


class WatchFeature(Feature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    def __init__(self):
        super().__init__()
        self._source = None
        self._sender = ""
        self.sse = SSEQueue()

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/watch")
        shell.route(p + "/events", self._handle_events)
        shell.route(p + "/select", self._handle_select)
        shell.route(p + "/bind", self._handle_bind, methods=("POST",))
        shell.route(p + "/img_subscribe", self._handle_img_subscribe)
        shell.route(p + "/state", self._handle_state)

    def status(self):
        st = super().status()
        st["sender"] = self._sender
        if self._source is not None:
            st["data_port"] = self._source.data_port
            st["peer_port"] = self._source.peer_port
        return st

    def on_start(self, config):
        sender = (config.get("sender") or config.get("robot") or "").strip()
        if not sender:
            return
        self._bind(sender)

    def on_stop(self):
        sender = self._sender
        sse = self.sse
        self._source = None
        if sender:
            fleet.release(sender, sse)

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            pass

    def _bind(self, sender):
        sender = (sender or "").strip()
        if not sender:
            raise ValueError("sender required")
        if self._source is not None:
            if sender != self._sender:
                raise ValueError("already watching %s" % self._sender)
            return
        self._sender = sender
        self._source = fleet.acquire(sender, self.sse)
        with self._lock:
            self._config["sender"] = sender

    def _handle_events(self, handler):
        write_sse(handler, self.sse, on_connect=self._on_sse_connect)

    def _on_sse_connect(self):
        if self._source:
            self._source.push_state()

    def _handle_bind(self, handler):
        body = json_body(handler)
        sender = (body.get("sender") or body.get("robot") or "").strip()
        try:
            self._bind(sender)
        except Exception as e:
            send_json(handler, {"ok": False, "error": str(e)}, code=409)
            return
        send_json(handler, self.status())

    def _handle_select(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "pick a robot first"}, code=409)
            return
        send_json(handler, {"ok": True, "sender": self._sender, "data_port": self._source.data_port})

    def _handle_img_subscribe(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "not running"}, code=409)
            return
        raw = handler.query.get("streams", [""])[0]
        streams = []
        seen = set()
        for part in raw.split(","):
            name = part.strip()
            if name and name not in seen:
                seen.add(name)
                streams.append(name)
        self._source.set_img_streams(id(self.sse), streams)
        send_json(handler, {"ok": True, "streams": streams, "sender": self._sender})

    def _handle_state(self, handler):
        st = self.status()
        if self._source:
            st["senders"] = [self._sender] if self._sender else []
            st["selected"] = self._sender
        send_json(handler, st)
