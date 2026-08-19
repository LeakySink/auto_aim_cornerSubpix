"""In-process UDP data backend — stdlib only, replaces C++ udp_backend."""

import base64
import json
import socket
import struct
import sys
import threading
import time

IMG_MARKER = 0xFF
MAX_UDP = 65536


class UdpBackend:
    """Bind one UDP port, parse plot/log/image packets, emit JSON lines."""

    def __init__(self, timeout_ms=3000):
        self.timeout_ms = timeout_ms
        self.on_output = None  # callback(line: str)
        self.on_state = None   # callback()
        self._lock = threading.Lock()
        self._sock = None
        self._thread = None
        self._status_thread = None
        self._running = False
        self._sender = ""
        self._port = 0
        self._last_pkt = 0.0
        self._last_from = ""
        self._was_connected = False

    @property
    def active_sender(self):
        return self._sender

    @property
    def active_port(self):
        return self._port

    @property
    def running(self):
        t = self._thread
        return self._running and t is not None and t.is_alive()

    def start(self, port, sender_name="default"):
        with self._lock:
            self._stop_locked()
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind(("0.0.0.0", int(port)))
            except OSError as e:
                sock.close()
                print(f"[udp] bind {port} failed: {e}", file=sys.stderr)
                return
            sock.settimeout(0.2)
            self._sock = sock
            self._sender = sender_name
            self._port = int(port)
            self._running = True
            self._last_pkt = 0.0
            self._was_connected = False
            self._thread = threading.Thread(target=self._recv_loop, daemon=True)
            self._thread.start()
            self._status_thread = threading.Thread(target=self._status_loop, daemon=True)
            self._status_thread.start()
        if self.on_state:
            self.on_state()

    def stop(self):
        with self._lock:
            self._stop_locked()
        if self.on_state:
            self.on_state()

    def switch(self, sender_name, port):
        with self._lock:
            if self._sender == sender_name and self._port == int(port) and self._running:
                return
            self._stop_locked()
        self.start(port, sender_name)

    def _stop_locked(self):
        self._running = False
        sock = self._sock
        self._sock = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        t, st = self._thread, self._status_thread
        self._thread = None
        self._status_thread = None
        if t is not None:
            t.join(timeout=2)
        if st is not None:
            st.join(timeout=2)
        self._sender = ""
        self._port = 0

    def _emit(self, obj):
        cb = self.on_output
        if cb:
            cb(json.dumps(obj, separators=(",", ":")))

    def _recv_loop(self):
        while self._running:
            sock = self._sock
            if sock is None:
                break
            try:
                data, _addr = sock.recvfrom(MAX_UDP)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                continue
            self._last_pkt = time.monotonic()
            try:
                self._handle(data)
            except Exception as e:
                print(f"[udp] handle error: {e}", file=sys.stderr)

    def _status_loop(self):
        timeout = self.timeout_ms / 1000.0
        while self._running:
            now = time.monotonic()
            connected = self._last_pkt > 0 and (now - self._last_pkt) < timeout
            if connected and not self._was_connected:
                self._emit({
                    "type": "status",
                    "connected": True,
                    "sender": self._last_from,
                })
            elif not connected and self._was_connected:
                self._emit({"type": "status", "connected": False})
            self._was_connected = connected
            time.sleep(0.2)

    def _handle(self, data):
        if data[0] == IMG_MARKER:
            self._handle_image(data)
        else:
            self._handle_json(data)

    def _handle_image(self, data):
        if len(data) < 17:
            return
        ts, meta_len = struct.unpack_from("<QI", data, 1)
        meta_off = 13
        if len(data) < meta_off + meta_len + 4:
            return
        meta_raw = data[meta_off:meta_off + meta_len]
        jpg_len = struct.unpack_from("<I", data, meta_off + meta_len)[0]
        jpg_off = meta_off + meta_len + 4
        if len(data) < jpg_off + jpg_len:
            return
        jpeg = data[jpg_off:jpg_off + jpg_len]
        try:
            meta = json.loads(meta_raw.decode("utf-8"))
        except Exception:
            meta = meta_raw.decode("utf-8", errors="replace")
        if isinstance(meta, dict):
            frm = meta.get("_from")
            if isinstance(frm, str):
                self._last_from = frm
        self._emit({
            "type": "image",
            "ts": ts,
            "meta": meta,
            "jpg_b64": base64.b64encode(jpeg).decode("ascii"),
        })

    def _handle_json(self, data):
        try:
            j = json.loads(data.decode("utf-8"))
        except Exception:
            return
        if not isinstance(j, dict):
            return
        frm = j.get("_from")
        if isinstance(frm, str):
            self._last_from = frm
        if "hb" in j:
            return
        ts = j.get("ts", 0)
        if isinstance(j.get("level"), str) and isinstance(j.get("msg"), str):
            out = {
                "type": "log",
                "ts": ts,
                "level": j["level"],
                "msg": j["msg"],
            }
            if frm:
                out["_from"] = frm
            self._emit(out)
        else:
            self._emit({"type": "plot", "ts": ts, "data": j})
