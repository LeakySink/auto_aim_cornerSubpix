# 旋转步兵被识别成前哨站 —— 问题定位

> 针对 `src/auto_aim_debug_mpc.cpp` 这条主链路（YOLO11 + Tracker + Planner）。
> 日期：2026-10-20

---

## 0. 结论速览

**`auto_aim_debug_mpc.cpp` 里没有任何前哨站判定代码**（`grep outpost` 零命中）。
前哨站的身份完全由神经网络的 `class_id` 查表决定，**代码里不存在任何基于几何的二次校验**。
所以「旋转步兵被判成前哨站」本质上是一个**纯分类错误**，而代码里所有本可以兜住它的关卡都失效了。

按严重性排序的 7 个可疑点：

| # | 位置 | 性质 | 判断 |
|---|---|---|---|
| 1 | `armor.hpp:94` + `tracker.cpp:82` | **确定 bug** | `Armor::priority` 在 auto_aim 链路里**从未被赋值**，却用于 `std::list::sort` → 未定义行为 → `armors.front()` 不确定 |
| 2 | `tracker.cpp:65-70` | **确定缺陷** | 唯一用几何（27.5° vs −15° 重投影误差）区分前哨站的代码**被整段注释** |
| 3 | `yolo11.cpp:126` | 设计缺陷 | 分类取 argmax，**不检查 top-1 与 top-2 的间隔** |
| 4 | `tracker.cpp:252-267` | 设计缺陷 | `set_target` 用**单帧** `front().name` 定终身，`min_detect_count` 仅 5 帧 |
| 5 | `tracker.cpp:283-293` | 设计缺陷 | `update_target` **只按 name 匹配**，不检查装甲板是否属于该目标 |
| 6 | `yolo11.cpp:195-206` | 失效校验 | `check_type` 对 `outpost + small` **恒为 true** |
| 7 | `target.cpp:132` | 伤害放大器 | 一旦判成 outpost 且收敛，转速被**钳到 ±2.51 rad/s** |

---

## 1. 判定链路：一块装甲板要成为「前哨站」需要连过 5 关

```
yolo11.cpp:119   scores = output.row(r).colRange(4, 4+38)
yolo11.cpp:126   cv::minMaxLoc(scores) → max_point.x  =  class_id
armor.cpp:87     armor_properties[class_id] → (color, name, type)
                 索引 18 / 19 / 20 = (blue/red/extinguish, outpost, small)
yolo11.cpp:165   check_name : name != not_armor && confidence > 0.8
yolo11.cpp:170   check_type : small 时只禁 one / base
tracker.cpp:65   【几何校验 —— 已注释】
tracker.cpp:256  set_target : armors.front().name  →  target_.name
```

**关键事实：`armor_properties` 共 38 项，outpost 只出现在索引 18/19/20，且全部是 `small`。**
步兵的 three/four/five 也都有 small 项（索引 9-17）。两者在 `type` 上完全无法区分。

### 逐关拦截能力

1. **argmax** —— 取最大得分的列号，没有 margin 门槛。网络「犹豫」时（0.81 vs 0.79）照样硬判。
2. **check_name** —— `confidence > min_confidence`（yaml: 0.8）。只卡绝对值，不卡分布形状。
3. **check_type** —— `(type == small) ? (name != one && name != base) : (...)`。
   因为表里 outpost 只有 small，这条对 outpost **恒为 true**，等于没校验。
4. **几何校验** —— `tracker.cpp:65-70` 整段被注释。这是**唯一**用 pitch（27.5° vs −15°）
   区分前哨站与步兵装甲板的判据，也是唯一用到了「前哨站装甲板是向下倾斜的」这一物理事实的地方。
5. **set_target** —— 单帧决定。`min_detect_count: 5` 只要求「连续 5 帧检测到」，
   `update_target` 里 name 必须匹配才计入，所以有隐式一致性检查，但 5 帧太短。

---

## 2. 为什么偏偏是「旋转」的步兵

旋转（小陀螺）从四个方向同时放大了误判概率，这不是巧合：

