# 系统架构总览

> **本文只回答四件事**：系统怎么搭、数据怎么流、改动落在哪、怎么验证。
>
> 深度资料（都不在本文展开）：
> - 逐模块数学推导与字段级 walkthrough → [`archive/pipeline_details.md`](archive/pipeline_details.md)
>   （2026-10 逐行核对版，**行号已过期**，只作参考）
> - EKF → [`archive/ekf_model.md`](archive/ekf_model.md)；规划器 → [`archive/planner.md`](archive/planner.md)
> - 检测 → [`archive/detector.md`](archive/detector.md)；坐标系 → [`archive/coordinate_systems.md`](archive/coordinate_systems.md)
> - 参数速查 → [`archive/CONFIG_PARAMETERS_REFERENCE.md`](archive/CONFIG_PARAMETERS_REFERENCE.md)
> - 弹道表怎么生成 → [`../assets/ballistic/README.md`](../assets/ballistic/README.md)

---

## 1. 项目定位

RoboMaster 视觉自瞄后端（2027 赛季**重装**为主）。相机 → 装甲板检测 → PnP 位姿 →
EKF 目标估计 → 弹道解算 → 云台指令 → 开火判定，单进程、无 ROS 依赖。

- **目标**：重装命中率 ≥75% ⟺ 瞄准误差 σ ≤ 24 mm（见 [`heavy_aim_baseline.md`](heavy_aim_baseline.md)）
- **不依赖 ROS 消息总线**：ROS2 只用于哨兵导航通信，核心自瞄不需要
- **单入口按云台模式路由**：一个可执行文件同时管自瞄/全向感知

---

## 2. 四层架构

```
┌──────────────────────────────────────────────────────┐
│ src/     应用层：每种机器人一个入口，负责编排与模式路由  │
├──────────────────────────────────────────────────────┤
│ tasks/   任务层：auto_aim（检测/解算/跟踪/规划）/ 全景感知 │
├──────────────────────────────────────────────────────┤
│ tools/   工具层：EKF、弹道、日志、数学、YAML             │
├──────────────────────────────────────────────────────┤
│ io/      硬件层：相机（海康/迈德威视/USB）、云台串口/CAN  │
└──────────────────────────────────────────────────────┘
```

三条设计约束：**配置驱动**（换机器人只改 yaml）、**算法与硬件解耦**（`io` 统一接口）、
**手写算法不用第三方大库**（EKF/MPC 都是自己实现的，便于按需改）。

---

## 3. 数据流与延迟

```
相机线程 ──帧+时间戳──▶ 检测(YOLO) ──装甲板角点──▶ Solver(PnP+坐标变换)
                                                        │
云台线程 ──四元数（插值到曝光时刻）──────────────────▶ Tracker(关联+EKF)
                                                        │
                                          Aimer/Planner（弹道+提前量/MPC）
                                                        │
                                             Gimbal::send（串口/CAN）
```

| 环节 | 量级 | 补偿方式 |
|---|---|---|
| 采集 → 检测完成 | ~10 ms | — |
| 检测 → 云台执行 | ~5 ms | — |
| **总延迟** | **~15 ms** | `low_speed_delay_time` / `high_speed_delay_time`（按目标角速度分档） |

> 延迟补偿只是"把目标预测到未来"，**没有分解到各段**。真正标定要分段测（见
> `heavy_aim_baseline.md` 的 M5）。

---

## 4. 线程模型

| 入口 | 线程 | 通信 |
|---|---|---|
| `standard_mpc` / `auto_aim_debug_mpc` | 主线程（检测+跟踪）+ `plan_thread`（持续 plan/plot） | `ThreadSafeQueue<optional<Target>, true>`，队列满丢旧值 |
| `heavy_aim` | **单线程**（本项目默认） | 无队列 |
| `sentry_multithread` | 主线程 + 检测线程 | `ThreadSafeQueue`，`track(detection_queue, ...)` 重载 |
| 所有入口 | 云台读线程（在 `io::Gimbal` 内部） | 四元数队列 + mutex |

