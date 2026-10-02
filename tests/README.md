# tests/ 分类说明

构建约定：本目录每个 `*.cpp` 自动编成同名可执行文件（见 `cmake/auto_executables.cmake`），**保持扁平目录**；分类只作索引，不按文件夹拆分。

Python 脚本需手动跑；`*_offline` / `detector_video` 等一般要本地视频或 yaml。

---

## 1. RemoteLogger / Host 协议与证明

| 文件 | 类型 | 说明 |
|------|------|------|
| `rdbg_improvement_proof.cpp` | **证明 / 回归** | 落盘优先顺序、写盘吞吐、JPEG 降档体积；无真车 |
| `rdbg_host_proof.py` | **证明 / 回归** | Host RLG2 writer + LiveRecorder 洪水/稳态 |
| `multi_sender_test.cpp` | 协议集成 | 多车 beacon / register / data_port / img_subscribe |
| `fake_robot.cpp` | 夹具 | 假车进程，供 `multi_sender_test` 拉起 |
| `markers_pub_test.cpp` | 功能 | Watch 3D markers 上行 |
| `tf_pub_test.cpp` | 功能 | TF plot 上行（配合 yaml） |
| `tf_pub_fake.py` | 本地夹具 | 假 TF 源；注释写明可不入库 |

```bash
./build/tests/rdbg_improvement_proof
python3 tests/rdbg_host_proof.py
./build/tests/multi_sender_test   # 依赖 fake_robot
```

---

## 2. 感知 / 自瞄算法（离线或半离线）

| 文件 | 说明 |
|------|------|
| `auto_aim_test.cpp` | 自瞄管线：视频 + 标注回放 |
| `auto_aim_debug_mpc_offline.cpp` | MPC 自瞄离线调试 |
| `auto_buff_test.cpp` | 能量机关检测/解算 |
| `detector_video_test.cpp` | 检测器打视频 |
| `planner_test.cpp` / `planner_test_offline.cpp` | 规划器在线/离线 |
| `minimum_vision_system.cpp` | 最小视觉闭环（相机+检测+瞄准） |

---

## 3. IO 硬件冒烟

| 文件 | 说明 |
|------|------|
| `camera_test.cpp` / `camera_thread_test.cpp` / `camera_detect_test.cpp` | 工业相机读帧 / 线程 / 探测 |
| `usbcamera_test.cpp` / `usbcamera_detect_test.cpp` / `multi_usbcamera_test.cpp` | UVC 相机 |
| `cboard_test.cpp` | 电控板 CAN/串口 |
| `gimbal_test.cpp` / `gimbal_response_test.cpp` | 云台通信与响应 |
| `dm_test.cpp` | DM IMU |
| `fire_test.cpp` | 开火相关 IO |
| `handeye_test.cpp` | 手眼标定相关 |
| `calibrate_test.cpp` | 标定流程冒烟（链 calibration） |

---

## 4. ROS2（需 `USE_ROS2=ON` 或 ros2_* 依赖齐）

| 文件 | 说明 |
|------|------|
| `publish_test.cpp` / `subscribe_test.cpp` / `topic_loop_test.cpp` | 话题收发环回 |
| `ros2_image_pub_test.cpp` | 图像发布 |

---

## 5. 怎么选

| 你想验证… | 跑这些 |
|-----------|--------|
| 本次弱网落盘 / 降画质 / Host 录制是否正向 | **§1** `rdbg_*_proof` |
| 多车 Host 协议有没有挂 | **§1** `multi_sender_test` + `fake_robot` |
| 算法是否回归 | **§2** 对应 offline/video |
| 车上硬件是否通 | **§3** 插上设备后单测 |
| ROS2 桥 | **§4** |

**命名习惯（建议新测试遵守）**

- `*_proof`：可量化 A/B 或契约证明，CI/合并前可跑、尽量无硬件  
- `*_test`：功能/冒烟，可能要 yaml、视频或外设  
- `*_offline`：不依赖实时相机/电控  
- `fake_*`：测试夹具，不是断言主体  
