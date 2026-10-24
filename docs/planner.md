# Planner：MPC 轨迹规划

## 功能简介

Planner 使用 TinyMPC（ADMM 求解器）对云台 yaw/pitch 轨迹做有约束的最优控制。

**输入**：EKF 估计的目标状态（Target）+ 子弹速度  
**输出**：Plan 结构体，包含当前帧应发送给云台的 yaw、pitch、速度、加速度及开火决策

**MPC 解决的核心问题**：
- 轨迹优化：生成满足加速度约束的最优跟踪轨迹
- 约束处理：确保控制指令在云台物理能力范围内
- 射击决策：预测未来时刻，选择最佳开火时机
- 平滑控制：避免剧烈控制量变化

**与传统决策树的区别**：传统方法对静止/匀速/小陀螺分别处理，MPC 在统一优化框架下处理所有运动模式，自然生成平滑过渡轨迹。

---

## 关键常量

| 常量 | 值 | 含义 |
|---|---|---|
| `DT` | 0.01 s | 轨迹时间步长 |
| `HALF_HORIZON` | 50 | 半窗口步数 |
| `HORIZON` | 100 | MPC 总窗口步数（1 秒） |

---

## plan() 执行流程

### 入口：`plan(std::optional<Target>, bullet_speed)`

1. 根据目标角速度（`ekf_x[7]`）选择 `low_speed_delay_time` 或 `high_speed_delay_time`
2. 把 target 预测到 `now + delay_time`
3. 调用 `plan(Target, bullet_speed)`

### 核心：`plan(Target, bullet_speed)`

**Step 0** 子弹速度保底：不在 [10, 25] m/s 范围内则用 22 m/s。

**Step 1** 预测飞行时间：取最近装甲板，用 `tools::Trajectory` 计算弹道，把 target 再往前推 `fly_time`。

**Step 2** 生成参考轨迹：调用 `get_trajectory()`，得到 100 列的 `[yaw_rel, yaw_vel, pitch, pitch_vel]` 矩阵。

**Step 3** MPC 求解 yaw：
- `x0 = traj.col(0)`（轨迹第 0 列，即 t = -50步 时刻的目标角度）
- `Xref = traj` 全窗口
- 调用 `tiny_solve(yaw_solver_)`

**Step 4** MPC 求解 pitch：同上，`x0 = traj.col(0)` 的 pitch 行。

**Step 5** 取输出：
- `plan.yaw = x(0, HALF_HORIZON) + yaw0`（相对角 + 绝对偏置）
- `plan.pitch = -x(0, HALF_HORIZON)`（符号翻转：内部向上为负，发送给云台向上为正）
- `plan.fire`：在 `HALF_HORIZON + 2` 处检查跟踪误差是否小于 `fire_thresh`

---

## get_trajectory() 时间轴

```
调用前 target 已在 t = now + delay + fly_time

predict(-DT * 51)  → target 退回到 t - 51步
predict(DT)        → t - 50步，记为 yaw_pitch_last

循环 i = 0..99：
  predict(DT)      → t - 49步 .. t + 50步，记为 yaw_pitch_next
  中心差分速度 = (yaw_pitch_next - yaw_pitch_last) / (2*DT)
  traj.col(i) = yaw_pitch（当前步）的位置 + 中心差分速度

结果：
  traj.col(0)            → t - 50步（过去）
  traj.col(HALF_HORIZON) → t + 0步（当前时刻）
  traj.col(99)           → t + 49步（未来）
```

**设计意图**：MPC 看到"过去 50 步 + 未来 49 步"的完整轨迹，取 `HALF_HORIZON` 处的控制输出作为当前指令，使输出更平滑。

---

## aim() 弹道计算

对给定时刻的 target，取最近装甲板的 xyz，调用 `tools::Trajectory` 求解抛物线弹道：

```
yaw  = atan2(y, x) + yaw_offset
pitch = -bullet_traj.pitch - pitch_offset   // 向上为负
```

`Trajectory` 内部：取飞行时间较短的解（低仰角弹道）。

---

## MPC 模型

yaw 和 pitch 各自独立，状态 `[angle, vel]`，控制量 `[acc]`，离散积分器：

```
A = [[1, DT], [0, 1]]
B = [[0], [DT]]
```

**优化问题**：

$$\min_{u,x} \sum_{i=0}^{N-1} \left( \|x_i - x_{ref,i}\|_Q^2 + \|u_i\|_R^2 \right)$$

$$\text{s.t.} \quad x_{i+1} = Ax_i + Bu_i, \quad |u_i| \leq a_{max}$$

---

## TinyMPC / ADMM 求解器

