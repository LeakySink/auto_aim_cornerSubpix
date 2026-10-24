# EKF 目标状态估计

## 功能简介

EKF（扩展卡尔曼滤波）负责从检测器输出的离散装甲板观测中，恢复出目标机器人的完整运动状态，包括旋转中心位置、速度、旋转角、角速度以及几何参数。

**EKF 解决的核心问题**：
- 观测噪声：相机标定误差、特征提取误差
- 观测不连续：装甲板旋转切换时跳变
- 系统延迟：~15ms 延迟需要预测补偿

**Pipeline**：
```
检测器（装甲板4角点）→ PnP位姿解算
      ↓
  装甲板匹配（预测各装甲板位置，选最小误差）
      ↓
  EKF预测：x(k|k-1) = F·x(k-1)，P = F·P·F' + Q
      ↓
  EKF更新：K 增益，x = x + K·(z - h(x))，P = (I-KH)P(I-KH)' + KRK'
      ↓
  轨迹预测（补偿延迟，predict 到未来时刻）
      ↓
后续：MPC 轨迹规划 → 云台控制
```

---

## 状态向量设计

**11维状态向量**（`target.cpp`）：

$$\mathbf{x} = [x, v_x, y, v_y, z, v_z, \theta, \omega, r, l, h]^T$$

| 索引 | 变量 | 物理意义 | 单位 |
|---|---|---|---|
| 0 | $x$ | 旋转中心 X | m |
| 1 | $v_x$ | X 方向速度 | m/s |
| 2 | $y$ | 旋转中心 Y | m |
| 3 | $v_y$ | Y 方向速度 | m/s |
| 4 | $z$ | 旋转中心 Z | m |
| 5 | $v_z$ | Z 方向速度 | m/s |
| 6 | $\theta$ | 旋转角（yaw） | rad |
| 7 | $\omega$ | 角速度 | rad/s |
| 8 | $r$ | 短轴半径（主装甲板） | m |
| 9 | $l$ | 长短轴差 $(r_2 - r_1)$ | m |
| 10 | $h$ | 高度差 $(z_2 - z_1)$ | m |

**设计说明**：
- 位置和速度：匀速模型（CV），加速度作为过程噪声
- $r, l, h$：描述目标几何，状态中估计，Q=0（假设时不变）
- $l, h$ 仅对4装甲板机器人有意义（1、3号装甲板使用 $r+l$、$z+h$）

---

## 目标几何模型

第 $i$ 个装甲板的位置（`target.cpp:266-277`）：

$$\begin{cases}
x_{a,i} = x - r_i \cos(\theta + i \cdot \frac{2\pi}{N}) \\
y_{a,i} = y - r_i \sin(\theta + i \cdot \frac{2\pi}{N}) \\
z_{a,i} = z + h_i
\end{cases}$$

其中 $N$ 是装甲板数量（2/3/4），且：
- 主要装甲板（4装甲板中 id=0,2）：$r_i = r$，$h_i = 0$
- 次要装甲板（4装甲板中 id=1,3）：$r_i = r + l$，$h_i = h$

---

## 状态转移模型

线性部分（匀速）：

$$\mathbf{F} = \text{diag}\left(\begin{bmatrix}1 & dt \\ 0 & 1\end{bmatrix}, \begin{bmatrix}1 & dt \\ 0 & 1\end{bmatrix}, \begin{bmatrix}1 & dt \\ 0 & 1\end{bmatrix}, \begin{bmatrix}1 & dt \\ 0 & 1\end{bmatrix}, \mathbf{I}_3\right)$$

非线性处理：角度更新后执行 `limit_rad` 归一化到 $[-\pi, \pi]$。

前哨站特殊处理（`target.cpp:131-132`）：当 $|\omega| > 2$ 时，限制为 $\pm 2.51$ rad/s。

---

## 过程噪声矩阵 Q

**分段白噪声模型（Piecewise White Noise）**（`target.cpp:96-120`）：

假设加速度在采样间隔内恒定，不同时刻独立：

$$Q_{\text{线性}} = v_1 \begin{bmatrix} dt^4/4 & dt^3/2 \\ dt^3/2 & dt^2 \end{bmatrix}, \quad Q_{\text{角}} = v_2 \begin{bmatrix} dt^4/4 & dt^3/2 \\ dt^3/2 & dt^2 \end{bmatrix}$$

**实际值（以代码为准）**：

```cpp
double v1 = 100;  // 线加速度方差 (m²/s⁴)  — target.cpp:101
double v2 = 75;   // 角加速度方差 (rad²/s⁴) — target.cpp:102
```

几何参数 $(r, l, h)$ 的 Q = 0（假设时不变）。

**调参建议**：
| 参数 | 增大效果 | 减小效果 | 场景 |
|---|---|---|---|
| v1 | 响应快但抖动 | 平滑但滞后 | 目标加速能力强 → 增大 |
| v2 | 角速度响应快 | 角速度平滑 | 小陀螺模式 → 增大到 150 |

**注意**：两份旧文档（EKF算法详解.md 和 EKF_TUNING_GUIDE.md）中的 v1/v2 数值（0.9/80 和 0.9/100）均与当前代码不符，以代码 `v1=100, v2=75` 为准。

---

## 观测模型

**4维观测向量**：

$$\mathbf{z} = [\text{yaw}, \text{pitch}, \text{distance}, \text{armor\_angle}]^T$$

