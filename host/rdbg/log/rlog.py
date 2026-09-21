"""Read RemoteLogger local .rlog (v1 JSON-only RLOG, v2 RLG2 json+jpeg)."""

import json
import struct
import sys
from pathlib import Path

MAGIC_V1 = 0x524C4F47  # numeric "RLOG" (legacy)
MAGIC_V2 = 0x32474C52  # LE bytes "RLG2"
REC_JSON = 0x00
REC_IMG = 0x01


def _read_exact(fp, n):
    buf = fp.read(n)
    if len(buf) != n:
        return None
    return buf


def _parse_json(ts, raw):
    try:
        obj = json.loads(raw.decode("utf-8"))
    except Exception:
        obj = {"raw": raw.decode("utf-8", errors="replace")}
    if not isinstance(obj, dict):
        obj = {"value": obj}
    if "ts" not in obj:
        obj["ts"] = ts
    return {"kind": "json", "ts": ts, "obj": obj}


def _meta_from(raw):
    try:
        meta = json.loads(raw.decode("utf-8"))
    except Exception:
        meta = {"raw": raw.decode("utf-8", errors="replace")}
    if not isinstance(meta, dict):
        meta = {"value": meta}
    return meta


def iter_records(path, include_jpeg=True):
    """Yield records in file order.

    json: {"kind": "json", "ts": int, "obj": dict}
    img:  {"kind": "img",  "ts": int, "meta": dict, "jpeg": bytes}
    """
    p = Path(path)
    try:
        fp = p.open("rb")
    except OSError as e:
        print(f"[rlog] open failed: {e}", file=sys.stderr)
        return

    with fp:
        hdr = _read_exact(fp, 4)
        if hdr is None:
            print(f"[rlog] too short: {p}", file=sys.stderr)
            return
        magic = struct.unpack("<I", hdr)[0]
        if magic == MAGIC_V1:
            yield from _iter_v1(fp)
        elif magic == MAGIC_V2:
            yield from _iter_v2(fp, include_jpeg)
        else:
            print(
                f"[rlog] bad magic 0x{magic:08x} "
                f"(want 0x{MAGIC_V1:08x} or 0x{MAGIC_V2:08x}): {p}",
                file=sys.stderr,
            )


def summarize(path):
    """Count records and infer sender without keeping JPEG payloads."""
    n_json = n_img = 0
    name = ""
    for rec in iter_records(path, include_jpeg=False):
        if rec.get("kind") == "img":
            n_img += 1
            frm = (rec.get("meta") or {}).get("_from")
        else:
            n_json += 1
            frm = (rec.get("obj") or {}).get("_from")
        if not name and isinstance(frm, str) and frm:
            name = frm
    return n_json, n_img, name


def _iter_v1(fp):
    while True:
        hdr = _read_exact(fp, 12)
        if hdr is None:
            return
        ts, length = struct.unpack("<QI", hdr)
        raw = _read_exact(fp, length)
        if raw is None:
            print("[rlog] truncated v1 record, stop", file=sys.stderr)
            return
        yield _parse_json(ts, raw)


def _iter_v2(fp, include_jpeg):
    while True:
        hdr = _read_exact(fp, 1 + 8)
        if hdr is None:
            return
        typ = hdr[0]
        ts = struct.unpack_from("<Q", hdr, 1)[0]
        if typ == REC_JSON:
            ln = _read_exact(fp, 4)
            if ln is None:
                print("[rlog] truncated json header, stop", file=sys.stderr)
                return
            length = struct.unpack("<I", ln)[0]
            raw = _read_exact(fp, length)
            if raw is None:
                print("[rlog] truncated json, stop", file=sys.stderr)
                return
            yield _parse_json(ts, raw)
        elif typ == REC_IMG:
            ln = _read_exact(fp, 4)
            if ln is None:
                print("[rlog] truncated img meta header, stop", file=sys.stderr)
                return
            meta_len = struct.unpack("<I", ln)[0]
            meta_raw = _read_exact(fp, meta_len)
            if meta_raw is None:
                print("[rlog] truncated img meta, stop", file=sys.stderr)
                return
            ln = _read_exact(fp, 4)
            if ln is None:
                print("[rlog] truncated jpeg header, stop", file=sys.stderr)
                return
            jpg_len = struct.unpack("<I", ln)[0]
            if include_jpeg:
                jpeg = _read_exact(fp, jpg_len)
                if jpeg is None:
                    print("[rlog] truncated jpeg, stop", file=sys.stderr)
                    return
            else:
                cur = fp.tell()
                fp.seek(0, 2)
                end = fp.tell()
                if cur + jpg_len > end:
                    print("[rlog] truncated jpeg, stop", file=sys.stderr)
                    return
                fp.seek(cur + jpg_len)
                jpeg = b""
            yield {
                "kind": "img",
                "ts": ts,
                "meta": _meta_from(meta_raw),
                "jpeg": jpeg,
                "jpeg_len": jpg_len,
            }
        else:
            print(f"[rlog] unknown type 0x{typ:02x}, stop", file=sys.stderr)
            return
