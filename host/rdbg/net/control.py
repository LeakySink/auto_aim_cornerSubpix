"""Control plane: LAN discovery + register to robots. See host/PROTOCOL.md."""

import json
import socket
import threading
import time

DISCOVER_PORT = 15999
BEACON_STALE_S = 3.0


def _udp(broadcast=False, reuse_port=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if reuse_port and hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return sock


class Discovery:
    """Listen for robot beacons on :15999. Beacon never stops on the robot."""

    def __init__(self, port=DISCOVER_PORT, host_id=""):
        self.port = int(port)
        self.host_id = host_id
        self.on_beacon = None
        self._sock = None
        self._thread = None
        self._running = False

    def start(self):
        sock = _udp(broadcast=True, reuse_port=True)
        sock.bind(("0.0.0.0", self.port))
        sock.settimeout(0.5)
        self._sock = sock
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        self.probe()
        print("[discover] listening on UDP 0.0.0.0:%s" % self.port)

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

    def probe(self):
        sock = self._sock
        if sock is None:
            return
        msg = json.dumps({"v": 1, "type": "who", "host_id": self.host_id})
        try:
            sock.sendto(msg.encode(), ("255.255.255.255", self.port))
        except OSError as e:
            print("[discover] who failed: %s" % e)

    def _loop(self):
        while self._running:
            sock = self._sock
            if sock is None:
                break
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg = json.loads(data.decode())
            except Exception:
                continue
            if not isinstance(msg, dict) or msg.get("type") != "beacon":
                continue
            name = (msg.get("name") or "").strip()
            ip = (msg.get("ip") or "").strip()
            if not name or not ip:
                continue
            control = int(msg.get("control") or 15000)
            cb = self.on_beacon
            if cb:
                cb(name, ip, control, addr)


class RobotClient:
    """Send-only control messages to a robot's :control_port."""

    def __init__(self, host_id, host_name, data_port, peer_port):
        self.host_id = host_id
        self.host_name = host_name
        self.data_port = int(data_port)
        self.peer_port = int(peer_port)
        self._sock = _udp()

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def _send(self, ip, port, obj):
        try:
            self._sock.sendto(json.dumps(obj).encode(), (ip, int(port)))
        except OSError as e:
            print("[ctrl] send %s:%s failed: %s" % (ip, port, e))

    def register(self, ip, control_port):
        self._send(ip, control_port, {
            "v": 1,
            "type": "register",
            "host_id": self.host_id,
            "name": self.host_name,
            "data_port": self.data_port,
            "peer_port": self.peer_port,
        })

    def deregister(self, ip, control_port):
        self._send(ip, control_port, {
            "v": 1,
            "type": "deregister",
            "host_id": self.host_id,
            "peer_port": self.peer_port,
        })

    def head_alive(self, ip, control_port):
        self._send(ip, control_port, {
            "v": 1,
            "type": "head_alive",
            "host_id": self.host_id,
            "data_port": self.data_port,
            "peer_port": self.peer_port,
        })

    def query_head(self, ip, control_port):
        self._send(ip, control_port, {
            "v": 1,
            "type": "query_head",
            "host_id": self.host_id,
            "peer_port": self.peer_port,
        })

    def img_subscribe(self, ip, control_port, streams):
        self._send(ip, control_port, {
            "v": 1,
            "type": "img_subscribe",
            "host_id": self.host_id,
            "peer_port": self.peer_port,
            "streams": list(streams or []),
        })
