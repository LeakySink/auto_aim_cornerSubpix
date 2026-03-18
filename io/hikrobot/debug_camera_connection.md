# 海康相机连接问题排查清单

## 紧急修复方案

### 1. 增加daemon重试间隔（临时方案）

修改 `io/hikrobot/hikrobot.cpp:23`:

```cpp
// 原来：
std::this_thread::sleep_for(100ms);

// 改为：
std::this_thread::sleep_for(1000ms);  // 给相机更多恢复时间
```

### 2. 添加重置失败保护

在daemon线程中添加重置计数器：

```cpp
int reset_count = 0;
while (!daemon_quit_) {
  std::this_thread::sleep_for(1000ms);

  if (capturing_) {
    reset_count = 0;  // 正常工作时重置计数
    continue;
  }

  if (reset_count < 3) {  // 最多连续重置3次
    capture_stop();
    reset_usb();
    capture_start();
    reset_count++;
  } else {
    tools::logger()->error("Camera reset failed 3 times, giving up");
    std::this_thread::sleep_for(5000ms);  // 等待5秒后再试
    reset_count = 0;
  }
}
```

## 根本原因分析

### 可能原因1：USB带宽不足
```bash
# 检查USB速度
lsusb -v -d 2bdf:0001 | grep -E "bcdUSB|bDeviceClass|bDeviceProtocol"

# 应该显示：
# bcdUSB 3.00  (确保是USB 3.0)
```

### 可能原因2：Type-C供电不足
```bash
# 检查USB电流
sudo lsusb -v -d 2bdf:0001 | grep -E "MaxPower"

# 如果显示 500mA，可能是供电问题
# 建议：使用带外部供电的USB集线器
```

### 可能原因3：采集线程异常退出

查看采集线程退出原因，在 `io/hikrobot/hikrobot.cpp:112-116`:

```cpp
ret = MV_CC_GetImageBuffer(handle_, &raw, nMsec);
if (ret != MV_OK) {
  tools::logger()->warn("MV_CC_GetImageBuffer failed: {:#x}", ret);
  break;  // 这里直接break，导致capturing_=false
}
```

**关键错误码**：
- `0x80000013` - USB通信异常
- `0x80000000` - 设备未就绪

### 可能原因4：USB线缆质量问题

```bash
# 测试USB线缆
# 1. 更换已知良好的USB 3.0线缆
# 2. 直接连接到主板USB口，不要通过集线器
# 3. 避免线缆缠绕或靠近电机等干扰源
```

## 调试建议

### 1. 添加详细日志

在 `hikrobot.cpp` 的采集线程中添加：

```cpp
ret = MV_CC_GetImageBuffer(handle_, &raw, nMsec);
if (ret != MV_OK) {
  tools::logger()->error("GetImageBuffer failed: {:#x}, frame: {}/{}",
    ret, frame_count, error_count++);
  if (error_count > 10) break;  // 连续失败10次才退出
}
```

### 2. 监控USB状态

```bash
# 实时监控USB设备
watch -n 0.5 'lsusb -d 2bdf:0001'

# 查看内核日志
sudo dmesg -w | grep -i usb
```

### 3. 检查USB控制器

```bash
# 查看USB设备连接的控制器
lsusb -t

# 确保相机独占一个USB控制器，或与其他高速设备共享
```

## 推荐硬件配置

1. **USB线缆**：海康原装USB 3.0线（带磁环）
2. **USB端口**：直接连接主板USB 3.0口（蓝色）
3. **供电**：如果必须用Type-C，使用带供电的集线器
4. **长度**：线缆不超过1.5米

## 软件优化建议

### 1. 降低帧率测试

在 `hikrobot.cpp:90`:

```cpp
// 原来：MV_CC_SetFrameRate(handle_, 150);

// 临时降低到50fps测试稳定性
MV_CC_SetFrameRate(handle_, 50);
```

### 2. 调整曝光时间

在配置文件中增加曝光时间，减少USB带宽压力：

```yaml
# configs/standard3.yaml
exposure_ms: 5  # 从2ms增加到5ms
```

## 验证方案

### 使用官方工具测试

1. 下载海康MVS官方客户端
2. 连接相机，观察是否稳定
3. 如果官方工具也不稳定，确认是硬件问题

### 使用诊断工具

```bash
# 持续监控相机状态
watch -n 1 './build/diagnose_hik_camera'
```
