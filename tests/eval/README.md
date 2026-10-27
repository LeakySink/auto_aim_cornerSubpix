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

## Stage 2：重跑检测（`rlog_replay`，C++）

上面的工具只分析**已记录的量**，不重跑检测。要回答"新模型比旧模型检出率高多少"，
需要用 `tests/eval/rlog_replay.cpp`（C++，CMake 自动编成 `./build/rlog_replay`）：

```bash
./build/rlog_replay configs/sentry.yaml eval_data/run_xxx.rlog --out replay.csv
#   可选：--topic raw（默认） --limit 300 --tracker --min-conf 0.3 --quiet
```

输出 CSV：`frame,ts,det_n,max_conf,mean_conf,best_conf,best_name,best_type,best_color,`
`best_dist,best_armor_yaw,best_cx,best_cy,obs_dist,dist_diff`（`--tracker` 时追加
`a,w,r,l,h,armor_id,nis`）。末行打印汇总：处理帧数、检出帧比例、PnP 距离均值、
**与录制 `obs_dist` 的差值均值**。

### 两个前置条件（否则结论无效）

1. **图片必须是原始帧**：`reprojection` / `tup_detect` 都画过框和关键点，拿它们重跑
   检测会被自己画的标注污染。`auto_aim_debug_mpc` 已发布 `raw` 话题（未画标注），
   工具默认只吃 `--topic raw`，找不到会告警并回退到全部图片。
2. **必须是全分辨率**：`.rlog` 里的图片会被 `remote_logger.img_width` 压缩（当前 640），
   而实车采集是 1440 —— 检测的等效尺度会变，**结论不能与实车对比**。工具会打印
   图片分辨率并在过小时告警。两种解决办法：
   - 录制评估数据时把 `img_width` 设成实车宽度（如 1440）；或
   - 直接用下面的 `eval_record`（推荐）。

## 全分辨率采集：`src/eval_record.cpp`

```bash
./build/eval_record <config.yaml> [--out <file.rlog>] [--every K] [--max-frames N]
                    [--quality Q] [--no-detect] [--quiet]
```

按**全分辨率**写标准 RLG2 `.rlog`（每帧 JSON 元数据 + 每 K 帧一张原始 JPEG），
因此同一份数据可以同时喂给三套工具：

```bash
./build/eval_record configs/sentry.yaml --out eval_data/spin_01.rlog --every 2 --max-frames 3000
./build/rlog_replay configs/sentry.yaml eval_data/spin_01.rlog --out spin_01.csv  # 重跑检测
python -m rdbg metrics eval_data/spin_01.rlog                                     # 逐帧指标
python -m rdbg replay  eval_data/spin_01.rlog                                     # host 可视化回放
```

写盘量估算：1440×1080 JPEG q90 ≈ 250 KB/张；`--every 2` @60fps ≈ 7.5 MB/s，
3000 帧（约 100 s）≈ 750 MB，注意磁盘空间。

> 1 月重训 YOLO 需要的**全分辨率原始帧**也由这条路径产出（`--no-detect` 可省 CPU）。
