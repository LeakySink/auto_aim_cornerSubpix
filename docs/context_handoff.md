# 上下文交接 · 视觉自瞄后端

> **最近更新**：2026-11-15（回写 R1/R3 状态、记录打符移除）
> **仓库**：`D:\my\ckyf_vision\auto_aim_cornerSubpix-main`
> **分支**：`feat/heavy-aim`（只做 2027 重装自瞄）
>
> 本文件回答三个问题：**这个仓库经历了什么、现在站在哪、下一步做什么**。
> 它是**会话级**交接，记录的是「决策 + 证据」；
> 长期研发计划在 [`heavy_handover.md`](heavy_handover.md)，两者互补，不重复。

---

## 0. 三行状态

1. **主链路可用且已编译验证**：YOLO → 关键点校验 → PnP → 11 维 EKF → MPC → 云台，
   `60f03de` 已在本机用真实依赖编过（9 个 TU 零错误、零新增告警），详见
   [`build_verification_60f03de.md`](build_verification_60f03de.md) §2。
   **重装已有了专属入口 `src/heavy_aim.cpp`**（2026-11-16）：瞄旋转中心、
   到达时刻有板压线才开火，不走 MPC；核心在 `tasks/auto_aim/center_aimer.*`，
   单测 `tests/algo/center_aimer_check.cpp` 已通过。策略与限制见
   [`architecture.md`](architecture.md) 的《重装策略》一节，**实车未验证**。
2. **重装参数：弹道已补齐，其余未落地** —— `big.csv` 已重生成到
   `V∈[13.0, 16.6]`（16.5 档可用，12 m 处修正 209 mm）；仍缺 `configs/heavy.yaml`、
   `armor.hpp` 里 `big` 仍是 230 mm（2027 已取消大装甲）。
3. **下一件阻塞事是 `R` 标定，不是编译**：实车 `.rlog` 显示 NIS 均值 0.104（名义 4）、
   越限 0.0%，Bad Converge 看门狗**确认永不触发**；新息门控在 123 s 里**触发 0 次**。
   改 `R` 后必须重录对照。

---

## 1. 时间线：这个仓库经历了什么

`git log` 共 70 个提交，本工作流覆盖从 `058f23c` 到 `60f03de`。按阶段分：

### 阶段一 · 崩溃与未定义行为加固（`8d8913…104ec02`）

| 提交 | 内容 |
|---|---|
| `a8d8913` | `limit_rad` 原本在 NaN 输入下**死循环**，改成 O(1) + 非有限值安全 |
| `8f733a7` | `omniperception` 构造函数按值捕获 worker 线程参数（悬垂引用） |
| `2957d98` | OpenVINO tensor 拷进自有 buffer 再 `start_async`（UAF） |
| `911fbcb` | CAN 帧长度 / 模式枚举校验后才索引 `MODES` |
| `749af73` | YOLO 输出 tensor 的 rank/dtype 校验后才包成 `cv::Mat` |
| `e74667d` | `Trajectory` 拒绝 NaN/inf 输入而非传播 |
| `104ec02` | `diverged()` 短状态保护；`buff.rowRange` 偏移修正 |

**教训**：这些 bug 的共同形态是「不可信输入直接进索引/循环条件」。
新增代码时凡是有 `while` 或 `[]`，先问一句"最坏输入是什么"。

### 阶段二 · 弹道模型（`058f23c…49c8bea`）

| 提交 | 内容 |
|---|---|
| `058f23c` | 接入 `ballistic-main` 离线拟合多项式表（poly 分支 + RK4 回退） |
| `19c7350` | 多项式对拍测试（不依赖 OpenCV/Eigen，本机可编译运行） |
| `a49dea8` | `sentry.yaml` 启用 poly；修正与 22 m/s 不自洽的阻力系数 |
| `9031cd1` | **删除** `drag_coefficient_up/down/drag_height_boundary` 整条链路 |
| `49c8bea` | **修 pitch 语义错误**（见 D2） |

