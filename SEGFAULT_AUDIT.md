# 段错误 / 崩溃风险审计（auto_aim_cornerSubpix-main）

- 日期：2026-10-19
- 范围：自有源码 ~18k 行（tasks/、io/、tools/、src/、calibration/），**排除**第三方（io/serial、io/hikrobot/include、io/mindvision/include）
- 方法：分三路并行静态审查 + 对全部高危条目回到源码逐行复核；另从 `assets/*.xml` 解析了各模型的**真实输出维度**用来校验后处理解析
- 状态：**只做分析，未修改任何代码**

## 前置事实（决定了下面严重度的判定）

1. `CMakeLists.txt:6` 是 `CMAKE_BUILD_TYPE=Release` → **`-DNDEBUG` 生效**。这直接导致：
   - **Eigen 的 `eigen_assert` 被关掉** → 矩阵维度不匹配不再断言，变成**裸越界写**
   - **OpenCV 的 `CV_DbgAssert` 被关掉** → `Mat::at<T>()` 越界/类型不符不再抛异常，变成**裸越界读**
   - 注意 `CV_Assert`（不带 Dbg）**不受 NDEBUG 影响**，仍会抛 `cv::Exception`；主循环无 try/catch → 未捕获异常 → `std::terminate` → SIGABRT
2. 我用脚本核对了配置现状：`p0` 长度**全部 = 11**，`armor_num` **全部为 3 或 4**。所以"配置驱动的越界"目前都不会触发，属于潜伏缺陷而非现网 bug——下文按此区分。

---

## 0. 结论速览

| 级别 | 条目 | 位置 | 当前是否可达 |
|---|---|---|---|
| 🔴 **P0** | OpenVINO 异步推理持有已释放的 cv::Mat（use-after-free） | `mt_detector.cpp:52-62` | **是**（多线程检测链路） |
| 🔴 **P0** | `Perceptron` 构造按引用捕获形参 → 悬垂引用 | `perceptron.cpp:26-29` | **是**（`sentry_multithread.cpp:62`） |
| 🔴 **P0** | `limit_rad(±inf)` **死循环**（主线程卡死） | `math_tools.cpp:8-13` | 是（EKF 发散后） |
| 🔴 **P0** | CAN 枚举未校验 → `MODES[mode]` 越界 | `cboard.cpp:97,108` + `standard.cpp:62` | 是（下位机异常值） |
| 🔴 **P0** | 模型输出 rank/类型未校验 → 巨大 Mat 越界 | `yolov5.cpp:97`、`yolotup.cpp:109`、`yolo11_buff.cpp:60` 等 6 处 | 是（换模型时） |
| 🟠 **P1** | 排序比较器对可能为空的 list 调 `front()` | `decider.cpp:169-188` | 是（目标全被过滤时） |
| 🟠 **P1** | HikRobot 未初始化 `handle_` 传给 SDK | `hikrobot.hpp:37` + `hikrobot.cpp:60-83` | 是（相机不存在时） |
| 🟠 **P1** | 多线程并发写同一 `nlohmann::json` | `standard_mpc.cpp:70,117` | 是（MPC 入口） |
| 🟠 **P1** | 遍历 list 的同时另一线程整体赋值 | `auto_aim_debug_mpc.cpp:110-120,140` | 是（MPC 入口） |
| 🟠 **P1** | 线程函数逃逸异常 / USB 相机持锁阻塞 | `hikrobot.cpp:174`、`usbcamera.cpp:61,134` | 特定硬件条件 |
| 🟡 **P2** | `armor_xyza_list[0]` 对空 vector | `aimer.cpp:193,217,252` | **否**（配置 armor_num=3/4），改错即崩 |
| 🟡 **P2** | 6 个 Armor 构造无条件索引 `keypoints[0..3]` | `armor.cpp:63,111,153,194,228,265` | 取决于 YOLO 后处理 |
| 🟡 **P2** | yaml 数组直接喂 Eigen 定长矩阵 | `solver.cpp:34-41` | 改标定/漏配即崩（**启动期**） |
| 🟡 **P2** | `ARMOR_NAMES[name]` 越界 | `detector.cpp:344,444`、`tracker.cpp:156` | 换分类模型即崩 |
| 🟡 **P2** | `rowRange` 越界 3 行（关键点全错位） | `yolo11_buff.cpp:90` | **否**（`detect_24/detect_debug` 无调用点） |
| 🟡 **P2** | `diverged()` 对空 `ekf_.x` 取 `[8]` | `target.cpp:257-259` | 状态机兜底，改状态机即崩 |
| 🟡 **P2** | NaN 穿透弹道解 → `static_cast<int>(NaN)` UB | `trajectory.cpp:22-43` → `aimer.cpp:114` | EKF 发散后 |

