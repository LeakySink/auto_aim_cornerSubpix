# OPTIMIZATION_HANDOVER.md 逐条复核

- 复核日期：2026-10-18
- 复核对象：`auto_aim_cornerSubpix-main` 相对 `auto_aim_cornerSubpix_backup_20261011_0005` 的 7 处改动
- 复核方式：逐文件 diff + 调用链追踪 + 独立数值实验（自写小矩阵复现 NIS/NEES 统计量分布；`std::clamp`/chrono 类型实测编译）

## 0. 结论速览

| # | 改动 | 是否成立 | 备注 |
|---|---|---|---|
| 1a | 卡方阈值 0.711 → 9.488 / 19.675 | **必须改、且改后正确** | 已在 §0-B 用三重证据定稿；代码已按严格卡方定义修正 NIS |
| 1b | 3 次显式求逆 → LDLT | **正确** | 数学等价 `Kᵀ = S⁻¹HP`，实现无误 |
| 2a | 复用灰度图、imshow 收进 debug | **正确** | 真实省一次全帧 cvtColor |
| 2b | `vector<Point>` → `vector<Point2f>` | **正确** | 但"结果完全等价"的说法略绝对（P5） |
| 2c | ROI 回写重构 + 可选亚像素 | **正确（原子性 OK）** | 逻辑等价，失败不留半成品 |
| 2d | MAX_SHIFT 漂移防护 / 灯条端点同步 | **可编译、无害，但价值有限** | P3 / P4 |
| 3 | detector.hpp 签名同步 | **正确** | 全仓调用点仅 1 处，已同步 |
| 4a | iteration_target 惰性拷贝 | **正确且等价** | 每个副本仍只 predict 一次 |
| 4b | 未收敛日志 | **正确** | 无害 |
| 4c | yaw limit_rad 限幅 | **正确但收益极小** | 真正的 CAN 溢出点在 horizon_distance（P6） |
| 5 | IMU 插值除零 + clamp | **正确** | `duration/duration → double`，`std::clamp` 可编译（已实测） |
| 6 | yaml 显式化 | **正确** | 键名与 aimer 解析完全一致，取值等价 |
| 7 | 新增 ekf_update_check | **能编译、有效但覆盖窄** | P7 |

**总体判断：7 处代码改动全部可以保留，没有发现引入回归的改动。** 需要处理的是文档里的 3 处表述错误（P1/P2/P5）和 5 个"做了但没做到位"的点（P3/P4/P6/P7/P8）。

---

## 0-B. 卡方阈值：确定性结论（2026-10-18 定稿，含代码修正）

> 这一节回答"阈值是否必须改、改完是否正确"。结论由**精确分位数计算 + 真实模型蒙特卡洛 + 新旧实现逐帧比对**三重证据支撑，
> 证据与可复现程序见 `verification/chi2_threshold/`。

### 结论一：旧阈值 0.711 必须改 —— 它在 nominal 条件下有 ~93% 的更新被判失败

χ²(4) 的生存函数有闭式 `P(X>x) = e^{-x/2}(1+x/2)`，无需任何库即可验证：

| 量 | 值 | 含义 |
|---|---|---|
| χ²(4) CDF(0.711) | **0.050034** | 0.711 是 **5% 分位数**（不是文档说的 50%；50% 分位数是 3.3567） |
| χ²(4) 95% 分位数 | **9.4877** | 9.488 正确，P(X>9.488)=5.00% |
| χ²(11) 95% 分位数 | **19.6751** | 19.675 正确，P(X>19.675)=5.00% |

用**真实模型**（`target.cpp` 的 `h`/`h_jacobian`/`F`/`Q`/`R_dig`，配置取 `standard3.yaml`：`r_yaw=4e-2`、`r_dist_base=1.5e-2`、`q_xyz_acc_var=100`、`q_yaw_acc_var=75`、`p0=[1,64,1,64,1,64,0.4,400,1e-5,1,1]`、`armor_num=4`）跑 300 条轨迹 × 1000 帧：

