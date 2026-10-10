"""One beacon listener; each robot gets its own free data/peer UDP ports.

HTTP: GET /api/robots → snapshot()（见 host/API.md）。
列表离线由 BEACON_STALE_S 过滤；不清车上 host 队列。
"""

from __future__ import annotations

import socket
import threading
import time
import uuid

from ..net.control import BEACON_STALE_S, Discovery, resolve_portal_feature, normalize_app
from .live import LiveSource

DISCOVER_PORT = 15999
DATA_BASE = 15001
PEER_BASE = 15100


def _bindable(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", int(port)))
    except OSError:
        return False
    finally:
        sock.close()
    return True


class _Fan:
    def __init__(self):
        self.subs = []

    def put(self, line):
        for q in list(self.subs):
            try:
                q.put(line)
            except Exception:
                pass

    @property
    def sse(self):
        return self


class RobotFleet:
    def __init__(self):
        self.host_id = uuid.uuid4().hex
        self.discover = Discovery(port=DISCOVER_PORT, host_id=self.host_id)
        self.discover.on_beacon = self._on_beacon
        self._lock = threading.Lock()
        self._robots = {}
        self._slots = {}
        self._used = set()
        self._started = False

    def start(self):
        if self._started:
            return
        self.discover.start()
        self._started = True

    def stop(self):
        with self._lock:
            names = list(self._slots)
        for name in names:
            self._drop(name)
        if self._started:
            self.discover.stop()
            self._started = False

    def attach(self, shell):
        shell.route("/api/robots", self._handle_robots)

    def snapshot(self):
        now = time.monotonic()
        with self._lock:
            out = []
            for name, info in self._robots.items():
                if now - info["last"] > BEACON_STALE_S:
                    continue
                slot = self._slots.get(name)
                out.append({
                    "name": name,
                    "ip": info["ip"],
                    "control": info["control"],
                    "app": info.get("app") or "normal",
                    "feature": info.get("feature") or resolve_portal_feature(
                        info.get("feature_raw"), info.get("app")),
                    "data_port": slot["data_port"] if slot else 0,
                    "peer_port": slot["peer_port"] if slot else 0,
                    "watches": len(slot["fan"].subs) if slot else 0,
                })
        out.sort(key=lambda row: row["name"])
        return out

    def _handle_robots(self, handler):
        from ..features.base import send_json
        send_json(handler, {"robots": self.snapshot()})

    def _on_beacon(self, name, ip, control, _addr, app="normal", feature=""):
        app = normalize_app(app)
        feat = resolve_portal_feature(feature, app)
        with self._lock:
            self._robots[name] = {
                "ip": ip,
                "control": int(control),
                "app": app,
                "feature_raw": (feature or "").strip().lower(),
                "feature": feat,
                "last": time.monotonic(),
            }
            slot = self._slots.get(name)
        if slot:
            slot["source"].note_beacon(name, ip, control)

    def _take_port(self, start):
        for port in range(int(start), int(start) + 200):
            if port == DISCOVER_PORT or port in self._used:
                continue
            if _bindable(port):
                self._used.add(port)
                return port
        raise RuntimeError("no free UDP port from %s" % start)

    def acquire(self, sender, sse):
        sender = (sender or "").strip()
        if not sender:
            raise ValueError("sender required")
        with self._lock:
            slot = self._slots.get(sender)
            if slot is not None:
                if sse not in slot["fan"].subs:
                    slot["fan"].subs.append(sse)
                return slot["source"]
            data_port = self._take_port(DATA_BASE)
            peer_port = self._take_port(PEER_BASE)
            fan = _Fan()
            fan.subs.append(sse)
            src = LiveSource(
                data_port, peer_port, DISCOVER_PORT,
                target=sender, manage_discovery=False)
            src.shell = fan
            src.udp.on_output = fan.put
            slot = {
                "fan": fan,
                "source": src,
                "data_port": data_port,
                "peer_port": peer_port,
            }
            self._slots[sender] = slot
        try:
            src.start()
            if not src.udp.running:
                raise RuntimeError("bind data port %s failed" % data_port)
        except Exception:
            try:
                src.stop()
            except Exception:
                pass
            with self._lock:
                self._slots.pop(sender, None)
                self._used.discard(data_port)
                self._used.discard(peer_port)
            raise
        with self._lock:
            info = dict(self._robots.get(sender) or {})
        if info:
            src.note_beacon(sender, info["ip"], info["control"])
        return src

    def release(self, sender, sse):
        sender = (sender or "").strip()
        with self._lock:
            slot = self._slots.get(sender)
            if slot is None or sse not in slot["fan"].subs:
                return
            slot["fan"].subs.remove(sse)
            empty = not slot["fan"].subs
            source = slot["source"]
        try:
            source.clear_img_key(id(sse))
        except Exception:
            pass
        if empty:
            self._drop(sender)

    def _drop(self, sender):
        with self._lock:
            slot = self._slots.pop(sender, None)
            if slot is None:
                return
            self._used.discard(slot["data_port"])
            self._used.discard(slot["peer_port"])
        try:
            slot["source"].stop()
        except Exception:
            pass


fleet = RobotFleet()
