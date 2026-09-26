"""In-process UDP data backend — stdlib only."""

import base64
import json
import socket
import struct
import sys
import threading
import time

IMG_MARKER = 0xFF
IMG_FRAG_MARKER = 0xFE
MAX_UDP = 65536
FRAG_TIMEOUT_S = 0.5
FRAG_MAX_FRAMES = 32


def _parse_from_legacy_image(data):
    if len(data) < 17:
        return ""
    meta_len = struct.unpack_from("<I", data, 9)[0]
    meta_off = 13
    if len(data) < meta_off + meta_len:
        return ""
    try:
        meta = json.loads(data[meta_off:meta_off + meta_len].decode("utf-8"))
    except Exception:
        return ""
    if isinstance(meta, dict):
        frm = meta.get("_from")
        return frm if isinstance(frm, str) else ""
    return ""


def _parse_from_frag(data):
    if len(data) < 16:
        return ""
    from_len = data[15]
    if len(data) < 16 + from_len:
        return ""
    try:
        return data[16:16 + from_len].decode("utf-8")
    except Exception:
        return ""


def packet_sender(data):
    """_from of a data-plane UDP packet, or ''."""
    if not data:
        return ""
    try:
        if data[0] == IMG_FRAG_MARKER:
            return _parse_from_frag(data)
        if data[0] == IMG_MARKER:
            return _parse_from_legacy_image(data)
        j = json.loads(data.decode("utf-8"))
        if isinstance(j, dict):
            frm = j.get("_from")
            return frm if isinstance(frm, str) else ""
    except Exception:
        return ""
    return ""


class _FragAssembler:
    """Reassemble 0xFE image fragments; drop incomplete frames on timeout."""

    def __init__(self):
        self._lock = threading.Lock()
        # (sender, frame_seq) -> state
        self._frames = {}

    def push(self, data):
        if len(data) < 16:
            return None
        ts = struct.unpack_from("<Q", data, 1)[0]
        frame_seq, frag_idx, frag_cnt = struct.unpack_from("<HHH", data, 9)
        from_len = data[15]
        off = 16
        if from_len == 0 or len(data) < off + from_len:
            return None
        sender = data[off:off + from_len].decode("utf-8", errors="replace")
        off += from_len
        if frag_cnt == 0 or frag_idx >= frag_cnt:
            return None

        key = (sender, frame_seq)
        now = time.monotonic()
        with self._lock:
            self._expire_locked(now)
            st = self._frames.get(key)
            if st is None:
                if len(self._frames) >= FRAG_MAX_FRAMES:
                    oldest = min(self._frames.items(), key=lambda kv: kv[1]["t"])
                    self._frames.pop(oldest[0], None)
                st = {
                    "t": now,
                    "ts": ts,
                    "cnt": frag_cnt,
                    "parts": {},
                    "meta": None,
                    "jpg_total": None,
                    "sender": sender,
                }
                self._frames[key] = st
            elif st["cnt"] != frag_cnt:
                return None

            if frag_idx in st["parts"]:
                return None

            if frag_idx == 0:
                if len(data) < off + 2:
                    return None
                meta_len = struct.unpack_from("<H", data, off)[0]
                off += 2
                if len(data) < off + meta_len + 4:
                    return None
                meta_raw = data[off:off + meta_len]
                off += meta_len
                jpg_total = struct.unpack_from("<I", data, off)[0]
                off += 4
                try:
                    st["meta"] = json.loads(meta_raw.decode("utf-8"))
                except Exception:
                    st["meta"] = meta_raw.decode("utf-8", errors="replace")
                st["jpg_total"] = jpg_total
                st["ts"] = ts

            chunk = data[off:]
            st["parts"][frag_idx] = chunk
            st["t"] = now

            if len(st["parts"]) != st["cnt"] or st["jpg_total"] is None:
                return None

            jpeg = b"".join(st["parts"][i] for i in range(st["cnt"]))
            meta = st["meta"]
            ts_out = st["ts"]
            del self._frames[key]
            if len(jpeg) != st["jpg_total"]:
                return None
            return ts_out, meta, jpeg, sender

    def _expire_locked(self, now):
        dead = [k for k, v in self._frames.items() if now - v["t"] > FRAG_TIMEOUT_S]
        for k in dead:
            del self._frames[k]


class UdpBackend:
    """Bind one UDP port, parse plot/log/image packets, emit JSON lines."""

    def __init__(self, timeout_ms=3000):
        self.timeout_ms = timeout_ms
        self.on_output = None
        self.on_state = None
        self.on_raw = None
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
        self._filter = ""
        self._frags = _FragAssembler()
        self._last_img_ts = 0  # drop late/out-of-order reassembled frames

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

    def set_filter(self, sender_name):
        self._filter = sender_name or ""
        self._last_img_ts = 0

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
            self._frags = _FragAssembler()
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
            raw_cb = self.on_raw
            if raw_cb:
                try:
                    raw_cb(data)
                except Exception as e:
                    print(f"[udp] forward error: {e}", file=sys.stderr)
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
        if data[0] == IMG_FRAG_MARKER:
            self._handle_frag(data)
        elif data[0] == IMG_MARKER:
            self._handle_image(data)
        else:
            self._handle_json(data)

    def _drop_filtered(self):
        return bool(self._filter and self._last_from and self._last_from != self._filter)

    def _emit_image(self, ts, meta, jpeg):
        if isinstance(meta, dict):
            frm = meta.get("_from")
            if isinstance(frm, str):
                self._last_from = frm
        if self._drop_filtered():
            return
        # 分片重组完成顺序 ≠ 拍摄顺序：旧帧晚到会看起来“前后抖”
        try:
            ts_i = int(ts)
        except (TypeError, ValueError):
            ts_i = 0
        if ts_i > 0:
            if ts_i < self._last_img_ts:
                return
            self._last_img_ts = ts_i
        self._emit({
            "type": "image",
            "ts": ts,
            "meta": meta,
            "jpg_b64": base64.b64encode(jpeg).decode("ascii"),
        })

    def _handle_frag(self, data):
        frm = _parse_from_frag(data)
        if frm:
            self._last_from = frm
        got = self._frags.push(data)
        if not got:
            return
        ts, meta, jpeg, sender = got
        if sender:
            self._last_from = sender
        self._emit_image(ts, meta, jpeg)

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
        self._emit_image(ts, meta, jpeg)

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
        if "img_streams" in j:
            streams = j.get("img_streams") or []
            if not isinstance(streams, list):
                return
            if self._drop_filtered():
                return
            self._emit({
                "type": "img_streams",
                "streams": [s for s in streams if isinstance(s, str)],
                "_from": frm or "",
            })
            return
        if self._drop_filtered():
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
