# 系统架构总览

## 项目定位

本项目是一套用于 RoboMaster 竞赛的**自瞄系统**，运行在机器人车载 NUC 上，通过视觉识别敌方装甲板并控制云台自动瞄准射击。

与传统 ROS 方案不同，本项目采用**模块化视觉框架**：
- 单一可执行文件按云台模式信号路由到不同任务模块（自瞄/打符/全景感知）
- 不依赖 ROS 消息总线，延迟更低、部署更简单
- ROS2 仅用于哨兵导航通信，核心自瞄功能不需要

---

## 四层架构

```
┌─────────────────────────────────────────────────────┐
│  src/  应用层                                        │
│  standard_mpc.cpp / sentry.cpp / uav.cpp            │
│  每种机器人一个可执行文件，负责模块编排和模式路由      │
├─────────────────────────────────────────────────────┤
│  tasks/  任务层                                      │
│  auto_aim/  打符/  全景感知/                         │
│  具体功能实现：检测、跟踪、规划、控制                  │
├─────────────────────────────────────────────────────┤
│  tools/  工具层                                      │
│  EKF / 弹道计算 / YAML配置 / 线程安全队列 / 录制      │
├─────────────────────────────────────────────────────┤
│  io/  硬件抽象层                                     │
│  camera / gimbal / serial / imu                     │
│  统一接口，屏蔽相机型号和通信协议差异                  │
└─────────────────────────────────────────────────────┘
```

---

## 数据流

```
相机线程
  图像 + 时间戳
       ↓
  云台线程（四元数）
       ↓
  Detector（装甲板4角点）
       ↓
  Estimator / Tracker（EKF 目标运动状态）
       ↓
  Planner（TinyMPC 轨迹优化）
       ↓
  Controller（云台指令）
       ↓
  Gimbal（串口/CAN 发送）
```

**关键延迟节点**：
- 图像采集 → 检测完成：~10ms（OpenVINO GPU 推理）
- 检测完成 → 云台执行：~5ms（EKF + MPC）
- 总系统延迟：~15ms，由 `low_speed_delay_time` / `high_speed_delay_time` 补偿

---

## 多线程模型

### 标准模式（standard_mpc）

```
主线程：
  while(true) {
    img = camera.get()          // 阻塞等待新帧
    q   = gimbal.q(img.time)    // 插值获取对应时刻四元数
    detections = detector(img)  // YOLO 推理
    target = tracker.update(detections, q)  // EKF 更新
    plan = planner.plan(target, bullet_speed)
    gimbal.send(plan)
  }

云台读线程（独立）：
  while(true) {
    读串口 → 解析 GimbalToVision → 推入四元数队列
  }
```

### 多线程模式（mt_auto_aim_debug）

检测在独立线程池中运行，通过 `thread_safe_queue` 与主线程通信，进一步降低延迟。

---

## 支持的机器人类型

| 可执行文件 | 机器人 | 特点 |
|---|---|---|
| `standard_mpc` | 步兵 | MPC 轨迹规划，主力版本 |
| `auto_aim_debug_mpc` | 步兵（调试） | 带 PlotJuggler 可视化 |
| `mt_auto_aim_debug` | 步兵（多线程） | 检测线程分离 |
| `sentry` | 哨兵 | 需要 ROS2，含导航通信 |

---

## 硬件支持

### 相机

| 类型 | 驱动位置 | 说明 |
|---|---|---|
| 迈德威视（MindVision） | `io/mindvision/` | 工业相机，SDK 驱动 |
| 海康威视（HikRobot） | `io/hikrobot/` | 工业相机，SDK 驱动 |
| USB 相机 | `io/usbcamera/` | OpenCV VideoCapture |

通过 YAML 配置切换，代码层面统一接口。

### 云台通信

| 协议 | 驱动位置 | 说明 |
|---|---|---|
| 虚拟串口 | `io/serial/` + `io/gimbal/` | 新协议，当前主用 |
| USB2CAN | SocketCAN | 旧协议 |

---

## 构建与运行

```bash
# 配置并编译
cmake -B build
make -C build/ -j$(nproc)

# 运行主程序
./build/standard_mpc

# 调试工具
./build/planner_test_offline   # 离线测试规划器（不需要硬件）
./build/gimbal_response_test   # 测量云台阶跃响应
./build/auto_aim_test          # 集成测试（录制视频）
```

---

## 配置文件

所有参数集中在 `configs/` 目录下的 YAML 文件中：

```
configs/
  standard3.yaml      # 步兵参数（相机、云台、EKF、MPC）
  sentry.yaml         # 哨兵参数
  calibration.yaml    # 相机内参 + 手眼标定结果
```

详细参数说明见 [ekf_model.md](ekf_model.md) 和 [planner.md](planner.md)。