| 统计量 | 期望 | 单帧误报率 |
|---|---|---|
| 旧阈值 0.711 作用于现行统计量 | — | **92.6%（30fps）/ 93.6%（60fps）/ 94.3%（120fps）** |
| 9.488 作用于现行统计量 | E=3.26~3.70（应为 4） | 2.2%~3.6% |
| 9.488 作用于标准 NIS（先验） | **E=4.004** | **5.03%** |

后果是确定性的：`tracker.cpp:104-110` 的 "Bad Converge" 判据是"最近 10000 帧中失败数 ≥ 4000"。
实测 **旧阈值在第 4208~4324 帧（中位 4265）必然触发**（理论预测 4000/0.936 ≈ 4274 帧，吻合）。
即 60 fps 下约 **71 秒**、30 fps 下约 **2.4 分钟**，任何目标都会被判发散并重置为 lost，之后周而复始。

### 结论二：9.488 只有在 NIS 用"先验"定义时才是正确的 —— 已完成代码修正

改前代码在 `x`/`P` 更新**之后**才计算残差，`S` 用的是后验 `P⁺`，得到的不是 NIS（实测期望 3.58 而非 4，误报率只有 3.2%，且对目标跳变不敏感）。
修正方式：**把新息与卡方检验挪到状态更新之前**（`tools/extended_kalman_filter.cpp`）：

```cpp
Eigen::VectorXd innovation = z_subtract(z, h(x));          // 先验新息 v = z − h(x⁻)
double nis = (innovation.transpose() * S_prior.ldlt().solve(innovation)).value();  // ~ χ²(4)
...
x = x_add(x, K * innovation);                              // K*innovation 与原实现完全等价
```

**这次修正不改变任何滤波结果**（这是关键）：

```
逐帧比对 新旧 EKF： max|x_new - x_old| = 2.6e-11 , max|P_new - P_old| = 7.8e-11
```

（差异仅为 LDLT 与显式求逆的浮点舍入；`K*innovation` 与 `K*z_subtract(z,h(x))` 在数学上完全一致。）

修正后真实模型实测：**E[NIS] = 4.0021，P(NIS > 9.488) = 5.05%** —— 与 χ²(4) 理论值 4 和 5% 吻合，
因此 9.488 现在**严格**对应 5% 显著性水平。安全性也验证了：nominal 误报率 5% 远低于 tracker 的 40% 触发线
（窗口内期望失败数 500 ≪ 4000，偏离 160σ），不可能误触发重置。

### 结论三：NEES 的 19.675 不是统计意义上的阈值（已保留，但注释已改正）

标准 NEES 需要真值：`(x⁺−x_true)ᵀP⁺⁻¹(x⁺−x_true) ~ χ²(11)`，而真值不可得。
代码用的是状态修正量 `(x⁺−x⁻)ᵀP⁺⁻¹(x⁺−x⁻)`，其协方差是 `P⁻−P⁺`（秩 ≤ 观测维数 4），**不服从 χ²(11)**：

- 实测中位数 0.22、p99 = 1.48，但均值被重尾拉到 ~55；
- **99.9% 的爆表帧（1080/1081）都源于 yaw 跨越 ±π 时 `x_add` 的回绕**，使 `dx[6]≈2π`（与"每帧 yaw 步进 / 2π ≈ 0.32%"的预期频率一致）；
- 阈值 19.675 实际误触发率 0.19%~0.77%，且 `nees_fail` **只进日志、不参与任何逻辑**（`grep` 已确认）。

由此可知：无真值时唯一可用的卡方一致性判据就是 NIS（自由度 = 观测维数 4），不存在可算的 χ²(11) 判据。
19.675 作为"惰性上限"是安全的，注释已改为如实说明。

### 固化进仓库的回归保护

`tests/algo/ekf_update_check.cpp` 已重写，新增 4 项断言（本机 MinGW + Eigen 3.4 实测全绿）：

