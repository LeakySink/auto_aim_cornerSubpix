"""Live UDP source — discover robots, register, head-forward. PROTOCOL.md."""

import json
import socket
import threading
import time
import uuid

from ..net.control import BEACON_STALE_S, Discovery, RobotClient
from ..net.peer import PeerHub
from ..net.udp import UdpBackend, packet_sender
from ..util import parse_stream_list, sse_state


class LiveSource:
    id = "live"

    def __init__(self, data_port=15001, peer_port=15100, discover_port=15999,
                 target="", manage_discovery=True):
        self.data_port = int(data_port)
        self.peer_port = int(peer_port)
        self.discover_port = int(discover_port)
        self._target = (target or "").strip()
        self._manage_discovery = bool(manage_discovery)
        self.host_id = uuid.uuid4().hex
        self.host_name = socket.gethostname() or "watch"

        self.udp = UdpBackend()
        self.udp.on_state = self.push_state
        self.udp.on_raw = self._on_raw

        self.discover = Discovery(port=self.discover_port, host_id=self.host_id)
        self.discover.on_beacon = self._on_beacon
        self.client = RobotClient(
            self.host_id, self.host_name, self.data_port, self.peer_port)
        self.peer = PeerHub(self.host_id, self.peer_port, self.data_port)
        self.peer.on_msg = self._on_peer
        self.peer.is_head = self._is_head

        self.shell = None
        self._lock = threading.Lock()
        self._running = False
        self._poller = None
        self._selected = ""
        # name -> {ip, control, last}
        self.robots = {}
        # name -> head|follower
        self.roles = {}
        self.heads = {}
        self.queues = {}
        self._last_register = {}
        self._last_alive = {}
        self._lost_since = {}
        self._last_senders = []
        self._img_by_key = {}
        self._last_img_sub = 0.0
        if self._target:
            self._selected = self._target

    def attach(self, shell):
        self.shell = shell
        self.udp.on_output = lambda line: shell.sse.put(line)
        shell.sse_route("/events", on_connect=self.push_state)
        shell.route("/select", self._handle_select)
        shell.route("/img_subscribe", self._handle_img_subscribe)

    def start(self):
        self._running = True
        self.peer.start()
        if self._manage_discovery:
            self.discover.start()
        self.udp.start(self.data_port, sender_name=self._target)
        if self._target:
            self.udp.set_filter(self._target)
        self._poller = threading.Thread(target=self.poller, daemon=True)
        self._poller.start()
        self.push_state()
        print("[watch] host_id %s data:%s peer:%s" % (
            self.host_id[:8], self.data_port, self.peer_port))

    def stop(self):
        self._running = False
        with self._lock:
            robots = dict(self.robots)
        for name, info in robots.items():
            self.client.deregister(info["ip"], info["control"])
        self.udp.stop()
        if self._manage_discovery:
            self.discover.stop()
        self.peer.stop()
        self.client.close()

    def live_senders(self):
        now = time.monotonic()
        with self._lock:
            return [n for n, info in self.robots.items()
                    if now - info["last"] <= BEACON_STALE_S]

    def push_state(self):
        if not self.shell:
            return
        senders = self.live_senders()
        active = self._selected if self._selected in senders else (
            senders[0] if senders else "")
        self.shell.sse.put(sse_state(active, senders))

    def note_beacon(self, name, ip, control):
        self._on_beacon(name, ip, control, None)

    def select(self, name):
        if self._target and name and name != self._target:
            return
        self._selected = name or ""
        self.udp.set_filter(self._selected)
        self._push_img_subscribe(force=True)
        self.push_state()

    def _handle_select(self, handler):
        self.select(handler.query.get("sender", [""])[0])
        handler.send(200, b'{"ok":true}', "application/json")

    def _handle_img_subscribe(self, handler):
        raw = handler.query.get("streams", [""])[0]
        ordered = parse_stream_list(raw)
        self.set_img_streams("query", ordered)
        handler.send(
            200,
            json.dumps({"ok": True, "streams": ordered}).encode(),
            "application/json",
        )

    def set_img_streams(self, key, streams):
        with self._lock:
            self._img_by_key[key] = list(streams or [])
        self._push_img_subscribe(force=True)

    def clear_img_key(self, key):
        with self._lock:
            self._img_by_key.pop(key, None)
        self._push_img_subscribe(force=True)

    def _push_img_subscribe(self, force=False):
        now = time.monotonic()
        if not force and now - self._last_img_sub < 1.0:
            return
        with self._lock:
            streams = self._streams_locked()
            selected = self._target or self._selected
            robots = dict(self.robots)
        if not selected:
            return
        info = robots.get(selected)
        if not info:
            return
        if now - info["last"] > BEACON_STALE_S:
            return
        self.client.img_subscribe(info["ip"], info["control"], streams)
        self._last_img_sub = now

    def _streams_locked(self):
        seen = set()
        ordered = []
        for streams in self._img_by_key.values():
            for s in streams:
                if s not in seen:
                    seen.add(s)
                    ordered.append(s)
        return ordered

    def _is_head(self, robot):
        with self._lock:
            return self.roles.get(robot) == "head"

    def _on_beacon(self, name, ip, control, _addr, app="normal", feature=""):
        if self._target and name != self._target:
            return
        now = time.monotonic()
        changed = False
        with self._lock:
            prev = self.robots.get(name)
            if prev is None or prev["ip"] != ip or prev["control"] != control:
                changed = True
            self.robots[name] = {
                "ip": ip, "control": control, "last": now,
                "app": app or "normal", "feature": feature or "",
            }
        if changed:
            self.client.register(ip, control)
            self._last_register[name] = now
            print("[watch] beacon %s at %s:%s app=%s feature=%s" % (
                name, ip, control, app or "normal", feature or ""))
            self.push_state()

    def _on_raw(self, data):
        robot = packet_sender(data)
        if not robot:
            return
        with self._lock:
            role = self.roles.get(robot)
        if role == "head":
            self.peer.forward(robot, data)

    def _on_peer(self, msg, addr):
        t = msg.get("type", "")
        robot = msg.get("robot") or ""
        if t == "register_ack":
            self._apply_ack(msg)
        elif t == "promote" and robot:
            self._become_head(robot, msg.get("queue") or [])
        elif t in ("queue_update", "peer_handoff") and robot:
            head = msg.get("head") or {}
            self._follow(robot, head, msg.get("queue") or [])
        elif t == "deregister_ack":
            pass

    def _apply_ack(self, msg):
        if msg.get("status") != "ok":
            return
        robot = msg.get("robot") or ""
        role = msg.get("role") or ""
        if not robot or not role:
            return
        queue = msg.get("queue") or []
        if role == "head":
            self._become_head(robot, queue)
        else:
            self._follow(robot, msg.get("head") or {}, queue)

    def _become_head(self, robot, queue):
        with self._lock:
            self.roles[robot] = "head"
            self.queues[robot] = list(queue)
            self.heads[robot] = {
                "host_id": self.host_id,
                "ip": "",
                "peer_port": self.peer_port,
                "data_port": self.data_port,
            }
        self.peer.unfollow(robot)
        print("[watch] head of %s" % robot)
        info = self.robots.get(robot)
        if info:
            self.client.head_alive(info["ip"], info["control"])
            self._last_alive[robot] = time.monotonic()
        self._push_img_subscribe(force=True)
        self.push_state()

    def _follow(self, robot, head, queue):
        if not head or not head.get("ip") or not head.get("peer_port"):
            return
        if head.get("host_id") == self.host_id:
            self._become_head(robot, queue)
            return
        with self._lock:
            self.roles[robot] = "follower"
            self.heads[robot] = dict(head)
            self.queues[robot] = list(queue)
        self.peer.subscribe(robot, head["ip"], head["peer_port"])
        print("[watch] follow %s via %s:%s" % (
            robot, head.get("ip"), head.get("peer_port")))
        self.push_state()

    def poller(self):
        while self._running:
            time.sleep(0.5)
            try:
                self._tick()
            except Exception:
                pass

    def _tick(self):
        now = time.monotonic()
        self.peer.refresh_subscribes(now)
        self.peer.expire_subscribers()

        with self._lock:
            robots = dict(self.robots)
            roles = dict(self.roles)

        if self._target:
            if self._selected != self._target:
                self.select(self._target)
        else:
            senders = self.live_senders()
            if senders != self._last_senders:
                self._last_senders = list(senders)
                if senders and (not self._selected or self._selected not in senders):
                    self.select(senders[0])
                else:
                    self.push_state()

        names = [self._target] if self._target else list(robots)
        for name in names:
            info = robots.get(name)
            if not info:
                continue
            stale = now - info["last"] > BEACON_STALE_S
            if stale:
                continue
            last_reg = self._last_register.get(name, 0)
            if now - last_reg >= 3.0:
                self.client.register(info["ip"], info["control"])
                self._last_register[name] = now
            if roles.get(name) == "head":
                last_hb = self._last_alive.get(name, 0)
                if now - last_hb >= 0.5:
                    self.client.head_alive(info["ip"], info["control"])
                    self._last_alive[name] = now
            elif roles.get(name) == "follower":
                if self.udp._last_pkt and now - self.udp._last_pkt > 3.0:
                    if now - self._lost_since.get(name, 0) > 3.0:
                        self.client.query_head(info["ip"], info["control"])
                        self._lost_since[name] = now
                else:
                    self._lost_since.pop(name, None)

        self._push_img_subscribe(force=False)
