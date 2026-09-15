# 相机内参一键标定（网页）

车上跑无窗口标定程序，**只标定相机内参**。调试 PC 用 host 打开网页看覆盖度并点按钮。

## 编译

```bash
cmake -B build
cmake --build build --target calibrate calibrate_test -j$(nproc)
```

## 运行

**1. 车上（需要相机，无 CLI 参数）：**

```bash
./build/calibrate          # 建议在仓库根目录，或任意子目录（会自动找 calibration/result.yaml）
```

**2. 调试 PC：**

```bash
./host/calibrate.sh                 # http://localhost:8090
```

## 操作

挥动标定板填满 X/Y/Size/Skew，至少约 20 张样本：

| 按钮 / 键 | 作用 |
|---|---|
| **ADD** / `SPACE` | 强制采样 |
| **CALIBRATE** / `C` | 带 host 时间 → 车上计算 → **自动写 result.yaml** → 回传结果 |
| **DROP** / `D` | 丢掉最后一张 |
| **RESET** / `R` | 清空 |

点 **CALIBRATE** 之后的握手：

1. host 下发 `calibrate` + `host_time`
2. 车计算内参，保存 `calibrated_at` / `camera_matrix` / `distort_coeffs` 到 `calibration/result.yaml`
3. 车用 plot 回传结果（`calib_done`）
4. 网页展示内参；host 再下发 `quit`
5. 车退出；host HTTP 进程随后退出

## 结果文件

[`result.yaml`](result.yaml)（相对仓库根；从 `build/` 启动也会向上查找）：

- `calibrated_at`：host 本机时间
- `camera_matrix` / `distort_coeffs`

## 协议

```json
{"v":1,"type":"json","host_id":"...","data":{"cmd":"calibrate","host_time":"2026-09-15 21:50:00"}}
{"v":1,"type":"json","host_id":"...","data":{"cmd":"quit"}}
```

车 → host（plot）：

```json
{"calib":true,"calib_done":1,"saved":1,"calibrated_at":"...","reproj":0.2,
 "camera_matrix":[...],"distort_coeffs":[...],"result_path":"..."}
```

## 调试后门（结束后删除）

```bash
./build/calibrate --test
```

标 `CALIB_TEST_FEED`；结束后删 `test_feed.hpp` 与相关标记。

```bash
./build/calibrate_test
```