**教训（重要）**：`49c8bea` 是一个"看起来测过了"却实际打偏的 bug。
之前的测试全在 `h=0` 工况下跑，而错误只在 `h≠0` 时暴露。

### 阶段三 · 几何配置化与评测台（`7e15884…ce11145`）

| 提交 | 内容 |
|---|---|
| `02a2305` | `.rlog` 逐帧质量分析（纯标准库，本机可跑） |
| `e181db1` | 全分辨率采集 + C++ 离线回放（`rlog_replay`） |
| `7e15884` | `armor_geometry.hpp` + `keypoint_check.hpp`，硬编码常量全部进 yaml |
| `8bb3cde` | `publish_raw_frame` 改 yaml 可选（默认关），避免 debug 行程翻倍 |

### 阶段四 · 滤波与关联（`13cee2d`、`60f03de`）

| 提交 | 内容 |
|---|---|
| `13cee2d` | PnP 装甲板朝向的 **±π 二义性**几何消歧（修 `armor_id` 卡 0） |
| `536caa1` | Planner 过滤 EKF 预测的近距"幽灵装甲板" |
| `9cb83a9` | NIS 看门狗改按比例判定 + 关联加空间门控 |
| `60f03de` | **R 自适应 + 新息门控**（见 D5/D6） |

### 阶段五 · 转向重装（`28d3d77` 之后，以文档为主）

`28d3d77` 路线图 + 假设清单 → `a338691` 兵种特性 → `d54aef4` 机械尺度 →
`8f6e738` 重装后端清单 → `c87bf56` 基线 → `b2cc9ef` 交接书 →
`ad90562` UKF 评估 + 2026 材料归档 → `60f03de` R 自适应。

---

## 2. 关键决策记录

记录的是**为什么这么改**，避免后人推翻一件已经论证过的事。

### D1 · 删掉 `drag_coefficient_up/down`，阻力只留一个 `k`

- **原状态**：按弹道高度二选一阻力系数，`sentry.yaml` 里 `up=0.2415`，
  与真实弹速 22 m/s 严重不自洽，表现为 **>8 m 直接 `Unsolvable target`**。
- **决策**：整条链路删除（`9031cd1`）。阻力系数回到 `tools::trajectory.hpp`
  的单一 `kDefaultDragCoefficient = 0.01875`（17mm 实测拟合值），
  重装 42mm 约 **0.00513**。
- **理由**：`ballistic-main` 的多项式表已经把阻力效应吸收进系数里，
  再叠加一个独立的阻力系数是**双重计入**。
- **遗留**：这个 `k` 仍是硬编码，且是 17mm 的值（见 §4 R4）。

### D2 · 多项式表的 `pitch` 是「补角」，不是「绝对俯仰角」

- **证据**：`ballistic-main/g.py` 里 `Pitch = degrees(solved_pitch - atan2(Y, X))`。
- **后果**：运行时必须 `pitch_deg = pitch_comp_deg + atan2(h, d)`，否则 h≠0 时系统性打低。
  d=2 / h=2 时错误输出 1.23°，正确值 46.23°——**差 45°**。
- **测试**：`h ∈ {−1.9, −1.0, 0, 1.0, 1.9, 2.0}` × `d ∈ {2,5,8,12,15}` × `v0 ∈ {22,28}`，
  断言与 RK4 差 < 0.25°，实测最大 **0.041°**。
- **教训**：**只测 h=0 等于没测**。所有仰角相关的改动必须扫 h。

### D3 · 装甲几何全部配置化

- `tasks/auto_aim/armor_geometry.hpp`：`small_width=0.135 / big_width=0.230 /
  lightbar_length=0.056 / tilt_deg=15 / outpost_tilt_deg=-15 / thickness=0.01`，
  顺序固定 `[右上, 左上, 左下, 右下]`。
