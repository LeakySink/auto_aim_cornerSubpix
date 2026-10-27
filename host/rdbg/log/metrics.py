"""Analyse a .rlog into CSV + a per-frame quality report.

Why: Watch page is good for live curves, but "旋转靶车抖动 / 锁不上" needs per-frame
quantification (id 是否推进 / w 抖多少 / nis 越限比例 / 按距离分桶). 本模块只用标准库，
不需要 OpenCV，因此任何机器都能对录制的 .rlog 直接跑。

用法：
    python -m rdbg metrics run.rlog                 # 打印报告
    python -m rdbg csv run.rlog -o data.csv         # 导出逐帧 CSV
数据来源：车端 RemoteLogger::plot(data) 的 JSON 记录（字段见 docs/architecture.md 附录 B）
与 plot_image 的 JPEG 记录（本模块只统计数量与时间跨度，不落盘图片）。
"""

from __future__ import annotations

import math
from pathlib import Path

from .rlog import iter_records

# CSV 排在前的字段（其余按字母序补后）
_PREFERRED = [
    "ts", "t", "gimbal_yaw", "gimbal_pitch", "gimbal_yaw_vel", "gimbal_pitch_vel",
    "plan_yaw", "plan_pitch", "yaw_error", "pitch_error", "yp_error", "fire",
    "armor_id", "w", "target_x", "target_y", "target_z", "target_vx", "target_vy",
    "target_vz", "target_yaw", "obs_yaw", "obs_pitch", "obs_dist", "obs_armor_yaw",
    "nis", "nis_threshold", "nees", "nees_threshold", "recent_nis_failures",
]


def load(path):
    rows, imgs = [], []
    for rec in iter_records(path):
        if rec.get("kind") == "json":
            obj = dict(rec.get("obj") or {})
            obj.setdefault("ts", rec.get("ts"))
            rows.append(obj)
        elif rec.get("kind") == "img":
            imgs.append((rec.get("ts"), rec.get("meta") or {}))
    rows.sort(key=lambda r: r.get("ts") or 0)
    imgs.sort(key=lambda x: x[0] or 0)
    return {"json": rows, "imgs": imgs}


