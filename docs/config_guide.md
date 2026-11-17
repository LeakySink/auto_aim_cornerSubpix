# 配置手册

> **为什么有这份文档**：`configs/*.yaml` 现在只保留 `#####-----段名-----#####` 分隔行，
> 其余注释一律删掉。所有参数的语义、单位、默认值与"改了会怎样"都写在这里。
>
> 覆盖的是**当前代码真正读的键**。2026 版全量参数表已过期，见
> [`archive/CONFIG_PARAMETERS_REFERENCE.md`](archive/CONFIG_PARAMETERS_REFERENCE.md)（仅历史参考）。

---

## 0. 怎么用

### 文件一览

| 文件 | 用途 |
|---|---|
| `sentry.yaml` | 哨兵；也是**默认配置**（多数入口的 `@config-path` 默认值） |
| `standard3.yaml` / `standard4.yaml` | 步兵（3 号/4 号机器人） |
| `ascento.yaml` / `demo.yaml` / `mvs.yaml` | 特定车/特定相机的变体 |
| `example.yaml` | 全键模板（含 USB 相机项） |
| `camera.yaml` | 后视相机（哨兵全向感知用） |
| `tf_pub.yaml` | TF 发布测试用，非实战 |

### 改配置的流程

```bash
cp configs/sentry.yaml configs/heavy.yaml     # 重装就新建一份，别改 sentry
vim configs/heavy.yaml                        # 只改下面 §1 列出的键
./build/tests/ballistics_poly_check           # 弹道表自检
./build/src/heavy_aim configs/heavy.yaml      # 跑
```

**注意**：yaml 里没有 `heavy.yaml`，需要自己建；入口的默认值仍是 `configs/sentry.yaml`。

---

## 1. 重装（42mm）必须改的 6 个键

| 键 | 17mm 步兵常见值 | 重装应设 | 说明 |
|---|---|---|---|
| `bullet_speed_default` | 缺键 → 22 | **16.5** | 缺键/越界时的兜底弹速；串口协议不给真弹速，所以这个值大概率就是实际用的 |
| `bullet_speed_min` / `bullet_speed_max` | 缺键 → 10 / 25 | 10 / 20 | 判定"下发的弹速是否可信"的区间 |
| `ballistics_csv` | `assets/ballistic/small.csv` | **`assets/ballistic/big.csv`** | 阻力系数差 3.6 倍，用错表 12 m 偏 380 mm |
| `center_aim.launch_delay` | 0.0 | 实测值 | 判定→出膛延迟；0.05 s 在 1 rad/s 下就是 2.9° |
| `ekf.<robot>.radius` | `normal: 0.2` | 重装实测 | 旋转中心到板面距离 |
| `ekf.<robot>.armor_num` | `normal: 4` | 按实际 | 3 = 前哨站，4 = 普通车 |

`ekf` 段有三组初值（`outpost` / `base` / `normal`），分别对应前哨站、英雄/工程基础、
普通车。**重装通常用 `normal`**，但 `radius` 要按重装车的实测值改。

---

## 2. 检测与前处理

| 键 | 单位 | 说明 |
|---|---|---|
| `enemy_color` | — | `"blue"` / `"red"`，按敌方颜色过滤装甲板 |
| `yolo_name` | — | `tup` / `yolo11` / `yolov5` / `yolov8`，选检测器实现 |
| `*_model_path` | — | 各检测器的 IR/ONNX 路径（`assets/*.xml` 是 OpenVINO IR） |
| `classify_model` | — | 数字分类模型（`assets/tiny_resnet.onnx`） |
| `device` | — | `CPU` / `GPU`（OpenVINO 设备名） |
| `min_confidence` | 0–1 | 检测置信度下限；太低会放进"几个像素的板"，导致 PnP 病态 |
| `use_traditional` | bool | 传统灯条检测与神经网络**并行**跑，两路结果合并 |
| `publish_detections` | bool | 发 `tup_detect` / `tup_detect_input` 两路调试图（只在 TUP 实现里） |
| `publish_image_name` | — | 上面那路图的话题名，缺省 `tup_detect` |
| `save_detection_frames` | 帧 | 保存检测结果图的数量（调试用，默认 0 = 不存） |
| `reject_bad_keypoints` | bool | 关键点几何校验开关；**关掉会让离谱位姿进 EKF**，建议保持开 |
| `use_corner_subpix` | bool | 角点亚像素细化（`detector.cpp`） |
| `use_roi` + `roi{x,y,width,height}` | px | 只在该矩形内检测；`use_roi: false` 时 `roi` 无效 |