```
A1/A2 LDLT 状态/协方差更新 == 显式求逆        OK (0.0)
B     data["nis"] == 先验新息卡方统计量        OK
C1    9.488 是 χ²(4) 95% 分位(误报率 5%)      OK (0.049994)
C2    0.711 是 χ²(4) 5%  分位(误报率 95%)     OK (0.050034)
D     样本 72000: E[NIS]=3.9614, P(>9.488)=4.81%, P(>0.711)=94.88%   OK
```

任何人把 NIS 改回后验收敛量（B 失败）或把阈值改回 0.711（C1/D3 失败），测试都会红。

---

## 1. `tools/extended_kalman_filter.cpp`

### 1.1 卡方阈值

代码位置与实际 diff 一致（阈值常量）。9.488 = χ²(4) 的 95% 分位、19.675 = χ²(11) 的 95% 分位，查表值本身没错。

**已定稿：见 §0-B。** 下面 P1/P2 两条是复核时提出的文档问题，其结论已被 §0-B 的真实模型数据取代（此处保留以便对照交接文档的原始表述）。

**P1：0.711 不是"自由度 4 的 50% 分位数"，而是 5% 分位数。**

χ²(4) 的 CDF 有闭式：`P(X≤x) = 1 − e^(−x/2)(1 + x/2)`。代入 x=0.711 → 0.9500，即 **5% 分位**。χ²(4) 的 50% 分位是 3.357。

这直接影响文档对"影响面"的判断：

> 原文："旧阈值下约一半更新会被误判为失败"

实际是 **约 93%**。我用代表性几何（目标 4 m、P 取收敛后的典型对角值、R 取 target.cpp 的 `R_dig`）做了 20 万次蒙特卡洛：

```
E[真 NIS(先验 S)]        = 4.01     （chi2(4) 理论值 4）
E[现行代码算出的 NIS]     = 3.28     （后验量，天然偏小 18%）
失效率 现行统计量 > 0.711 = 92.8%
失效率 现行统计量 > 9.488 =  2.22%
失效率 真 NIS    > 9.488 =  5.07%
```

所以这次修复比文档说的更必要（旧值几乎必然触发 tracker 的 "Bad Converge" 重置，`window_size=10000`、阈值 40%，跑约 4300 帧后必触发），而新值 9.488 在实测分布下失效率 2.2%，略保守但完全合理。

**P2：NIS/NEES 是用"后验量"算的，自由度解释不严格。**

L56-58 先更新了 P 和 x，L61-65 才用 `z − h(x⁺)` 和 `S = H P⁺ Hᵀ + R` 统计，这不是标准 NIS（标准 NIS 用先验 S 与新息 `z − h(x⁻)`）。

- 对 NIS 影响不大：后验残差被压缩、S 也被压缩，两个效应部分抵消，实测期望 3.28 vs 4，只差 18%，9.488 仍然可用。
- 对 NEES 影响是致命的：`(x−x⁻)ᵀ P⁺⁻¹ (x−x⁻)` 的期望等于 `tr((P⁻ − P⁺) P⁺⁻¹)`，实测 0.54，而注释标称"自由度 11"（期望应为 11），差 20 倍。也就是说 NEES 判据在旧阈值和新阈值下都基本不会触发。

好消息：`nees_fail` 只进日志（`src/*_debug.cpp`、`auto_aim_test.cpp`），不参与任何逻辑判断，所以**功能上无风险**，只是文档里"NEES 自由度 = 状态维数 11 → 19.675"这句话站不住。

建议（如果想让卡方语义成立，一行改动）：

```cpp
// 挪到 x/P 更新之前：用先验 S 与新息
Eigen::VectorXd innovation = z_subtract(z, h(x_prior));
Eigen::MatrixXd S_prior = H * P * H.transpose() + R;   // 已有
double nis = (innovation.transpose() * S_prior.ldlt().solve(innovation)).value();
```

### 1.2 LDLT 替换显式求逆

数学推导无误：

```
K = P Hᵀ S⁻¹  ⟹  Kᵀ = S⁻ᵀ (P Hᵀ)ᵀ = S⁻¹ H P    （S、P 对称）
```