- `tasks/auto_aim/keypoint_check.hpp`：四点有限性 / 凸性 / 对边比 / 长宽比校验，
  由 `sentry.yaml` 的 `reject_bad_keypoints` 控制（**默认 false**）。
- **理由**：2027 大装甲取消、板宽变化，硬编码常量会导致 PnP 距离整体偏 ~70%。

### D4 · 不把 EKF 换成 UKF

见 [`filter_ekf_vs_ukf.md`](archive/filter_ekf_vs_ukf.md)。核心论据：

- `h_jacobian()` 是**解析手推**的，不是数值差分，正常工况下 EKF ≈ UKF（精度差 ~3%）；
- 代价是 **5 倍运行时**；
- 极端工况下 UKF 会收敛到镜像解，反而更差；
- 结论：现在不换；若未来要换，先解决"极端工况观测本身不可信"的问题。

### D5 · R 自适应：`view_penalty = 1/max(|cos(delta_angle)|, 0.5)`

- **问题**：侧视时板在图像上被压扁，单板 PnP 反解的距离与 yaw 迅速病态
  （`σ_d ~ 1/cos`），但原 `r_yaw` 是常数、距离方差只随 `|delta_angle|` 对数微增。
- **floor 取 0.5 而不是 0.25**，依据是**过拟合对照**：

  | 方案 | 各向同性噪声（噪声其实不随侧视恶化） | 物理真实场景 |
  |---|---|---|
  | 固定 R | 67.0 mm | 128.2 mm |
  | `floor=0.25` | **70.7 mm（+5.5%，反而变差）** | 110.7 mm |
  | `floor=0.5` | 68.2 mm（+1.8%） | **115.5 mm** |

  即 `0.25` 是过拟合，`0.5` 是收益/鲁棒折中最优。
- **pitch 方差不动**：pitch 与视角弱相关。

### D6 · 新息门控：`d² = νᵀS⁻¹ν > 18.47` 就丢帧

- `18.47 = χ²(4, 99.9%)`。**不用 9.488（95%）**：95% 会误杀 0.4% 的正常观测，拖慢重捕。
- 8% 观测为粗差时：

  | 配置 | 位置 RMSE | 朝向 RMSE | 拒绝率 |
  |---|---|---|---|
  | 原版 | 1810.7 mm | 7.047° | 0% |
  | **R + 门控** | **104.9 mm** | **0.887°** | 7.6% |

  无粗差时：94.6 → 90.6 mm，**误杀 0%**。
- **Eigen 坑**：`νᵀS⁻¹ν` 是 1×1 表达式，必须显式取 `(0,0)` 再赋给 `double`，
  不能依赖不同 Eigen 版本的隐式转换。

### D7 · 撤销「旋转方向门控」（写错了，已回滚）

曾想用"预测朝向 vs 观测朝向"过滤跳变，但比较的是
`atan2(armor.y, armor.x)`（**视线方位角**）与 `predicted[i][3]`（**装甲板绝对朝向**）——
两个物理量不同，会**误杀有效观测**。`tracker.cpp` 已确认无改动（`git diff` 为空）。
正规做法是 D6 的新息门控。

### D8 · `publish_raw_frame` 默认关

`auto_aim_debug_mpc` 里发布全分辨率 raw 帧会让行程翻倍，
改为 yaml 开关（`sentry.yaml: false`），需要时再开。

### D9 · 2026 参赛材料归档而不是删除

`docs/archive/`（5 份 md + 图，~120 KB）+ `docs/archive/README.md` 说明缘由：
留作答辩/展示与 git 历史引用，但**不再维护**。

### D10 · 开火限流用规则的射击热量机制，不用固定间隔

**决策**：`fire` 的第三道闸是热量，不是 `min_fire_gap`。

依据 2027 比赛规则 5.1.3 + 表 5-16（重装 42mm）：