传统灯条检测（`use_traditional: true` 时才用）：`threshold`（二值化）、`max_angle_error`、
`min/max_lightbar_ratio`、`min_lightbar_length`、`min/max_armor_ratio`、`max_side_ratio`、
`max_rectangular_error`、`min_detect_count`。这些是灯条/装甲板的形状判据，调之前先看
`archive/detector.md`。

---

## 3. `armor_geometry` —— 板面尺寸

| 键 | 单位 | 默认 | 说明 |
|---|---|---|---|
| `small_width` | m | 0.135 | 小装甲板宽（两灯条外缘） |
| `big_width` | m | 0.230 | 大装甲板宽；**2027 已取消大装甲**，正常不会用到 |
| `lightbar_length` | m | 0.056 | 灯条长度（= 程序里的"板高"） |
| `tilt_deg` | ° | 15.0 | 普通板面俯仰 |
| `outpost_tilt_deg` | ° | −15.0 | 前哨站板面俯仰 |
| `thickness` | m | 0.01 | 只用于 3D 可视化 |

**这一组同时影响 PnP 解算和"能不能打中"的窗口宽度**：板宽写大 10%，命中窗口也会宽 10%，
但 PnP 距离会偏。改之前先量实物。

---

## 4. `ekf` —— 滤波器噪声

| 键 | 单位 | 哨兵现值 | 说明 |
|---|---|---|---|
| `q_xyz_acc_var` | m²/s⁴ | 70.0 | 位置/速度通道的过程噪声（大 = 更信观测） |
| `q_yaw_acc_var` | rad²/s⁴ | 100.0 | 角度/角速度通道的过程噪声 |
| `r_yaw` | rad² | 5e-3 | 视线 yaw 观测方差（σ≈4.0°） |
| `r_pitch` | rad² | 2e-3 | 视线 pitch 观测方差（σ≈2.6°） |
| `r_dist_base` | rad² | 2e-1 | ⚠️ **名字有歧义**：它加在**板朝向**那个通道上（`R[3] = log(dist+1)/200 + r_dist_base`），不是距离通道 |

实际使用的 `R`（`target.cpp`，`view_penalty = 1/max(|cos(板法向与视线夹角)|, 0.5)`）：

```
R[0] 视线 yaw   = r_yaw × view_penalty
R[1] 视线 pitch = r_pitch
R[2] 距离       = (log(|Δangle| + 1) + 1) × view_penalty      ← 与配置无关，恒 1.0~2.42 m²
R[3] 板朝向     = log(dist + 1)/200 + r_dist_base
```

**陷阱**：`R[2]` 的 σ 是 **1.0~1.6 m**（写死的公式），实测只有 0.05~0.09 m。
这让新息门控（χ² 检验）几乎不可能触发，Bad Converge 看门狗也永不响应。
细节与实测数据见 [`context_handoff.md`](context_handoff.md) 的 R2。

每机器人初值（`outpost` / `base` / `normal`）：

| 键 | 说明 |
|---|---|
| `radius` | m，旋转中心到板面距离（`normal: 0.2`、`base: 0.3205`、`outpost: 0.2765`） |
| `armor_num` | 板数：4 = 普通车，3 = 前哨站 |
| `p0` | 11 维初值协方差对角元，顺序 `x vx y vy z vz a w r l h`；最后三项（`r l h`）没有过程噪声，写小一点 |

---

## 5. 跟踪与选板