`S_prior.ldlt().solve(H * P).transpose()` 正是 `Kᵀ` 的转置，量纲 4×11 → 11×4 也对。S 恒对称半正定，LDLT 适用；比 `inverse()` 少一次矩阵求逆，数值条件更好。

`residual.transpose() * S.ldlt().solve(residual)` 得到 1×1，`.value()` 合法（Eigen 1×1 矩阵允许）。Joseph 形式、`x_add`、后验 S 的定义确实没动 — 与文档一致。

唯一小瑕疵：`z_subtract` 被调用了两次（L58、L61），和原实现一致，未引入新开销。

---

## 2. `tasks/auto_aim/detector.cpp`

| 位置 | 判断 | 依据 |
|---|---|---|
| L31-33 读 `use_corner_subpix` | 正确 | `IsDefined()` 保护，旧 yaml 不受影响；其余 7 个 config 无此键 → 默认 false |
| L48 imshow 收进 `debug_` | 正确 | 与 `show_result` 的开关一致，非 debug 不再弹窗阻塞 |
| L123-124 复用 `gray_img` | 正确 | `gray_img` 此处未被修改（threshold 不写回），语义等价，省一次 1440×1080 cvtColor |
| L151-152 `Point2f` | 正确 | 原 `vector<Point>` 会用 `Point_` 的跨类型构造做 saturate_cast 取整 |
| L228-247 回写重构 | 正确 | 先写 ROI 坐标、再可选精化、最后统一加 offset；`refine_armor_subpix` 的所有校验都在赋值之前，失败即整块放弃，不会留"精化了一半"的角点 |
| L249-279 `refine_armor_subpix` | 见 P3 | — |
| L281-303 灯条同步 | 见 P4 | — |

**P3：亚像素精化对"灯条短边中点"这个输入是不适定的，MAX_SHIFT=3 挡不住真正的漂移。**

`Lightbar` 的 `top/bottom` 是 `minAreaRect` 四个角按 y 排序后取的两个短边中点（`armor.cpp:14-24`），本质是**一条 1D 边缘的中点**，不是鞍点角。cornerSubPix 内部解 2×2 结构张量：

- 沿灯条方向梯度为 0 → 张量秩 1 → `det ≈ 0`，OpenCV 在 `det==0` 时把步长置 0，但 `det` 只是"接近 0"时步长会被放大，这是真正的爆点；
- 更常见的是沿灯条方向的 1~2 px 系统性滑动 —— 而这种量级 **正好在 MAX_SHIFT=3 的放行区间内**。

所以这个防护对"数值爆炸"有效（这点值得肯定，比改动前无条件接受输出要好），但对"系统性偏 1~2 px"无效。叠加 `solvePnP(IPPE)`，2 px 的角点偏差在远距离会直接进距离估计。

结论：保持 `use_corner_subpix: false`（现状正确）；不要把它当成精度优化项上报。真要做亚像素，正确路径是"灯条两侧长边各拟合一条直线 → 求交得到鞍点角"，而不是对边中点调 cornerSubPix。

**P4：灯条端点同步是"半同步"，且当前无人消费。**

追踪结果：`armor.left/right` 在全仓只有两处下游使用 —
- `detector.cpp:367-370` 的 `get_pattern()`，而它在 L124 精化**之前**执行；
- `detector.cpp:447` 调试绘制用的文字锚点 `armor.left.bottom`。

也就是说这几行同步对功能没有任何影响（不是坏事，但也不是文档暗示的"消除不一致"）。同时它只同步了 `top/bottom/top2bottom/points`，`center`、`length`、`angle`、`ratio`、`rotated_rect` 仍是旧值，对象进入**自相矛盾**状态。另外 ROI 二次检测路径（L232-242）反而没有做同样的同步，两条路径行为不一致。

建议：删掉这 8 行（最省事），或补全 `center = (top+bottom)/2`、`length`、`angle` 并让 ROI 路径也走同一个 helper。

**P5：有三处默认路径的行为差异没在文档里列全。**

