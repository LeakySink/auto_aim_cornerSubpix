# 相机内参标定 — 使用方法

只标定 **相机内参**（`camera_matrix` / `distort_coeffs`），不标手眼外参。  
车上跑无窗口程序推流；调试 PC 用 host 打开网页操作。

---

## 1. 准备

| 项目 | 说明 |
|------|------|
| 棋盘格 | 默认 **11×8 内角点**，方格 **40 mm**（在 `calibration/result.yaml` 改） |
| 相机 | 默认海康 `hikrobot`，曝光约 10 ms，连续自动增益 |
| 网络 | 车与 PC 同一局域网；车发 beacon，host 自动发现 |
| 目录 | 建议在**仓库根目录**运行（也可从子目录启动，会向上找 `result.yaml`） |

可选：编辑 [`result.yaml`](result.yaml) 里的相机字段：

```yaml
camera_name: "hikrobot"   # 或 mindvision
exposure_ms: 10           # 偏暗认不出棋盘格时调大
gain: 16.0                # 自动增益失败时的回退增益
vid_pid: "2bdf:0001"
```

---

## 2. 编译

```bash
cmake -B build
cmake --build build --target calibrate -j$(nproc)
```

---

## 3. 启动（两台机器）

**车上（接好相机）：**

```bash
./build/calibrate
```

成功时日志里应能看到类似：

- `open camera 'hikrobot' exposure_ms=...`
- `GainAuto=CONTINUOUS`（若不支持会 WARN 并回退固定增益）

**调试 PC：**

```bash
./host/calibrate.sh
# Windows: host\calibrate.bat
```

浏览器打开 `http://localhost:8090`（一般会自动打开）。  
顶部出现 `linked · calibrate` 表示已连上车。

无相机联调 host / 网页时（车上）：

```bash
./build/calibrate --test
```

---

## 4. 标定步骤

1. 把棋盘格放进画面，网页左侧有推流；右侧 **BOARD OK** 表示当前帧识别到板子。  
2. **缓慢**挥动标定板，覆盖不同位置 / 远近 / 倾斜，让 X / Y / Size / Skew 进度条尽量变绿。  
   - 程序会自动采样（姿态太像会跳过）。  
   - 也可按 **ADD** / `SPACE` 强制采一张。  
3. 样本数 ≥ **20** 后，**CALIBRATE** 按钮可点（或按 `C`）。  
4. 点击 **CALIBRATE** 后自动完成下面流程，无需再点保存：

```
host 下发 calibrate + 本机时间
    → 车计算内参
    → 写入 calibration/result.yaml
    → 把结果推回网页显示
    → host 通知车 quit
    → 车退出；host 进程随后退出
```

| 按钮 / 键 | 作用 |
|-----------|------|
| **ADD** / `SPACE` | 强制采样 |
| **CALIBRATE** / `C` | 开始计算并自动保存、结束 |
| **DROP** / `D` | 丢掉最后一张 |
| **RESET** / `R` | 清空样本 |

---

## 5. 结果

写在 [`calibration/result.yaml`](result.yaml)：

| 字段 | 含义 |
|------|------|
| `calibrated_at` | host 点击标定时的本机时间 |
| `camera_matrix` | 内参 3×3（行优先 9 个数） |
| `distort_coeffs` | 畸变系数 |
| 网页上的 reproj | 重投影误差（px），一般希望 **&lt; 0.5** |

需要给某辆车用时，把 `camera_matrix` / `distort_coeffs` **手工拷贝**到对应 `configs/*.yaml`。

---

## 6. 常见问题

| 现象 | 处理 |
|------|------|
| 一直 BOARD --，认不出板 | 检查 `pattern_cols/rows` 是否与实物一致；调大 `exposure_ms`；光线别太暗/反光 |
| 日志 `GainAuto ... failed` | 相不支持自动增益，已用固定 `gain`；可把 `gain` 调到 16～20 |
| 日志 `image too large ... skipped` | 推流 JPEG 过大被丢（当前已压质量）；仍出现可再降预览分辨率 |
| 点了标定没写文件 | 看日志里 `saved .../calibration/result.yaml`；确认对仓库有写权限 |
| host 找不到车 | 同网段、防火墙放行 UDP；车端 `remote_logger` 默认已开 |

---

## 7. 自检（无相机）

```bash
cmake --build build --target calibrate_test -j$(nproc)
./build/calibrate_test
```

---

## 附录：协议摘要

host → 车：

```json
{"v":1,"type":"json","host_id":"...","data":{"cmd":"calibrate","host_time":"2026-09-15 21:50:00"}}
{"v":1,"type":"json","host_id":"...","data":{"cmd":"quit"}}
```

车 → host（plot）：

```json
{"calib":true,"calib_done":1,"saved":1,"calibrated_at":"...","reproj":0.2,
 "camera_matrix":[...],"distort_coeffs":[...],"result_path":"..."}
```

更完整的控制面说明见 [`../host/PROTOCOL.md`](../host/PROTOCOL.md)。
