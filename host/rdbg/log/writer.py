"""Write Host-side RLG2 .rlog (same layout as vehicle Session)."""

from __future__ import annotations

import json
import os
import struct
import threading
import time
from pathlib import Path

MAGIC_V2 = 0x32474C52  # LE "RLG2"
REC_JSON = 0x00
REC_IMG = 0x01
SYNC_INTERVAL_S = 1.0


class RlogWriter:
    """Append-only RLG2 writer. Prefer one worker thread; methods are locked."""

    def __init__(self, path: str | Path):
        self.path = Path(path).expanduser().resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._fp = self.path.open("wb")
        self._fp.write(struct.pack("<I", MAGIC_V2))
        self._fp.flush()
        self._lock = threading.Lock()
        self._last_sync = 0.0
        self._closed = False
        self.n_json = 0
        self.n_img = 0

    def write_json_bytes(self, ts: int, raw: bytes):
        with self._lock:
            if self._closed:
                return
            self._fp.write(struct.pack("<BQI", REC_JSON, int(ts), len(raw)))
            self._fp.write(raw)
            self.n_json += 1
            self._maybe_sync_locked(False)

    def write_json_obj(self, ts: int, obj: dict):
        raw = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        self.write_json_bytes(ts, raw)

    def write_image(self, ts: int, meta, jpeg: bytes):
        if isinstance(meta, dict):
            meta_b = json.dumps(meta, separators=(",", ":")).encode("utf-8")
        elif isinstance(meta, str):
            meta_b = meta.encode("utf-8")
        else:
            meta_b = bytes(meta)
        jpeg = bytes(jpeg or b"")
        with self._lock:
            if self._closed:
                return
            self._fp.write(struct.pack("<BQI", REC_IMG, int(ts), len(meta_b)))
            self._fp.write(meta_b)
            self._fp.write(struct.pack("<I", len(jpeg)))
            self._fp.write(jpeg)
            self.n_img += 1
            self._maybe_sync_locked(False)

    def _maybe_sync_locked(self, force: bool):
        now = time.monotonic()
        if not force and self._last_sync and (now - self._last_sync) < SYNC_INTERVAL_S:
            return
        self._fp.flush()
        try:
            os.fdatasync(self._fp.fileno())
        except (AttributeError, OSError):
            try:
                os.fsync(self._fp.fileno())
            except OSError:
                pass
        self._last_sync = now

    def close(self):
        with self._lock:
            if self._closed:
                return
            self._maybe_sync_locked(True)
            self._fp.close()
            self._closed = True