def _scalar(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def columns(rows):
    keys = set()
    for r in rows:
        for k, v in r.items():
            if _scalar(v):
                keys.add(k)
    head = [k for k in _PREFERRED if k in keys]
    rest = sorted(k for k in keys if k not in head and k != "ts")
    return head + rest


def _fmt(v):
    if isinstance(v, float):
        if math.isnan(v) or math.isinf(v):
            return ""
        return repr(round(v, 9))
    return str(v)


def to_csv(rows, out_path):
    cols = columns(rows)
    skipped = set()
    lines = [",".join(cols)]
    for r in rows:
        cells = []
        for c in cols:
            v = r.get(c)
            if v is None:
                cells.append("")
            elif _scalar(v):
                cells.append(_fmt(v))
            else:
                skipped.add(c)
                cells.append("")
        lines.append(",".join(cells))
    p = Path(out_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"file": str(p), "rows": len(rows), "cols": cols, "skipped": sorted(skipped)}


# ---------------------------------------------------------------- 指标

def _series(rows, key):
    return [(r.get("ts") or 0, r[key]) for r in rows if _scalar(r.get(key))]


def _unwrap(vals):
    out, acc, prev = [], 0.0, None
    for v in vals:
        if prev is not None:
            acc += (v - prev + math.pi) % (2 * math.pi) - math.pi
        prev = v
        out.append(acc)
    return out


def _stats(vals):
    n = len(vals)
    if n == 0:
        return {"n": 0}
    mean = sum(vals) / n
    var = sum((v - mean) ** 2 for v in vals) / n
    return {"n": n, "mean": mean, "std": math.sqrt(var), "min": min(vals), "max": max(vals)}


def _diff_stats(vals):
    """一阶差分 RMS = 抖动强度；sign_flips = 变号次数。"""
    if len(vals) < 2:
        return {"n": 0}
    d = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    rms = math.sqrt(sum(x * x for x in d) / len(d))
    flips = sum(1 for i in range(1, len(d)) if d[i] * d[i - 1] < 0)
    return {"n": len(d), "rms": rms, "sign_flips": flips}


def analyze(rows):
    out = {"frames": len(rows)}
    ts = [r.get("ts") or 0 for r in rows]
    if len(ts) >= 2 and ts[-1] > ts[0]:
        dur = (ts[-1] - ts[0]) / 1e9
        out["duration_s"] = dur
        out["fps"] = (len(ts) - 1) / dur if dur > 0 else 0.0
    else:
        dur = 0.0
        out["duration_s"] = 0.0
        out["fps"] = 0.0

    # 1) 关联
    ids = [int(v) for _, v in _series(rows, "armor_id")]
    if ids:
        pairs, back = {}, 0
        for a, b in zip(ids, ids[1:]):
            if a != b:
                pairs[(a, b)] = pairs.get((a, b), 0) + 1
        for i in range(len(ids) - 2):
            if ids[i] == ids[i + 2] and ids[i] != ids[i + 1]:
                back += 1
        out["armor_id"] = {
            "distinct": sorted(set(ids)),
            "transitions": {"%d->%d" % k: v for k, v in sorted(pairs.items())},
            "back_and_forth": back,
            "stuck_at": ids[0] if len(set(ids)) == 1 else None,
        }

    # 2) 相位 a
    a_vals = [v for _, v in _series(rows, "target_yaw")]
    if len(a_vals) >= 3:
        un = _unwrap(a_vals)
        n = len(un)
        xs = list(range(n))
        mx, my = sum(xs) / n, sum(un) / n
        den = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, un)) / den if den else 0.0
        resid = [y - (my + slope * (x - mx)) for x, y in zip(xs, un)]
        rms = math.sqrt(sum(r * r for r in resid) / n)
        dt = (dur / (n - 1)) if (dur > 0 and n > 1) else 0.0
        out["phase"] = {
            "unwrapped_span_rad": max(un) - min(un),
            "slope_rad_per_s": slope / dt if dt > 0 else 0.0,
            "residual_rms_deg": math.degrees(rms),
            "d_phase_rms_rad": _diff_stats(un).get("rms"),
        }

    # 3) 角速度 w
    #    注意两个指标含义不同：
    #      d_rms                  一阶差分 RMS —— 高频抖动幅度（越小越平滑）
    #      zero_crossings_per_s   w 过零次数/秒 —— 旋转方向反转次数；稳定的自转应 ≈0，
    #                             而"w 在 0 附近来回摆"会很大（这才是抖动的病态特征）
    #    早期版本用"差分变号次数"，那是白噪声的固有属性（与幅度无关，恒 ≈50%），
    #    对任何信号都判"抖"，已废弃。
    w_vals = [v for _, v in _series(rows, "w")]
    if w_vals:
        st, d = _stats(w_vals), _diff_stats(w_vals)
        zc = sum(1 for i in range(len(w_vals) - 1) if w_vals[i] * w_vals[i + 1] < 0)
        out["w"] = {
            "mean": st["mean"], "std": st["std"], "min": st["min"], "max": st["max"],
            "d_rms": d.get("rms"),
            "zero_crossings": zc,
            "zero_crossings_per_s": (zc / dur) if dur > 0 else 0.0,
        }

    # 4) 卡方
    nis = [v for _, v in _series(rows, "nis")]
    if nis:
        th = [v for _, v in _series(rows, "nis_threshold")]
        thr = th[0] if th else 9.488
        over = sum(1 for v in nis if v > thr)
        out["nis"] = {
            "threshold": thr, "over": over, "ratio": over / len(nis),
            "max": max(nis), "mean": sum(nis) / len(nis),
        }
    if [v for _, v in _series(rows, "recent_nis_failures")]:
        out["recent_nis_failures"] = _stats([v for _, v in _series(rows, "recent_nis_failures")])

    # 5) 观测 vs 估计距离
    obs = [v for _, v in _series(rows, "obs_dist")]
    if obs:
        out["obs_dist"] = _stats(obs)
        est = [math.hypot(r["target_x"], r["target_z"]) for r in rows
               if _scalar(r.get("target_x")) and _scalar(r.get("target_z"))]
        if est:
            m = min(len(obs), len(est))
            diff = [abs(obs[i] - est[i]) for i in range(m)]
            out["obs_vs_est"] = {"mean_abs_diff_m": sum(diff) / m, "max_abs_diff_m": max(diff)}
        gaps, cur = [], 0
        for r in rows:
            if _scalar(r.get("obs_dist")):
                if cur:
                    gaps.append(cur)
                cur = 0
            else:
                cur += 1
        if cur:
            gaps.append(cur)
        if gaps:
            out["gaps"] = {"count": len(gaps), "max_frames": max(gaps),
                           "mean_frames": sum(gaps) / len(gaps)}
    return out