文档 §4 只列了 3 项行为差异（imshow、阈值、IMU NaN）。实际还有：

1. `vector<Point2f>` 让 `minAreaRect().boundingRect()` 从 `floor/ceil` 的整点框变成浮点框，**ROI 边界最多偏 1 px**，`armor.pattern` 的裁剪内容与 offset 与旧版不完全逐像素一致。功能无影响，但"结果与原实现等价"应改成"几何等价、ROI 边界可能有 ≤1 px 抖动"。
2. 全流程路径（`use_corner_subpix` 不控制它）现在多了 `MAX_SHIFT` 拒绝分支 —— 改动前任何幅度的输出都被接受。虽然触发概率低，但严格说默认输出变了。
3. `refine_corners_subpix` 现在要求单通道图，传 BGR 会**静默失效**（文档已说，但这是"静默"而非"报错"，建议加一行 `assert`/`WARN` 日志，避免后人踩坑）。

---

## 3. `tasks/auto_aim/detector.hpp`

签名/成员声明与实现完全对应（L24-25、L42、L45-46）。全仓 `refine_corners_subpix` 调用点只有 `detector.cpp:124` 一处，已同步为 `gray_img`；`use_corner_subpix_` 的默认值 `false` 放在类内初始化，`Detector` 所有构造路径都覆盖。

唯一遗留：`refine_armor_subpix` 是 private const，`detect(Armor&, bgr_img)` 里调用它改的是 `armor.points`（非 const 参数），语义上没问题，但"const 成员函数改参数"容易误读，加注释即可，不必改。

---

## 4. `tasks/auto_aim/aimer.cpp`

**4.1 惰性构造迭代副本 — 等价，正确。**

原实现 `vector<Target>(10, target)` 预生成 10 份深拷贝，每份只 `predict` 一次；新实现每轮 `emplace_back(target)` 后立刻用 `iteration_target[iter]`，同样是"每份副本只 predict 一次"，语义完全一致。`reserve(10)` 保证迭代中不发生重分配（下标引用稳定），但注意**所有副本活到函数结束**，内存峰值与原来相同（≤10 份），收益只是在"通常 2~3 次收敛"时少拷 7 份。这个改动安全。

**4.2 未收敛日志 — 无害。** `converged` 此前是只写不读的变量，现在有了诊断输出；`tools/remote_logger.hpp` 已在 L8 包含。

**4.3 yaw 限幅 — 正确，但收益极小（P6）。**

`tools::limit_rad` 输出区间 `(-π, π]`，π = 3.1416 < 32767/1e4 = 3.2767，所以确实能保证 `int16_t(yaw*1e4)` 不溢出。逻辑成立。

但需要认识到：`atan2` 本身输出就在 `[-π, π]`，只有 `yaw_offset_` 足够大（> 0.135 rad ≈ 7.7°）且 yaw 贴近 ±π 时才可能越界 —— 这几乎不会发生。第二处（L181）`command.yaw - yaw_offset_ + yaw_offset` 的越界条件同理。

**P6：同一个 `CBoard::send` 里 `horizon_distance` 的溢出是确定性的，比 yaw 严重得多。**

`io/cboard.cpp:67-68` 把 `command.horizon_distance * 1e4` 打成 int16，而 `commandgener.cpp:52-56` 给的正是**水平距离（米）**：只要目标 > 3.2767 m，`(int16_t)(d*1e4)` 必然溢出回绕（5 m → 50000 → −15536）。

如果固件侧确实是 `/1e4` 解释（与 yaw/pitch 同比例），这就是每帧都发生的确定性 bug；如果固件另作解释，则该字段协议需要单独确认。既然这次是以"防 CAN 溢出"为由做的加固，建议把这一处一并核对，否则只堵住了概率极小的那一端。

---

## 5. `io/cboard.cpp`

**除零防护正确**：`while(queue_.pop(...), data_behind_.timestamp <= timestamp)` 之后 `t_ab = t_b − t_a` 可能为 0（两帧时间戳相同）或负（时间戳乱序），原实现 `k = t_ac/t_ab` 会得到 `inf/NaN`，`slerp(NaN)` 之后整条 `R_gimbal2world_` 全废。新增的早返回 `q_b`（最新帧）是合理兜底。