| 键 | 单位 | 说明 |
|---|---|---|
| `min_detect_count` | 帧 | 连续检出多少帧才建目标（抗虚警） |
| `max_temp_lost_count` | 帧 | 普通目标允许丢多少帧（15） |
| `outpost_max_temp_lost_count` | 帧 | 前哨站更宽松（75） |
| `comming_angle` / `leaving_angle` | ° | 小陀螺选板：板"转进来/转出去"的取舍角度（`aimer.cpp`） |
| `decision_speed` | rad/s | 判"小陀螺"的角速度门限 |
| `high_speed_delay_time` / `low_speed_delay_time` | s | 按目标角速度分档的系统延迟补偿 |
| `yaw_offset` / `pitch_offset` | ° | 机械安装偏差补偿（`aimer`/`center_aimer` 都用） |

---

## 6. 弹道

| 键 | 说明 |
|---|---|
| `ballistics_model` | `poly`（用拟合表，首选）/ `rk4`（内置 RK4 + 编译期阻力系数） |
| `ballistics_csv` | 相对工程根的 CSV 路径：`assets/ballistic/small.csv`（17mm）/ `big.csv`（42mm） |
| `ballistics_x_range` | `[min, max]` 水平距离，超出即**回退 RK4**（不外推） |
| `ballistics_y_range` | `[min, max]` 高度差 |
| `bullet_speed_default` / `min` / `max` | 兜底弹速与可信区间，缺键 = 22 / 10 / 25 |

CSV 的 `pitch` 列是**相对视线的补偿量**，程序内部会再加 `atan2(h, d)`；表怎么生成见
[`../assets/ballistic/README.md`](../assets/ballistic/README.md)。
`big.csv` 的 `V` 范围是 `[13.0, 16.6]`，16.5 可用；超范围按端点夹取并打 `v_clamp` 日志。

---

## 7. 瞄准与开火（传统链路，`aimer` + `shooter`）

| 键 | 单位 | 说明 |
|---|---|---|
| `min_valid_dist` | m | 距离下限：PnP 结果与预测板都按它过滤（幽灵板护栏），缺省 0.5 |
| `max_valid_dist` | m | 距离上限（PnP 校验），缺省 50 |
| `first_tolerance` / `second_tolerance` | ° | 开火角度容差：近距离用前者，远距离用后者 |
| `judge_distance` | m | 用哪个容差的距离分界 |
| `auto_fire` | bool | 总开关；`false` 时永远不发开火位 |
| `fire_thresh` | rad | MPC 链路里 `hypot(yaw_err, pitch_err)` 的开火门限。**各配置差很多**：sentry 0.15（8.6°）、standard3 0.0035、demo 0.003 |
| `max_yaw_acc` / `max_pitch_acc` | rad/s² | 加速度约束（TinyMPC 的输入上下限） |
| `Q_yaw` / `Q_pitch` | — | MPC 状态代价权重 |
| `R_yaw` / `R_pitch` | — | MPC 输入代价权重 |

---

## 8. `center_aim` —— 重装专用（`heavy_aim`）

| 键 | 默认 | 说明 |
|---|---|---|
| `hit_margin` | 0.30 | 只用板面中间 `(1−margin)` 判命中，吸收 PnP/超前量误差 |
| `gimbal_tol_deg` | 0.5 | 云台到位容差；太紧会导致 `fire` 恒 0 |
| `require_settled` | true | 云台没到位就不开火；排查时可临时设 `false` |
| `max_iter` | 6 | 飞行时间迭代次数 |
| `launch_delay` | 0.0 | **判定→出膛总延迟 [s]**，必须实测：静态靶打靶，看弹着点是否系统性偏"将要转到的一侧" |

策略与限制见 [`architecture.md`](architecture.md) §6。

---

## 9. 云台、CAN 与串口

| 键 | 说明 |
|---|---|
| `com_port` | 云台串口设备（`/dev/ttyACM0` 等）。**新协议走串口，这是实战路径** |
| `can_interface` | SocketCAN 接口名（旧 CAN 链路） |
| `quaternion_canid` / `bullet_speed_canid` / `send_canid` | 旧 CAN 链路的报文 id（只有 `CBoard` 类入口用：`standard` / `sentry*`） |
| `yaw_kp` / `yaw_kd` / `pitch_kp` / `pitch_kd` | 下位机 PID 参数（发下去，不是本程序用） |