**共享数据必须加锁**：`armors` 列表由主线程整体赋值、plan 线程遍历，历史上这里出过
UAF（悬垂迭代器）。现在的写法是"局部算完 → 锁内 move"。

---

## 5. 入口（`src/*.cpp`，共 9 个，名字即文件名）

`cmake/auto_executables.cmake` 会把 `src/` 下每个 `.cpp` 编成同名可执行文件，
产物在 `build/src/`。

| 可执行文件 | 硬件 | 控制 | 用途 |
|---|---|---|---|
| `heavy_aim` | `Gimbal` + `Camera` | `CenterAimer` | **重装主力**：瞄中心 + 等板转过来开火（见 §6），不走 MPC |
| `auto_aim_debug_mpc` | `Gimbal` + `Camera` | `Planner`（MPC） | **全链路调试**：多一个 plot 线程，信息最全 |
| `standard_mpc` | `Gimbal` + `Camera` | `Planner`（MPC） | 步兵实战版（去 plot、检测开窗） |
| `standard` | `CBoard`(CAN) + `Camera` | `Aimer` + `CommandGener` | 单线程步兵版，弹速走 CAN 真值 |
| `sentry` / `sentry_bp` / `sentry_debug` / `sentry_multithread` | `CBoard` + 前/后视相机 | `Aimer` + `Decider` | 哨兵（全向感知）；**只在 `USE_ROS2=ON` 时生成** |
| `eval_record` | `Camera` + `Gimbal` | — | 全分辨率采 `.rlog`（重训/离线评估） |

> **2026-11 移除**：打符子系统（`tasks/auto_buff/`、两个 `auto_buff_debug*` 入口、
> 专用模型）与 `uav` / `uav_debug` / `mt_standard` / `mt_auto_aim_debug` 入口。
> `GimbalMode` 只剩 `IDLE` / `AUTO_AIM`；下位机若仍上报 mode 2/3，按 `IDLE` 处理（不接管云台）。

**想读懂自瞄怎么跑**：读 `heavy_aim.cpp`（156 行）与 `auto_aim_debug_mpc.cpp`（228 行）就够。

---

## 6. 重装策略：瞄中心 + 等板转过来（`heavy_aim`）

2026-11 组会定的目标：**枪口始终压在对面车的旋转中心，程序判断"此刻发射能正好打到
转过来的那块板"时 `fire=1`。**

### 为什么不逐板瞄准

4 板目标在 45° 处两块板同时可见，逐板瞄准会让云台在两者之间来回跳；重装云台惯量大，
跳变的代价远大于把枪口稳定在一个点上。**中心是唯一稳定点。**

### 为什么瞄中心也能打中

板心到旋转中心的距离就是 `r`（0.2~0.3 m）。子弹沿"云台→中心"这条线飞，
只要**弹丸到达的那一刻**有板正好压在这条线上，就命中板面。所以判据不是
"现在瞄得准不准"，而是"**到达时刻**板在不在线上"。

### 每个控制周期的时序（`tasks/auto_aim/center_aimer.cpp`）

预测时刻 = **`launch_delay` + `fly_time`**（前者是"判定→出膛"的延迟）：

1. 迭代求 `fly_time`（距离↔飞行时间互相依赖，2~3 次收敛）；
2. 把 EKF 状态外推到**弹丸落点时刻** `t + launch_delay + fly_time`，得到那时的中心与各板位置；
3. 瞄准点 = 外推后的中心，`yaw = atan2(y, x)`，`pitch = -(弹道抬角 + offset)`；
4. 逐板算到达时刻该板相对瞄准线的 yaw/pitch 偏差，与**板面角宽**比较：
   `角宽 = 2·atan2(板宽·(1-margin)/2, 距离)`，默认 `margin=0.30`（只用板面中间 70%）；
5. 任一板 yaw 与 pitch 都落在窗口内，且云台已到位（`gimbal_tol_deg`）→ `fire=1`。

> **连发节奏不在这里管**：`fire` 是"此刻开火能命中"的判定，板扫过窗口期间会持续为真
> （1 rad/s 下约 0.36 s）。最小开火间隔由**电控**处理，本模块不设冷却。