**`std::clamp` 的类型是对的**（这点容易踩坑，我单独编译确认过）：

```cpp
std::chrono::duration<double> / std::chrono::duration<double>  →  double   // 不是 duration
std::clamp(double, 0.0, 1.0)  →  double    // 编译通过，static_assert 已验
```

如果把 `t_ac/t_ab` 写成 `t_ac.count()/t_ab.count()` 也一样。`<algorithm>` 已补。

小建议：`k` 被限幅到 [0,1] 后，如果 `t_ac < 0`（查询时刻早于两帧）会退化成 `q_a`，此时其实 `q_a` 也未必是最接近的帧；不过这属于极端乱序，可不管。

---

## 6. `configs/standard3.yaml`

键名逐个核对过 `aimer.cpp:22-36` 的解析分支，**完全匹配**（注意是 `drag_height_boundary`，不是文档表格里简写的 `boundary`）。

取值等价性验证：

| 场景 | 改动前 | 改动后 |
|---|---|---|
| `drag_coefficient_up` | 缺键 → 0.0 | 0.0 |
| `drag_coefficient_down` | `down` 缺键 → 回退 `up` = 0.0 | 0.0 |
| `drag_height_boundary` | 缺键 → 0.0 | 0.0 |

三项都一致，`Aimer` 走到 `k=0` 时 `Trajectory` 退化为纯抛物线，行为不变。`use_corner_subpix: false` 也是默认值。文档里"参考 sentry.yaml 的 up=0.2415, down=0.0, boundary=-0.3"与 `sentry.yaml:72-74` 实际值一致。

唯一的措辞问题：注释写"传统全流程检测始终开启"，这句是**如实**的（`detect()` 里 `refine_corners_subpix` 不受该开关控制），但容易被后来人误解成"开关能关掉传统路径的精化"。建议改成"仅控制 YOLO+ROI 二次检测后的精化；传统全流程路径始终精化"。

---

## 7. `tests/algo/ekf_update_check.cpp`

**能进入构建：** `cmake/auto_executables.cmake` 用 `file(GLOB CONFIGURE_DEPENDS)` 扫 `tests/algo/*.cpp` → 生成同名 target，`DEFAULT_LIBS` 带 `tools`；文件有 `main()`，include 风格 `"tools/..."` 与其它测试一致。不会破坏构建。

**测试本身有效但覆盖很窄：**

- 有效：`dx/dP` 与"显式求逆参考实现"对比，这正是 LDLT 改动最需要的回归保护，等价性断言（`<1e-10`）合理。
- 不足 1：所谓"检查 NIS/NEES 阈值逻辑"其实没验证。构造用例里 `P0 = AᵀA + 0.1I`（A 是 `Random`，特征值可达 10 量级）、`R = 0.04I`、`z` 又几乎是 `Hx₀`，所以 `S` 特别大 → `nis = 9e-5`，0.711 和 9.488 两个阈值在这组数据上根本没区别。
- 不足 2：`Eigen::MatrixXd::Random` 未 `srand`，序列虽然确定（libstdc++ 默认种子），但跨标准库实现不保证可复现；建议改成硬编码矩阵。
- 不足 3：断言用了 `data["nis_fail"]`，而 `nis_fail` 是**只置 1 不清零**的粘滞字段（既有 quirk），一旦某帧失败就永远是 1，不适合做断言输入。
- 不足 4：没有 `add_test`，`ctest` 不会跑它（只编译、需手动执行）。
- 瑕疵：文件头用法注释路径错了 —— 写的是 `tests/ekf_update_check.cpp`（实际在 `tests/algo/`）和 `-Itools`（实际 include 是 `"tools/extended_kalman_filter.hpp"`，需要 `-I .`）。

