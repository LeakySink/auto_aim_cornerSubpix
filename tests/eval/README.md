# tests/eval —— 离线评测（W1–W2）

对录好的 `.rlog` 做**逐帧量化分析**，不需要实车、不需要重跑检测。
纯 Python 标准库，任何机器可跑（车端可直接跑，笔记本也行）。

## 用法

```bash
# 1) 报告（markdown，直接贴进 issue / 文档）
python -m rdbg metrics logs/run_xxx.rlog
python -m rdbg metrics logs/run_xxx.rlog --derived derived.csv   # 顺便导出逐帧 CSV

# 2) 只导出逐帧 CSV（Excel / pandas 自己画）
python -m rdbg csv logs/run_xxx.rlog -o data.csv
```

（在 `host/` 目录下执行，或先 `source host/_env.sh` 用统一的 Python 环境。）

## 报告里有什么

| 小节 | 指标 | 判读 |
|---|---|---|
| 概览 | 帧数 / 时长 / fps / 图片记录数 | 图少说明录制丢图 |
| 关联（armor_id） | 编号集合、转移矩阵、**来回跳（A→B→A）次数** | 旋转目标应看到 `0->1->2->3->0` 推进；`[!] 全程锁在 N 号板` 说明关联退化 |
| 相位 a | 解缠跨度、**线性拟合斜率**（估计转速）、去趋势残差 RMS | 斜率应≈真实转速；残差 >5° 说明在抖 |
| 角速度 w | mean/std/min/max、一阶差分 RMS、**过零次数/秒** | 稳定自转应几乎**不过零**；在 0 附近摆动 → 方向反复反转 |
| 卡方 | nis 均值/最大、**越限比例**、recent_nis_failures | 名义越限 5%；>20% 说明观测与模型不一致 |
| 观测/估计一致性 | `|obs_dist − |(target_x,target_z)||` | 反映 PnP 与 EKF 是否在同一量纲/同一目标 |
| 分桶 | 按 `obs_dist`、`obs_armor_yaw` 分桶的帧数与 nis 均值 | 定位"哪个距离/角度区间变差" |

> **注意两个 w 指标别混用**：`一阶差分 RMS` 是**抖动幅度**（越小越平滑），
> `过零次数/秒` 是**旋转方向反转**（稳定自转≈0）。早期版本曾用"差分变号次数"，
> 那是白噪声的固有属性（恒 ≈50%，与幅度无关），对任何信号都判抖，已废弃。

## 自测

```bash
python tests/eval/check_metrics.py     # 生成合成 rlog 并断言指标能区分好坏
```

合成数据由 `tests/eval/make_fake_rlog.py` 生成，两种场景：

- `good`：旋转靶车、关联正常（id 随旋转推进、w 稳定、相位平滑）
- `bad`：复现现场病态（id 恒 0、相位只能靠跳变追、w 在 0 附近乱摆）

也可以单独生成，用来当回归基线：

```bash
python tests/eval/make_fake_rlog.py /tmp/rlog --mode both
```

## 局限（下一步）

本工具只分析**已记录的量**（`plot(data)` 的字段），**不重跑检测**。因此：

- 能回答"关联/相位/滤波/一致性"层面的问题；
- **不能**回答"新模型比旧模型检出率高多少"——那需要 C++ 侧回放（读 `.rlog` 的图片 →
  重新走 Detector→Solver→Tracker）。见 `docs/roadmap.md` W1–W2 的 Stage 2。
