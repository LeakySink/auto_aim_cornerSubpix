#!/usr/bin/env python3
"""Replay a local .rlog by impersonating RemoteLogger over UDP to Host."""

import argparse
import json
import signal
import socket
import struct
import sys
import time
from pathlib import Path

from rlog import iter_records, summarize

IMG_MARKER = 0xFF
MAX_UDP = 60000
REGISTER_RETRY_S = 3.0


def _register(host, ctrl_port, name, timeout=3.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    payload = json.dumps({"type": "register", "name": name}).encode()
    sock.sendto(payload, (host, ctrl_port))
    try:
        data, _ = sock.recvfrom(2048)
        resp = json.loads(data.decode("utf-8"))
    except Exception as e:
        sock.close()
        raise RuntimeError(f"register recv failed: {e}") from e
    sock.close()
    if resp.get("status") != "ok" or "port" not in resp:
        raise RuntimeError(f"register rejected: {resp}")
    return int(resp["port"])


def _deregister(host, ctrl_port, name):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    try:
        sock.sendto(
            json.dumps({"type": "deregister", "name": name}).encode(),
            (host, ctrl_port),
        )
        sock.recvfrom(512)
    except Exception:
        pass
    sock.close()


def _pack_img(ts, meta, jpeg):
    meta_b = json.dumps(meta, separators=(",", ":")).encode("utf-8")
    pkt = bytearray()
    pkt.append(IMG_MARKER)
    pkt += struct.pack("<Q", ts)
    pkt += struct.pack("<I", len(meta_b))
    pkt += meta_b
    pkt += struct.pack("<I", len(jpeg))
    pkt += jpeg
    return bytes(pkt)


def _inject(obj, name, ts):
    if not isinstance(obj, dict):
        obj = {"value": obj}
    out = dict(obj)
    if "ts" not in out:
        out["ts"] = ts
    out["_from"] = name
    return out


def run(rlog_path, host="127.0.0.1", ctrl_port=15000, name="", speed=1.0,
        gap_cap=0.5, hb_ms=500, loop=False):
    path = Path(rlog_path)
    if not path.is_file():
        print(f"[replayer] not a file: {path}", file=sys.stderr)
        return 1

    n_json, n_img, inferred = summarize(path)
    if n_json + n_img == 0:
        print(f"[replayer] no records in {path}", file=sys.stderr)
        return 1

    name = name or inferred or path.stem
    speed = max(0.05, float(speed))
    print(
        f"[replayer] {path}  json={n_json} img={n_img}  "
        f"name={name}  host={host}:{ctrl_port}  speed={speed}x",
        file=sys.stderr,
    )

    port = None
    while port is None:
        try:
            port = _register(host, ctrl_port, name)
            print(f"[replayer] registered '{name}' -> data port {port}", file=sys.stderr)
        except Exception as e:
            err = str(e)
            print(f"[replayer] register failed ({e}), retry in {REGISTER_RETRY_S}s",
                  file=sys.stderr)
            if "already registered" in err:
                _deregister(host, ctrl_port, name)
            time.sleep(REGISTER_RETRY_S)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (host, port)
    stop = {"v": False}

    def _stop(*_a):
        stop["v"] = True

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    last_hb = time.monotonic()

    def maybe_hb():
        nonlocal last_hb
        if hb_ms <= 0:
            return
        now = time.monotonic()
        if (now - last_hb) * 1000 < hb_ms:
            return
        last_hb = now
        hb = {"hb": 1, "_from": name, "ts": time.time_ns()}
        sock.sendto(json.dumps(hb, separators=(",", ":")).encode(), dest)

    try:
        while not stop["v"]:
            t0_wall = time.monotonic()
            t0_rec = None
            prev_ts = None
            sent_img = sent_json = skipped_img = 0

            for rec in iter_records(path):
                if stop["v"]:
                    break
                ts = int(rec.get("ts") or 0)
                if t0_rec is None:
                    t0_rec = ts
                    prev_ts = ts

                if prev_ts is not None and ts >= prev_ts and gap_cap > 0:
                    gap = (ts - prev_ts) / 1e9 / speed
                    if gap > gap_cap:
                        t0_wall -= (gap - gap_cap)

                target = (ts - t0_rec) / 1e9 / speed
                delay = target - (time.monotonic() - t0_wall)
                if delay > 0.001:
                    end = time.monotonic() + delay
                    while not stop["v"]:
                        left = end - time.monotonic()
                        if left <= 0:
                            break
                        time.sleep(min(left, 0.05))
                        maybe_hb()

                if stop["v"]:
                    break

                if rec.get("kind") == "img":
                    meta = _inject(rec.get("meta") or {}, name, ts)
                    pkt = _pack_img(ts, meta, rec.get("jpeg") or b"")
                    if len(pkt) > MAX_UDP:
                        skipped_img += 1
                    else:
                        sock.sendto(pkt, dest)
                        sent_img += 1
                else:
                    obj = rec.get("obj") or {}
                    if "hb" in obj:
                        prev_ts = ts
                        continue
                    payload = json.dumps(
                        _inject(obj, name, ts), separators=(",", ":")
                    ).encode()
                    sock.sendto(payload, dest)
                    sent_json += 1

                maybe_hb()
                prev_ts = ts

            print(
                f"[replayer] pass done  json={sent_json} img={sent_img} "
                f"img_skip={skipped_img}",
                file=sys.stderr,
            )
            if not loop or stop["v"]:
                break
            print("[replayer] loop", file=sys.stderr)
    finally:
        _deregister(host, ctrl_port, name)
        sock.close()
        print("[replayer] deregistered", file=sys.stderr)
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Replay .rlog as a RemoteLogger sender to Host",
    )
    p.add_argument("rlog", help="path to .rlog")
    p.add_argument("--host", default="127.0.0.1", help="Host IP")
    p.add_argument("--ctrl-port", type=int, default=15000, help="UDP control port")
    p.add_argument("--name", default="", help="sender name (default: file _from, else stem)")
    p.add_argument("--speed", type=float, default=1.0, help="playback speed")
    p.add_argument("--gap-cap", type=float, default=0.5,
                   help="max wait between records in seconds (0=off)")
    p.add_argument("--hb", type=int, default=500, help="heartbeat ms, 0=off")
    p.add_argument("--loop", action="store_true", help="repeat after EOF")
    args = p.parse_args()
    return run(
        args.rlog,
        host=args.host,
        ctrl_port=args.ctrl_port,
        name=args.name,
        speed=args.speed,
        gap_cap=args.gap_cap,
        hb_ms=args.hb,
        loop=args.loop,
    )


if __name__ == "__main__":
    raise SystemExit(main())