```
每发 +100 热量，Q0 = 300，冷却 100/秒，10Hz 结算
Q1 > Q0  -> 锁发射机构 + 断图传，直到 Q1 = 0
Q1 >= 500 -> 本局剩余时间全部锁定
```

**为什么不用固定间隔**：固定间隔的"正确值"无法从任何地方推导出来，
而热量给出的约束是**规则本身**——连打 3 发后必须按 100/秒 冷却，
稳态天然约 1 发/秒。而且它防的不只是"打多了"，而是**锁机构**这种
以秒计成本的后果。

**关键实现点**：下位机按 `fire` 的**上升沿**算一发，所以热量只在上升沿记账。
按帧累加会让一个 30 帧的窗口被算成 30 发，3 帧就把 Q1 打满
（上一版 `58e693b` 就是这个错，`c26c10d` 修掉）。

### D11 · "热量预算下的开火时机决策"列为未来方向，暂不实现

**决策**：现在保持"能打就打"；把"挑最优窗口"放到未来，记在
[`heavy_handover.md`](heavy_handover.md) §6.5 F1。

理由（三条，详见该节）：缺真实转速分布数据、行为可预测性更重要、
热量只在高转速下才是瓶颈而高转速下的首要问题是误差不是窗口选择。

**触发条件**：实车 `.rlog` 显示 `heavy_heat_ok=0` 占比 >30%，
或可控转速靶架上的 A/B 证明"挑窗口"确实提升命中率。

---

## 3. 验证状态：什么被证明了，什么没有

| 结论 | 验证方式 | 强度 |
|---|---|---|
| 多项式弹道与 RK4 一致（含 h≠0） | C++ 单元测试，9 工况 × 2 弹速 | **强**（可复现、可回归） |
| 装甲几何配置化不改变行为 | 代码走查 + 参数比对 | 中 |
| `armor_id` ±π 消歧有效 | 逻辑推演 + 现场现象对应 | 中（未实车计数） |
| R 自适应 + 门控有效 | **Python 复刻模型的 Monte Carlo** | **弱——不是实车** |
| EKF 优于 UKF 的性价比 | 单帧/跟踪实测对照 | 中 |
| 崩溃/UB 加固 | 逐条审计 + 代码走查 | 中（无 ASAN 运行） |
| 重装参数（16.5 m/s 等）| 2027 规则手册 | 规则 PDF V1.1.1 是**扫描件无文字层**，数值待复核 |

**共同的缺口**：**所有滤波/控制类改动都没有实车数据背书**。
`heavy_aim_baseline.md` §3 定义了补齐路径：O 层（`rdbg metrics` 看 NIS 越限率 /
`armor_id` 推进 / `w` 过零次数）+ B 层（静态靶弹着点）。

---

## 4. 未闭环风险清单（按优先级）

### ~~R1 · `60f03de` 从未编译~~ ✅ 已闭合（2026-11-14）

用真实依赖（OpenCV 4.12 / Eigen 3.4 / fmt 11.2）编了 `tasks/auto_aim/` 的 9 个 TU：
`target.cpp` / `armor` / `solver` / `tracker` / `voter` / `kalman_markers` / `planner`
零错误，**告警集合与 `60f03de^` 完全一致（9 条既有 `-Wreorder`）**。
`aimer.cpp` / `shooter.cpp` 仅差 `linux/can.h`（Windows 平台事实）。
证据与命令见 [`build_verification_60f03de.md`](build_verification_60f03de.md)。

### R2 · Bad Converge 看门狗~~可能变迟钝~~ **已确认失效** ⬅ 当前最高优先

实车 `.rlog`（`proof_run_1794666076342127058`，123 s）实测：

```
nis mean 0.104 / max 3.46    4 维名义期望是 4  →  S 过估约 38 倍
越限比例 0.0%                看门狗判据是 ≥40%
recent_nis_failures 恒 0
新息门控触发 0 次           （扫全 225 MB rlog，门控消息 0 条）
```

