"""Fleet-bound feature base — Watch / Calibrate / TF Viz share bind + SSE.

Per-instance HTTP (prefix usually /api/i/<id>, see host/API.md):
  GET  {p}/events          SSE
  POST {p}/bind            {sender|robot}
  GET  {p}/state
  GET  {p}/select?sender=  if select=True
  GET  {p}/img_subscribe?streams=  if img_subscribe=True (Watch)
"""

from __future__ import annotations

import threading

from ..http.sse import SSEQueue, write_sse
from ..sources.fleet import fleet
from ..util import parse_stream_list, sse_state
from .base import Feature, json_body, send_json


class FleetBoundFeature(Feature):
    """One open page ↔ one fleet slot (UDP data/peer) + per-page SSE queue.

    Subclass hooks:
      allow_rebind          Calibrate may switch sender; Watch locks once bound
      default_img_streams   If set, subscribe these image streams on bind/SSE
      use_source_push_state Watch uses LiveSource.push_state; Calibrate pushes fleet list
    """

    allow_rebind = False
    default_img_streams = None  # optional list[str]
    use_source_push_state = True

    def __init__(self):
        super().__init__()
        self._source = None
        self._sender = ""
        self.sse = SSEQueue()

    def attach_fleet_routes(self, shell, *, select=True, img_subscribe=False):
        """Register shared fleet HTTP routes under self.api_prefix."""
        p = getattr(self, "api_prefix", f"/api/{self.id}")
        shell.route(p + "/events", self._handle_events)
        shell.route(p + "/bind", self._handle_bind, methods=("POST",))
        shell.route(p + "/state", self._handle_state)
        if select:
            shell.route(p + "/select", self._handle_select)
        if img_subscribe:
            shell.route(p + "/img_subscribe", self._handle_img_subscribe)

    def status(self):
        st = super().status()
        st["sender"] = self._sender
        if self._source is not None:
            st["data_port"] = self._source.data_port
            st["peer_port"] = self._source.peer_port
        return st

    def on_start(self, config):
        sender = (config.get("sender") or config.get("robot") or "").strip()
        if sender:
            self._bind(sender)

    def on_stop(self):
        sender = self._sender
        sse = self.sse
        self._source = None
        self._sender = ""
        if sender:
            fleet.release(sender, sse)

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            self.on_tick()

    def on_tick(self):
        """Optional periodic work while the feature thread is running."""

    def list_senders(self):
        return [self._sender] if self._sender else []

    def after_bind(self):
        """Called after a successful _bind (POST /bind or rebind select)."""

    def on_sse_connect(self):
        if not self._source:
            self.push_page_state()
            return
        streams = self.default_img_streams
        if streams is not None:
            self._source.set_img_streams(id(self.sse), list(streams))
        if self.use_source_push_state:
            self._source.push_state()
        else:
            self.push_page_state()

    def push_page_state(self, senders=None):
        names = list(senders if senders is not None else self.list_senders())
        active = self._sender if self._sender in names else (names[0] if names else "")
        self.sse.put(sse_state(active, names))

    def _bind(self, sender):
        sender = (sender or "").strip()
        if not sender:
            raise ValueError("sender required")
        if self._source is not None:
            if sender == self._sender:
                return
            if not self.allow_rebind:
                raise ValueError("already watching %s" % self._sender)
            fleet.release(self._sender, self.sse)
            self._source = None
            self._sender = ""
        self._sender = sender
        self._source = fleet.acquire(sender, self.sse)
        streams = self.default_img_streams
        if streams is not None:
            self._source.set_img_streams(id(self.sse), list(streams))
        with self._lock:
            self._config["sender"] = sender
        self.after_bind()

    def _robot_info(self):
        name = self._sender
        if not name or not self._source:
            return "", {}
        with self._source._lock:
            info = dict(self._source.robots.get(name) or {})
        return name, info

    def _handle_events(self, handler):
        write_sse(handler, self.sse, on_connect=self.on_sse_connect)

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
        sender = (handler.query.get("sender") or [""])[0].strip()
        if sender and self.allow_rebind:
            try:
                self._bind(sender)
            except Exception as e:
                send_json(handler, {"ok": False, "error": str(e)}, code=409)
                return
        if not self._source:
            send_json(handler, {"ok": False, "error": "pick a robot first"}, code=409)
            return
        out = {"ok": True, "sender": self._sender}
        if self._source is not None:
            out["data_port"] = self._source.data_port
        send_json(handler, out)

    def _handle_state(self, handler):
        st = self.status()
        st["senders"] = self.list_senders()
        st["selected"] = self._sender
        send_json(handler, st)

    def _handle_img_subscribe(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "not running"}, code=409)
            return
        raw = (handler.query.get("streams") or [""])[0]
        streams = parse_stream_list(raw)
        self._source.set_img_streams(id(self.sse), streams)
        send_json(handler, {"ok": True, "streams": streams, "sender": self._sender})
