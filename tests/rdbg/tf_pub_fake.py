#!/usr/bin/env python3
"""本地内测：假数据源模拟 tf_pub_test（不入库，勿 git add）。

用法（先开 host）：
  python3 tests/rdbg/tf_pub_fake.py
  # 然后门户点 TF Viz / 车辆列表里的 tf_fake

关完所有绑定到本假源的浏览器页面后，本进程会在约 1s 内自动退出
（依赖 host deregister / head_alive 超时）。

不依赖云台串口；beacon app/feature=tfviz；plot.tf 字段与 tf_pub_test 对齐。
"""

from __future__ import annotations

import json
import math
import socket
import sys
import threading
import time

NAME = "tf_fake"
APP = "tfviz"
CONTROL_PORT = 15000
BEACON_PORT = 15999
BEACON_HZ = 1.0
PLOT_HZ = 50.0
# 无 head_alive / 无注册 host 超过此时长则退出（浏览器全关）
HOST_STALE_S = 2.5
IDLE_EXIT_S = 1.0

# 与 configs/tf_pub.yaml 占位外参一致
R_GIMBAL2IMUBODY = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
R_CAMERA2GIMBAL = [0.0, 0.0, 1.0, -1.0, 0.0, 0.0, 0.0, -1.0, 0.0]
T_CAMERA2GIMBAL = [0.1, 0.0, 0.05]
CONFIG_PATH = "configs/tf_pub.yaml (fake)"
CONFIG_NAME = "tf_pub.yaml"


def now_ns() -> int:
    return time.time_ns()


def local_ipv4() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def mat_mul(A, B):
    C = [0.0] * 9
    for r in range(3):
        for c in range(3):
            C[r * 3 + c] = (
                A[r * 3 + 0] * B[0 * 3 + c]
                + A[r * 3 + 1] * B[1 * 3 + c]
                + A[r * 3 + 2] * B[2 * 3 + c]
            )
    return C


def mat_t(A):
    return [A[0], A[3], A[6], A[1], A[4], A[7], A[2], A[5], A[8]]


def mat_vec(R, v):
    return [
        R[0] * v[0] + R[1] * v[1] + R[2] * v[2],
        R[3] * v[0] + R[4] * v[1] + R[5] * v[2],
        R[6] * v[0] + R[7] * v[1] + R[8] * v[2],
    ]


def quat_yaw(yaw: float):
    """ZYX yaw-only quaternion (w,x,y,z), pitch=roll=0."""
    half = 0.5 * yaw
    return (math.cos(half), 0.0, 0.0, math.sin(half))


def quat_to_mat(q):
    w, x, y, z = q
    n = math.sqrt(w * w + x * x + y * y + z * z) or 1.0
    w, x, y, z = w / n, x / n, y / n, z / n
    return [
        1 - 2 * (y * y + z * z),
        2 * (x * y - w * z),
        2 * (x * z + w * y),
        2 * (x * y + w * z),
        1 - 2 * (x * x + z * z),
        2 * (y * z - w * x),
        2 * (x * z - w * y),
        2 * (y * z + w * x),
        1 - 2 * (x * x + y * y),
    ]


def r_gimbal2world(q):
    # Solver::set_R_gimbal2world
    R_imu = quat_to_mat(q)
    Rg2i = R_GIMBAL2IMUBODY
    return mat_mul(mat_mul(mat_t(Rg2i), R_imu), Rg2i)