---

## 1. P0 — 需要立刻处理的崩溃

### 1.1 OpenVINO 异步推理持有已释放的 `cv::Mat`（use-after-free）

`tasks/auto_aim/multithread/mt_detector.cpp:52-62`

```cpp
auto input = cv::Mat(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
// ... preprocessing into input ...
ov::Tensor input_tensor(ov::element::u8, {1, 640, 640, 3}, input.data);
infer_request.set_input_tensor(input_tensor);
infer_request.start_async();
queue_.push({img.clone(), t, std::move(infer_request)});
```

- **触发**：`push()` 返回后异步推理尚未读完输入（这是异步的正常情况，不是竞态条件）。
- **机理**：用 `void*` 构造的 `ov::Tensor` **不接管** `input.data` 的所有权。函数返回时局部 `input` 析构、缓冲区释放；队列里只存了原图 `img.clone()`，**没有存预处理后的 `input`**。OpenVINO 工作线程继续读已释放内存 → 跨线程 UAF。
- **为什么危险**：这是异步推理的典型错误用法，在负载高、推理耗时长时必现，且崩溃点在 OpenVINO 内部线程，core dump 里看不到本项目栈帧，极难定位。
- **修复**（推荐第一种）：
  ```cpp
  // 方案 A：用 OpenVINO 自有内存，拷进去
  auto in = infer_request.get_input_tensor();
  std::memcpy(in.data<uint8_t>(), input.data, total_bytes);
  // 方案 B：把 input 一起进队列，infer_request.wait() 后再释放
  queue_.push({img.clone(), input.clone(), t, std::move(infer_request)});
  ```

### 1.2 `Perceptron` 构造按引用捕获构造函数形参 → 悬垂引用

`tasks/omniperception/perceptron.cpp:24-29`（调用点 `src/sentry_multithread.cpp:62`）

```cpp
threads_.emplace_back([&] { parallel_infer(usbcam1, yolo_parallel1_); });
threads_.emplace_back([&] { parallel_infer(usbcam2, yolo_parallel2_); });
threads_.emplace_back([&] { parallel_infer(usbcam3, yolo_parallel3_); });
threads_.emplace_back([&] { parallel_infer(usbcam4, yolo_parallel4_); });
```

- **触发**：任意工作线程在构造函数返回**之后**才真正开始跑（线程调度完全允许，且这里有 `sleep_for(2s)` 在前面，实际启动时机更不可控）。
- **机理**：`usbcam1..4` 是构造函数**栈上的形参**。`[&]` 捕获的是这些形参变量的**引用**，不是指针值的副本。构造函数返回后形参销毁，lambda 里 `parallel_infer(usbcam1, ...)` 展开时从一个已销毁的栈槽读取指针值 → 拿到垃圾指针 → `cam->read(...)` 通过野指针调用成员函数 → SIGSEGV。
  - 注意：4 个相机对象本身份命周期比 `Perceptron` 长，但这救不了"捕获形参引用"这个错误。
- **修复**：按值捕获指针（1 行改动）：
  ```cpp
  threads_.emplace_back([this, usbcam1] { parallel_infer(usbcam1, yolo_parallel1_); });
  ```

### 1.3 `limit_rad(±inf)` 死循环 —— 主线程卡死

`tools/math_tools.cpp:8-13`（**我已逐行核对**）

