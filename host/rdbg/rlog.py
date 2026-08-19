"""Read RemoteLogger local .rlog files (plot / log JSON records)."""

import json
import struct
import sys
from pathlib import Path

MAGIC = 0x524C4F47  # native-endian uint32 written by C++ fwrite on LE hosts


def load(path):
    """Return a list of dicts. Each has 'ts' (ns). Invalid/truncated file -> []."""
    p = Path(path)
    try:
        data = p.read_bytes()
    except OSError as e:
        print(f"[rlog] open failed: {e}", file=sys.stderr)
        return []

    if len(data) < 4:
        print(f"[rlog] too short: {p}", file=sys.stderr)
        return []

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MAGIC:
        print(f"[rlog] bad magic 0x{magic:08x} (want 0x{MAGIC:08x}): {p}", file=sys.stderr)
        return []

    records = []
    off = 4
    n = len(data)
    while off + 12 <= n:
        ts, length = struct.unpack_from("<QI", data, off)
        off += 12
        if off + length > n:
            print(f"[rlog] truncated record at {off - 12}, stop", file=sys.stderr)
            break
        raw = data[off:off + length]
        off += length
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            obj = {"raw": raw.decode("utf-8", errors="replace")}
        if not isinstance(obj, dict):
            obj = {"value": obj}
        if "ts" not in obj:
            obj["ts"] = ts
        records.append(obj)
    return records


def to_sse(record):
    """Map one rlog record to the same SSE payload as live UDP."""
    if not isinstance(record, dict):
        return None
    if "hb" in record:
        return None
    ts = record.get("ts", 0)
    if isinstance(record.get("level"), str) and isinstance(record.get("msg"), str):
        out = {
            "type": "log",
            "ts": ts,
            "level": record["level"],
            "msg": record["msg"],
        }
        frm = record.get("_from")
        if isinstance(frm, str) and frm:
            out["_from"] = frm
        return out
    return {"type": "plot", "ts": ts, "data": record}


def sender_name(records, fallback="rlog"):
    for rec in records:
        frm = rec.get("_from")
        if isinstance(frm, str) and frm:
            return frm
    return fallback
