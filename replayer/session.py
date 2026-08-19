"""Load .rlog into memory for local playback."""

import sys
from collections import defaultdict
from pathlib import Path

from rlog import iter_records

MAX_BYTES = 1 << 30  # 1 GiB
SKIP_KEYS = frozenset({"_from", "ts", "hb"})


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def load_session(path, max_bytes=MAX_BYTES):
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(p)

    frames = []
    series = defaultdict(list)
    t0 = None
    mem = 0
    sender = ""

    for rec in iter_records(p):
        ts = int(rec.get("ts") or 0)
        if t0 is None:
            t0 = ts
        t_sec = (ts - t0) / 1e9

        if rec.get("kind") == "img":
            jpeg = rec.get("jpeg") or b""
            mem += len(jpeg)
            meta = rec.get("meta") or {}
            if not sender:
                frm = meta.get("_from")
                if isinstance(frm, str) and frm:
                    sender = frm
            frames.append({
                "t": t_sec,
                "meta": meta,
                "jpeg": jpeg,
            })
        else:
            obj = rec.get("obj") or {}
            if "hb" in obj:
                continue
            if isinstance(obj.get("level"), str) and isinstance(obj.get("msg"), str):
                continue
            if not sender:
                frm = obj.get("_from")
                if isinstance(frm, str) and frm:
                    sender = frm
            for k, v in obj.items():
                if k in SKIP_KEYS or k in ("level", "msg"):
                    continue
                if _is_number(v):
                    series[k].append([t_sec, float(v)])

        if mem > max_bytes:
            raise MemoryError(
                f"log payload exceeds {max_bytes // (1 << 20)} MiB limit "
                f"(currently ~{mem // (1 << 20)} MiB)"
            )

    if t0 is None:
        raise ValueError("empty log")

    frames.sort(key=lambda f: f["t"])
    duration = frames[-1]["t"] if frames else 0.0
    if series:
        last_t = max(pts[-1][0] for pts in series.values() if pts)
        duration = max(duration, last_t)

    fields = sorted(series.keys())
    return {
        "file": p.name,
        "path": str(p.resolve()),
        "sender": sender or p.stem,
        "t0_ns": t0,
        "duration": duration,
        "memory_bytes": mem,
        "frames": frames,
        "series": {k: series[k] for k in fields},
        "fields": fields,
    }


def load_session_or_exit(path, max_bytes=MAX_BYTES):
    try:
        sess = load_session(path, max_bytes=max_bytes)
    except MemoryError as e:
        print(f"[replayer] {e}", file=sys.stderr)
        raise SystemExit(2) from e
    except (FileNotFoundError, ValueError) as e:
        print(f"[replayer] {e}", file=sys.stderr)
        raise SystemExit(1) from e
    mb = sess["memory_bytes"] / (1 << 20)
    print(
        f"[replayer] loaded {sess['file']}  "
        f"frames={len(sess['frames'])} fields={len(sess['fields'])}  "
        f"duration={sess['duration']:.2f}s  mem≈{mb:.1f}MiB",
        file=sys.stderr,
    )
    return sess