```cpp
double limit_rad(double angle)
{
  while (angle > CV_PI) angle -= 2 * CV_PI;      // inf - 2π == inf，恒真
  while (angle <= -CV_PI) angle += 2 * CV_PI;    // -inf + 2π == -inf，恒真
  return angle;
}
```

- **调用点极多**：`target.cpp:127,155,156,159,202,221,228,250,285,298`、`aimer.cpp:161,181,201`、`buff_target.cpp:452` 等。
- **触发**：`angle == ±inf`。来源包括
  - EKF 发散使 `ekf_.x[6]`(yaw) 变成 inf；
  - `buff_target.cpp:452` 的 `a / w`，当 EKF 把角频率 `w = ekf_.x[8]` 推到 0 附近 → ±inf；该文件的发散检测分支（`buff_target.cpp:394-400`）**只打了日志就 return，没有把 `unsolvable_` 置回 true**，所以下一帧照常调用 `predict()`。
- **机理**：`while (inf > CV_PI)` 永远为真 → **100% CPU 空转**，主线程停摆。现象上等同死机（比崩溃更难排查）。
  - 注意 **NaN 反而安全**（两个比较对 NaN 都返回 false，直接返回 NaN 继续污染下游）。
- **修复**：
  ```cpp
  double limit_rad(double angle)
  {
    if (!std::isfinite(angle)) return 0.0;
    angle = std::fmod(angle + CV_PI, 2 * CV_PI);
    if (angle < 0) angle += 2 * CV_PI;
    return angle - CV_PI;
  }
  ```
  同时给 `buff_target.cpp:394` 的发散分支补 `unsolvable_ = true;`。

### 1.4 CAN 枚举值未校验 → `MODES[mode]` 越界

`io/cboard.cpp:95-108`（**已核对**）+ `io/cboard.hpp:26,35` + 下游 `src/standard.cpp:62`

```cpp
mode = Mode(frame.data[2]);            // 未检查取值范围
shoot_mode = ShootMode(frame.data[3]);
// ...
bullet_speed, MODES[mode], SHOOT_MODES[shoot_mode], ft_angle);   // ← 立刻用 [] 索引
```
```cpp
const std::vector<std::string> MODES = {"idle","auto_aim","small_buff","big_buff","outpost"};  // 5 个
const std::vector<std::string> SHOOT_MODES = {"left_shoot","right_shoot","both_shoot"};        // 3 个
```
主循环再次无保护：`if (last_mode != mode) { log("INFO", "Switch to {}", io::MODES[mode]); }`

- **触发**：收到 `can_id == bullet_speed_canid_` 的帧且 `data[2] ∉ [0,4]` 或 `data[3] ∉ [0,2]`。下位机发错、版本不匹配、总线上有噪声帧都会命中。另外**没有检查 `can_dlc >= 6`**，短帧复用到的旧字节也是非法协议值。
- **机理**：`std::vector::operator[]` 无边界检查 → 拿到一段被 reinterpret 成 `std::string` 的垃圾（含野指针），fmt 格式化时**拷贝该 string** → 从野地址 memcpy → SIGSEGV。
- **修复**：
  ```cpp
  if (frame.can_dlc < 6) return;
  if (frame.data[2] >= MODES.size() || frame.data[3] >= SHOOT_MODES.size()) return;
  ```
  主循环侧也应加范围判断，不要依赖底层永远正确。

### 1.5 模型输出 rank / 类型未校验

`yolov5.cpp:97`、`yolotup.cpp:109`、`yolov8.cpp:101`、`yolo11.cpp:97`、`yolo11_buff.cpp:60-61`、`mt_detector.cpp:73,91`（6 处同一模式）

```cpp
auto output_shape = output_tensor.get_shape();
cv::Mat output(output_shape[1], output_shape[2], CV_32F, output_tensor.data());
```

- **rank < 3**：`ov::Shape` 继承 `std::vector<size_t>`，`operator[]` **无边界检查**（Release）。读到垃圾 `size_t` 交给 `cv::Mat(rows, cols, ...)`：
  - 负数 → `CV_Assert` 抛异常 → 未捕获 → SIGABRT
  - 巨大正数 → 断言通过，得到声明尺寸巨大的 Mat，紧接着 `output.at<float>(r, c)` 是**裸越界读** → SIGSEGV
