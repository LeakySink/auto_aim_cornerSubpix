# tests/ 分类目录

每个子目录内的 `*.cpp` 编成**同名**可执行文件，输出仍在 `build/tests/`（不随源码分子目录）。
约定见 `cmake/auto_executables.cmake`。

```
tests/
  rdbg/   RemoteLogger / Host 协议与证明
  algo/   感知 / 自瞄算法
  io/     IO 硬件冒烟
  ros2/   ROS2 桥
```

---

## `rdbg/` — RemoteLogger / Host

| 文件 | 说明 |
|------|------|
| `rdbg_improvement_proof.cpp` | 落盘优先、写盘吞吐、JPEG 降档体积证明 |
| `rdbg_host_proof.py` | Host RLG2 writer / LiveRecorder 洪水与稳态 |
| `multi_sender_test.cpp` | 多车协议集成 |
| `fake_robot.cpp` | 假车夹具（供 multi_sender） |
| `markers_pub_test.cpp` | Watch 3D markers |
| `tf_pub_test.cpp` | TF plot 上行 |
| `tf_pub_fake.py` | 假 TF 源（本地夹具） |

```bash
./build/tests/rdbg_improvement_proof
python3 tests/rdbg/rdbg_host_proof.py
./build/tests/multi_sender_test
```

---

## `algo/` — 感知 / 自瞄

| 文件 | 说明 |
|------|------|
| `auto_aim_test.cpp` | 视频 + 标注回放 |
| `auto_aim_debug_mpc_offline.cpp` | MPC 离线 |
| `auto_buff_test.cpp` | 能量机关 |
| `detector_video_test.cpp` | 检测器打视频 |
| `planner_test.cpp` / `planner_test_offline.cpp` | 规划器 |
| `minimum_vision_system.cpp` | 最小视觉闭环 |

---

## `io/` — 硬件冒烟

| 文件 | 说明 |
|------|------|
| `camera_*.cpp` | 工业相机 |
| `usbcamera_*.cpp` / `multi_usbcamera_test.cpp` | UVC |
| `cboard_test.cpp` | 电控板 |
| `gimbal_*.cpp` | 云台 |
| `dm_test.cpp` | DM IMU |
| `fire_test.cpp` | 开火 |
| `handeye_test.cpp` / `calibrate_test.cpp` | 手眼 / 标定 |

---

## `ros2/` — ROS2

需 `USE_ROS2=ON`，或 `ros2_*` 在已装 rclcpp+sensor_msgs 时软启用。

| 文件 | 说明 |
|------|------|
| `publish_test.cpp` / `subscribe_test.cpp` / `topic_loop_test.cpp` | 话题环回 |
| `ros2_image_pub_test.cpp` | 图像发布 |

---

## 怎么选

| 目标 | 目录 |
|------|------|
| 弱网落盘 / 降画质 / Host 录制 | `rdbg/` 的 `*_proof` |
| 多车协议 | `rdbg/multi_sender_test` |
| 算法回归 | `algo/` |
| 外设通断 | `io/` |
| ROS2 | `ros2/` |

**命名**：`*_proof` 可量化证明（尽量无硬件）；`*_test` 功能/冒烟；`*_offline` 不依赖实时外设；`fake_*` 夹具。
