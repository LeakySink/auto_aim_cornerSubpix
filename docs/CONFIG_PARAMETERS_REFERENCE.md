# 配置参数全解析

## 目录
1. [参数文件结构](#参数文件结构)
2. [检测器参数](#检测器参数)
3. [跟踪器参数](#跟踪器参数)
4. [瞄准器参数](#瞄准器参数)
5. [射击器参数](#射击器参数)
6. [规划器参数](#规划器参数)
7. [相机参数](#相机参数)
8. [通信参数](#通信参数)
9. [标定参数](#标定参数)
10. [能量机关参数](#能量机关参数)

---

## 参数文件结构

### 配置文件位置
```
configs/
├── standard3.yaml      # 标准步兵(3装甲板)
├── standard4.yaml      # 标准步兵(4装甲板)
├── sentry.yaml         # 哨兵
├── uav.yaml            # 无人机
├── ascento.yaml        # Ascento机器人
├── demo.yaml           # 演示配置
├── mvs.yaml            # MindVision相机配置
├── camera.yaml         # 相机标定配置
└── calibration.yaml    # 手眼标定配置
```

### 参数分类
```
配置文件
├── 检测相关
│   ├── 神经网络参数
│   ├── 传统方法参数
│   └── ROI参数
├── 跟踪相关
│   └── Tracker参数
├── 控制相关
│   ├── Aimer参数
│   ├── Shooter参数
│   └── Planner参数
├── 硬件相关
│   ├── 相机参数
│   ├── 通信参数
│   └── 标定参数
└── 特殊模式
    └── 能量机关参数
```

---

## 检测器参数

### 神经网络参数

#### enemy_color
```yaml
enemy_color: "blue"  # 或 "red"
```
**作用**: 指定敌方颜色
**可选值**: `"red"`, `"blue"`
**调参建议**: 根据比赛规则设置

#### yolo_name
```yaml
yolo_name: yolov5  # 或 yolov8, yolo11
```
**作用**: 选择使用的YOLO模型
**可选值**: `yolov5`, `yolov8`, `yolo11`
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**:
- `yolov5`: 兼容性好，速度中等
- `yolov8`: 精度高，速度较慢
- `yolo11`: 最新版本，精度和速度平衡

#### classify_model
```yaml
classify_model: assets/tiny_resnet.onnx
```
**作用**: 装甲板数字分类模型路径
**说明**: 用于识别装甲板上的数字(1-5)

#### yolo_model_path
```yaml
yolo11_model_path: assets/yolo11.xml
yolov8_model_path: assets/yolov8.xml
yolov5_model_path: assets/yolov5.xml
```
**作用**: 各YOLO模型的模型文件路径
**格式**: OpenVINO IR格式(.xml)

#### device
```yaml
device: CPU  # 或 GPU
```
**作用**: 推理设备选择
**可选值**: `CPU`, `GPU`
**调参建议**:
- `CPU`: 兼容性好，无需额外配置
- `GPU`: 推理速度快3-5倍，需Intel集成显卡

#### min_confidence
```yaml
min_confidence: 0.8  # 置信度阈值 [0, 1]
```
**作用**: 检测置信度阈值，低于此值的检测将被过滤
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**:
| 场景 | 推荐值 | 效果 |
|------|--------|------|
| 室内/光照好 | 0.85~0.9 | 减少误检 |
| 室外/光照差 | 0.7~0.8 | 提高召回率 |
| 默认 | 0.8 | 平衡点 |

#### use_traditional
```yaml
use_traditional: true  # 是否使用传统方法作为后备
```
**作用**: 神经网络检测失败时，是否启用传统灯条检测
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**: 保持`true`，提高系统鲁棒性

---

### 传统方法参数

#### threshold
```yaml
threshold: 150  # 二值化阈值 [0, 255]
```
**作用**: 图像二值化阈值，用于提取灯条
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**:
| 光照条件 | 推荐值 | 说明 |
|----------|--------|------|
| 强光 | 180~220 | 避免过曝 |
| 正常 | 130~170 | 默认范围 |
| 弱光 | 80~120 | 提高灵敏度 |

#### max_angle_error
```yaml
max_angle_error: 45  # degree, 灯条倾角误差
```
**作用**: 允许的灯条与竖直方向的最大夹角
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**:
- 正常情况: `45°`
- 云台俯仰范围大时: 增大到`60°`
- 说明: 过大会误检，过小会漏检斜向装甲板

#### min_lightbar_ratio / max_lightbar_ratio
```yaml
min_lightbar_ratio: 1.5   # 最小长宽比
max_lightbar_ratio: 20    # 最大长宽比
```
**作用**: 灯条长宽比范围，用于筛选灯条候选
**调参建议**:
| 距离 | min | max | 说明 |
|------|-----|-----|------|
| 近距离 | 2.0 | 15 | 灯条较完整 |
| 中距离 | 1.5 | 20 | **默认** |
| 远距离 | 1.2 | 25 | 灯条模糊 |

#### min_lightbar_length
```yaml
min_lightbar_length: 8  # pixels, 最小灯条长度
```
**作用**: 过滤噪声点
**调参建议**:
| 分辨率 | 推荐值 |
|--------|--------|
| 640x480 | 5~8 |
| 1280x720 | 8~12 |
| 1440x1080 | 12~16 |

#### min_armor_ratio / max_armor_ratio
```yaml
min_armor_ratio: 1   # 最小装甲板长宽比
max_armor_ratio: 5   # 最大装甲板长宽比
```
**作用**: 装甲板整体长宽比范围
**说明**: 用于判断两个灯条是否构成有效装甲板

#### max_side_ratio
```yaml
max_side_ratio: 1.5  # 左右灯条长度比
```
**作用**: 同一装甲板左右灯条长度差异上限
**调参建议**: 保持1.5~2.0

#### max_rectangular_error
```yaml
max_rectangular_error: 25  # degree, 平行四边形误差
```
**作用**: 允许的装甲板形状误差
**说明**: 装甲板不一定是完美矩形

---

### ROI参数

#### roi (感兴趣区域)
```yaml
roi:
  x: 420
  y: 50
  width: 600
  height: 600
```
**作用**: 限定检测区域，减少计算量
**代码位置**: `tasks/auto_aim/detector.cpp`
**调参建议**:
```python
# 计算ROI中心
roi_center_x = x + width/2
roi_center_y = y + height/2

# 设置为图像中心
image_width = 1440
image_height = 1080
roi_center = (image_width/2, image_height/2)
```

#### use_roi
```yaml
use_roi: false  # 是否启用ROI
```
**作用**: 启用/禁用ROI功能
**调参建议**:
- 性能紧张时: `true`
- 性能充足时: `false` (全图检测更可靠)

---

## 跟踪器参数

### min_detect_count
```yaml
min_detect_count: 5  # 最小连续检测次数
```
**作用**: 确认为有效目标所需的最小连续检测次数
**代码位置**: `tasks/auto_aim/tracker.cpp:23`
**状态机影响**:
```
lost → detecting → tracking
         ↑
      需连续检测min_detect_count次
```
**调参建议**:
| 场景 | 推荐值 | 说明 |
|------|--------|------|
| 比赛正式 | 5~8 | 避免误锁定 |
| 测试调试 | 2~3 | 快速锁定 |
| 干扰多 | 8~10 | 提高鲁棒性 |

### max_temp_lost_count
```yaml
max_temp_lost_count: 15  # 最大临时丢失帧数
```
**作用**: 目标临时丢失后，保持跟踪的最大帧数
**代码位置**: `tasks/auto_aim/tracker.cpp:24`
**调参建议**:
| 帧率@100fps | 时间 | 推荐值 |
|-------------|------|--------|
| 10帧 | 0.1s | 10 |
| 15帧 | 0.15s | **15** |
| 20帧 | 0.2s | 20 |

**影响因素**:
- 遮挡频繁: 增大到20~30
- 目标切换快: 减小到5~10

### outpost_max_temp_lost_count
```yaml
outpost_max_temp_lost_count: 75  # 前哨站专用
```
**作用**: 前哨站目标的最大丢失帧数
**说明**: 前哨站旋转快，需要更长的丢失容忍时间

---

## 瞄准器参数

### yaw_offset / pitch_offset
```yaml
yaw_offset: 0      # degree, Yaw偏置
pitch_offset: 1    # degree, Pitch偏置
```
**作用**: 瞄准点系统性误差补偿
**代码位置**: `tasks/auto_aim/planner/planner.cpp:16-17`
**调参方法**:
```bash
# 1. 固定云台，瞄准已知距离的目标
# 2. 记录实际弹着点与瞄准点的偏差
# 3. 将偏差值填入配置文件

# 示例：
# 实际弹着点偏右2° → yaw_offset = -2
# 实际弹着点偏下1° → pitch_offset = 1
```

### comming_angle / leaving_angle
```yaml
comming_angle: 55   # degree, 进入角度
leaving_angle: 20   # degree, 离开角度
```
**作用**: 约束瞄准yaw角范围，避免云台过度旋转
**代码位置**: `tasks/auto_aim/aimer.cpp`
**调参建议**:
```python
# 根据云台物理限位设置
# comming_angle: 目标从左侧进入时允许的最大角度
# leaving_angle: 目标从右侧离开时允许的最小角度

# 典型值：
云台限位±90°: comming=70, leaving=20
云台限位±45°: comming=55, leaving=-20
```

### left_yaw_offset / right_yaw_offset
```yaml
left_yaw_offset: -1    # degree, 左侧偏置
right_yaw_offset: -0.6 # degree, 右侧偏置
```
**作用**: 分左右方向的Yaw偏置补偿
**说明**: 某些云台左右不对称时使用

### decision_speed
```yaml
decision_speed: 7  # rad/s, 高低速分界阈值
```
**作用**: 区分低速与高速旋转模式
**代码位置**: `tasks/auto_aim/planner/planner.cpp:108`
**用途**:
```cpp
if (abs(angular_velocity) > decision_speed) {
    使用high_speed_delay_time;
} else {
    使用low_speed_delay_time;
}
```
**调参建议**:
| 机器人类型 | 推荐值 | 说明 |
|-----------|--------|------|
| 标准/英雄 | 7~8 | 小陀螺约2-3 rad/s |
| 哨兵 | 10~12 | 旋转速度更快 |
| 无人机 | 12~15 | 机动灵活 |

### high_speed_delay_time / low_speed_delay_time
```yaml
high_speed_delay_time: 0.04  # s, 高速模式延迟
low_speed_delay_time: 0.04   # s, 低速模式延迟
```
**作用**: 系统延迟补偿时间
**代码位置**: `tasks/auto_aim/planner/planner.cpp:19-21`
**组成**:
```
总延迟 = 检测延迟 + 传输延迟 + 控制延迟
       ≈ 10ms + 10ms + 20ms
       = 40ms = 0.04s
```
**调参方法**:
```bash
# 使用PlotJuggler测量实际延迟
./build/standard_mpc | plotjuggler

# 测量步骤：
# 1. 观察目标检测时刻
# 2. 观察云台响应时刻
# 3. 计算时间差
```

### min_spin_speed
```yaml
min_spin_speed: 2  # rad/s, 最小旋转速度
```
**作用**: 判断目标是否在旋转
**用途**: 无人机特有参数

---

## 射击器参数

### first_tolerance
```yaml
first_tolerance: 3  # degree, 近距离射击容差
```
**作用**: 近距离(<judge_distance)允许的瞄准误差
**代码位置**: `tasks/auto_aim/shooter.cpp`
**调参建议**:
| 距离范围 | 容差 | 说明 |
|----------|------|------|
| <1m | 4~5° | 目标大，容差大 |
| 1-2m | 2~3° | **默认** |
| >2m | 使用second_tolerance | - |

### second_tolerance
```yaml
second_tolerance: 2  # degree, 远距离射击容差
```
**作用**: 远距离(≥judge_distance)允许的瞄准误差
**调参建议**: 通常比first_tolerance小1°~2°

### judge_distance
```yaml
judge_distance: 2  # m, 距离判断阈值
```
**作用**: 区分近距离与远距离射击
**代码位置**: `tasks/auto_aim/shooter.cpp`
**调参建议**:
```cpp
// 弹道下坠明显的距离
judge_distance ≈ v²/g * tan(2°)

// 22m/s弹速:
judge_distance ≈ 22²/9.8 * 0.035 ≈ 1.7m
```

### auto_fire
```yaml
auto_fire: true  # 是否由自瞄控制射击
```
**作用**: 启用/禁用自动射击
**调参建议**:
- 比赛: `true`
- 测试: `false` (手动控制射击)

---

## 规划器参数

### fire_thresh
```yaml
fire_thresh: 0.0035  # rad, 射击阈值
```
**作用**: MPC规划的射击决策阈值
**代码位置**: `tasks/auto_aim/planner/planner.cpp:18, 91-95`
**计算**:
```cpp
// 在预测时域的HALF_HORIZON + shoot_offset时刻
误差 = sqrt((yaw_ref - yaw_act)² + (pitch_ref - pitch_act)²)
if (误差 < fire_thresh) {
    允许射击;
}
```
**调参建议**:
| 射击场景 | fire_thresh | 命中率 | 射频 |
|----------|-------------|--------|------|
| 近距离(<3m) | 0.002~0.003 | 高 | 高 |
| 中距离(3-6m) | 0.0035~0.005 | 中 | 中 |
| 远距离(>6m) | 0.005~0.008 | 低 | 低 |

### max_yaw_acc / max_pitch_acc
```yaml
max_yaw_acc: 50    # rad/s², Yaw加速度限制
max_pitch_acc: 100 # rad/s², Pitch加速度限制
```
**作用**: MPC优化中的加速度约束
**代码位置**: `tasks/auto_aim/planner/planner.cpp:120, 143`
**调参建议**:
| 云台类型 | max_yaw_acc | max_pitch_acc |
|----------|-------------|---------------|
| 高性能 | 80~120 | 150~200 |
| 标准 | 40~60 | 80~120 |
| 低端 | 20~40 | 40~80 |

**测试方法**:
```bash
./build/gimbal_response_test

# 观察实际最大加速度
# 设置为实测值的90%
```

### Q_yaw / Q_pitch
```yaml
Q_yaw: [9e6, 0]    # [位置权重, 速度权重]
Q_pitch: [9e6, 0]
```
**作用**: MPC代价函数的状态权重
**代码位置**: `tasks/auto_aim/planner/planner.cpp:121, 144`
**调参建议**:
| 症状 | 调整 | 效果 |
|------|------|------|
| 跟踪误差大 | 增大到 1.2e6~1.5e6 | 更严格跟踪 |
| 响应超调 | 减小到 3e6~6e6 | 更平滑响应 |
| 震荡不稳定 | 检查是否>1e7 | 降低权重 |

**理论限制**:
```
Q_position < 1e7  # 避免数值问题
Q_position > 1e5  # 保证收敛
```

### R_yaw / R_pitch
```yaml
R_yaw: [1]   # 控制权重
R_pitch: [1]
```
**作用**: MPC代价函数的控制权重
**调参建议**:
| 目标 | R值 | 效果 |
|------|-----|------|
| 平滑优先 | 5~10 | 控制柔和 |
| 跟踪优先 | 0.1~1 | 响应快速 |
| 平衡 | 1 | **默认** |

**Q/R比例**:
```
典型范围: Q/R ∈ [1e6, 1e7]

跟踪优先: Q/R = 9e6/1 = 9e6
平滑优先: Q/R = 1e6/5 = 2e5
```

---

## 相机参数

### 工业相机参数

#### camera_name
```yaml
camera_name: "hikrobot"  # 或 "mindvision"
```
**作用**: 相机品牌选择
**可选值**: `hikrobot`, `mindvision`
**代码位置**: `io/camera.cpp`

#### exposure_ms
```yaml
exposure_ms: 2  # 曝光时间 [ms]
```
**作用**: 相机曝光时间
**调参建议**:
| 光照条件 | exposure_ms | 说明 |
|----------|-------------|------|
| 室外强光 | 0.5~1 | 避免过曝 |
| 室内正常 | 2~3 | **默认** |
| 弱光 | 5~10 | 提高亮度 |

**注意事项**:
- 曝光时间过长 → 运动模糊
- 曝光时间过短 → 图像暗

#### gain
```yaml
gain: 15  # 增益 [dB]
```
**作用**: 信号放大倍数
**调参建议**:
| 范围 | 效果 |
|------|------|
| 0~10 | 低噪声，亮度低 |
| 10~16 | **平衡** |
| 16~20 | 亮度高，噪声大 |

**调参策略**: 先调曝光，再调增益

#### vid_pid
```yaml
vid_pid: "2bdf:0001"  # USB设备ID
```
**作用**: 指定USB相机设备
**格式**: `vendor_id:product_id` (十六进制)
**查询方法**:
```bash
lsusb | grep -i camera
# 输出示例: Bus 001 Device 005: ID 2bdf:0001
```

### USB相机参数

#### image_width / image_height
```yaml
image_width: 1280
image_height: 720
```
**作用**: USB相机分辨率
**调参建议**:
| 分辨率 | 帧率 | 适用场景 |
|--------|------|----------|
| 640x480 | >200fps | 高速运动 |
| 1280x720 | 100~120fps | **平衡** |
| 1920x1080 | 30~60fps | 高精度 |

#### fov_h / fov_v
```yaml
fov_h: 87.7   # degree, 水平视场角
fov_v: 56.7   # degree, 垂直视场角
```
**作用**: 相机视场角
**用途**: 坐标变换和畸变校正

#### new_fov_h / new_fov_v
```yaml
new_fov_h: 67  # degree, 校正后水平视场角
new_fov_v: 40.9 # degree, 校正后垂直视场角
```
**作用**: 畸变校正后的有效视场角
**说明**: 畸变校正会损失部分视野

#### usb_frame_rate
```yaml
usb_frame_rate: 120  # fps
```
**作用**: USB相机目标帧率
**调参建议**: 根据分辨率设置

#### usb_exposure
```yaml
usb_exposure: 315  # exposure time [μs]
```
**作用**: USB相机曝光时间(微秒)
**调参建议**:
```python
# 换算关系
exposure_ms = usb_exposure / 1000

# 典型值
125  → 0.125ms (强光)
250  → 0.25ms  (正常)
500  → 0.5ms   (室内)
1000 → 1ms     (弱光)
```

#### usb_gamma
```yaml
usb_gamma: 160  # gamma值 [0-255]
```
**作用**: gamma校正
**调参建议**:
| 值 | 效果 |
|----|------|
| 100 | 线性 |
| 160 | **默认** |
| 200 | 高对比度 |

#### usb_gain
```yaml
usb_gain: 10  # [0-96]
```
**作用**: USB相机增益
**调参建议**: 保持10~20，避免噪声

---

## 通信参数

### CAN总线参数

#### quaternion_canid
```yaml
quaternion_canid: 0x100  # 云台姿态CAN ID
```
**作用**: 接收云台姿态(四元数)的CAN ID
**代码位置**: `io/cboard/`

#### bullet_speed_canid
```yaml
bullet_speed_canid: 0x101  # 弹速CAN ID
```
**作用**: 接收弹丸速度的CAN ID
**说明**: 用于弹道补偿

#### send_canid
```yaml
send_canid: 0xff  # 发送CAN ID
```
**作用**: 发送云台控制指令的CAN ID

#### can_interface
```yaml
can_interface: "can0"  # CAN接口名称
```
**作用**: SocketCAN接口
**查询方法**:
```bash
ip link show
# 输出: can0: <NOARP,UP,LOWER_UP>
```

### 串口参数

#### com_port
```yaml
com_port: "/dev/ttyACM0"  # 串口设备路径
```
**作用**: 云台通信串口
**代码位置**: `io/gimbal/gimbal.cpp`
**查询方法**:
```bash
ls /dev/tty*
# 常见: /dev/ttyACM0, /dev/ttyUSB0
```

---

## 标定参数

### 相机内参

#### camera_matrix
```yaml
camera_matrix: [fx, 0, cx, 0, fy, cy, 0, 0, 1]
```
**作用**: 相机内参矩阵
**格式**: 3x3矩阵，行优先存储
**说明**:
```
| fx  0   cx |
| 0   fy  cy |
| 0   0   1  |

fx, fy: 焦距(像素)
cx, cy: 主点坐标
```
**标定工具**: `./build/calibrate_camera`

#### distort_coeffs
```yaml
distort_coeffs: [k1, k2, p1, p2, k3]
```
**作用**: 畸变系数
**说明**: 径向畸变(k1,k2,k3) + 切向畸变(p1,p2)

### 外参标定

#### R_camera2gimbal
```yaml
R_camera2gimbal: [r11, r12, r13, r21, r22, r23, r31, r32, r33]
```
**作用**: 相机到云台的旋转矩阵
**格式**: 3x3旋转矩阵，行优先
**代码位置**: `tasks/auto_aim/aimer.cpp`
**标定工具**: `./build/calibrate_handeye`

#### t_camera2gimbal
```yaml
t_camera2gimbal: [tx, ty, tz]
```
**作用**: 相机到云台的平移向量[m]
**说明**: 相机原点在云台坐标系中的位置

#### R_gimbal2imubody
```yaml
R_gimbal2imubody: [1, 0, 0, 0, 1, 0, 0, 0, 1]
```
**作用**: 云台到IMU本体的旋转矩阵
**说明**: 通常为单位矩阵

---

## 能量机关参数

### buff_detector参数

#### model
```yaml
model: "assets/yolo11_buff_int8.xml"
```
**作用**: 能量机关检测模型路径
**格式**: OpenVINO IR格式(INT8量化)

### buff_aimer参数

#### fire_gap_time
```yaml
fire_gap_time: 0.700  # s, 射击间隔
```
**作用**: 连续射击的最小时间间隔
**说明**: 能量机关模式下的射频控制

#### predict_time
```yaml
predict_time: 0.120  # s, 预测时间
```
**作用**: 打击预测点的时间提前量
**调参建议**:
```python
# 考虑因素：
# 1. 子弹飞行时间 ~0.1s
# 2. 系统延迟 ~0.02s
predict_time = 0.1 + 0.02 = 0.12s
```

---

## 不同机器人配置对比

### 标准步兵 (standard3/4.yaml)

```yaml
# 特点: 平衡性能
max_yaw_acc: 50
decision_speed: 7~8
high_speed_delay_time: 0.04
min_detect_count: 5
max_temp_lost_count: 15
```

### 哨兵 (sentry.yaml)

```yaml
# 特点: 高速旋转，长丢失容忍
max_yaw_acc: 50
decision_speed: 10  # 更高，适应快速旋转
high_speed_delay_time: 0.026  # 更短，响应更快
min_detect_count: 5
max_temp_lost_count: 25  # 更长
outpost_max_temp_lost_count: 75
```

### 无人机 (uav.yaml)

```yaml
# 特点: 灵活机动，快速响应
max_yaw_acc: 80  # 更高加速度
decision_speed: 12  # 更高阈值
high_speed_delay_time: 0.005  # 极短延迟
min_detect_count: 5
max_temp_lost_count: 15
```

---

## 参数调参优先级

### 高优先级 (必须调整)

```yaml
# 1. 检测参数
enemy_color: "blue"  # 根据规则
min_confidence: 0.8   # 根据环境

# 2. 标定参数
R_camera2gimbal: [...]  # 必须标定
t_camera2gimbal: [...]
camera_matrix: [...]

# 3. 瞄准偏置
yaw_offset: 0
pitch_offset: 1

# 4. 射击参数
judge_distance: 2
auto_fire: true
```

### 中优先级 (优化性能)

```yaml
# 1. MPC参数
max_yaw_acc: 50  # 根据硬件性能
fire_thresh: 0.0035

# 2. 跟踪参数
min_detect_count: 5
max_temp_lost_count: 15

# 3. 相机参数
exposure_ms: 2
gain: 15
```

### 低优先级 (微调优化)

```yaml
# 1. 传统检测参数
threshold: 150
max_angle_error: 45

# 2. 权重参数
Q_yaw: [9e6, 0]
R_yaw: [1]
```

---

## 参数调参流程

### 初始配置

```bash
# 1. 复制模板配置
cp configs/standard3.yaml configs/my_robot.yaml

# 2. 设置基本参数
# - enemy_color
# - camera_name
# - auto_fire

# 3. 运行标定
./build/calibrate_camera
./build/calibrate_handeye

# 4. 更新标定参数
# - camera_matrix
# - distort_coeffs
# - R_camera2gimbal
# - t_camera2gimbal
```

### 性能调优

```bash
# 1. 调整检测参数
# - min_confidence
# - threshold
# - roi

# 2. 调整MPC参数
# - max_yaw_acc (基于实测)
# - fire_thresh (基于命中率)

# 3. 调整跟踪参数
# - min_detect_count (基于锁定速度)
# - max_temp_lost_count (基于遮挡情况)
```

### 验证测试

```bash
# 1. 静态测试
./build/auto_aim_test

# 2. 动态测试
./build/standard_mpc

# 3. 压力测试
./build/auto_aim_test --stress --duration=60
```

---

## 常见参数问题

### 问题1: 检测不到目标

**排查参数**:
```yaml
min_confidence: 0.8  # 是否过高？
threshold: 150       # 是否不匹配光照？
use_traditional: true  # 是否启用后备方案？
```

### 问题2: 频繁误检测

**排查参数**:
```yaml
min_confidence: 0.8  # 是否过低？
min_lightbar_length: 8  # 是否过小？
max_angle_error: 45  # 是否过大？
```

### 问题3: 跟踪不稳定

**排查参数**:
```yaml
min_detect_count: 5  # 是否过小？
max_temp_lost_count: 15  # 是否过小？
```

### 问题4: 射击不准

**排查参数**:
```yaml
yaw_offset: 0  # 是否需要调整？
pitch_offset: 1
fire_thresh: 0.0035  # 是否合适？
high_speed_delay_time: 0.04  # 延迟是否准确？
```

### 问题5: 响应变慢

**排查参数**:
```yaml
max_yaw_acc: 50  # 是否过小？
Q_yaw: [9e6, 0]  # 是否过小？
high_speed_delay_time: 0.04  # 是否过长？
```

---

## 参数文件模板

### 最小化配置模板

```yaml
# 必须配置的参数
enemy_color: "blue"
camera_name: "hikrobot"
exposure_ms: 2
gain: 15

# 标定参数(必须)
camera_matrix: [...]
distort_coeffs: [...]
R_camera2gimbal: [...]
t_camera2gimbal: [...]

# 基本参数
min_confidence: 0.8
auto_fire: true
yaw_offset: 0
pitch_offset: 0
```

### 完整配置模板

参考 `configs/standard3.yaml`

---

## 总结

### 参数分类总结

| 类别 | 参数数量 | 重要性 |
|------|----------|--------|
| 标定参数 | 6 | ⭐⭐⭐ |
| 检测参数 | 15 | ⭐⭐⭐ |
| 跟踪参数 | 3 | ⭐⭐ |
| 控制参数 | 12 | ⭐⭐⭐ |
| 硬件参数 | 10 | ⭐⭐ |

### 调参原则

1. **标定先行**: 标定参数必须准确
2. **检测为王**: 检测参数影响全局
3. **渐进调优**: 从默认值逐步调整
4. **数据驱动**: 用测试结果指导调参
5. **文档记录**: 记录每次调整的原因和效果

### 快速参考

**关键参数位置**:
- 检测: `yaml["min_confidence"]` → `detector.cpp`
- 跟踪: `yaml["min_detect_count"]` → `tracker.cpp`
- 瞄准: `yaml["yaw_offset"]` → `planner.cpp`
- 射击: `yaml["auto_fire"]` → `shooter.cpp`
- MPC: `yaml["max_yaw_acc"]` → `planner.cpp`
- 相机: `yaml["exposure_ms"]` → `camera.cpp`