- **非 f32**：`Tensor::data()` 这个非模板重载返回裸 `void*` 且**不做类型检查**。若是 f16，缓冲区只有一半大小，访问尾部时会越界 100%。`configs/ascento.yaml:9` 是 `device: GPU`，`classifier.cpp` 用 `"AUTO"` 编译 —— 这两处最可能踩到。
- 我从 `assets/*.xml` 读到的真实输出：`yolov5=[1,25200,22]`、`yolov8=[1,14,3549]`、`yolo11=[1,50,8400]`、`tup=[1,3549,21]`、`yolo11_buff=[1,17,8400]` —— **当前都是 FP32 且 rank=3，所以现在安全**，但换模型/NPU 插件即崩。
- **修复**（6 处统一）：
  ```cpp
  if (output_shape.size() != 3 ||
      output_tensor.get_element_type() != ov::element::f32) return {};
  ```

---

## 2. P1 — 高概率崩溃

| # | 位置 | 问题 | 触发条件 |
|---|---|---|---|
| 2.1 | `decider.cpp:169-188` | 排序比较器对 `DetectionResult` 的 list 调 `.front()`，但 `armor_filter()` 可能把该 list 清空 | 两个以上检测结果，且其中某个的装甲板**全部**被过滤（全友方 / 5号 / 前哨站 / 无敌）→ 空 list `front()` 是 UB |
| 2.2 | `hikrobot.hpp:37` + `hikrobot.cpp:60-83,181-204` | `void * handle_;` **未初始化**，`capture_start()` 失败提前返回后，守护线程仍用 `handle_` 调 `MV_CC_StopGrabbing/CloseDevice/DestroyHandle` | 相机不存在 / 枚举失败 / CreateHandle 失败 → 把垃圾指针传进闭源 SDK |
| 2.3 | `standard_mpc.cpp:70,72-91,117-119` | 计划线程与主线程无锁并发写同一个 `nlohmann::json data` | AUTO_AIM 模式且有目标时高频触发 → 树节点修改竞争，堆损坏/UAF |
| 2.4 | `auto_aim_debug_mpc.cpp:110-120,140-145` | 计划线程遍历 `armors`，同时主线程 `armors = yolo.detect(img)` **整体赋值**（会销毁旧节点） | 两线程时序重叠 → 迭代器悬垂 → UAF/段错误 |
| 2.5 | `hikrobot.cpp:174` | `type_map.at(pixel_type)` 用 `at()`，不支持的格式抛 `std::out_of_range`；采集线程 lambda 无 try/catch | 相机被配成 Mono8/RGB/Bayer10 等不支持格式 → 异常逃出 `std::thread` → `std::terminate` |
| 2.6 | `dm_imu.cpp:78-91` | packed 结构体字段做 `*(float*)&u32` 强转，违反对齐 + strict aliasing | 严格对齐平台 → SIGBUS；Release 优化下也可能误编译 |
| 2.7 | `socketcan.hpp:135-154` | `close()` 未把 `socket_fd_` 置 -1；`write()` 与重连线程竞争同一 fd | 重连时可能与 `write()` 并发；重复 close 同一数字可能关掉被复用的无关 fd |
| 2.8 | `gimbal.cpp:99-103,234-250` | 接收线程 `serial_.close()/open()` 与发送线程 `serial_.write()` **不在同一把锁下**（串口库 `open/close` 不取 write 锁） | 错误累计触发重连的同时有线程在发送 → 向已关闭/被复用的 fd 写入 |

**其余 P1 提示**：`mindvision.cpp:62-77`（`close()` 不重置 `handle_ = -1`，重复 UnInit 已释放句柄；`CameraGetCapability` 返回值未检查却读取其成员）。

---

## 3. P2 — 潜伏缺陷（当前配置下不触发，改动即崩）

### 3.1 `armor_xyza_list[0]` 对空 vector —— 配置改错即崩

`tasks/auto_aim/aimer.cpp:193, 217, 252`（**已核对**）