**不是"迟钝"，是永不触发。** 根因是 `R` 的四个分量普遍过估，实测反标定
（"现假设"取自 `configs/sentry.yaml` 实际配置，已与录包内配置核对）：

| 分量 | 现假设 σ | 实测 σ（二阶差分） | 方差过估 |
|---|---|---|---|
| `R[0]` 视线 yaw | **0.071 rad（4.0°）**（`r_yaw: 5e-3`） | ~0.001–0.015 rad | ~24× |
| `R[1]` pitch | **0.045 rad（2.6°）**（`r_pitch: 2e-3`） | ~0.0005 rad | ~10³× |
| `R[2]` 距离 | **1.0–1.6 m**（`log(|Δ|+1)+1`） | **0.05–0.09 m** | **~116–1096×** |
| `R[3]` 板朝向 | **0.46 rad（26°）**（`log(d+1)/200 + 0.2`） | 0.055–0.25 rad（重尾） | ~3–70× |

**主因是 `R[2]` 与 `R[3]`**（不是 `R[0]/R[1]`）：距离差 7 m 以内、
板朝向差一整个板位（90°）在模型里都算"正常观测"，所以门控永远够不到粗差。

**判据**：改 `R` 后重录同场景，`nis mean` 回到 1–6、越限率 ~0.1%，且 `w` 过零仍为 0。

### ~~R3 · `big.csv` 缺 16.5 m/s 档~~ ✅ 已闭合（2026-11-15）

根因是 `ballistic-main/v.py` 的 `dv` 少除 `(N-1)`，末点落在 15.9625。已修正并重生成：
`V∈[13.0, 16.6]`、80 行、步长 0.045570。`solve(16.5, 12m)` 从"夹取到 15.9625 → 14.374°"
变为"插值 → 13.377°"，**修正 0.997° ≈ 209 mm**。与独立 RK4 对拍差 0.015°。
详见 [`../assets/ballistic/README.md`](../assets/ballistic/README.md)。

### R4 · 弹速兜底 / 阻力系数 —— 部分闭合

- ✅ 兜底弹速已可配置：`bullet_speed_default / min / max`（Aimer + Planner），
  缺键保持旧行为；重装 yaml 写 `bullet_speed_default: 16.5` 即可。
- ✅ 夹取不再静默：`BallisticsPoly::solve()` 夹取时打 `v_clamp` 日志（2 s 限流）。
- ✅ 阻力系数按弹种拆常量：`kDragCoefficientSmall=0.01875` / `kDragCoefficientBig=0.00513`。
- ⚠️ **仍未接线**：`Trajectory` 默认 k 恒为 small，回退 RK4 时打 42mm 会用错 k
  （12 m 偏 1.81°≈380 mm）。需要"当前弹种"的进程级设置。

### R5 · `armor.hpp` 的 `armor_properties` 与 2027 冲突

`base/three/four/five` 仍绑到 `ArmorType::big`（230 mm），但 2027 **大装甲已取消**。
若检测端仍吐 `big`，PnP 距离会偏 ~70%。**需与前端确认标签定义**。

### R6 · MPC 参数超物理 10 倍

`sentry.yaml` 的 `max_yaw_acc: 50` / `max_pitch_acc: 100`，
而机械侧 `α_max ≈ 4–6 rad/s²`。后果是规划器给出**物理不可执行**的轨迹，
执行端只能饱和跟随，等效于额外滞后。

### R7 · 执行链断链：加速度被丢弃

`io/gimbal/gimbal.cpp::send()` 里 `(void)yaw_acc; (void)pitch_acc;` —— 
规划器算的加速度**根本没发给电控**，同时 `msg.distance = 3.0f` 是硬编码、`jmp_time = 0`。
这是 [`gimbal_mechanics_and_aiming.md`](archive/gimbal_mechanics_and_aiming.md) 指出的最大单项误差来源。

