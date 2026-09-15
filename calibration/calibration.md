# 相机内参一键标定（网页）

车上跑无窗口标定程序，**只标定相机内参**。调试 PC 用 host 打开网页看覆盖度并点按钮。

## 编译

```bash
cmake -B build
cmake --build build --target calibrate calibrate_test -j$(nproc)
```

## 运行

**1. 车上（需要相机，项目根目录执行，无参数）：**

```bash
./build/calibrate
```

棋盘格与结果在 [`result.yaml`](result.yaml)。相机默认海康；要改曝光等可在同文件写可选字段（见文件内注释）。RemoteLogger 使用内置默认。

**2. 调试 PC：**

```bash
./host/calibrate.sh                 # http://localhost:8090
# Windows: host\calibrate.bat
```

## 操作

挥动标定板，网页右侧看 X/Y/Size/Skew。按钮与快捷键：

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

- `calibrated_at`
- `camera_matrix` / `distort_coeffs`

不写外参。需要时再手工拷到各车 yaml。

## 调试后门（结束后删除）

无相机时：

```bash
./build/calibrate --test
```

会推送假棋盘格画面，beacon 名 `calibrate-test`。相关代码标 `CALIB_TEST_FEED`，结束后删 `calibration/test_feed.hpp` 及 `calibrate.cpp` 中同名标记段。

## 协议

```json
{"v":1,"type":"json","host_id":"...","data":{"cmd":"add|calibrate|save|drop|reset|undistort|quit"}}
```

兼容旧版 `calib_cmd`。仅已入队 host 有效。

## 建议

- 曝光短一点、增益补亮度；标定板平整、全板入画。
- 四条覆盖度尽量打满。
- 重投影一般应 &lt; 0.5 px。

```bash
./build/calibrate_test
```
