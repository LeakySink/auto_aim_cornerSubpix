"""Host-to-host peer: subscribe / verbatim UDP forward. See host/PROTOCOL.md."""

import json
import socket
import threading
import time

from .control import _udp


class PeerHub:
    def __init__(self, host_id, peer_port, data_port):
        self.host_id = host_id
        self.peer_port = int(peer_port)
        self.data_port = int(data_port)
        self.on_msg = None  # (msg dict, addr)
        self.is_head = lambda robot: False
        self._sock = None
        self._thread = None
        self._running = False
        self._lock = threading.Lock()
        # robot -> {host_id: {ip, data_port, last}}
        self.subscribers = {}
        # robot -> {ip, peer_port} we currently subscribe to
        self.following = {}
        self._last_sub = {}

    def start(self):
        sock = _udp()
        sock.bind(("0.0.0.0", self.peer_port))
        sock.settimeout(0.5)
        self._sock = sock
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        print("[peer] listening on UDP 0.0.0.0:%s" % self.peer_port)

    def stop(self):
        self._running = False
        sock = self._sock
        self._sock = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        t = self._thread
        self._thread = None
        if t is not None:
            t.join(timeout=2)

    def send(self, ip, port, obj):
        sock = self._sock
        if sock is None:
            return
        try:
            sock.sendto(json.dumps(obj).encode(), (ip, int(port)))
        except OSError as e:
            print("[peer] send %s:%s failed: %s" % (ip, port, e))

    def subscribe(self, robot, head_ip, head_peer_port):
        if not robot or not head_ip or not head_peer_port:
            return
        with self._lock:
            self.following[robot] = {"ip": head_ip, "peer_port": int(head_peer_port)}
        self._send_subscribe(robot, head_ip, int(head_peer_port))

    def unfollow(self, robot):
        with self._lock:
            prev = self.following.pop(robot, None)
        if prev:
            self.send(prev["ip"], prev["peer_port"], {
                "v": 1,
                "type": "unsubscribe",
                "host_id": self.host_id,
                "robot": robot,
            })

    def refresh_subscribes(self, now=None):
        now = time.monotonic() if now is None else now
        with self._lock:
            targets = list(self.following.items())
        for robot, head in targets:
            last = self._last_sub.get(robot, 0)
            if now - last < 1.0:
                continue
            self._send_subscribe(robot, head["ip"], head["peer_port"])
            self._last_sub[robot] = now

    def add_subscriber(self, robot, host_id, ip, data_port, peer_port=0):
        if not robot or not host_id or host_id == self.host_id:
            return
        with self._lock:
            bucket = self.subscribers.setdefault(robot, {})
            bucket[host_id] = {
                "ip": ip,
                "data_port": int(data_port),
                "peer_port": int(peer_port) if peer_port else self.peer_port,
                "last": time.monotonic(),
            }

    def drop_subscriber(self, robot, host_id):
        with self._lock:
            bucket = self.subscribers.get(robot)
            if bucket:
                bucket.pop(host_id, None)

    def expire_subscribers(self, timeout_s=3.0):
        now = time.monotonic()
        with self._lock:
            for robot, bucket in list(self.subscribers.items()):
                dead = [hid for hid, info in bucket.items()
                        if now - info["last"] > timeout_s]
                for hid in dead:
                    bucket.pop(hid, None)

    def forward(self, robot, data):
        with self._lock:
            bucket = dict(self.subscribers.get(robot) or {})
        sock = self._sock
        if sock is None or not bucket:
            return
        for info in bucket.values():
            try:
                sock.sendto(data, (info["ip"], info["data_port"]))
            except OSError:
                pass

    def handoff(self, robot, head):
        """Tell subscribers the next head, then drop them."""
        if not head:
            return
        payload = {
            "v": 1,
            "type": "peer_handoff",
            "robot": robot,
            "head": head,
        }
        with self._lock:
            bucket = dict(self.subscribers.pop(robot, {}) or {})
        for info in bucket.values():
            self.send(info["ip"], info.get("peer_port") or self.peer_port, payload)

    def _send_subscribe(self, robot, ip, peer_port):
        self.send(ip, peer_port, {
            "v": 1,
            "type": "subscribe",
            "host_id": self.host_id,
            "robot": robot,
            "data_port": self.data_port,
        })

    def _loop(self):
        while self._running:
            sock = self._sock
            if sock is None:
                break
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg = json.loads(data.decode())
            except Exception:
                continue
            if not isinstance(msg, dict):
                continue
            t = msg.get("type", "")
            if t == "subscribe":
                robot = msg.get("robot", "")
                if not self.is_head(robot):
                    continue
                self.add_subscriber(
                    robot,
                    msg.get("host_id", ""),
                    addr[0],
                    msg.get("data_port") or 0,
                    addr[1],
                )
                self.send(addr[0], addr[1] if addr[1] else self.peer_port, {
                    "v": 1,
                    "type": "subscribe_ack",
                    "status": "ok",
                    "robot": robot,
                })
            elif t == "unsubscribe":
                self.drop_subscriber(msg.get("robot", ""), msg.get("host_id", ""))
            cb = self.on_msg
            if cb:
                try:
                    cb(msg, addr)
                except Exception as e:
                    print("[peer] handler error: %s" % e)