```cpp
auto armor_num = armor_xyza_list.size();     // size 已取出
if (!target.jumped) return {true, armor_xyza_list[0]};   // 却没用来判空
```
`Target::armor_xyza_list()`（`target.cpp:245-254`）是 `for (i = 0; i < armor_num_; i++)`，`armor_num_ <= 0` 时**返回空 vector** → `data() == nullptr` → `operator[](0)` 解引用地址 0 → SIGSEGV。

> **当前安全**：我核对了全部 `configs/*.yaml`，`armor_num` 只有 3 和 4。193 行是**每帧每个未跳变目标**都走的热路径，一旦有人把配置写成 0（新增机器人类型漏填、脚本生成配置默认值）立刻崩。
> **修复**：函数头加 `if (armor_xyza_list.empty()) return {false, Eigen::Vector4d::Zero()};`，或构造时 `armor_num_ = std::clamp(n, 1, 8)`。

### 3.2 Armor 构造无条件索引 4 个关键点

`tasks/auto_aim/armor.cpp:63, 111, 153, 194, 228, 265`（**已核对其中 3 处**）

```cpp
center = (armor_keypoints[0] + armor_keypoints[1] + armor_keypoints[2] + armor_keypoints[3]) / 4;
```
`size() < 4` 时：空 vector → 解引用 nullptr；`size()==2` → `[2]/[3]` 越界读 8 字节结构体。同时 `detector.cpp:134-137` 的 `Detector::detect(Armor&, img)` 入口也直接索引 `armor.points[0..3]`。

> 同一个文件里 `refine_armor_subpix`（`detector.cpp:251`）明明写了 `points.size() != 4` 检查，说明作者知道这个前提，但入口没有。
> **修复**：`Armor` 各构造函数首行补 `if (armor_keypoints.size() < 4) armor_keypoints.resize(4);`；`detector.cpp:133` 前补 `if (armor.points.size() != 4) return false;`

### 3.3 yaml 数组直接喂 Eigen 定长矩阵

`tasks/auto_aim/solver.cpp:34-41`（**已核对**）

```cpp
R_gimbal2imubody_ = Eigen::Matrix<double,3,3,Eigen::RowMajor>(R_gimbal2imubody_data.data());
Eigen::Matrix<double,1,5> distort_coeffs(distort_coeffs_data.data());
```
`Eigen::Matrix<T,R,C>(double*)` 是**无长度参数**的构造函数，会无条件从指针连续加载 R×C 个 double。

> **崩溃位置很特殊：在 `Solver` 构造期（进程启动阶段）**。空 vector 的 `.data()` 是 nullptr → 解引用地址 0；元素不足 → 堆尾越界读。改标定、换相机漏同步 `camera.yaml` 即触发。
> **修复**：加载前逐个校验 `size()`（9/9/3/9/5），不匹配直接抛明确异常。

### 3.4 `ARMOR_NAMES[name]` 越界

`armor.hpp:39-40` 的 `ARMOR_NAMES` 只有 9 项，而 `classifier.cpp:58/111` 是 `armor.name = static_cast<ArmorName>(label_id)`（`label_id` 完全由模型输出维度决定），`armor.cpp:176-178` 的 `ArmorName(num_id)` 也没有范围校验。越界值传到 `detector.cpp:344,444`、`tracker.cpp:156` 的 `ARMOR_NAMES[...]` → fmt 拷贝野 string → SIGSEGV。

> 换 ONNX 模型或重训时类别数改变即触发。**修复**：`std::clamp(label_id, 0, ARMOR_NAMES.size()-1)`。

### 3.5 `yolo11_buff.cpp:90` 的 rowRange 越界 + 关键点全错位

```cpp
// :90  get_multicandidateboxes —— 错误
cv::Mat kpts = det_output.col(i).rowRange(NUM_POINTS, 15);          // rowRange(6, 15) → 9 行
// :196 get_onecandidatebox     —— 正确（同一份逻辑的另一写法）
cv::Mat kpts = det_output.col(best_index).rowRange(5, 5 + NUM_POINTS*2);  // rowRange(5, 17) → 12 行
```

`NUM_POINTS = 6`（`yolo11_buff.hpp:40`），模型输出 `[1, 17, 8400]`。