**弹速在串口协议下拿不到**（`GimbalState::bullet_speed` 恒 0），所以 §1 的
`bullet_speed_default` 就是实际用的值。

---

## 10. 相机与标定

| 键 | 单位 | 说明 |
|---|---|---|
| `camera_name` | — | `hikrobot` / `mindvision` / `usbcamera` |
| `exposure_ms` / `gain` | ms / — | 工业相机曝光与增益 |
| `vid_pid` | — | USB 相机 `vid:pid`（十六进制字符串） |
| `usb_exposure` / `usb_gain` / `usb_gamma` / `usb_frame_rate` | — | USB 相机专用 |
| `image_width` / `image_height` / `fov_h` / `fov_v` / `gamma` / `mode` | — | 部分相机驱动的分辨率与视场 |
| `camera_matrix` | — | 3×3 内参（`fx fy cx cy` 在这里） |
| `distort_coeffs` | — | 畸变系数 |
| `R_camera2gimbal` / `t_camera2gimbal` | — | 手眼标定结果，`Solver` 用它把相机系点搬到云台系 |
| `R_gimbal2imubody` | — | 云台到 IMU 本体的旋转 |
| `calibrated_at` | — | 标定时间戳（人看的） |

标定流程见 `calibration/`；**改相机或动过机械结构必须重标**，否则 PnP 距离系统性偏。

---

## 11. `remote_logger` —— 录制与远程调试

| 键 | 默认 | 说明 |
|---|---|---|
| `enable_remote` / `enable_local` | — | 发 UDP / 写本地 `.rlog`（两个可同时开） |
| `log_dir` | `./logs` | 本地 rlog 目录 |
| `control_port` | 15000 | 与 host 的 UDP 端口 |
| `var_buffer_size` / `img_buffer_size` | 1024 / 10 | 变量与图像环形缓冲深度 |
| `img_width` | — | 图像下采样宽度。**注意**：它同时决定 rlog 里图像的分辨率，用压缩过的 rlog 跑 `rlog_replay` 会得到错误的 PnP 距离（录包实测 2.28× 偏差）。要离线重跑检测请用 `eval_record` 的全分辨率流 |
| `img_quality` | — | JPEG 质量（1–100） |
| `sender_name` | — | 车名，host 端按它分流 |
| `heartbeat_interval_ms` / `beacon_interval_ms` / `head_timeout_ms` | ms | 心跳与丢失判定 |

---

## 12. 常见陷阱

1. **`5e-3` 这类写法**：yaml-cpp 解析成 double 没问题，但 **PyYAML 会当成字符串**。
   Python 侧读这些键时请 `float(...)` 包一层。
2. **弹速三段式**：串口不给真弹速 → 兜底值生效 → 忘了改 `bullet_speed_default`
   就等于按 17mm 弹速解算（12 m 偏 209 mm）。
3. **`r_dist_base` 不是距离方差**（见 §4）。
4. **`use_roi` 忘了开**：改了 `roi` 却把 `use_roi` 留 `false`，会以为配置没生效。
5. **`img_width` 的副作用**：它影响 rlog 图像分辨率，进而影响离线重放的 PnP 距离。
6. **`armor_geometry` 与检测端标签不一致**：检测吐 `big`（230 mm）而实物是 135 mm 时，
   PnP 距离会偏 ~70%。
7. **同一个键在不同文件里值不同**（尤其 `fire_thresh`），跨文件对比曲线时先确认用的是哪份。

---

## 13. 相关文档

- 策略与判据：[`architecture.md`](architecture.md) §6（重装）、§8（弹道）
- 当前风险与待办：[`context_handoff.md`](context_handoff.md)
- 目标与误差预算：[`heavy_aim_baseline.md`](heavy_aim_baseline.md)
- 逐模块数学与旧字段字典：[`archive/pipeline_details.md`](archive/pipeline_details.md)
- 2026 版全量参数表（已过期）：[`archive/CONFIG_PARAMETERS_REFERENCE.md`](archive/CONFIG_PARAMETERS_REFERENCE.md)
