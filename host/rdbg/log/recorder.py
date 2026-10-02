"""Async Live→RLG2 recorder: bounded queue, never blocks UDP/SSE."""

from __future__ import annotations

import base64
import json
import os
import queue
import threading
import time
from pathlib import Path

from .writer import RlogWriter

QUEUE_MAX = 512


class LiveRecorder:
    """Tap SSE JSON lines; write RLG2 on a background thread."""

    def __init__(self):
        self._lock = threading.Lock()
        self._q = None  # type: queue.Queue | None
        self._thread = None
        self._writer = None  # type: RlogWriter | None
        self._running = False
        self._started_at = 0.0
        self._dropped = 0

    @property
    def active(self):
        return self._running and self._writer is not None

    def status(self):
        with self._lock:
            w = self._writer
            return {
                "recording": self._running and w is not None,
                "path": str(w.path) if w else "",
                "n_json": w.n_json if w else 0,
                "n_img": w.n_img if w else 0,
                "dropped": self._dropped,
                "elapsed_s": (time.monotonic() - self._started_at) if self._started_at else 0,
            }

    def start(self, path: str | Path):
        self.stop()
        writer = RlogWriter(path)
        q = queue.Queue(maxsize=QUEUE_MAX)
        with self._lock:
            self._writer = writer
            self._q = q
            self._dropped = 0
            self._running = True
            self._started_at = time.monotonic()
            self._thread = threading.Thread(target=self._loop, daemon=True)
            self._thread.start()
        return str(writer.path)

    def stop(self):
        with self._lock:
            self._running = False
            q = self._q
            t = self._thread
            w = self._writer
            self._q = None
            self._thread = None
        if q is not None:
            try:
                q.put_nowait(None)
            except queue.Full:
                pass
        if t is not None:
            t.join(timeout=5)
        path = ""
        if w is not None:
            path = str(w.path)
            w.close()
        with self._lock:
            self._writer = None
            self._started_at = 0.0
        return path

    def put(self, line: str):
        """SSE fan subscriber API — never block recv path."""
        if not self._running:
            return
        q = self._q
        if q is None:
            return
        try:
            q.put_nowait(line)
        except queue.Full:
            # Prefer drop images: try to make room by skipping this if image-heavy.
            self._dropped += 1
            try:
                old = q.get_nowait()
                if old is not None and '"type":"image"' not in old:
                    # put back non-image if we can
                    try:
                        q.put_nowait(old)
                    except queue.Full:
                        pass
                q.put_nowait(line)
            except (queue.Empty, queue.Full):
                pass

    def _loop(self):
        while True:
            q = self._q
            if q is None:
                break
            try:
                line = q.get(timeout=0.2)
            except queue.Empty:
                if not self._running:
                    break
                continue
            if line is None:
                break
            self._handle_line(line)

    def _handle_line(self, line: str):
        w = self._writer
        if w is None:
            return
        try:
            msg = json.loads(line)
        except Exception:
            return
        if not isinstance(msg, dict):
            return
        t = msg.get("type")
        if t == "plot":
            data = msg.get("data")
            if isinstance(data, dict):
                ts = int(msg.get("ts") or data.get("ts") or 0)
                w.write_json_obj(ts, data)
        elif t == "log":
            ts = int(msg.get("ts") or 0)
            obj = {
                "level": msg.get("level") or "INFO",
                "msg": msg.get("msg") or "",
                "ts": ts,
            }
            if msg.get("_from"):
                obj["_from"] = msg["_from"]
            w.write_json_obj(ts, obj)
        elif t == "image":
            ts = int(msg.get("ts") or 0)
            meta = msg.get("meta") if isinstance(msg.get("meta"), dict) else {}
            b64 = msg.get("jpg_b64") or ""
            try:
                jpeg = base64.b64decode(b64)
            except Exception:
                return
            w.write_image(ts, meta, jpeg)
        # status / img_streams / state — skip


def default_record_path(sender: str, log_dir: str | None = None) -> Path:
    base = log_dir or os.environ.get("RDBG_HOST_LOG_DIR") or "logs"
    ts = time.strftime("%Y%m%d_%H%M%S")
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in (sender or "host"))
    return Path(base) / f"host_{safe}_{ts}.rlog"