class FakeRobot:
    def __init__(self):
        self._lock = threading.Lock()
        self._queue = []  # list of dict host_id,name,ip,data_port,peer_port,last_alive
        self._head = None  # (ip, data_port)
        self._running = True
        self._had_host = False
        self._empty_since = None

        self.ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ctrl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ctrl.bind(("0.0.0.0", CONTROL_PORT))
        self.ctrl.settimeout(0.2)

        self.beacon = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.beacon.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.beacon.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            self.beacon.bind(("0.0.0.0", BEACON_PORT))
        except OSError:
            pass
        self.beacon.settimeout(0.2)

        self.data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def close(self):
        self._running = False
        for s in (self.ctrl, self.beacon, self.data):
            try:
                s.close()
            except OSError:
                pass

    def _send_json(self, ip, port, obj):
        try:
            self.data.sendto(json.dumps(obj, separators=(",", ":")).encode(), (ip, int(port)))
        except OSError as e:
            print("[tf_fake] send failed:", e, file=sys.stderr)

    def make_beacon(self):
        return {
            "v": 1,
            "type": "beacon",
            "name": NAME,
            "app": APP,
            "feature": "tfviz",
            "ip": local_ipv4(),
            "control": CONTROL_PORT,
            "ts": now_ns(),
        }

    def broadcast_beacon(self):
        payload = json.dumps(self.make_beacon()).encode()
        try:
            self.beacon.sendto(payload, ("255.255.255.255", BEACON_PORT))
        except OSError as e:
            print("[tf_fake] beacon failed:", e, file=sys.stderr)

    def _apply_head(self):
        if not self._queue:
            self._head = None
            return
        h = self._queue[0]
        self._head = (h["ip"], h["data_port"])

    def _touch_empty_clock(self):
        if self._queue:
            self._empty_since = None
        elif self._had_host and self._empty_since is None:
            self._empty_since = time.monotonic()

    def expire_stale_hosts(self):
        now = time.monotonic()
        with self._lock:
            before = len(self._queue)
            self._queue = [
                h for h in self._queue if now - h.get("last_alive", now) <= HOST_STALE_S
            ]
            if len(self._queue) != before:
                dropped = before - len(self._queue)
                print(f"[tf_fake] dropped {dropped} stale host(s)")
                self._apply_head()
            self._touch_empty_clock()

    def should_exit(self) -> bool:
        with self._lock:
            if not self._had_host:
                return False
            if self._queue:
                return False
            if self._empty_since is None:
                return False
            return (time.monotonic() - self._empty_since) >= IDLE_EXIT_S

    def handle_ctrl(self, raw: bytes, addr):
        try:
            msg = json.loads(raw.decode())
        except Exception:
            return
        if not isinstance(msg, dict):
            return
        typ = msg.get("type")
        host_id = msg.get("host_id") or ""

        if typ == "who":
            self._send_json(addr[0], addr[1], self.make_beacon())
            return

        if typ == "register":
            data_port = int(msg.get("data_port") or 0)
            peer_port = int(msg.get("peer_port") or 0)
            if not host_id or not data_port or not peer_port:
                return
            now = time.monotonic()
            with self._lock:
                existing = next((h for h in self._queue if h["host_id"] == host_id), None)
                if existing:
                    existing["ip"] = addr[0]
                    existing["data_port"] = data_port
                    existing["peer_port"] = peer_port
                    existing["name"] = msg.get("name") or existing.get("name") or ""
                    existing["last_alive"] = now
                else:
                    self._queue.append(
                        {
                            "host_id": host_id,
                            "name": msg.get("name") or "",
                            "ip": addr[0],
                            "data_port": data_port,
                            "peer_port": peer_port,
                            "last_alive": now,
                        }
                    )
                    print(f"[tf_fake] host queued {host_id[:8]} -> {addr[0]}:{data_port}")
                self._had_host = True
                self._empty_since = None
                self._apply_head()
                is_head = self._queue and self._queue[0]["host_id"] == host_id
                ids = [h["host_id"] for h in self._queue]
                head = self._queue[0] if self._queue else None
            ack = {
                "v": 1,
                "type": "register_ack",
                "status": "ok",
                "robot": NAME,
                "queue": ids,
                "role": "head" if is_head else "follower",
            }
            if not is_head and head:
                ack["head"] = {
                    "host_id": head["host_id"],
                    "ip": head["ip"],
                    "peer_port": head["peer_port"],
                    "data_port": head["data_port"],
                }
            self._send_json(addr[0], peer_port, ack)
            return

        if typ == "deregister":
            with self._lock:
                self._queue = [h for h in self._queue if h["host_id"] != host_id]
                self._apply_head()
                self._touch_empty_clock()
            print(f"[tf_fake] host deregistered {host_id[:8] if host_id else '?'}")
            return

        if typ == "head_alive":
            with self._lock:
                for h in self._queue:
                    if h["host_id"] == host_id or (not host_id and self._queue and h is self._queue[0]):
                        h["last_alive"] = time.monotonic()
                        break
            return

        if typ == "query_head":
            with self._lock:
                head = self._queue[0] if self._queue else None
            if not head:
                return
            self._send_json(
                addr[0],
                int(msg.get("peer_port") or addr[1]),
                {
                    "v": 1,
                    "type": "query_head_ack",
                    "robot": NAME,
                    "head": {
                        "host_id": head["host_id"],
                        "ip": head["ip"],
                        "peer_port": head["peer_port"],
                        "data_port": head["data_port"],
                    },
                },
            )

    def poll_ctrl(self):
        try:
            raw, addr = self.ctrl.recvfrom(65535)
        except socket.timeout:
            return
        except OSError:
            return
        self.handle_ctrl(raw, addr)

    def poll_beacon_who(self):
        try:
            raw, addr = self.beacon.recvfrom(65535)
        except (socket.timeout, OSError):
            return
        try:
            msg = json.loads(raw.decode())
        except Exception:
            return
        if isinstance(msg, dict) and msg.get("type") == "who":
            self._send_json(addr[0], addr[1], self.make_beacon())

    def make_plot(self, t: float):
        yaw = 0.4 * math.sin(t * 0.7)
        pitch = 0.15 * math.sin(t * 0.5 + 1.0)
        q = quat_yaw(yaw)
        Rg2w = r_gimbal2world(q)
        Rc2w = mat_mul(Rg2w, R_CAMERA2GIMBAL)
        tc2w = mat_vec(Rg2w, T_CAMERA2GIMBAL)
        tf = {
            "config_path": CONFIG_PATH,
            "config_name": CONFIG_NAME,
            "q": list(q),
            "R_gimbal2imubody": list(R_GIMBAL2IMUBODY),
            "R_camera2gimbal": list(R_CAMERA2GIMBAL),
            "t_camera2gimbal": list(T_CAMERA2GIMBAL),
            "R_gimbal2world": Rg2w,
            "R_camera2world": Rc2w,
            "t_camera2world": tc2w,
        }
        return {
            "ts": now_ns(),
            "_from": NAME,
            "tf": tf,
            "gimbal_yaw": yaw,
            "gimbal_pitch": pitch,
            "gimbal_roll": 0.0,
            "cam_x": tc2w[0],
            "cam_y": tc2w[1],
            "cam_z": tc2w[2],
        }

    def send_plot(self, plot: dict):
        with self._lock:
            head = self._head
        if not head:
            return
        self._send_json(head[0], head[1], plot)


def main():
    print(f"[tf_fake] starting name={NAME} app={APP} control=:{CONTROL_PORT}")
    print("[tf_fake] open host TF Viz；关完所有相关页面后本进程会自动退出。Ctrl+C 也可停。")
    robot = FakeRobot()
    t0 = time.time()
    next_beacon = 0.0
    next_plot = 0.0
    try:
        while True:
            robot.poll_ctrl()
            robot.poll_beacon_who()
            robot.expire_stale_hosts()
            if robot.should_exit():
                print("[tf_fake] no host left — exit")
                break
            now = time.time()
            if now >= next_beacon:
                robot.broadcast_beacon()
                next_beacon = now + 1.0 / BEACON_HZ
            if now >= next_plot:
                robot.send_plot(robot.make_plot(now - t0))
                next_plot = now + 1.0 / PLOT_HZ
    except KeyboardInterrupt:
        print("\n[tf_fake] stop")
    finally:
        robot.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
