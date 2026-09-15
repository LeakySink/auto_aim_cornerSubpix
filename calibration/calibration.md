# 相机一键标定

实时挥动棋盘格采集样本，一次计算内参和手眼外参，写回 yaml。交互对齐 ROS `camera_calibration`。

## 编译

```bash
cmake -B build
cmake --build build --target calibrate calibrate_test -j$(nproc)
```

## 运行

车上需要相机。默认还要云台 IMU（手眼用四元数）。桌上只标内参时加 `--camera-only`。

```bash
./build/calibrate configs/calibration.yaml
./build/calibrate configs/calibration.yaml -o configs/sentry.yaml
./build/calibrate configs/calibration.yaml --camera-only
```

棋盘格默认 **11×8 内角点**、方格 **40 mm**，在 `configs/calibration.yaml` 里改。相机曝光、内参、手眼仍写在各车自己的 yaml（或标定配置）里。

## 操作

右侧是可视化面板：覆盖度条、样本 XY 分布、去畸变预览，以及可点击按钮。快捷键仍然可用。

| 按钮 / 键 | 作用 |
|---|---|
| 挥动标定板 | 自动采样（X/Y/Size/Skew 有增益才收） |
| 点画面 / `SPACE` | 强制采样（仍会拒绝几乎重复的姿态） |
| **CALIBRATE** / `C` | 标定。至少 10 张；有 IMU 时同时算手眼 |
| **SAVE** / `S` | 写回 yaml（`-o` 指定的文件，默认就是配置文件） |
| **UNDISTORT** / `U` | 主画面去畸变；标定后右侧也有预览小图 |
| **DROP LAST** / `D` | 丢掉最后一张 |
| **RESET** / `R` | 清空重来 |
| `Q` / `Esc` | 退出 |

画面里会画出已采集的棋盘格外框（绿=带 IMU，黄=仅内参）。标定完成后，当前板上会画三维坐标轴。右侧 coverage XY 是样本中心在画面中的分布。

右侧四条进度条和 ROS 含义相同：

- **X / Y**：棋盘格中心在画面里扫过的范围
- **Size**：远近变化
- **Skew**：斜视角

条满后显示 `coverage READY`。条没满但已有 10 张也可以按 `C`，精度会差一些。

## 结果写什么

`S` 会更新目标 yaml 里的这些键，其它内容不动：

- `camera_matrix`、`distort_coeffs`（附注重投影误差）
- `R_camera2gimbal`、`t_camera2gimbal`（米；注释为相对理想安装的 yaw/pitch/roll）

把 `-o` 指到该车配置（如 `configs/sentry.yaml`）即可直接给自瞄用。也可以先写进 `configs/calibration.yaml`，再自己拷。

## 建议

- 曝光短一点、增益补亮度，减少拖影；标定板要平整、全板在画面内。
- 四条覆盖度都尽量打满，手眼再多转几个 yaw/pitch。
- 重投影误差一般应小于 **0.5 px**。明显偏大就 `R` 重采。
- 手眼注释里的偏角应接近机械安装；差到十几度先检查 IMU 方向和 `R_gimbal2imubody`。
- 可用 `./build/handeye_test` 看地面网格是否套得上（需单独的 handeye 配置）。

离线自检（不需要相机）：

```bash
./build/calibrate_test
```
