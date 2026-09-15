# 相机内参一键标定（网页）

车上跑无窗口标定程序，**只标定相机内参**（`camera_matrix` / `distort_coeffs`），不标手眼外参。调试 PC 用 host 打开网页看覆盖度并点按钮。通信复用 RemoteLogger / host 发现与队首协议。

## 编译

```bash
cmake -B build
cmake --build build --target calibrate calibrate_test -j$(nproc)
```

## 运行

**1. 车上（需要相机，不需要云台）：**

```bash
./build/calibrate configs/calibration.yaml
```

棋盘格尺寸与内参结果写在 [`result.yaml`](result.yaml)。

**2. 调试 PC：**

```bash
./host/calibrate.sh                 # http://localhost:8090
# Windows: host\calibrate.bat
```

`configs/calibration.yaml` 只负责相机与 `remote_logger`。

## 操作

挥动标定板，网页右侧看 X/Y/Size/Skew 覆盖度和样本分布。按钮与快捷键：

| 按钮 / 键 | 作用 |
|---|---|
| 挥动标定板 | 自动采样（姿态太像会丢） |
| **ADD** / `SPACE` | 强制采样 |
| **CALIBRATE** / `C` | 至少 10 张，算内参 |
| **SAVE** / `S` | 写 `calibration/result.yaml`（含 `calibrated_at`） |
| **UNDISTORT** / `U` | 去畸变预览 |
| **DROP** / `D` | 丢掉最后一张 |
| **RESET** / `R` | 清空 |

## 结果

`SAVE` 更新 [`result.yaml`](result.yaml)：

- `calibrated_at`：本次内参计算完成时间
- `camera_matrix` / `distort_coeffs`

不写 `R_camera2gimbal` / `t_camera2gimbal`。需要时再手工把内参拷到各车 yaml。

## 协议

推荐（host `RobotClient.send_json` / 车 `RemoteLogger::poll_json`）：

```json
{"v":1,"type":"json","host_id":"...","data":{"cmd":"add|calibrate|save|drop|reset|undistort|quit"}}
```

兼容旧版：

```json
{"v":1,"type":"calib_cmd","host_id":"...","cmd":"add|..."}
```

仅已入队 host 有效。车 → 队首：`plot`（`"calib":true`）+ `plot_image`（`name=calibrate`）。

## 建议

- 曝光短一点、增益补亮度；标定板平整、全板入画。
- 四条覆盖度尽量打满。
- 重投影一般应 &lt; 0.5 px。

离线算法自检（不需要相机/网页）：

```bash
./build/calibrate_test
```
