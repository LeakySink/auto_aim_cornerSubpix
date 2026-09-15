# 相机一键标定（网页）

车上跑无窗口标定程序，调试 PC 用 host 打开网页做覆盖度可视化和按钮操作。交互对齐 ROS `camera_calibration`，通信复用 RemoteLogger / host 发现与队首协议。

## 编译

```bash
cmake -B build
cmake --build build --target calibrate calibrate_test -j$(nproc)
```

## 运行

**1. 车上（需要相机；默认还要云台 IMU）：**

```bash
./build/calibrate configs/calibration.yaml
./build/calibrate configs/calibration.yaml --camera-only
```

棋盘格尺寸与标定结果写在 [`result.yaml`](result.yaml)，不写回各车配置。

**2. 调试 PC：**

```bash
./host/calibrate.sh                 # http://localhost:8090
# Windows: host\calibrate.bat
```

浏览器会自动打开。`configs/calibration.yaml` 只负责相机 / 云台 / `remote_logger`。

## 操作

挥动标定板，网页右侧看 X/Y/Size/Skew 覆盖度和样本分布。按钮与快捷键：

| 按钮 / 键 | 作用 |
|---|---|
| 挥动标定板 | 自动采样（姿态太像会丢） |
| **ADD** / `SPACE` | 强制采样 |
| **CALIBRATE** / `C` | 至少 10 张；有 IMU 时同时算手眼 |
| **SAVE** / `S` | 写 `calibration/result.yaml`（含 `calibrated_at`） |
| **UNDISTORT** / `U` | 去畸变预览 |
| **DROP** / `D` | 丢掉最后一张 |
| **RESET** / `R` | 清空 |

画面由车上推流；进度条和按钮在网页。

## 结果

`SAVE` 更新 [`result.yaml`](result.yaml)：

- `calibrated_at`：本次标定计算完成时间
- `camera_matrix` / `distort_coeffs`
- 有手眼时还有 `R_camera2gimbal` / `t_camera2gimbal`

需要时再手工拷到各车 yaml。

## 协议

推荐（通用 JSON，host `RobotClient.send_json` / 车 `RemoteLogger::poll_json`）：

```json
{"v":1,"type":"json","host_id":"...","data":{"cmd":"add|calibrate|save|drop|reset|undistort|quit"}}
```

兼容旧版（仍可用）：

```json
{"v":1,"type":"calib_cmd","host_id":"...","cmd":"add|..."}
```

仅已入队 host 有效。车 → 队首：`plot`（`"calib":true`）+ `plot_image`（`name=calibrate`）。

## 建议

- 曝光短一点、增益补亮度；标定板平整、全板入画。
- 四条覆盖度尽量打满，手眼多转 yaw/pitch。
- 重投影一般应 &lt; 0.5 px。

离线算法自检（不需要相机/网页）：

```bash
./build/calibrate_test
```
