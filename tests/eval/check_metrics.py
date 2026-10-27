"""Self-test for host/rdbg/log/metrics.py — 生成合成 rlog 并断言指标能区分好坏。

    python tests/eval/check_metrics.py

断言（good）：关联推进、w 稳定、相位平滑
断言（bad） ：id 恒 0、w 频繁变号、相位残差大
退出码 0 = 全部通过。
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(ROOT / "tests" / "eval"))

from rdbg.log import metrics  # noqa: E402
from make_fake_rlog import gen_bad, gen_good  # noqa: E402

FAIL = []


def check(ok, what, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", what, detail))
    if not ok:
        FAIL.append(what)


def main():
    tmp = Path(tempfile.mkdtemp(prefix="rlog_selftest_"))
    good = gen_good(tmp / "good.rlog")
    bad = gen_bad(tmp / "bad.rlog")

    print("== 解析 ==")
    g = metrics.load(good)
    b = metrics.load(bad)
    check(len(g["json"]) == 300, "good 解析出 300 帧", "实际 %d" % len(g["json"]))
    check(len(g["imgs"]) == 100, "good 解析出 100 张图片", "实际 %d" % len(g["imgs"]))
    check(len(b["json"]) == 300, "bad 解析出 300 帧", "实际 %d" % len(b["json"]))

    print("== CSV 导出 ==")
    csv_info = metrics.to_csv(g["json"], tmp / "good.csv")
    check(csv_info["rows"] == 300, "CSV 行数 300", str(csv_info["rows"]))
    check("target_yaw" in csv_info["cols"] and "nis" in csv_info["cols"], "CSV 含关键列")
    first = (tmp / "good.csv").read_text(encoding="utf-8").split("\n")[0]
    check("armor_id" in first and "obs_dist" in first, "CSV 表头正确")

    print("== 指标：good（应判定为正常跟踪）==")
    mg = metrics.analyze(g["json"])
    check(mg["armor_id"]["stuck_at"] is None, "id 不是恒 0")
    check(len(mg["armor_id"]["transitions"]) >= 3, "id 有多次推进",
          str(mg["armor_id"]["transitions"]))
    check(abs(mg["phase"]["slope_rad_per_s"] - 10.0) < 1.0,
          "估计转速 ≈10 rad/s", "%.2f" % mg["phase"]["slope_rad_per_s"])
    check(mg["w"]["zero_crossings_per_s"] < 1.0, "w 几乎不过零（方向稳定）",
          "%.1f 次/秒" % mg["w"]["zero_crossings_per_s"])
    check(mg["phase"]["residual_rms_deg"] < 5.0, "相位去趋势残差 <5°",
          "%.2f°" % mg["phase"]["residual_rms_deg"])

    print("== 指标：bad（应判定为抖动/锁死）==")
    mb = metrics.analyze(b["json"])
    check(mb["armor_id"]["stuck_at"] == 0, "识别出 id 恒 0")
    check(mb["w"]["zero_crossings_per_s"] > 5.0, "w 频繁过零（方向反转）",
          "%.1f 次/秒" % mb["w"]["zero_crossings_per_s"])
    check(abs(mb["phase"]["slope_rad_per_s"]) < 2.0, "相位几乎不推进（没跟上旋转）",
          "%.2f rad/s" % mb["phase"]["slope_rad_per_s"])
    check(mb["phase"]["residual_rms_deg"] > 2.0, "相位抖动残差 >2°",
          "%.2f°" % mb["phase"]["residual_rms_deg"])

    print("== 报告渲染 ==")
    rep = metrics.report(g["json"], g["imgs"], title="good")
    check("## 关联（armor_id）" in rep and "距离分桶" in rep, "报告含关联与分桶小节")
    check("[!]" not in rep, "good 报告无告警")
    repb = metrics.report(b["json"], b["imgs"], title="bad")
    check("[!]" in repb, "bad 报告带告警")

    print("")
    if FAIL:
        print("存在 %d 项失败：%s" % (len(FAIL), FAIL))
        return 1
    print("全部通过（合成数据自测）")
    print("产物目录：%s" % tmp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