### 参数（`configs/*.yaml`）

```yaml
center_aim:
  hit_margin: 0.30        # 只用板面中间 70% 判定，吸收 PnP/超前量误差
  gimbal_tol_deg: 0.5     # 云台到位判据
  require_settled: true   # 没到位不开火
  max_iter: 6             # fly_time 迭代次数
  launch_delay: 0.0       # 判定→出膛总延迟 [s]，实测标定后填
```

**`launch_delay` 怎么标定**：目标静止、板正对，用高速相机（或慢动作手机）拍
"视觉给出 fire → 枪口出弹"的间隔；也可用静态靶打靶反推——先填 0，看弹着点是否系统性偏
"目标将要转到的一侧"，每 0.05 s ≈ `w × 0.05` 的角度。
**漏掉它的后果**：0.05 s 在 1 rad/s 下就是 2.9°，远大于 1.08° 的命中窗口。

### 已知限制（实车前必须知道）

- **窗口很窄**：5 m、板宽 135 mm（打七折 94.5 mm）→ 角宽 ≈1.08°，半宽 0.54°。
  1 rad/s 时板扫过窗口约 0.36 s；**5 rad/s 时只有 ~70 ms**。
- **只瞄中心 ⇒ 打不到上下错位的板**：`h≠0` 的板在 pitch 方向超出窗口会被自动排除，
  等效只剩 2 块板（每 180° 一次窗口）。
- **没补偿云台相位滞后**：`launch_delay` 只覆盖"判定→出膛"；云台从收到指令到到位
  仍有一段滞后。实测 >20 ms 时要么在指令角上加超前，要么改回 MPC + 同一判据。
- **`fire` 是"此刻能命中"的持续信号**，限流由电控负责。

### 单元测试

```bash
./build/tests/center_aimer_check        # 不需要硬件
```

6 节断言：瞄准角严格指向中心；`fly_time` 量级正确（5 m/16.5 m/s ≈0.319 s）；
fire 必须落在窗口内；开火事件数 ≈ 每 90° 一次（`h≠0` 时每 180°）；
竖直超窗口的板不被选中；云台偏 5° 不开火；`total_time - fly_time` 精确等于 `launch_delay`。

---

## 7. 关键模块速查

| 模块 | 文件 | 关键点 |
|---|---|---|
| 检测 | `tasks/auto_aim/yolo.cpp`、`yolos/*`、`detector.cpp` | YOLO11/v5/v8/TUP + 传统灯条；输出 4 角点 |
| 关键点校验 | `keypoint_check.hpp` | 对边比/夹角异常直接拒，防"几个像素的板"解出离谱位姿 |
| 解算 | `solver.cpp` | IPPE PnP、±π 朝向消歧、相机系→云台系→世界系；失败返回 `false` |
| 跟踪 | `tracker.cpp` | 状态机 + 关联空间门控 + Bad Converge 看门狗 |
| 估计 | `target.cpp` + `tools/extended_kalman_filter.cpp` | 11 维 EKF：`x vx y vy z vz a w r l h` |
| 瞄准（传统） | `aimer.cpp` | 选板 + 提前量不动点迭代 + 弹道 |
| 规划 | `planner/planner.cpp` | TinyMPC（双积分 + 加速度约束），输出加速度受限的指令 |
| 开火（传统） | `shooter.cpp` | 角度容差 + 指令突变门控 |
| 重装 | `center_aimer.cpp` | 见 §6 |
| 日志/回放 | `tools/rdbg/`、`host/rdbg/` | `.rlog`、`python -m rdbg metrics/dump` |

---

## 8. 弹道

三条路径，按优先级：

1. **多项式表（首选）**：`assets/ballistic/{small,big}.csv`，每个弹速一组 10 系数，
   运行时按弹速**线性插值**；超出 `ballistics_x_range / y_range` 直接返回 false，**不外推**。
2. **RK4 回退**：表不可用或超范围时，用编译期阻力系数
   `kDragCoefficientSmall = 0.01875`（17mm）/ `kDragCoefficientBig = 0.00513`（42mm）。
   ⚠️ **目前回退路径恒用 small 的 k**，打 42mm 会偏（12 m 约 380 mm）——见 R4。
