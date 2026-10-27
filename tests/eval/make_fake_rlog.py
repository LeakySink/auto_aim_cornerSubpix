"""Generate synthetic .rlog files for self-testing the metrics tool (no OpenCV needed).

Scenarios (mirror the real plot(data) fields of auto_aim_debug_mpc):
  good  旋转靶车、关联正常：armor_id 随之 0->1->2->3 推进，w 稳定在真值附近
  bad   复现现场病态：armor_id 恒 0，相位 a 只能靠 ±90° 跳变去追旋转，w 在 0 附近乱摆

Usage:
    python tests/eval/make_fake_rlog.py OUTDIR            # 生成两个文件
    python tests/eval/make_fake_rlog.py OUTDIR --mode good
"""

from __future__ import annotations

import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))

from rdbg.log.writer import RlogWriter  # noqa: E402

# 1x1 灰色 JPEG（最小合法图片，用来验证图片记录统计）
TINY_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb00430008060607060508070707"
    "0909080a0c140d0c0b0b0c1912130f141d1a1f1e1d1a1c1c20242e2720222c231c1c"
    "28372a2c30313434341f27393d38323c2e333432ffc0000b080001000101011100ff"
    "c40014000100000000000000000000000000000000ffc4001410010000000000000000"
    "000000000000000000ffda0008010100003f00d2cf20ffd9"
)


def frame(t, a, w, armor_id, rng, dist, front=True):
    """构造一帧 plot(data)。front=True 表示这块板正对相机（法向指向云台）。"""
    obs_armor_yaw = a + armor_id * (2 * math.pi / 4)
    nis = max(0.0, rng.gauss(4.0, 1.6))
    return {
        "t": t,
        "gimbal_yaw": math.atan2(5.0 * math.sin(a), 5.0 * math.cos(a)),
        "gimbal_pitch": -0.02,
        "plan_yaw": a * 0.9,
        "plan_pitch": 0.05,
        "yaw_error": rng.gauss(0.0, 0.002),
        "pitch_error": rng.gauss(0.0, 0.002),
        "fire": 0,
        "armor_id": armor_id,
        "w": w,
        "target_x": dist * math.cos(a * 0.1),
        "target_z": dist * math.sin(a * 0.1),
        "target_y": 0.0,
        "target_vx": 0.0,
        "target_vz": 0.0,
        "target_yaw": a,
        "obs_yaw": a * 0.1,
        "obs_pitch": -0.02,
        "obs_dist": dist + rng.gauss(0.0, 0.05),
        "obs_armor_yaw": obs_armor_yaw,
        "nis": nis,
        "nis_threshold": 9.488,
        "nis_fail": 1.0 if nis > 9.488 else 0.0,
        "nees": max(0.0, rng.gauss(11.0, 6.0)),
        "nees_threshold": 19.675,
        "recent_nis_failures": min(1.0, max(0.0, rng.gauss(0.2, 0.1))),
        "_from": "fake",
    }


def gen_good(path: Path, seconds=5.0, fps=60.0, w_true=10.0):
    """正常：id 随旋转推进，w 稳定。"""
    rng = random.Random(1)
    wr = RlogWriter(path)
    dt, n = 1.0 / fps, int(seconds * fps)
    t0 = 1_760_000_000_000_000_000
    a = 0.0
    for i in range(n):
        a += w_true * dt
        aid = int(a / (math.pi / 2)) % 4
        obj = frame(i * dt, a, w_true + rng.gauss(0.0, 0.35), aid, rng, 5.0)
        wr.write_json_obj(t0 + int(i * dt * 1e9), obj)
        if i % 3 == 0:
            wr.write_image(t0 + int(i * dt * 1e9), {"name": "tup_detect", "w": 640}, TINY_JPEG)
    wr.close()
    return path


def gen_bad(path: Path, seconds=5.0, fps=60.0):
    """病态：id 恒 0，a 只在窄带里抖（相位靠跳变追旋转），w 在 0 附近乱摆。"""
    rng = random.Random(2)
    wr = RlogWriter(path)
    dt, n = 1.0 / fps, int(seconds * fps)
    t0 = 1_760_000_000_000_000_000
    for i in range(n):
        a = 0.35 + rng.gauss(0.0, 0.06)                      # 窄带内抖动
        w = 0.35 * math.sin(i * 0.7) + rng.gauss(0.0, 0.12)  # 在 0 附近摆动、频繁变号
        obj = frame(i * dt, a, w, 0, rng, 5.0)               # armor_id 恒 0
        obj["obs_armor_yaw"] = a + (math.pi / 2) * ((i // 12) % 4)  # 板上跳变
        wr.write_json_obj(t0 + int(i * dt * 1e9), obj)
        if i % 3 == 0:
            wr.write_image(t0 + int(i * dt * 1e9), {"name": "tup_detect", "w": 640}, TINY_JPEG)
    wr.close()
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description="生成合成 .rlog（自测评测工具用）")
    ap.add_argument("outdir", help="输出目录")
    ap.add_argument("--mode", choices=("good", "bad", "both"), default="both")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--fps", type=float, default=60.0)
    a = ap.parse_args(argv)
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    made = []
    if a.mode in ("good", "both"):
        made.append(gen_good(out / "fake_good.rlog", a.seconds, a.fps))
    if a.mode in ("bad", "both"):
        made.append(gen_bad(out / "fake_bad.rlog", a.seconds, a.fps))
    for p in made:
        print("[make_fake_rlog] %s (%d bytes)" % (p, p.stat().st_size))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
