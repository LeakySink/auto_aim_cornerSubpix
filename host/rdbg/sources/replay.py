"""Replay source — preload .rlog and serve meta / JPEG on the HTTP shell."""

import base64
import json

from ..log.session import load_session_or_exit


def public_meta(session):
    frames = [
        {"i": i, "t": f["t"], "meta": f.get("meta") or {}}
        for i, f in enumerate(session.get("frames") or [])
    ]
    return {
        "file": session.get("file", ""),
        "path": session.get("path", ""),
        "sender": session.get("sender", ""),
        "t0_ns": session.get("t0_ns", 0),
        "duration": session.get("duration", 0),
        "memory_bytes": session.get("memory_bytes", 0),
        "fields": session.get("fields") or [],
        "series": session.get("series") or {},
        "frames": frames,
        "logs": session.get("logs") or [],
    }


class ReplaySource:
    id = "replay"

    def __init__(self, session):
        self.session = session
        self.shell = None

    def attach(self, shell):
        self.shell = shell
        shell.page("/", "replay.html")
        shell.page("/index.html", "replay.html")
        shell.sse_route("/events", on_connect=self.push_hello)
        shell.route("/select", self._handle_select)
        shell.route("/api/meta", self._handle_meta)
        shell.route("/api/session", self._handle_meta)
        shell.route("/api/frame/", self._handle_frame, prefix=True)

    def start(self):
        self.push_hello()

    def stop(self):
        pass

    def push_hello(self):
        if not self.shell:
            return
        name = self.session.get("file") or self.session.get("sender") or "replay"
        self.shell.sse.put(json.dumps({
            "type": "state",
            "active_sender": name,
            "senders": [name],
        }))
        self.shell.sse.put(json.dumps({
            "type": "status",
            "connected": True,
            "sender": name,
        }))

    def _handle_select(self, handler):
        handler.send(200, b'{"ok":true}', "application/json")

    def _handle_meta(self, handler):
        body = json.dumps(public_meta(self.session), separators=(",", ":"))
        handler.send(200, body, "application/json; charset=utf-8")

    def _handle_frame(self, handler):
        try:
            idx = int(handler.route_path.rsplit("/", 1)[-1])
        except ValueError:
            handler.send_error(400)
            return
        frames = self.session.get("frames") or []
        if idx < 0 or idx >= len(frames):
            handler.send_error(404)
            return
        fr = frames[idx]
        jpeg = fr.get("jpeg")
        if not jpeg:
            b64 = fr.get("b64")
            if b64:
                jpeg = base64.b64decode(b64)
        if not jpeg:
            handler.send_error(404)
            return
        handler.send(200, jpeg, "image/jpeg", cache="public, max-age=86400")


def load_or_exit(rlog, max_mb=1024):
    max_bytes = max(64, int(max_mb)) << 20
    return load_session_or_exit(str(rlog), max_bytes=max_bytes)