3. **无阻力解析解**：`k = 0`，仅测试对照。

**补角语义（最容易搞错的一处）**：CSV 的 `pitch` 列是**相对视线的抬高补偿量**，
不是绝对仰角（`ballistic-main/g.py`：`Pitch = degrees(solved_pitch − atan2(y,x))`）。
所以 `BallisticsPoly::solve()` 内部必须再加 `atan2(h, d)`；漏加时 **h=0 完全看不出来**，
但 h≠0 会系统性打低（d=2/h=2 应 46.23°，漏加只给 1.23°）。

**弹速超表范围**：按端点**夹取**（不是失败），并打 `v_clamp` 日志（2 s 限流）。
`big.csv` 已补到 `V∈[13.0, 16.6]`，16.5 可用；`small.csv` 是 `[12.54, 29.91]`。
`tests/algo/ballistics_poly_check.cpp` §7 会断言表上限 ≥16.5。

细节公式（RK4 的 4 元 ODE、牛顿迭代、poly 与 RK4 的实测对照表）见
[`archive/pipeline_details.md`](archive/pipeline_details.md) 的 C.6 / C.6.1。

---

## 9. 脏数据护栏

远距离低置信度框只有几个像素，4 角点噪声会让 PnP 解出几十米甚至 NaN 的位姿；
`r/l/h` 在 Q 里没有过程噪声，被污染后很难自恢复。因此进 EKF 前有三道闸：

| 位置 | 加固 | 解决什么 |
|---|---|---|
| `Solver::solve()` → `bool` | 校验深度 z>0、位姿有限、距离 ∈ [`min_valid_dist`, `max_valid_dist`]；不合格清零并 WARN | 离谱 PnP 不进滤波 |
| `Tracker::set_target/update_target` | `solve()` 失败即丢弃该板；全被拒按"未找到"处理 | 不用脏观测初始化/更新 |
| `Target::update()` | `allFinite()` 守卫，NaN/inf 观测跳过本帧 | 所有更新路径的共同收口 |
| `Planner::aim/plan` | 跳过非有限装甲板、无可用板早退、日志限流 2 s | 消除 UB 与刷屏 |
| `Tracker` 关联 | 空间门控：观测须落在预测板附近（`1.0 + 0.2·dist`） | 防"同类同型第二块板"串目标 |
| `Tracker` 看门狗 | 失败**比例** ≥40%（样本 ≥30），窗口 100 | 原判据几乎永不触发 |
| `Planner` 预测板过滤 | 丢弃 `dist < min_valid_dist`（缺省 0.5 m）的预测板 | 防"幽灵板 d=0.30 m"劫持弹道 |

> `Solver::solve()` 变成 `bool` 后，**忽略返回值的调试调用点会静默退化**（画出 0/0/0）。
> 新增调用点请检查返回值。

---

## 10. 硬件与配置

| 相机 | 驱动 | 说明 |
|---|---|---|
| 海康 HikRobot | `io/hikrobot/` | SDK；`libMvCameraControl.so` 需在 `LD_LIBRARY_PATH` |
| 迈德威视 MindVision | `io/mindvision/` | SDK；同上 |
| USB 相机 | `io/usbcamera/` | OpenCV VideoCapture（调试用） |

| 云台 | 驱动 | 说明 |
|---|---|---|
| 虚拟串口（新协议） | `io/serial/` + `io/gimbal/` | 当前主用；**不下发加速度**（见 R7） |
| USB2CAN（旧协议） | SocketCAN | 历史 |

配置都在 `configs/`：`sentry.yaml`（哨兵/默认）、`standard3.yaml`/`standard4.yaml`（步兵）、
`ascento.yaml`/`demo.yaml`/`mvs.yaml`/`example.yaml`、`camera.yaml`（后视）、`tf_pub.yaml`。
换机器人只改 yaml，键的语义见 [`archive/CONFIG_PARAMETERS_REFERENCE.md`](archive/CONFIG_PARAMETERS_REFERENCE.md)。

---

