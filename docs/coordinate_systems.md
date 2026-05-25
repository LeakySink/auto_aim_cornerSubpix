# 坐标系说明

## 概述

系统中存在三个坐标系，理解它们的关系是调试和开发的基础。

---

## 三个坐标系

### 相机坐标系（Camera Frame）

标准光学坐标系：

```
Z 轴：向前（镜头朝向）
X 轴：向右
Y 轴：向下
```

装甲板检测结果（PnP 解算）输出的位置在此坐标系下。

### 云台坐标系（Gimbal Frame）

与相机坐标系通过手眼标定矩阵 `R_cam2gimbal` 关联，存储在 `configs/calibration.yaml`。

### 世界坐标系（World Frame）

以 IMU 初始化时刻为原点，Z 轴竖直向上的惯性坐标系。

目标的 EKF 状态（位置、速度）在此坐标系下估计，保证云台旋转时目标状态不跳变。

---

## 坐标变换链

```
装甲板角点（图像坐标）
       ↓ PnP 解算
装甲板位姿（相机坐标系）
       ↓ R_cam2gimbal（手眼标定）
装甲板位姿（云台坐标系）
       ↓ R_gimbal2world（IMU 四元数）
装甲板位姿（世界坐标系）
       ↓ EKF 状态估计
目标旋转中心（世界坐标系）
       ↓ 弹道计算 + MPC
云台控制指令（yaw/pitch，rad）
```

### R_gimbal2world 的计算

来自 `io/gimbal/gimbal.cpp`，IMU 上报 roll/pitch/yaw（单位：度），转换为旋转矩阵：

```cpp
// gimbal.cpp:199-202
Eigen::AngleAxisd roll_aa(roll_rad,    Eigen::Vector3d::UnitX());
Eigen::AngleAxisd pitch_aa(-pitch_rad, Eigen::Vector3d::UnitY());  // 注意负号
Eigen::AngleAxisd yaw_aa(yaw_rad,      Eigen::Vector3d::UnitZ());
Eigen::Quaterniond q = (yaw_aa * pitch_aa * roll_aa).normalized();
```

`pitch_aa` 使用 `-pitch_rad` 的原因：下位机 pitch 正方向为抬头（向上），而右手定则绕 Y 轴正转对应低头（向下），两者方向相反，需取负号对齐。

---

## Pitch 符号约定

pitch 在各层之间的符号约定不同，容易混淆：

| 位置 | 正方向 | 说明 |
|---|---|---|
| 下位机上报 `GimbalToVision.pitch` | 抬头为正 | 电控约定 |
| `gimbal.cpp` 存储的 `state_.pitch` | 抬头为正 | 直接来自下位机 |
| `tools::Trajectory.pitch` | 抬头为正 | 弹道计算结果 |
| `aim()` 返回的 pitch | 抬头为负 | 已取负号：`-bullet_traj.pitch` |
| `traj` 矩阵中存储的 pitch | 抬头为负 | 与 `aim()` 一致 |
| `plan.pitch` 发出前 | 抬头为正 | `plan.pitch = -plan.pitch` 再次翻转 |
| `gimbal.send()` 接收的 pitch | 抬头为正 | 内部 `* RAD2DEG` 后发送 |

**完整符号链**：

```
弹道计算（抬头为正）
    → aim() 取负（抬头为负）
    → traj 存储（抬头为负）
    → MPC 求解（抬头为负）
    → plan.pitch = -x（抬头为正）
    → gimbal.send(pitch)（抬头为正）
    → 下位机执行（抬头为正）
```

---

## Yaw 符号约定

| 位置 | 正方向 |
|---|---|
| 下位机上报 `GimbalToVision.yaw` | 逆时针为正（俯视） |
| `aim()` 返回的 yaw | `atan2(y, x) + yaw_offset`，逆时针为正 |
| `traj` 中存储的 yaw | 相对量：`yaw - yaw0`（相对当前朝向） |
| `plan.yaw` | 绝对量：`x(0, HALF_HORIZON) + yaw0` |

---

## 手眼标定

相机与云台之间的固定变换通过手眼标定获得：

```bash
# 采集标定图像
./build/capture

# 计算相机内参
./build/calibrate_camera

# 计算手眼变换
./build/calibrate_handeye
```

结果存储在 `configs/calibration.yaml` 的 `R_cam2gimbal` 字段。