| 机制 | 说明 |
|---|---|
| **大角度侧视** | 装甲板从正面转到侧面，投影宽度从 135 mm 压到接近 0，图案严重形变，与远处倾斜的前哨站装甲板相似 |
| **运动模糊** | 高速旋转 + 曝光时间，图像特征退化，分类置信度分布的 top-1 优势被削平 |
| **可见窗口短** | ω=2 rad/s 时每块板约 23 帧可见；而锁定目标只需 5 帧 —— **一次持续 5 帧的分类漂移就足以建立错误目标** |
| **small / big 摇摆** | three/four/five 在表里**各有 small 与 big 两类**（索引 9-17 与 29-37）。旋转导致透视宽度变化，网络可能在 small/big 间跳变，而 `update_target` 要求 `type == target_.armor_type`，type 一变就 `found=false` |
| **多板同框** | 旋转时侧面常同时出现 2 块板，画面里装甲板数量增多，被 `front()` 选中误分类那块的概率上升 |

---

## 3. 后果量化（真实 11 维 EKF 蒙特卡洛）

仿真程序：`verification/outpost_misid/misid_impact.cpp`
（真值步兵 4 块板 / r=0.20 m / 距离 5 m / 60 fps / 弹丸飞行时间 0.20 s，观测按真值 4 装甲板生成）

| 真值转速 | 配置 | E[NIS] | P(>9.488) | **提前量滞后角** |
|---|---|---|---|---|
| 2 rad/s | 正确 n=4, r=0.20 | 3.94 | 4.89% | **2.15°** |
| 2 rad/s | 误判 n=3, r=0.2765 + 钳位 | 4.42 | 7.83% | **5.90°** |
| 5 rad/s | 正确 | 3.70 | 3.72% | **2.19°** |
| 5 rad/s | 误判 | 4.93 | 9.94% | **27.69°** |
| 10 rad/s | 正确 | 3.74 | 4.83% | **2.26°** |
| 10 rad/s | 误判 | 7.81 | 32.06% | **84.04°** |

### 三条重要推论

**（1）转速钳位是最致命的一环。**
`target.cpp:132`：

```cpp
if (this->convergened() && this->name == ArmorName::outpost && std::abs(this->ekf_.x[7]) > 2)
  this->ekf_.x[7] = this->ekf_.x[7] > 0 ? 2.51 : -2.51;
```

前哨站是静止目标，这个钳位本身合理。但步兵小陀螺真值 ω 常达 5~10 rad/s，
一旦被误判，收敛后（update_count_ > 10）角速度被强行压到 2.51，**提前量算不出来**。

滞后角换算成弧长（× r = 0.2 m）：
- 5 rad/s → 27.69° ≈ **9.7 cm**
- 10 rad/s → 84.04° ≈ **29.3 cm**

装甲板宽仅 13.5 cm。**5 rad/s 时瞄准点已经偏出板边，必然脱靶。**

**（2）卡方检验发现不了这个误判。**
E[NIS] 只从 3.9 升到 4.4（ω=2）或 7.8（ω=10），远低于 40% 的 Bad Converge 触发线。
原因是 EKF 有 11 个自由度，`x[6]`(yaw) / `x[8]`(r) / `x[9]`(l) / `x[10]`(h) 会自由漂移去吸收
armor_num 错配——实测收敛后 `r` 跑到 **0.2754**（真值 0.20），模型自洽了。

**推论：你不会在日志里看到 "Bad Converge"，只会表现为「锁上了但打不中」。**

**（3）armor_num 错配本身伤害有限。**
单独把 n 从 4 改成 3、r 从 0.20 改成 0.2765（不钳位），
E[NIS] 仅从 3.897 升到 4.582，P(>9.488) 从 4.35% 升到 8.84%
（`verification/outpost_misid/armornum_mismatch.cpp`）。
**真正的杀手是第 (1) 条的转速钳位，不是几何错配。**

---

## 4. 排查清单（按成本从低到高）