## 11. 构建、运行、评测

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ls build/src/heavy_aim build/src/eval_record build/tests/rlog_replay   # 产物在子目录
```

```bash
./build/src/heavy_aim configs/sentry.yaml            # 重装主力
./build/src/auto_aim_debug_mpc configs/sentry.yaml   # 全链路调试
./build/src/eval_record configs/sentry.yaml --out logs/eval_xxx.rlog   # 全分辨率采集
./build/tests/ballistics_poly_check                  # 弹道表自检
```

**采样 + 打包一条命令**（编译→自检→录制→指标→证明包）：

```bash
bash scripts/collect_evidence.sh all -d 60 -r debug
```

离线分析：

```bash
./build/tests/rlog_replay configs/sentry.yaml logs/run_xxx.rlog --out replay.csv --tracker
cd host && python -m rdbg metrics ../logs/run_xxx.rlog     # 逐帧质量报告
```

> Windows 上也能编大部分 TU：用 MSYS2 ucrt64 的 g++ + 真实依赖头文件，
> 详见 [`build_verification_60f03de.md`](build_verification_60f03de.md) §1。

---

## 12. 调试：`RemoteLogger::plot(data)` 字段

上机对着曲线看，比翻日志快。`auto_aim_debug_mpc` 写的字段（`heavy_aim` 额外有 `heavy_*`）：

| key | 单位 | 含义 / 坑 |
|---|---|---|
| `t` | s | 运行时长（不是帧时间戳） |
| `gimbal_yaw` / `gimbal_pitch` | rad | **抬头为负**；`*_vel` 恒 0（新协议不给） |
| `plan_yaw` | rad | MPC 第 50 步（0.5 s 后）的**绝对** yaw |
| `plan_pitch` | rad | `-x(0,50)`，抬头为负；`plan_pitch_vel` **未取负**（历史遗留） |
| `yaw_error` | rad | `limit_rad(plan.yaw0 − gimbal.yaw)` |
| `pitch_error` | rad | `plan.pitch0 + gimbal.pitch`（**加号**，两侧抬头符号相反） |
| `yp_error` / `fire` | rad / 0-1 | 与 `fire_thresh`（yaml，各配置不同）比较 |
| `target_x/vx` `target_y/vy` `target_z/vz` | m, m/s | `ekf_x()[0..5]`：**x/y 水平、z 竖直**（源码注释里的 `//z` 是错的） |
| `target_yaw` / `w` | rad / rad/s | `ekf_x()[6]` 目标相位、`[7]` 角速度 |
| `armor_id` | 0…n−1 | 最近一次观测命中的板号（调试用） |

**EKF 诊断**（由 `extended_kalman_filter.cpp` 自己发）：`nis`、`nis_threshold`、
`nees`、`nees_threshold`、`recent_nis_failures`。
4 维观测下 `nis` 的名义均值是 **4**；远小于 4 说明 `R` 过松（见
[`context_handoff.md`](context_handoff.md) 的 R2）。

完整字段字典（含每个字段的写入位置）见
[`archive/pipeline_details.md`](archive/pipeline_details.md) 附录 B。

---

## 13. 兜底与坑（保留的必要记忆）

1. **弹速兜底**：`Gimbal::state().bullet_speed` 在新串口协议下**恒为 0**，
   走 `planner/aimer` 的 yaml 兜底（`bullet_speed_default/min/max`，缺键 22/10/25）。
   重装务必写 `bullet_speed_default: 16.5`，否则按 17mm 时代的值解算。
2. **pitch 符号只反了一半**：下发 `plan.pitch` 取负，但 `plan.pitch_vel/acc` 没取负
   （新协议也不下发 acc），所以 `pitch_error` 用加号。
3. **开火有两套判定**：入口里的 `fire`（`hypot(yaw_err,pitch_err) < fire_thresh`）
   与 `Planner::plan.fire`（比对参考轨迹与解算轨迹）**不是一回事**，JSON 里是前者。
4. **构建产物在子目录**：`build/src`、`build/tests`、`build/calibration`（仓库没设
   `RUNTIME_OUTPUT_DIRECTORY`）。
