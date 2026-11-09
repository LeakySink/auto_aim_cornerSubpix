# 2027 重装自瞄：后端改造清单

> 目标：把现有"为步兵调的后端"改造成能调**重装机器人**的后端。
> **前端（检测/训练/关键点样式）不在本文范围**——那部分由别的同学负责，1 月等官方
> 点阵屏幕样式。本文只列后端（弹道、几何、状态估计、控制、开火、配置）需要动的地方。
>
> 配套：`docs/robot_types_and_aiming.md`（兵种/规则）、`docs/gimbal_mechanics_and_aiming.md`
> （机械惯量→电控带宽）、`docs/assumptions.md`（物理假设）。

---

## 0. 一句话：重装不是"参数不同的步兵"，是"形态会变的两台车"

| 维度 | 步兵（现有代码假设） | 重装（2027） |
|---|---|---|
| 弹种 | 17mm，25 m/s 上限 | **42mm，16.5 m/s**（且开局无发射机构） |
| 装甲 | 4×小装甲 135 mm，radius 0.2 | **4×小装甲**（大装甲取消），但带机械臂、**允许遮挡 1 块板**、radius 需重标 |
| 云台 | 轻（J≈0.04），α_max≈75 rad/s²，f_c 15–30 Hz | 装 42mm 后 **J≈0.5–0.9**，α_max≈4 rad/s²，f_c 3–8 Hz |
| 射速 | 高（持续） | **每发 +100 热量 / 上限 300 → 一轮约 3 发** |
| 形态 | 固定 | **两阶段**：开局抓能量单元不开火 → 三级装配/装模块后才有 42mm |
| 兵种识别 | `ArmorName` 无"重装"类 | 需新增类别或复用 `one..five` 并区分 |

**因此后端不是改几个数，而是需要"按形态切换的一整套配置"**。下面按优先级列。

---

## 1. P0：不改就一定算错（弹道与弹速）

### 1.1 弹速兜底是 17mm 的

```cpp
// tasks/auto_aim/planner/planner.cpp:62-63
if (bullet_speed < 10 || bullet_speed > 25) bullet_speed = 22;   // 17mm 兜底 22
// tasks/auto_aim/aimer.cpp:50
if (bullet_speed < 14) bullet_speed = 23;                        // Aimer 又是 23
```

重装 42mm 上限 **16.5 m/s**：
- `planner` 的兜底 22 → **偏 33%**，弹道抬枪量严重不足；
- `aimer` 的 `<14→23` 在 16.5 时不触发，但一旦 CAN 弹速为 0 会拿到 23；
- 两条链路兜底值还不一致（22 vs 23）。

**改法**：把兜底值做成 yaml 键（如 `bullet_speed_default` / `bullet_speed_min/max`），
重装配置写 16.5，步兵写 22；`Aimer` 与 `Planner` 共用同一份。

### 1.2 弹道表用错 / 缺档

```yaml
# configs/sentry.yaml
ballistics_csv: assets/ballistic/small.csv     # 17mm, V∈[12.54, 29.91]
```

- `big.csv`（42mm）只覆盖 **V∈[13.0, 15.9625]**，**重装 16.5 m/s 超出上限** → 会走
  `BallisticsPoly::solve` 的"端点夹取"（`note=弹速高于表上限`），用 15.96 档近似；
- 更糟的是**当前配置指向 small.csv**，重装会拿 17mm 的补偿量表去解 42mm 弹道。

**改法**：
1. 用 `ballistic-main/g.py` 补 `big.csv` 的 **V=16.5** 档（可顺带补 16.0/16.5/17.0 几档
   便于插值），重出表；
2. 重装配置改 `ballistics_csv: assets/ballistic/big.csv`；
3. 用 `tests/algo/ballistics_poly_check.cpp` 加 V=16.5 的对拍用例。

### 1.3 回退 RK4 的阻力系数是 17mm 的

```cpp
// tools/trajectory.hpp
constexpr double kDefaultDragCoefficient = 0.01875;   // 17mm: ρCdS/(2M)
```

42mm 物理值 ≈ **0.00513**（M=43g、d=42.5mm、Cd=0.27），差 **3.7 倍**。
超出拟合网格（d>15 m 或 |h|>2 m）时 RK4 回退会用错阻力 → 远距离系统性偏低。

**改法**：按"当前弹种"选回退 k。最小改动是在 `Trajectory` 构造时由调用方传
`kDefaultDragCoefficient42`，或做成 `ballistics_profile` 配置段带 `fallback_k`；
生产链路优先走 poly 表，回退才用 k。

### 1.4 弹道补偿量的语义（已修，但要确认重装表同样适用）

`ballistic-main` 的 CSV 里 `pitch` 是**相对视线的补偿量**，`solve()` 内部已加
`atan2(h,d)`（见 `docs/architecture.md` C.6.1）。重装的 42mm 弹道弯曲更大
（16.5 m/s 平射 10 m 需抬约 10.3° vs 步兵 4.5°），**补角语义对两张表一致**，
补完 `big.csv` 后必须用 `h≠0` 的用例复核（这正是上次漏掉的坑）。