- **起点错**：前 5 行是 box(4)+conf(1)，应从**第 5 行**开始，写成 6 导致所有关键点错位一行（把上一个点的 y 当下一个点的 x）。
- **长度错**：6 个点需要 **12** 行，实际只取到 9 行。循环 `j=0..5` 访问 index `0..11`，**index 9/10/11 越界**；`j=5` 的第二次访问落到 `det_output` 第 17 行，而它只有 0..16 → 越界最多约 33 KB。
- **当前不可达**：我已核实 `get_multicandidateboxes` 只被 `detect_24`（`buff_detector.cpp:89`）和 `detect_debug`（`:162`）调用，这两个函数**在 `src/` 下无任何调用点**；活跃路径 `Buff_Detector::detect`（`:125`，被 `mt_standard.cpp:117` 调用）用的是正确的 `get_onecandidatebox`。
- **修复**：`:90` 改成与 `:196` 一致的 `rowRange(5, 5 + NUM_POINTS * 2)`。

### 3.6 NaN 穿透弹道解 → 时间戳 UB

`tools/trajectory.cpp:22-43` → `tasks/auto_aim/aimer.cpp:114`

```cpp
if (delta < 0.0) return false;              // NaN < 0.0 为 false → 放行
...
return true;                                 // 明明无解却返回 true
...
std::chrono::microseconds(static_cast<int>(prev_fly_time * 1e6));   // static_cast<int>(NaN) 是 UB
```
`Trajectory` 构造只挡 `v0 <= 0 || d <= 0`，**这两个判断对 NaN 都是 false**，NaN 一路穿过 `solve_no_drag`。x86 下 `static_cast<int>(NaN)` = `INT_MIN` → 时间戳被回拨约 35 分钟 → 下一帧 `dt` 变成巨大值 → EKF 的 `dt⁴` 项爆炸 → 进一步喂出 inf → **触发 §1.3 的死循环**。

> **修复**：`trajectory.cpp` 补 `isfinite` 检查；构造函数把 `v0 <= 0` 改成 `!(v0 > 0.0)`（对 NaN 也拦截）。

### 3.7 `diverged()` 对空 `ekf_.x` 取 `[8]`

`target.cpp:257-259`（**已核对**）。默认构造的 `Target` 的 `ekf_.x` 是 `size()==0`、`data()==nullptr`，`x[8]` 解引用地址 64 → SIGSEGV。

> 当前由状态机兜底（`state_` 只在 `set_target` 后才离开 `"lost"`，而 `set_target` 必然赋值 `target_`），**不可达**，但属于"靠不变量保命"——改状态机即崩。`kalman_markers.cpp:43` 写了 `if (x.size() < 11)` 说明 codebase 里确实预期过短 x 的存在。
> **修复**：`diverged()` 开头 `if (ekf_.x.size() < 11) return true;`

### 3.8 未初始化的构造函数

`target.cpp:53-67` 的第二个构造函数只初始化了 `armor_num_` 和 `ekf_`，**未初始化** `name`、`t_`、`update_count_`、`ekf_params_` 等。其中 `t_` 默认为 epoch(0)，第一次 `predict(t)` 会得到 `dt ≈ 系统启动至今秒数` → `Q` 的 `dt⁴` 项巨大 → EKF 一帧打飞。（当前只在 `tests/` 使用，生产路径未用。）

另外 `buff_aimer.cpp:35`、`planner.cpp:51,193` 都是先声明未初始化的 `double yaw/pitch;` 或 `Eigen::Vector3d xyz;` 再可能被跳过赋值。

---

## 4. 挂死类（非 SIGSEGV，但同样致命）