def buckets(rows, key, edges, min_count=20):
    """按 key 分桶（用于距离/角度分桶评估）。"""
    res = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        sel = [r for r in rows if _scalar(r.get(key)) and lo <= r[key] < hi]
        if len(sel) < min_count:
            continue
        e = {"lo": lo, "hi": hi, "n": len(sel)}
        for m in ("nis", "obs_dist"):
            vals = [r[m] for r in sel if _scalar(r.get(m))]
            if vals:
                e[m + "_mean"] = sum(vals) / len(vals)
        res.append(e)
    return res


def report(rows, imgs=None, title=None):
    m = analyze(rows)
    L = []
    L.append("# .rlog 逐帧质量报告" + (("：" + title) if title else ""))
    L.append("")
    L.append("- 帧数 **%d**，时长 **%.2f s**，平均 **%.1f fps**"
             % (m["frames"], m["duration_s"], m["fps"]))
    if imgs:
        span = (imgs[-1][0] - imgs[0][0]) / 1e9 if (len(imgs) >= 2 and imgs[0][0]) else 0.0
        L.append("- 图片记录 **%d** 张%s" % (len(imgs), ("，跨度 %.2f s" % span) if span > 0 else ""))
    L.append("")

    a = m.get("armor_id")
    if a:
        L.append("## 关联（armor_id）")
        L.append("")
        L.append("- 出现的编号 `%s`" % a["distinct"])
        L.append("- 编号转移 `%s`" % (a["transitions"] or "无"))
        L.append("- **来回跳（A→B→A）次数：%d**" % a["back_and_forth"])
        if a["stuck_at"] is not None:
            L.append("- [!] **全程锁在 %d 号板**：旋转时相位只能靠跳变去追 → 抖动" % a["stuck_at"])
        L.append("")

    p = m.get("phase")
    if p:
        L.append("## 相位 a（target_yaw）")
        L.append("")
        L.append("- 解缠跨度 %.2f rad，**线性拟合斜率 ≈ %.2f rad/s**（估计转速）"
                 % (p["unwrapped_span_rad"], p["slope_rad_per_s"]))
        L.append("- 去趋势残差 RMS **%.2f°**（越小越平滑；>5° 说明在抖）" % p["residual_rms_deg"])
        L.append("")

    w = m.get("w")
    if w:
        L.append("## 角速度 w")
        L.append("")
        L.append("- mean %.3f / std %.3f / min %.3f / max %.3f rad/s"
                 % (w["mean"], w["std"], w["min"], w["max"]))
        L.append("- 高频抖动（一阶差分 RMS）%.3f rad/s" % w["d_rms"])
        L.append("- **w 过零 %.1f 次/秒**（稳定自转应 ≈0；在 0 附近摆动会很大 → 方向反转）"
                 % w["zero_crossings_per_s"])
        L.append("")

    n = m.get("nis")
    if n:
        L.append("## 卡方检验")
        L.append("")
        L.append("- nis mean %.2f / max %.2f，阈值 %.3f" % (n["mean"], n["max"], n["threshold"]))
        L.append("- **越限比例 %.1f%%**（名义 5%%；>20%% 说明观测与模型不一致）" % (n["ratio"] * 100))
        r = m.get("recent_nis_failures")
        if r:
            L.append("- recent_nis_failures mean %.3f / max %.3f（≥0.4 会丢目标重锁）"
                     % (r["mean"], r["max"]))
        L.append("")

    o = m.get("obs_vs_est")
    if o:
        L.append("## 观测/估计一致性")
        L.append("")
        L.append("- |obs_dist - |(target_x,target_z)||：mean %.3f m / max %.3f m"
                 % (o["mean_abs_diff_m"], o["max_abs_diff_m"]))
        g = m.get("gaps")
        if g:
            L.append("- 观测中断 %d 段，最长 %d 帧（均 %.1f）"
                     % (g["count"], g["max_frames"], g["mean_frames"]))
        L.append("")

    for key, edges, name in (
        ("obs_dist", [0, 3, 5, 8, 12, 20, 100], "距离分桶"),
        ("obs_armor_yaw", [-4.0, -2.0, -1.0, 1.0, 2.0, 4.0], "板朝向分桶"),
    ):
        b = buckets(rows, key, edges)
        if b:
            L.append("## %s（%s）" % (name, key))
            L.append("")
            L.append("| 区间 | 帧数 | nis 均值 | obs_dist 均值 |")
            L.append("|---|---|---|---|")
            for e in b:
                L.append("| [%.1f, %.1f) | %d | %s | %s |"
                         % (e["lo"], e["hi"], e["n"],
                            ("%.2f" % e["nis_mean"]) if "nis_mean" in e else "",
                            ("%.2f" % e["obs_dist_mean"]) if "obs_dist_mean" in e else ""))
            L.append("")
    return "\n".join(L)