### 步骤 1：肉眼确认（5 分钟）
`src/auto_aim_debug_mpc.cpp:46` 把 debug 打开：

```cpp
auto_aim::YOLO yolo(config_path, false);   // → true
```

会弹出 `detection` 窗口，每张装甲板标注 `置信度 颜色 名称 类型`（`yolo11.cpp:249-253`）。
直接看旋转时标签是否在 `three/four/five` 与 `outpost` 之间跳。

### 步骤 2：打印 top-2 间隔（15 分钟）
在 `yolo11.cpp:126` 后面加：

```cpp
cv::minMaxLoc(scores, nullptr, &score, nullptr, &max_point);
// 新增：计算 top-2 间隔
cv::Mat sorted; cv::sort(scores, sorted, cv::SORT_EVERY_ROW | cv::SORT_DESCENDING);
float margin = sorted.at<float>(0,0) - sorted.at<float>(0,1);
```

若误判帧的 `margin` 普遍 < 0.1，说明网络在「犹豫」→ 加 margin 门槛即可挡掉大部分。

### 步骤 3：记录 target.name 时序
在 `tracker.cpp:264` 后加日志，打印 `ARMOR_NAMES[armor.name]` 与帧号。
观察误判是「偶发单帧」还是「连续 5 帧以上」——后者才会真正建立错误目标。

### 步骤 4：确认 priority 未初始化的影响
在 `tracker.cpp:82` 前打印所有 `armor.priority`。
若出现非 1..5 的值或随机值，即坐实 UB。

---

## 5. 修复建议（按侵入性从低到高）

### A. 立即可做（零风险）

**A1. 初始化 `priority`** —— 消除 UB。
在 `Armor` 的每个构造函数初始化列表里加 `priority(ArmorPriority::third)`，
或在 `armor.hpp:94` 改成 `ArmorPriority priority = ArmorPriority::third;`。

**A2. 恢复几何校验** —— 解注释 `tracker.cpp:65-70`。
注意 `oupost_reprojection_error` 是**非 const 成员函数**且按值传 `Armor`，
先确认 `solver_.` 在该上下文可调用（当前是被注释状态，需验证编译）。

### B. 小改动、收益大

**B1. 分类 margin 门槛**（`yolo11.cpp:126` 附近）：
`score` 高但 top-1/top-2 间隔小时，降为 `not_armor` 或直接丢弃。

**B2. 跨帧投票**（`tracker.cpp:252`）：
`set_target` 前要求同一 name 连续出现 ≥ N 帧（N 可从 5 提到 15~20），
或者对 `armors` 里所有装甲板的 name 做一次多数表决再定目标。

**B3. `update_target` 加空间门控**（`tracker.cpp:283`）：
`name` 匹配之外，再加一条「该装甲板必须在预测的目标附近」的距离判据，
避免画面里另一台车的装甲板污染当前目标。

### C. 需要实测数据才能定

**C1. 转速钳位的触发条件加保护**（`target.cpp:132`）：
钳位前先判断「这个目标是否真的静止」，例如要求连续若干帧位置变化低于阈值，
否则即便 name 是 outpost 也不钳位。

**C2. 网络侧**：给训练集补充小陀螺姿态下的步兵样本，
特别是大角度侧视 + 运动模糊的样本。

---

## 6. 复现仿真

```bash
g++ -std=c++17 -O2 -I <eigen_include> verification/outpost_misid/misid_impact.cpp -o misid_impact
./misid_impact
```

依赖：Eigen 3.4（仅用到 `Eigen/Dense`）。不进 CMake 构建。

---

## 附：确认无误的部分

- `class_num_ = 38` 与 `armor_properties` 长度一致，索引 18/19/20 = outpost 核对无误。
- `colRange(4, 42)` 取类别得分、`colRange(42, 50)` 取 8 个关键点坐标（4 点），偏移正确。
- `cv::minMaxLoc` 在单行 Mat 上返回 `max_point.x` = 列索引 = class_id，语义正确。
- 观测维度 4（yaw / pitch / distance / armor_yaw），与 EKF 的 `NZ=4` 一致。