- **`tools/thread_safe_queue.hpp:39-71` 没有 close/cancel 语义**：等待谓词只有 `!queue_.empty()`。启动时收不到两帧 IMU、相机掉线后不再入队、退出时生产线程阻塞在 `camera.read()` → `pop()` / `join()` **永久阻塞**。`clear()` 虽然 notify 了但谓词仍为假。
- **`io/usbcamera/usbcamera.cpp:61-69,134-142`**：采集线程在 `cap_.read()` 外面持 `cap_mutex_`；析构要取同一把锁才能 `close()` → V4L 驱动卡住时**无法通过关闭设备中断读操作**，析构挂死。
- **`tools/exiter.cpp:8-19`**：信号处理器写普通 `bool`（未用 `volatile std::sig_atomic_t`）且与各线程无同步 → 线程可能常驻看不到退出标志。
- **`tasks/auto_aim/multithread/commandgener.cpp:18-25,37-40`**：写 `stop_` 持锁但读不持锁，且 `cv_` 被 notify 而工作线程从不 wait → 理论可挂死。

---

## 5. 已核查确认**没有问题**的点（避免误报）

对这些位置我逐一推演过，排除嫌疑：

- `aimer.cpp:199-203` 的 `delta_angle_list[i]`：push_back 次数与循环上界同为 `armor_num`，size 恒一致
- `aimer.cpp:228` 的 `armor_xyza_list[lock_id_]`：`lock_id_` 只取到 `id0/id1`（∈ `[0,armor_num)`）或 -1 后立即重赋值，**不会用 -1 索引**；跨目标残留也不会越界（我专门推演了目标切换场景）
- `detector.cpp:93-119` 去重：只改元素不改容器，`std::list` 无迭代器失效
- `target.cpp:150` 的 `xyza_list[i]`：容器与上界同为 `armor_num_`
- 各 yolos 的 NMS 结果容器：`ids/confidences/boxes` 同一轮同步 `emplace_back`，配对永远一致；且用的是 `NMSBoxes(..., indices)` 引用型重载，OpenCV 4.x 正确用法
- `buff_type.hpp:59` 的 `fanblades[0]`：`PowerRune` 构造保证非空，且 `buff_solver.cpp:60` 有 `!has_value()` 提前返回
- `tinympc` 接口维度：`HORIZON=100`、`Xref` 是 `2×100`、`u` 是 `1×99`，与 `planner.cpp` 的赋值**精确匹配**
- 全部 `GRID_SIZES/STRIDES`：`2704+676+169 = 3549`，与 TUP 输出行数精确相等
- `MT/USBCamera/SocketCAN` 的 `bool quit_/ok_`：跨线程读写非 atomic —— 这属于 UB 但目前只影响能否干净退出，单列为「 Shelf 风险」而非必然崩溃

---

## 6. 建议修复顺序

| 顺序 | 动作 | 代价 |
|---|---|---|
| 1 | `math_tools.cpp:8` 换 `fmod` + `isfinite`；`buff_target.cpp:394` 补 `unsolvable_` | 3 行，堵住**卡死** |
| 2 | `perceptron.cpp:26-29` `[&]` → `[this, usbcamN]` | 4 行，堵住悬垂引用 |
| 3 | `mt_detector.cpp` 输入张量改用 OV 自有内存 | ~5 行，堵住异步 UAF |
| 4 | `cboard.cpp:95-108` 加 `can_dlc` 与枚举范围校验 | 3 行，挡住下位机脏数据 |
| 5 | 6 处 `output_shape` 加 rank + f32 校验 | 每处 3 行，换模型不再崩 |
| 6 | `decider.cpp` 过滤后剔除空结果；`aimer.cpp:189` 加判空；`detector.cpp:132` 加 size 检查 | 各 1 行 |
| 7 | `hikrobot.hpp:37` `handle_(nullptr)` + 状态化 open/close；`mindvision` close 后 `handle_=-1` | 中等，但相机断连是赛场上最常见故障 |
| 8 | `standard_mpc.cpp` / `auto_aim_debug_mpc.cpp` 的并发 json 与 list 改为快照传值 | 中等 |
| 9 | `solver.cpp` 加配置尺寸校验；`classifier.cpp` 加 clamp | 小 |

**验证建议**：优先级 1~2 的改动上机后跑 30 min 静态录制（含目标丢失/重捕获），观察是否还有主线程卡死；优先用 `-fsanitize=address,undefined` 编一版 Debug 跑同一份录包，ASan 能把 §1.1、§1.2、§2.4 的野指针直接报到源码行。