### R8 · `docs/CONFIG_PARAMETERS_REFERENCE.md` 落后

缺 `ballistics_*`、`publish_raw_frame`、`armor_geometry`、`reject_bad_keypoints`、
`min_valid_dist` 等新键。

### R9 · 2027 规则数值待复核

规则 PDF V1.1.1 是单页扫描件、无文字层，本机无 OCR。命中的数值（16.5 m/s、
装甲 135×56、热量 +100/300）来自可读版本，建议人工对照原件再核一次。

---

## 5. 本机环境与限制

| 项 | 状态 |
|---|---|
| 系统 | Windows + PowerShell（**GBK 控制台**，打印 UTF-8 中文会花屏，但**文件本身没问题**） |
| 可用 | Python 3.14 + numpy 2.4.3 / pymupdf / pypdf；Git |
| **不可用** | **OpenCV、Eigen、yaml-cpp、OpenVINO → 无法编译任何 C++** |
| 因此 | 纯算法测试（如多项式对拍）可以用 Python 复刻验证；涉及库的改动**只能静态检查** |

**可用的静态检查手段**（每次改完 C++ 都做）：
括号配对计数、`<cmath>/<algorithm>` 引入检查、grep 关键符号、
`git diff` 逐行复核。这些**不能替代编译**，只是降低低级错误概率。

**PowerShell 坑**：