---

## 2. P0：几何与兵种识别

### 2.1 大装甲已取消，但代码还绑着 230 mm

2027 三份规则手册**全文检索"大装甲"均无结果**（rmuc_C-003 / spec_B-011）：
重装也改用 **4 块小装甲**。而 `tasks/auto_aim/armor.hpp` 的 `armor_properties`：

```cpp
{blue, base, big}, ... {blue, three, big}, {blue, four, big}, {blue, five, big}
```

仍把 `base` 和 `three/four/five` 绑定到 `ArmorType::big`（230 mm）。

**后果**：如果检测器给重装输出 big 类（旧模型习惯），PnP 会用 230 mm 物体点 →
**距离偏 70%**，后面弹道/EKF 全错。

**改法（后端部分）**：
- `armor_geometry` 的 `big_width` 默认改 0.135，或把 `ArmorType::big` 语义收敛为
  "非机器人装甲"；
- `armor_properties` 里 big 条目删除/改 small（**这属于"类别表"，需与前端同学约定**：
  新模型输出里不再有 big 类即可，后端不再依赖它）；
- `Solver::solve` 用 `armor.type` 选物体点——若新模型只输出 small，这条自动正确。

### 2.2 车体 radius / armor_num 要重标

`configs/sentry.yaml` 的 `ekf.normal.radius=0.2, armor_num=4` 是步兵值。
重装带机械臂 + 可拆装 42mm 模块，4 块小装甲的安装半径、板间相位、以及
**允许 1 块板被遮挡**（spec_B-030）都与步兵不同；且 2027 **装甲支撑架图纸整体更换**
（spec_B-026，中心距 95→86）。

**改法**：
- 新增 `ekf.heavy` 配置段（radius / armor_num / p0 独立）；
- `Tracker::set_target` 按 `ArmorName` 选配置——目前只有
  `outpost / base / normal` 三分支（`tracker.cpp:281-284`），需加重装分支；
- 用 `eval_record` 采静态多角度数据重标 radius（这是 W3–W4 之后能做的）。

### 2.3 遮挡板鲁棒性

重装允许 1 块板有条件遮挡（距上边缘 >150 mm 或左右 >170 mm 处）。
`Target::armor_xyza_list()` 假设 N 块板全部可见、`armor_num_` 固定。
若只看到 3 块，关联与相位估计会偏。

**改法（后端）**：空间门控（`tracker.cpp` 已有）要容忍"缺一块板"；
`Target` 的 `armor_num` 与可见板数解耦，或对缺板情况降低该帧观测权重。

---

## 3. P0：控制/开火（机械尺度决定的，见 gimbal_mechanics 文档）

### 3.1 MPC 加速度约束超物理

```yaml
max_yaw_acc: 50        # 步兵合理
max_pitch_acc: 100
```

重装装 42mm 后实测 α_max ≈ **4–6 rad/s²**（`docs/gimbal_mechanics_and_aiming.md` §1.3）。
50/100 超出一个数量级 → MPC 下发不可能实现的轨迹。

**改法**：`max_yaw_acc/max_pitch_acc` 按形态分档；重装装模块后降到 ~5/10 rad/s²。

### 3.2 发射延迟与开火判据

```cpp
// planner.cpp
high_speed_delay_time_: 0.065 / low_speed_delay_time_: 0.055   // 单一常数
fire_thresh_: 0.15 rad
auto shoot_offset_ = 2;   // 硬编码
```

问题（详见机械文档 §2.2 / §2.3）：
- 延迟不含"云台到位时间"（重装 0.2–2 s 量级）；
- `fire_thresh` 比的是 **MPC 内部 traj 与 x 之差**，不是实际云台角与理想瞄点之差；
- 42mm 一轮约 3 发，**开火门控必须极严**，否则浪费每发价值极高的子弹。

**改法**：
- `fire_thresh` 改用 `gs.yaw/pitch` 实测反馈，或把云台滞后模型纳入判据；
- 延迟按形态 + 云台角误差动态计算；
- `shoot_offset_` 也做成可配（重装射速低，判决点应更保守）。

### 3.3 执行链断链（若电控侧也归后端）

`io/gimbal/gimbal.cpp::send()` 里 `(void)yaw_acc; (void)pitch_acc;` 丢弃了 MPC 的
角加速度，`msg.distance=3.0f` 硬编码、`jmp_time=0` 无前馈。
**如果电控串口协议由你们负责，这几处必须一起改**；如果电控是独立模块，至少要把
"重装形态下需要更大的前馈/更长的到位时间"这条需求提给他们。

---

## 4. P1：状态估计（重装特有的动力学）

| 项 | 步兵现状 | 重装需要 |
|---|---|---|
| 角速度 `w` | 小陀螺匀速假设 | 重装**不转小陀螺**（它是抓取/射击平台），`w` 先验应接近 0；`q_yaw_acc_var=100` 过大 |
| 位置过程噪声 | `q_xyz_acc_var=70` | 重装底盘功率仅 50W、质量 36–41kg → 加速慢，应显著调小 |
| `p0` 初值 | 步兵 4 板 p0 | 重装需独立 `p0`（更大 r/h 不确定性） |
| 形态切换 | 无 | 开局→装模块瞬间，云台质心/惯量跳变，`R_gimbal2world` 与 EKF 不需要重置，但**控制参数必须切** |