**非线性观测函数** $h(\mathbf{x})$（`target.cpp:200-206`）：
1. 由状态计算装甲板世界坐标 $(x_a, y_a, z_a)$
2. 球坐标变换：$(x, y, z) \to (\text{yaw}, \text{pitch}, \text{distance})$
3. 装甲板法向角：$\theta + i \cdot \frac{2\pi}{N}$

---

## 自适应测量噪声 R

（`target.cpp:187-198`）：

```cpp
R_dig = {
    4e-2,                                         // yaw 方差 (rad²)
    4e-2,                                         // pitch 方差 (rad²)
    log(|delta_angle| + 1) + 1,                   // 距离方差 (m²)
    log(|distance| + 1) / 200 + 9e-2              // 角度方差 (rad²)
};
```

**自适应机制**：
- **距离方差**：装甲板侧面朝向相机时（$\Delta\theta$ 大）→ 距离测量不准 → R 增大
- **角度方差**：距离越远 → 角度估计越不准 → R 增大

**调参建议**：
- 检测精度高：可以将基础噪声从 4e-2 减小到 1e-2
- 近距离场景：可以适当减小所有 R 值
- 检测不稳定：增大基础 yaw/pitch 方差

---

## 初始协方差 P0

（`tracker.cpp:245-261`）：

```cpp
// 4装甲板机器人（标准/英雄）
P0_dig = {1, 64, 1, 64, 1, 64, 0.4, 100, 1e-5, 1, 1};

// 3装甲板机器人（哨兵）
P0_dig = {1, 64, 1, 64, 1, 81, 0.4, 100, 1e-4, 0, 0};

// 3装甲板机器人（前哨站/基地）
P0_dig = {1, 64, 1, 64, 1, 64, 0.4, 100, 1e-4, 0, 0};
```

**设计原则**：
- 位置：1 m²（标准差 1m，反映初始距离误差）
- 速度：64-81 m²/s²（标准差 8-9 m/s，速度完全未知）
- 角速度：100 rad²/s²（标准差 10 rad/s）
- 已知几何参数（$r$）：极小值（1e-5），未知参数（$l, h$）：1

**调参建议**：
- 远距离检测（>5m）：位置协方差增大到 2.0~3.0
- 小陀螺场景：角速度协方差增大到 150~200
- 发散率高：减小速度初始方差（64 → 36）

---

## 卡尔曼滤波数学性质

### EKF 算法

**预测步骤**：
$$\hat{\mathbf{x}}_{k|k-1} = f(\hat{\mathbf{x}}_{k-1|k-1}), \quad \mathbf{P}_{k|k-1} = \mathbf{F}_k \mathbf{P}_{k-1|k-1} \mathbf{F}_k^T + \mathbf{Q}_k$$

**更新步骤**（约瑟夫形式，保证 P 正定）：
$$\mathbf{K}_k = \mathbf{P}_{k|k-1} \mathbf{H}_k^T \mathbf{S}_k^{-1}$$
$$\hat{\mathbf{x}}_{k|k} = \hat{\mathbf{x}}_{k|k-1} + \mathbf{K}_k (\mathbf{z}_k - h(\hat{\mathbf{x}}_{k|k-1}))$$
$$\mathbf{P}_{k|k} = (\mathbf{I} - \mathbf{K}_k \mathbf{H}_k) \mathbf{P}_{k|k-1} (\mathbf{I} - \mathbf{K}_k \mathbf{H}_k)^T + \mathbf{K}_k \mathbf{R}_k \mathbf{K}_k^T$$

### NIS 一致性检验

$$\text{NIS} = \tilde{\mathbf{y}}_k^T \mathbf{S}_k^{-1} \tilde{\mathbf{y}}_k \sim \chi^2(4)$$

理想范围：NIS < 0.711（4自由度，95%置信区间）。连续失败率 > 40% 说明 Q/R 参数不匹配。

---

## 调参流程

### 数据采集

```bash
./build/auto_aim_debug_mpc  # 实时查看 EKF 状态
./build/auto_aim_test       # 录制数据用于离线分析
```

关注指标：NIS、残差（residual_yaw/pitch/distance）、收敛帧数、发散率。

### 调参顺序

**Step 1：调 P0**（目标：NIS 在理想区间比例 > 80%）
- 固定 Q 和 R，根据初始不确定性调整
- 观察前 10 帧的 NIS 变化

**Step 2：调 Q**（目标：预测误差合理）
- 速度抖动严重 → 增大 Q（更信任模型变化）
- 速度响应太慢 → 减小 Q（更信任历史）

**Step 3：调 R**（目标：噪声水平准确反映实际）
- 统计静止目标的测量方差，R 设置略小于统计值

### 常见问题

**滤波器发散**（半径超出 [0.05, 0.5] m）：
```cpp
P0[1] = P0[3] = 36;  // 速度方差从 64 减到 36
v1 = 0.5;            // 过程噪声从 100 减到 0.5
```

**响应速度慢**（速度估计滞后）：
```cpp
v1 = 1.5 * 100;      // 增大线加速度方差
P0[1] = P0[3] = 100; // 增大速度初始方差
```

**小陀螺跟踪不稳定**：
```cpp
v2 = 150;   // 角加速度方差从 75 增大到 150
P0[7] = 150; // 角速度初始方差从 100 增大到 150
```

### 参数合理性检查清单
- P0 对角线元素 > 0
- v1 > 0，v2 > 0
- Q/R 比例在合理范围（1~100）
- NIS < 0.711 的比例 > 80%
- 发散率 < 5%
- 特殊场景：静止收敛、匀速跟踪、小陀螺稳定、装甲板切换无跳变
