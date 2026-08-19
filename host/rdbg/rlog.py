"""Read RemoteLogger local .rlog files (plot / log / image records)."""

import base64
import json
import struct
import sys
from pathlib import Path

# Little-endian uint32 written by C++ fwrite on LE hosts
MAGIC_V1 = 0x524C4F47  # "RLOG" — JSON-only (legacy)
MAGIC_V2 = 0x32474C52  # "RLG2" — typed var + img

REC_JSON = 0x00
REC_IMG = 0x01


def load(path):
    """Return a list of dicts. Each has 'ts' (ns). Invalid/truncated file -> [].

    Image records: {"ts", "_rlog": "img", "meta", "jpeg": bytes}.
    """
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
    if magic == MAGIC_V1:
        return _load_v1(data, p)
    if magic == MAGIC_V2:
        return _load_v2(data, p)
    print(
        f"[rlog] bad magic 0x{magic:08x} "
        f"(want 0x{MAGIC_V1:08x} or 0x{MAGIC_V2:08x}): {p}",
        file=sys.stderr,
    )
    return []


def _load_v1(data, _path):
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
        records.append(_parse_json_record(ts, raw))
    return records


def _load_v2(data, _path):
    records = []
    off = 4
    n = len(data)
    while off < n:
        if off + 1 + 8 + 4 > n:
            print(f"[rlog] truncated header at {off}, stop", file=sys.stderr)
            break
        typ = data[off]
        off += 1
        ts = struct.unpack_from("<Q", data, off)[0]
        off += 8

        if typ == REC_JSON:
            length = struct.unpack_from("<I", data, off)[0]
            off += 4
            if off + length > n:
                print(f"[rlog] truncated json at {off - 13}, stop", file=sys.stderr)
                break
            raw = data[off:off + length]
            off += length
            records.append(_parse_json_record(ts, raw))
        elif typ == REC_IMG:
            meta_len = struct.unpack_from("<I", data, off)[0]
            off += 4
            if off + meta_len + 4 > n:
                print(f"[rlog] truncated img meta at {off - 13}, stop", file=sys.stderr)
                break
            meta_raw = data[off:off + meta_len]
            off += meta_len
            jpg_len = struct.unpack_from("<I", data, off)[0]
            off += 4
            if off + jpg_len > n:
                print(f"[rlog] truncated jpeg at {off - 4}, stop", file=sys.stderr)
                break
            jpeg = data[off:off + jpg_len]
            off += jpg_len
            try:
                meta = json.loads(meta_raw.decode("utf-8"))
            except Exception:
                meta = {"raw": meta_raw.decode("utf-8", errors="replace")}
            if not isinstance(meta, dict):
                meta = {"value": meta}
            records.append({"ts": ts, "_rlog": "img", "meta": meta, "jpeg": jpeg})
        else:
            print(f"[rlog] unknown record type 0x{typ:02x} at {off - 9}, stop", file=sys.stderr)
            break
    return records


def _parse_json_record(ts, raw):
    try:
        obj = json.loads(raw.decode("utf-8"))
    except Exception:
        obj = {"raw": raw.decode("utf-8", errors="replace")}
    if not isinstance(obj, dict):
        obj = {"value": obj}
    if "ts" not in obj:
        obj["ts"] = ts
    return obj


def to_sse(record):
    """Map one rlog record to the same SSE payload as live UDP."""
    if not isinstance(record, dict):
        return None
    if record.get("_rlog") == "img":
        jpeg = record.get("jpeg") or b""
        meta = record.get("meta") if isinstance(record.get("meta"), dict) else {}
        return {
            "type": "image",
            "ts": record.get("ts", 0),
            "meta": meta,
            "jpg_b64": base64.b64encode(jpeg).decode("ascii"),
        }
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
        if rec.get("_rlog") == "img":
            meta = rec.get("meta")
            if isinstance(meta, dict):
                frm = meta.get("_from")
                if isinstance(frm, str) and frm:
                    return frm
            continue
        frm = rec.get("_from")
        if isinstance(frm, str) and frm:
            return frm
    return fallback