- 反引号 `` ` `` 是转义符 → 写文件用 `@'...'@` 单引号 here-string；
- `@"..."@` 双引号 here-string 会展开 `$`，文档里带 `$` 会被吃掉；
- 写文件统一 `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($false)))`；
- `git add -A` 会把临时 commit message 文件也提交进去 → 用 `-F` 后记得重 stage 删除或 `--amend`。

---

## 6. 下一步（严格按顺序）

### ~~第 1 步 · 编译验证~~ ✅ 已做（2026-11-14）

本机用真实依赖编过 9 个 TU，零错误零新增告警；车上仍建议跑一次
`cmake --build build -j` 覆盖 `aimer/shooter/detector`。

### 第 2 步 · 改 `R` 并重录对照 ⬅ **当前阻塞点**

按 R2 的实测值收 `R`（`R[2]` 需从 σ=1.0–1.6 m 收到 0.08 m 量级），然后：

```bash
bash scripts/collect_evidence.sh all -d 60 -r debug   # 编译+自检+录制+指标+打包
```

**判据**：`nis mean` 1–6、越限率 ~0.1%、门控触发 0–0.1%、`w` 过零仍 0、
`armor_id` 每圈折返 ≤0.39。**场景必须与 Before 一致**（4.4 m、~5.28 rad/s）。

### 第 2.5 步 · 重装 `heavy_aim` 上车台架（2026-11-16 新增）

`src/heavy_aim.cpp` 已实现「瞄旋转中心 + 等板转过来开火」，单测已过，**实车未验证**。
上车顺序：

1. **静态板**：板正对，确认 `fire=1`、`heavy_align_err_deg≈0`；
2. **手转靶车**：慢转，看 `heavy_t_to_cross` 是否随转速变化、
   `fire` 是否只在板快压线时置 1；
3. **标定 `launch_delay`**：静态靶打靶，看弹着点是否系统性偏"目标将要转到的一侧"，
   每 0.05 s ≈ `w × 0.05` 的角度；填进 `center_aim.launch_delay`；
4. 标完把实测值回写本文档与 `architecture.md`。

**仍欠两件事**：`configs/heavy.yaml` 未建（radius / `ekf.heavy` / `big.csv` / 16.5 弹速）；
`heavy_aim` 不补偿云台相位滞后（见 architecture.md 已知限制）。

### ~~第 3 步 · 补 `big.csv` 16.5 档~~ ✅ 已做（2026-11-15）

`V∈[13.0, 16.6]`，`solve(16.5,12m)` 修正 209 mm。剩下的只是 `configs/heavy.yaml` 与
`ekf.heavy` 分支。

### 第 3.5 步 · 把 k 接上弹种（R4 遗留）

`Trajectory` 默认 `k` 恒为 17mm 值，回退 RK4 打 42mm 会偏 380 mm。
需要"当前弹种"的进程级设置，与 `install_ballistics_from_config()` 一起初始化。

### 第 4 步 · 执行器辨识（最大单项误差）

`tests/io/gimbal_response_test.cpp` 已有激励信号发生器（三角/正弦注入），
**缺自动拟合**（从响应曲线反解 f_c、阻尼比、纯延迟）。
做完才能给 MPC 加含滞后的模型。

### 第 5 步 · 多帧位姿优化（BA）

解决"距离噪声经弹速放大"的第二大误差（8 m 处 1 px ≈ 0.26 m 距离误差，
经 `∂pitch/∂d ≈ 1.03°/m` 放大到 1.3 mrad）。

---

## 7. 工作约定（本仓库的规矩）

1. **一处改动一个提交**，中文提交信息，格式 `类型: 主题`。
2. **验证有效后再提交**（用户明确要求）。验证方式写进提交信息的正文。
3. 提交信息正文要写清：**动机 → 改法 → 验证数据 → 注意事项**。
   现有提交（尤其 `60f03de`）是这个格式的范本。
4. **文档与代码同步**：改行为就改 `docs/`，并在 `docs/README.md` 索引里加一行。
5. 根目录只留 `readme.md`，其余文档进 `docs/` 或 `docs/archive/`。
6. 不要在没把握时"顺手优化"——先记录到风险清单，单独开提交。

---

## 8. 定位表：要改 X 看哪里

| 想改… | 文件 |
|---|---|
| 检测/后处理 | `tasks/auto_aim/yolo.cpp`、`tasks/auto_aim/detector.cpp` |
| PnP / 关键点校验 | `tasks/auto_aim/solver.cpp`、`keypoint_check.hpp` |
| 装甲尺寸/倾角 | `tasks/auto_aim/armor_geometry.hpp`（+ `configs/*.yaml` 的 `armor_geometry:`）|
| EKF 状态/观测/噪声 | `tasks/auto_aim/target.cpp`（`update_ypda`、`h_jacobian`、`predict`）|
| 关联/航迹/看门狗 | `tasks/auto_aim/tracker.cpp` |
| 弹道 | `tools/trajectory.cpp`（RK4）、`tools/ballistics_poly.cpp`（多项式）|
| 规划/开火门控 | `tasks/auto_aim/planner/`、`tasks/auto_aim/aimer.cpp` |
| 云台协议 | `io/gimbal/gimbal.cpp`（**加速度被丢弃，见 R7**）|
| 调试数据输出 | 各 `src/*.cpp` 的 `plot()` 调用；字段表见 `docs/architecture.md` §12（完整字典在 `docs/archive/pipeline_details.md` 附录 B）|
| 离线评测 | `tests/eval/`、`tools/rdbg/` |

---

## 9. 一页纸速记（给赶时间的人）

```
目标      重装自瞄命中率 ≥75%  ⟺  σ_aim ≤ 24 mm（目标面）
现状      主链路通 + 已编译；弹道 16.5 档已补齐；R 待标定
分支      feat/heavy-aim（只做重装自瞄，打符与无人机/多线程入口已移除）
第一步    ✅ 编译（2026-11-14 本机 9 TU 零错误）
第二步    ✅ big.csv 补到 V∈[13.0,16.6]（12 m 修正 209 mm）
第三步    收 R → 重录同场景 → NIS 回到 1–6            ⬅ 卡在这里
最大坑    R2 看门狗确认失效、R4 回退 k 仍按 17mm、R7 加速度被丢弃、heavy_aim 未上实车
规矩      验证有效再提交，中文提交信息，文档同步索引
```