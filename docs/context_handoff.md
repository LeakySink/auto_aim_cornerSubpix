# 上下文交接 · 视觉自瞄后端

> **生成时间**：2026-11-14
> **仓库**：`D:\my\ckyf_vision\auto_aim_cornerSubpix-main`
> **分支**：`feat/heavy-aim`（= `origin/feat/heavy-aim`，HEAD `60f03de`）
>
> 本文件回答三个问题：**这个仓库经历了什么、现在站在哪、下一步做什么**。
> 它是**会话级**交接，记录的是「决策 + 证据」；
> 长期研发计划在 [`heavy_handover.md`](heavy_handover.md)，两者互补，不重复。

---

## 0. 三行状态

1. **主链路可用**：YOLO → 关键点语义校验 → PnP → 11 维 EKF → MPC/Planner → 云台，
   已在**步兵量级**跑通，弹道也接进了运行时。
2. **重装专属配置尚未落地**：弹速兜底仍是 22 m/s、`assets/ballistic/big.csv` 缺 16.5 档、
   没有 `configs/heavy.yaml`、`armor.hpp` 里 `big` 仍是 230 mm（2027 已取消大装甲）。
3. **最近一次代码改动没有编译过**：`60f03de`（R 自适应 + 新息门控）只有
   Python 复刻模型的 Monte Carlo 验证，**本机没有 OpenCV/Eigen/yaml-cpp，C++ 一行都没编**。
   接手第一件事就是把它编出来。

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

见 [`filter_ekf_vs_ukf.md`](filter_ekf_vs_ukf.md)。核心论据：

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

### R1 · `60f03de` 从未编译 ⬅ **最高优先**

改动只有 47 增 3 删，但涉及 `<algorithm>` 引入、`std::max`、Eigen 表达式。
可能的编译问题：`std::max(cos_view, 0.5)` 的 `double/double` 没问题，
但 `Eigen::VectorXd` 初始化列表里混了 `int` 与 `double` 会告警。
**判据**：`cmake -B build && cmake --build build -j` 零错误。

### R2 · Bad Converge 看门狗可能变迟钝

R 变大 → NIS 变小。物理真实场景 NIS 越限率从 **3.5% → 1.0%**。
看门狗判据是"越限比例 ≥40%、样本 ≥30"。
门控与看门狗用的是同族统计量，**要一起调**。
**判据**：故意喂一段跳变目标，看门狗仍能触发。

### R3 · `big.csv` 缺 16.5 m/s 档

现有 `V ∈ [13.0, 15.9625]`。重装打 16.5 会走 `out_of_grid` → 回退 RK4 → 阻力系数是 17mm 的
→ 弹道偏高。**判据**：补齐后 `eval(16.5, …)` 不返回 false。

### R4 · 弹速兜底 22 m/s / 阻力系数硬编码

三处都要改成可配置：`ballistic-main` 兜底弹速、`kDefaultDragCoefficient`、回退 RK4 的 `k`。
重装应为 16.5 m/s + `k≈0.00513`。

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
这是 [`gimbal_mechanics_and_aiming.md`](gimbal_mechanics_and_aiming.md) 指出的最大单项误差来源。

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

### 第 1 步 · 编译验证（阻塞一切）

```bash
cd auto_aim_cornerSubpix-main
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**判据**：零错误、零新增告警。若有错，改动应局限于 `tasks/auto_aim/target.cpp`
（47 增 3 删），修复后 `git commit --amend` 保持单提交。

### 第 2 步 · 离线回放对照（同一段 `.rlog`）

```bash
./build/rlog_replay <config> <record.rlog>    # 重跑检测
./build/rdbg metrics <record.rlog>
```

对比 `60f03de` 前后：NIS 越限率、`armor_id` 推进次数、`w` 过零次数。
**判据**：`armor_id` 推进不退化；NIS 越限率变化在 R2 的预期内（3.5% → ~1%）。

### 第 3 步 · 重装 M1（参数体系）

按 [`heavy_robot_backend.md`](heavy_robot_backend.md) 的 P0 顺序：
补 `big.csv` 的 16.5 档 → 弹速/阻力系数可配置 → `configs/heavy.yaml` → `ekf.heavy` + Tracker 分支。

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
| 调试数据输出 | 各 `src/*.cpp` 的 `plot()` 调用；字段字典见 `docs/architecture.md` 附录 B |
| 离线评测 | `tests/eval/`、`tools/rdbg/` |

---

## 9. 一页纸速记（给赶时间的人）

```
目标      重装自瞄命中率 ≥75%  ⟺  σ_aim ≤ 24 mm（目标面）
现状      主链路通，重装参数未落地，最后一次改动未编译
分支      feat/heavy-aim @ 60f03de
第一步    把 60f03de 编出来
第二步    同一段 .rlog 跑 rdbg metrics 对比 NIS / armor_id / w
最大坑    R2 看门狗迟钝、R3 big.csv 缺 16.5、R7 加速度被丢弃
规矩      验证有效再提交，中文提交信息，文档同步索引
```