TinyMPC 使用 ADMM（交替方向乘子法）将 QP 问题分解为多个简单子问题：

1. **x/u 更新**（无约束 QP，Riccati 递推）
2. **v/z 更新**（投影到约束集，即 clip 操作）
3. **对偶变量更新**（拉格朗日乘子）

**特点**：轻量级、嵌入式友好，求解时间 < 1ms（max_iter=10）。

---

## 符号约定

| 量 | 正方向 |
|---|---|
| `Trajectory.pitch` | 抬头为正（弹道计算结果） |
| `aim()` 返回的 pitch | 向上为负（已取负号） |
| `traj` 中存储的 pitch | 向上为负 |
| `plan.pitch` 发出前 | 再次取负 → 向上为正 |
| `gimbal.send()` 接收的 pitch | 向上为正，内部 `* RAD2DEG` 发送 |

---

## YAML 参数说明

（`configs/standard3.yaml`）

| 参数 | 当前值 | 含义 |
|---|---|---|
| `fire_thresh` | 0.0035 rad | 开火误差阈值（约 0.2°） |
| `max_yaw_acc` | 50 rad/s² | yaw 最大加速度 |
| `max_pitch_acc` | 100 rad/s² | pitch 最大加速度 |
| `Q_yaw` | [9e6, 0] | MPC 状态代价权重 [angle, vel] |
| `R_yaw` | [1] | MPC 控制代价权重 |
| `Q_pitch` | [9e6, 0] | 同上 |
| `R_pitch` | [1] | 同上 |
| `low_speed_delay_time` | 0.04 s | 低速预测延时 |
| `high_speed_delay_time` | 0.023 s | 高速预测延时 |
| `decision_speed` | 7 rad/s | 高低速切换阈值 |
| `pitch_offset` | 0.8° | pitch 机械零点补偿 |
| `yaw_offset` | 0° | yaw 机械零点补偿 |

---

## 调参指南

### 加速度约束（max_yaw_acc / max_pitch_acc）

**测量方法**：
```bash
./build/gimbal_response_test  # 测量云台阶跃响应
```

| 云台类型 | max_yaw_acc | max_pitch_acc |
|---|---|---|
| 高性能 | 80~120 | 120~180 |
| 标准 | 40~60 | 80~100 |
| 低端 | 20~40 | 40~60 |

pitch 通常设为 yaw 的 1.5~2.0 倍（负载更小，响应更快）。

### 代价函数权重（Q / R）

**Q[0]（位置跟踪权重）**：
- 跟踪误差大 → 增大到 1.5e7
- 响应超调 → 减小到 3e6~6e6
- 合理范围：1e5 ~ 1e7

**R（控制平滑权重）**：
- 平滑优先：5~10
- 跟踪优先：0.1~1
- 当前默认：1（平衡点）

**Q/R 比例**：当前 9e6/1 = 9e6，典型范围 1e6~1e7。

### 开火阈值（fire_thresh）

| 距离 | 推荐值 |
|---|---|
| <3m | 0.002~0.003 |
| 3~6m | 0.0035~0.005 |
| >6m | 0.005~0.008 |

### 延迟补偿（delay_time）

总延迟 ≈ 检测延迟（~10ms）+ 传输延迟（~10ms）+ 云台响应（~20ms）= ~40ms。

**测量方法**：用 PlotJuggler 对比 `plan_yaw` 发出时刻与 `gimbal_yaw` 响应时刻。

### 调参流程

**Step 1**：运行 `gimbal_response_test`，测量实际最大加速度，设置 `max_acc = 实测值 × 0.9`

**Step 2**：初始权重 `Q=[9e6,0], R=[1]`，观察跟踪误差

**Step 3**：误差大 → 增大 Q；震荡 → 减小 Q 或增大 R

**Step 4**：调整 `fire_thresh`，在命中率和射频之间取平衡

**Step 5**：测量实际延迟，更新 `delay_time`

### 常见问题

**跟踪误差大**：
- 增大 `max_yaw_acc`（云台能力不足）
- 增大 `Q_yaw[0]`（权重不够）
- 检查 EKF 参数（状态估计不准）

**响应震荡**：
- 增大 `R_yaw`（从 1 增到 5）
- 减小 `Q_yaw[0]`（从 9e6 减到 3e6）

**求解不收敛**（日志出现 "Unsolvable target"）：
- 增大 `max_yaw_acc`（放宽约束）
- 减小 `Q_yaw[0]`（降低跟踪要求）

**求解器参数**：`max_iter = 10`（默认，~0.6ms）；性能紧张时可降到 5（~0.3ms，精度损失约 5%）。
