"""Netcheck feature — discover / echo / ping via HTTP (threaded workers).

Routes: discover/* · echo/* · POST /ping · jobs/（host/API.md §6）。
Does not populate GET /api/robots.
"""

from __future__ import annotations

import json
import socket
import struct
import threading
import time
import uuid

from .base import Feature, json_body, send_json

MAGIC = b"NCHK"
HDR = struct.Struct("<4sIQ")
BEACON_PORT = 15999


def _udp(broadcast=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return sock


class NetcheckFeature(Feature):
    id = "netcheck"
    title = "Netcheck"
    description = "UDP 通达度：discover / echo / ping"

    def __init__(self):
        super().__init__()
        self._workers = {}
        self._wlock = threading.Lock()
        self._beacons = []
        self._beacon_lock = threading.Lock()

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/netcheck")
        shell.route(p + "/discover/start", self._disc_start, methods=("POST",))
        shell.route(p + "/discover/stop", self._disc_stop, methods=("POST",))
        shell.route(p + "/discover/beacons", self._disc_list)
        shell.route(p + "/echo/start", self._echo_start, methods=("POST",))
        shell.route(p + "/echo/stop", self._echo_stop, methods=("POST",))
        shell.route(p + "/echo/status", self._echo_status)
        shell.route(p + "/ping", self._ping, methods=("POST",))
        shell.route(p + "/jobs/", self._job, prefix=True)

    def on_start(self, config):
        pass

    def on_stop(self):
        with self._wlock:
            ids = list(self._workers.keys())
        for wid in ids:
            self._stop_worker(wid)

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            pass

    def _ensure_running(self):
        if self.status().get("state") != "running":
            self.start({})

    def _stop_worker(self, wid):
        with self._wlock:
            w = self._workers.get(wid)
        if not w:
            return
        w["stop"].set()
        t = w.get("thread")
        if t and t.is_alive():
            t.join(timeout=2.0)
        with self._wlock:
            self._workers.pop(wid, None)

    def _disc_start(self, handler):
        self._ensure_running()
        body = json_body(handler)
        port = int(body.get("port") or BEACON_PORT)
        wid = "discover"
        self._stop_worker(wid)
        stop = threading.Event()
        with self._beacon_lock:
            self._beacons = []

        def _loop():
            sock = _udp(broadcast=True)
            try:
                sock.bind(("0.0.0.0", port))
            except OSError as e:
                with self._wlock:
                    if wid in self._workers:
                        self._workers[wid]["error"] = str(e)
                        self._workers[wid]["state"] = "error"
                return
            sock.settimeout(0.5)
            seen = {}
            while not stop.is_set():
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
                ip = (msg.get("ip") or addr[0] or "").strip()
                control = int(msg.get("control") or 15000)
                key = name or ip
                entry = {
                    "name": name or "?",
                    "ip": ip,
                    "control": control,
                    "from": f"{addr[0]}:{addr[1]}",
                    "ts": time.time(),
                }
                with self._beacon_lock:
                    # upsert
                    self._beacons = [b for b in self._beacons if (b.get("name") or b.get("ip")) != key]
                    self._beacons.append(entry)
                seen[key] = entry
            sock.close()
            with self._wlock:
                if wid in self._workers:
                    self._workers[wid]["state"] = "idle"

        th = threading.Thread(target=_loop, name="net-discover", daemon=True)
        with self._wlock:
            self._workers[wid] = {"id": wid, "state": "running", "stop": stop, "thread": th, "error": ""}
        th.start()
        send_json(handler, {"ok": True, "worker": wid})

    def _disc_stop(self, handler):
        self._stop_worker("discover")
        send_json(handler, {"ok": True})

    def _disc_list(self, handler):
        with self._beacon_lock:
            beacons = list(self._beacons)
        send_json(handler, {"beacons": beacons})

    def _echo_start(self, handler):
        self._ensure_running()
        body = json_body(handler)
        port = int(body.get("port") or 15050)
        wid = "echo"
        self._stop_worker(wid)
        stop = threading.Event()
        stats = {"replies": 0, "last": ""}

        def _loop():
            sock = _udp()
            try:
                sock.bind(("0.0.0.0", port))
            except OSError as e:
                with self._wlock:
                    if wid in self._workers:
                        self._workers[wid]["error"] = str(e)
                        self._workers[wid]["state"] = "error"
                return
            sock.settimeout(0.5)
            while not stop.is_set():
                try:
                    data, addr = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if len(data) < HDR.size or data[:4] != MAGIC:
                    continue
                sock.sendto(data, addr)
                stats["replies"] += 1
                stats["last"] = f"{addr[0]}:{addr[1]} len={len(data)}"
            sock.close()
            with self._wlock:
                if wid in self._workers:
                    self._workers[wid]["state"] = "idle"

        th = threading.Thread(target=_loop, name="net-echo", daemon=True)
        with self._wlock:
            self._workers[wid] = {
                "id": wid, "state": "running", "stop": stop, "thread": th,
                "error": "", "stats": stats, "port": port,
            }
        th.start()
        send_json(handler, {"ok": True, "worker": wid, "port": port})

    def _echo_stop(self, handler):
        self._stop_worker("echo")
        send_json(handler, {"ok": True})

    def _echo_status(self, handler):
        with self._wlock:
            w = self._workers.get("echo")
            if not w:
                send_json(handler, {"state": "idle"})
                return
            send_json(handler, {
                "state": w.get("state"),
                "error": w.get("error", ""),
                "port": w.get("port"),
                "stats": dict(w.get("stats") or {}),
            })

    def _ping(self, handler):
        self._ensure_running()
        body = json_body(handler)
        host = body.get("host") or ""
        port = int(body.get("port") or 15050)
        size = max(int(body.get("size") or 1200), HDR.size)
        count = int(body.get("count") or 50)
        interval = float(body.get("interval") or 0.05)
        timeout = float(body.get("timeout") or 0.5)
        if not host:
            send_json(handler, {"ok": False, "error": "host required"}, code=400)
            return
        job_id = uuid.uuid4().hex[:10]
        with self._wlock:
            self._workers[job_id] = {
                "id": job_id, "state": "running", "stop": threading.Event(),
                "thread": None, "error": "", "result": None,
            }

        def _work():
            payload = b"\x00" * (size - HDR.size)
            sock = _udp()
            sock.settimeout(timeout)
            try:
                sock.connect((host, port))
            except OSError as e:
                with self._wlock:
                    self._workers[job_id]["state"] = "error"
                    self._workers[job_id]["error"] = str(e)
                return
            sent = recv = 0
            rtts = []
            losses = []
            for seq in range(count):
                if self._workers[job_id]["stop"].is_set():
                    break
                pkt = HDR.pack(MAGIC, seq, time.time_ns()) + payload
                t_send = time.monotonic()
                try:
                    sock.send(pkt)
                    sent += 1
                except OSError:
                    losses.append(seq)
                    time.sleep(interval)
                    continue
                got = False
                deadline = t_send + timeout
                while True:
                    remain = deadline - time.monotonic()
                    if remain <= 0:
                        break
                    sock.settimeout(remain)
                    try:
                        data = sock.recv(65535)
                    except (socket.timeout, OSError):
                        break
                    if len(data) < HDR.size or data[:4] != MAGIC:
                        continue
                    _m, rseq, _t = HDR.unpack_from(data)
                    if rseq != seq:
                        continue
                    rtts.append((time.monotonic() - t_send) * 1000.0)
                    recv += 1
                    got = True
                    break
                if not got:
                    losses.append(seq)
                sleep_for = interval - (time.monotonic() - t_send)
                if sleep_for > 0:
                    time.sleep(sleep_for)
            sock.close()
            loss_pct = (100.0 * (sent - recv) / sent) if sent else 0.0
            result = {
                "sent": sent, "recv": recv, "loss_pct": loss_pct,
                "rtts_ms": rtts, "lost_seq": losses[:20],
            }
            if rtts:
                s = sorted(rtts)
                result["rtt"] = {
                    "min": s[0], "avg": sum(rtts) / len(rtts),
                    "max": s[-1], "p50": s[len(s) // 2],
                }
            with self._wlock:
                self._workers[job_id]["state"] = "done"
                self._workers[job_id]["result"] = result

        th = threading.Thread(target=_work, name=f"net-ping-{job_id}", daemon=True)
        with self._wlock:
            self._workers[job_id]["thread"] = th
        th.start()
        send_json(handler, {"ok": True, "job_id": job_id})

    def _job(self, handler):
        parts = handler.route_path.strip("/").split("/")
        if len(parts) < 2 or parts[-2] != "jobs":
            handler.send_error(404)
            return
        jid = parts[-1]
        with self._wlock:
            w = self._workers.get(jid)
            if not w:
                send_json(handler, {"error": "unknown job"}, code=404)
                return
            send_json(handler, {
                "id": jid,
                "state": w.get("state"),
                "error": w.get("error", ""),
                "result": w.get("result"),
            })
