#!/usr/bin/env python3
"""Host 侧正向证明：RLG2 writer / recorder 在洪水下不丢契约、有界队列不堵主路径。"""

from __future__ import annotations

import base64
import json
import os
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))

from rdbg.log.recorder import LiveRecorder  # noqa: E402
from rdbg.log.rlog import iter_records  # noqa: E402
from rdbg.log.writer import RlogWriter  # noqa: E402


def proof_writer_roundtrip(tmpdir: Path) -> bool:
    path = tmpdir / "roundtrip.rlog"
    w = RlogWriter(path)
    n = 500
    t0 = time.perf_counter()
    for i in range(n):
        w.write_json_obj(i + 1, {"i": i, "_from": "host_proof", "cam_fps": 120.0})
    tiny = base64.b64decode(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
        "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIy"
        "MjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIA"
        "AhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAn/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFQEB"
        "AQAAAAAAAAAAAAAAAAAAAAX/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAGcP//E"
        "ABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAQUCf//EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAI"
        "AQMBAT8Bf//EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQIBAT8Bf//Z"
    )
    # use minimal jpeg header-ish bytes; reader accepts any blob
    jpeg = b"\xff\xd8\xff\xd9"
    for i in range(50):
        w.write_image(1000 + i, {"name": "front", "_from": "host_proof"}, jpeg)
    w.close()
    ms = (time.perf_counter() - t0) * 1000
    recs = list(iter_records(path))
    n_json = sum(1 for r in recs if r["kind"] == "json")
    n_img = sum(1 for r in recs if r["kind"] == "img")
    print(f"\n=== H1. RlogWriter 往返 ===")
    print(f"  wrote {n}+50 in {ms:.1f} ms; read back json={n_json} img={n_img}")
    ok = n_json == n and n_img == 50
    print(f"  RESULT   : {'PASS' if ok else 'FAIL'}")
    return ok


def proof_recorder_nonblocking(tmpdir: Path) -> bool:
    path = tmpdir / "live.rlog"
    rec = LiveRecorder()
    rec.start(path)

    # --- H2a: 主线程洪水 put 绝不阻塞（有界队列必丢）---
    t0 = time.perf_counter()
    burst = 5000
    for i in range(burst):
        rec.put(
            json.dumps(
                {"type": "plot", "ts": i + 1, "data": {"i": i, "_from": "x", "ts": i + 1}},
                separators=(",", ":"),
            )
        )
    put_ms = (time.perf_counter() - t0) * 1000
    st_burst = rec.status()
    time.sleep(0.8)

    # --- H2b: 可跟上的速率 → 应接近完整落盘 ---
    # 清空后再录一段稳态
    rec.stop()
    path2 = tmpdir / "steady.rlog"
    rec.start(path2)
    steady = 400
    for i in range(steady):
        rec.put(
            json.dumps(
                {
                    "type": "plot",
                    "ts": 10000 + i,
                    "data": {"i": i, "_from": "x", "ts": 10000 + i},
                },
                separators=(",", ":"),
            )
        )
        time.sleep(0.002)  # ~500 Hz，低于写盘
    time.sleep(1.0)
    st_steady = rec.status()
    out = rec.stop()
    n_json = sum(1 for r in iter_records(out) if r["kind"] == "json")

    print(f"\n=== H2. LiveRecorder 有界队列 ===")
    print(
        f"  burst put {burst} lines wall={put_ms:.1f} ms "
        f"({burst * 1000 / put_ms:.0f} put/s) dropped≥{st_burst.get('dropped', 0)}"
    )
    print(f"  steady {steady} @ ~500Hz → file json={n_json} dropped={st_steady.get('dropped', 0)}")
    ok_fast = put_ms < 1500
    ok_drop = st_burst.get("dropped", 0) > 0  # 洪水必触发有界丢弃
    ok_steady = n_json >= steady * 0.95 and st_steady.get("dropped", 0) == 0
    ok = ok_fast and ok_drop and ok_steady
    print(
        f"  RESULT   : {'PASS' if ok else 'FAIL'}"
        f"（快 put={ok_fast} 洪水丢包={ok_drop} 稳态完整={ok_steady}）"
    )
    return ok


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="rdbg_host_proof_") as d:
        tmp = Path(d)
        ok = True
        ok &= proof_writer_roundtrip(tmp)
        ok &= proof_recorder_nonblocking(tmp)
    print("\n=== HOST SUMMARY ===")
    print("ALL PASS" if ok else "SOME FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