**改法**：`ekf.heavy` 段给独立的 `q_*` 与 `p0`；`Tracker` 按兵种选择。
另外重装的"两阶段"意味着**同一台车在中途换了动力学**，建议读取裁判系统交互数据
（自定义终端已开放该能力）或由操作手/上位机显式下发形态标志，后端据此切配置。

---

## 5. P1：弹着点集中度（2027 自适应装甲，重装收益最大）

规则：`Λ=η/σ` → `ρ=Λ己/Λ敌` → `A=板面积·ρ²/(2+ρ²)`；
伤害 **未加固区 ×1.5 / 已加固 ×0.5**（`docs/robot_types_and_aiming.md` §3.5）。

重装一轮只有约 3 发 42mm，但每发原始伤害 200（×1.5 未加固 = 300），
**是全队单发价值最高的火力**。因此：

- 自瞄输出不应只是"瞄哪块板"，而应是**板面坐标系内的选点 (u,v)**；
- 需要在后端能读/推断对手加固中心点（赛事引擎交互数据），主动避开加固圆；
- `eval_record` / `rlog_replay` 已能记录 `best_cx/best_cy`，但缺板面归一化与 σ 统计。

**改法**：在 `Aimer::choose_aim_point()` / `Planner::aim()` 输出里增加"板上偏移"；
这属于纯后端，不依赖 1 月视觉样式。

---

## 6. 配置组织：建议的落地方式

不要把所有差异散落在代码里，做成**按兵种分文件 + 共享基线**：

```yaml
# configs/heavy.yaml（新建，或 sentry.yaml 里加 profile 段）
robot_profile: heavy              # 后端据此切换下列所有分支
bullet_speed_default: 16.5
ballistics_csv: assets/ballistic/big.csv
ballistics_x_range: [0.5, 15.0]
ballistics_y_range: [-2.0, 2.0]
ballistics_fallback_k: 0.00513

ekf:
  heavy: { radius: <重标值>, armor_num: 4, p0: [...], q_xyz_acc_var: <更小>, q_yaw_acc_var: <更小> }

max_yaw_acc: 5                   # 装 42mm 后
max_pitch_acc: 10
high_speed_delay_time: 0.2       # 含云台到位（实测后填）
low_speed_delay_time:  0.2
fire_thresh: 0.05                # 更严
shoot_offset: 5
```

代码侧需要改的**集中在这 6 个文件**（不含前端）：

| 文件 | 改什么 |
|---|---|
| `tasks/auto_aim/planner/planner.cpp` | 弹速兜底 yaml 化、`shoot_offset` 可配、开火判据用实测角/动态阈值、延迟按形态 |
| `tasks/auto_aim/aimer.cpp` | 弹速兜底与 Planner 统一 |
| `tools/ballistics_config.cpp` + `tools/trajectory.hpp` | 回退 k 可配（分弹种）；补 `big.csv` 16.5 档 |
| `configs/sentry.yaml`（或新 `heavy.yaml`） | 上面整套 profile |
| `tasks/auto_aim/tracker.cpp` | 按 `ArmorName` 增加重装几何分支 |
| `tasks/auto_aim/armor.hpp` | big 类收敛（与前端约定后） |

---

## 7. 施工顺序（都不依赖 1 月新装甲样式）

1. **补 `big.csv` 的 V=16.5 档** + 对拍用例（本机可验证，零风险）；
2. **弹速/弹道/k 三项配置化**（P0 全部），新建 `heavy.yaml` 走通一遍；
3. **重标 `ekf.heavy.radius`**（用 `eval_record` 采静态多角度数据）；
4. **阶跃响应辨识云台动力学**（`gimbal_mechanics` §6-P1），据此填 `max_*_acc` 与延迟；
5. **板面选点 + σ 统计**（P1，为自适应装甲铺路）；
6. 1 月前端样式确定后，只接 `IDetector` 适配层与类别表，**后端不再动**。

---

## 8. 与前端的分工边界（明确一下）

| 归前端（不归你） | 归后端（本文） |
|---|---|
| 点阵屏幕样式识别、模型重训、关键点定义（4/6 点） | 弹道表 / 弹速 / k / 补角语义 |
| 类别表 `one..five`/编号显示解析 | `ArmorType::big` 收敛、`armor_geometry` |
| 输入尺寸、NMS、置信度阈值 | `ekf.*` 几何/噪声、`Tracker` 分支 |
| 检测框/关键点的可视化 | MPC 约束、延迟、开火判据、形态切换 |
| | 板面选点、σ、自适应装甲对接 |

> 唯一需要两边"约定"的是：**新模型输出里不再有 big 类**（2027 已无大装甲），
> 后端据此把 `ArmorType::big` 收敛掉。其余全部可独立推进。