**验证状态复核：** 文档 §3 说 detector/aimer/cboard 因本机无 OpenCV 未编译 —— 属实，本机同样只有 MinGW g++、无 OpenCV/fmt/yaml-cpp，无法推翻也无法确认。我只能对新增代码做人工可编译性检查（`cv::norm(Point2f)`、`minAreaRect(vector<Point2f>)`、`std::isfinite(float)`、`vector<Point2f> = {p1,p2}`、`std::clamp` 均成立），**最终仍需在 Ubuntu/OpenVINO 目标机 `cmake -B build && make -j` 确认**。

---

## 8. `OPTIMIZATION_HANDOVER.md` 本身需要改的地方

**P8：§5 的回滚方案会连 git 仓库一起删掉。**

现在 `auto_aim_cornerSubpix-main` 有 `.git`（3 个提交，工作区干净），而备份目录没有。`rm -rf auto_aim_cornerSubpix-main && cp -r backup main` 会把 `.git` 抹掉。

建议改成：

```bash
cd D:/my/ckyf_vision/auto_aim_cornerSubpix-main
git revert 69a787b      # 或 git checkout 76f6893 -- .   （76f6893 = 改动前基线）
```

另外文档说"除上述文件外无任何其他文件被修改"，实测 `diff -rq` 还多出 `.git/`、`OPTIMIZATION_HANDOVER.md`、`RULE_2027_IMPACT.md`（都是后加的非代码内容），措辞上加一句"（另有 .git 与后续 md 文档）"更严谨。

**逐处措辞修正清单：**

1. §2.1 "实为自由度 4 的 50% 分位数" → "实为 χ²(4) 的 5% 分位数（0.711），即约 95% 的正常更新会被判失败；实测（含后验量偏小效应）失效率约 93%"。
2. §2.1 "旧阈值下约一半更新会被误判" → "约 93%"，并说明"跑满 10000 帧窗口后必然触发 Bad Converge 重置"。
3. §1 / §2.2 "NEES 自由度 11 → 19.675" → 加一句"当前 NEES 用后验 P 计算，期望≈0.5 而非 11，该阈值实际不触发；因 nees_fail 仅进日志，无功能影响"。
4. §2.2 L228-247 "结果与原实现等价" → "几何等价；ROI 边界因浮点取整最多偏 1 px"。
5. §2.2 "新增 3 px 漂移防护" → 补一句"输入是灯条短边中点而非鞍点，1~2 px 的沿边漂移不在防护范围内，故该精化不建议作为精度手段启用"。
6. §1 表格 "drag_coefficient_up/down/boundary" → `drag_height_boundary`。

---

## 9. 如果要继续做（按性价比排序）

1. ~~**NIS 改回先验定义**（10 行内）~~ → **已完成**（见 §0-B）：实测 E[NIS]=4.0021、P(>9.488)=5.05%，状态/协方差与旧实现逐帧一致。
2. **`horizon_distance` 打包溢出**：确认固件协议，必要时限幅到 ±3.2 m 或改字段语义。
3. **`solver.cpp:61` IPPE 双解重投影比较**：这个比本次所有改动的影响都大（单帧 yaw 可能错 180°，现在靠 `target.cpp:155-159` 的 flip-cost 兜底，是"事后打补丁"）。
4. 角点质量：灯条长边拟合 + 交点，替代 cornerSubPix。
5. `tracker.cpp` 图像中心硬编码 1440/1080 → 从 config/图像尺寸取。
6. `yolov5.cpp:77-78` x/y scale 与 rows/cols 对调。

---

## 附：复核用的数值实验

本地 `C:\Users\wyh\AppData\Local\Temp\ekf_review\` 下有两个一次性程序（未进仓库）：

- `nis2.cpp`：手写 11×11 小矩阵 Gauss 求解，按 `target.cpp` 的 H/R 结构与典型收敛 P，蒙特卡洛 20 万次，复现"先验 NIS / 后验统计量 / NEES"三者期望与失效率。输出见 §1.1。
- `clamp_test.cpp`：验证 `duration<double>/duration<double>` 结果是 `double`，`std::clamp` 可编译。
