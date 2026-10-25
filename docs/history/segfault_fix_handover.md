# 段错误风险修复交接报告

- 日期：2026-10-20
- 基线：`d5b53ed`（审计文档提交，代码未改）
- 结果：`4c0768c`，11 次提交，25 文件，**+805 / −66**
- 依据：`segfault_audit.md`（分三路静态审查 + 逐条回源码复核 + 从 `assets/*.xml` 解析真实模型输出维度）

---

## 1. 一句话总结

修掉了 5 项 P0（其中 3 项当前可达、会真的崩或卡死）与 6 项 P1/高价值 P2。  
**默认行为不变**，全部改动都是"加判据 + 加锁 + 修正错误常量"，没有重写任何算法。

最值得注意的两点：

1. **`limit_rad(±inf)` 是死循环而非崩溃** —— 进程还在、看门狗可能还在喂，但自瞄完全停摆，比段错误更难查。
2. **`Trajectory` 的 NaN 会一路穿透到 `static_cast<int>(NaN)`** —— 这是 UB，x86 上得 `INT_MIN`，时间戳回拨约 35 分钟 → 下一帧 `dt` 巨大 → EKF 的 `dt⁴` 项爆炸 → 推成 `inf` → **正好触发第 1 条的死循环**。两者是同一条故障链的上下游，本次一并堵住。

---

## 2. 逐项改动明细

### 2.1 `tools/math_tools.cpp` — `limit_rad` 死循环 ★P0 当前可达

**commit `a8d8913`**

```cpp
// 原实现
while (angle > CV_PI) angle -= 2 * CV_PI;     // inf - 2π == inf，恒真
while (angle <= -CV_PI) angle += 2 * CV_PI;   // -inf + 2π == -inf，恒真
```

- **触发**：`angle == ±inf`。来源有两条：EKF 发散把 `ekf_.x[6]`(yaw) 推成 inf；`buff_target.cpp` 的 `a / w` 在角频率 `w → 0` 时出 ±inf。
- **后果**：调用线程 100% CPU 空转，主线程停摆。（NaN 反而安全：两个比较对 NaN 都是 false，直接返回并继续污染下游。）
- **改法**：`isfinite` 守卫（非有限降级返回 0）+ `fmod` 做 O(1) 折算。判据写成 `angle <= 0` 时补 2π，以保持与原实现一致的取值区间 **(-π, π]**。
- **配套**：`buff_target.cpp:394` 的发散分支原本只打日志就 `return`，`unsolvable_` 仍停留在此前设的 `false`，下一帧 `predict()` 继续用发散状态算 → 持续产出 inf。补 `unsolvable_ = true;`。
- **验证**：`tests/algo/limit_rad_check.cpp` —— `[-100,100] rad` 内新旧实现**逐位一致**；9 组 NaN/±inf 全部返回有限 0；大角度精度反而提升（x=1e6 时误差 1.39e-06 → 3.39e-11）。

### 2.2 `tasks/omniperception/perceptron.cpp` — 悬垂引用 ★P0 当前可达

**commit `8f733a7`**

```cpp
// 原：usbcam1..4 是构造函数形参（栈上变量）
threads_.emplace_back([&] { parallel_infer(usbcam1, yolo_parallel1_); });
```

`[&]` 捕获的是**形参变量本身的引用**，不是指针值副本。构造函数返回后形参销毁，线程可能之后才被调度 → 从已销毁栈槽读指针 → 野指针 → `cam->read(...)` SIGSEGV。

- **改法**：`[this, usbcamN]` 按值捕获。
- **验证**：`tests/algo/lambda_capture_check.cpp` —— 用真实线程 + `noinline` 栈覆写复现该模式，验证按值捕获在栈污染后仍拿到正确地址。

### 2.3 `tasks/auto_aim/multithread/mt_detector.cpp` — 异步推理 UAF ★P0 当前可达

**commit `2957d98`**

```cpp
auto input = cv::Mat(640, 640, CV_8UC3, cv::Scalar(0,0,0));   // 局部
ov::Tensor input_tensor(ov::element::u8, {1,640,640,3}, input.data);  // 不接管所有权
infer_request.set_input_tensor(input_tensor);
infer_request.start_async();
queue_.push({img.clone(), t, std::move(infer_request)});      // 没存 input
```

`void*` 构造的 `ov::Tensor` **不接管**缓冲区。`push()` 返回后 `input` 析构，推理线程继续读已释放内存。

- **改法**：`infer_request.get_input_tensor()` 拿到 OV 自有张量，`memcpy` 进去。该张量与 `infer_request` 同生命周期。
- 附带加 `rank==4` 与字节数校验。
- **为什么重要**：崩溃点在 OV 内部线程，core dump 里看不到本项目栈帧，是全部条目里最难定位的一条。

### 2.4 `io/cboard.cpp` — CAN 枚举越界 ★P0 当前可达

**commit `911fbcb`**

`mode = Mode(frame.data[2]); shoot_mode = ShootMode(frame.data[3]);` 无范围校验，且未查 `can_dlc`；随后 `MODES[mode]` / `SHOOT_MODES[shoot_mode]` 直接索引。

- **后果**：`std::vector::operator[]` 在 Release（`-DNDEBUG`）下不检查边界 → 把越界内存当 `std::string` 解释 → fmt 拷贝该"字符串" → 从野指针 memcpy → SIGSEGV。
- **改法**：入口校验 `can_dlc >= 6` + 两个枚举范围，非法则丢帧。这样 `mode`/`shoot_mode` 成员**只可能是合法值**，下游全部 5 处索引（`cboard.cpp` 与 4 个主循环）一次性消除越界可能。

### 2.5 模型输出 rank / dtype 未校验 ★P0（换模型/换设备才触发）

**commit `749af73`**

7 处同一模式：`cv::Mat output(output_shape[1], output_shape[2], CV_32F, output_tensor.data());`

- `ov::Shape` 继承 `std::vector<size_t>`，`operator[]` 越界无检查。rank < 3 → 垃圾 size_t → 负数则 `CV_Assert` 抛异常（主循环无 try/catch → SIGABRT），巨大正数则声明超大 Mat、后续 `at<float>()` 裸越界。
- `data()` 非模板重载不做类型检查。若实际是 f16，缓冲区只有一半大小。
- **现状**：已从 `assets/*.xml` 确认 `yolov5=[1,25200,22]`、`yolov8=[1,14,3549]`、`yolo11=[1,50,8400]`、`tup=[1,3549,21]`、`yolo11_buff=[1,17,8400]`，全是 rank=3 + FP32，**故现在不触发**。
- **改法**：新增 `tools/ov_tensor_utils.hpp::to_mat_f32()` 统一校验；失败返回空 Mat。已逐个确认各 `parse()` 都是 `for (r = 0; r < output.rows; ++r)`，`rows==0` 时返回空 list，等价于"本帧无检测结果"。

### 2.6 `aimer.cpp` / `detector.cpp` — 空容器索引

**commit `cf0141b`**

- `aimer.cpp choose_aim_point()`：`auto armor_num = armor_xyza_list.size();` 取了 size 却没用它判空，紧接着 `armor_xyza_list[0]`（共 3 处）。
- `detector.cpp detect(Armor&, img)`：入口直接索引 `armor.points[0..3]`。同文件 `refine_armor_subpix` 已写 `points.size() != 4`，说明作者认同该前提，入口漏了。
- **当前可达性**：`configs/*.yaml` 的 `armor_num` 全部是 3/4，故现在不崩；但 `aimer.cpp:193` 是每帧每目标都走的热路径，配置写错立刻崩。

### 2.7 `decider.cpp` / `hikrobot.*` / `mindvision.cpp` — 决策与相机

**commit `d3037f6`**

| 位置                            | 问题                                                                                                              | 改法                                    |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------- | ------------------------------------- |
| `decider.cpp sort()`          | 比较器读 `a.armors.front()`，但 `armor_filter()` 的 4 个 `remove_if` 可能把 list 清空，其返回值被忽略                                | 排序前 erase-remove 掉空 `DetectionResult` |
| `hikrobot.hpp:37`             | `void * handle_;` 未初始化；`capture_start()` 有 4 条提前返回路径，前三者走完 `handle_` 仍是栈垃圾，守护线程 100ms 后就拿它调 MVS SDK             | `= nullptr` + `capture_stop()` 判空     |
| `hikrobot.cpp capture_stop()` | 三个清理调用"任一失败即 return"，跳过 `CloseDevice`/`DestroyHandle` → 句柄泄漏，守护线程又 `CreateHandle` 累积                            | 依次全部执行，末尾 `handle_ = nullptr`         |
| `hikrobot.cpp:164`            | `type_map.at()` 对未知像素格式抛 `std::out_of_range`，而这段在**采集线程函数体**内 → 异常逃出 `std::thread` → `std::terminate` → SIGABRT | 改 `find()`，未知格式记日志 + 丢帧（仍释放缓冲）        |
| `mindvision.cpp close()`      | 判 `handle_ == -1` 却从不设回 -1，幂等守卫形同虚设                                                                             | 补 `handle_ = -1`                      |
| `mindvision.cpp open()`       | 未检查 `CameraGetCapability` 返回值就读 `sResolutionRange`，失败时是未初始化栈内存 → `width_`/`height_` 垃圾 → 采集线程按此构造 `cv::Mat` 越界  | 检查返回值，失败先 `close()` 再抛                |

> `hikrobot` 那两项在赛场上很常见（相机没插/没认到），是本次**实际收益最高**的一组。

### 2.8 `tools/trajectory.cpp` — NaN 穿透 ★P0 链条上游

**commit `e74667d`**

```cpp
if (v0 <= 0.0 || d <= 0.0) { unsolvable = true; return; }   // 对 NaN 都是 false
...
if (delta < 0.0) return false;                              // 对 NaN 也是 false
```

- **改法**：判据改为 `!(isfinite(x) && x > 0)`。这个写法一次覆盖两类：NaN（`x > 0` 为 false）、**+inf**（`isfinite` 为 false）。
  > 只写 `!(x > 0)` 拦不住 `+inf` —— 这是写测试时实测发现并修掉的。
- 收尾的有限性检查除了置 `unsolvable`，还把 `pitch`/`fly_time` 清零（否则结构体里留着 NaN，调用方忘查标志仍是 UB）。
- **验证**：`tests/algo/trajectory_nan_check.cpp` —— 9 组 NaN/±inf 全拦截；正常弹道未被静默改变：23 m/s 打 5 m 平射 → `pitch 0.0463 rad, t 0.2176 s`；打 10 m 抬 2 m → `0.2922 rad, 0.4540 s`。

### 2.9 `target.cpp` / `yolo11_buff.cpp`

**commit `104ec02`**

- `Target::diverged()` 对空 `ekf_.x` 取 `[8]`/`[9]` → 补 `if (ekf_.x.size() < 11) return true;`（与 `kalman_markers.cpp:43` 的既有写法一致）。
- `yolo11_buff.cpp get_multicandidateboxes()`：`rowRange(NUM_POINTS, 15)` 起点错（应为 5）+ 长度不足（9 而行 12）→ 关键点全错位一行 + 越界读 3 行。改为与同文件 `:196` 一致的 `rowRange(5, 5 + NUM_POINTS * 2)`。该函数当前无调用点（`detect_24`/`detect_debug` 在 `src/` 下无人调用），属潜伏缺陷。

### 2.10 `io/socketcan.hpp` — fd 生命周期

**commit `d5904c7`**

`close()` 判 `socket_fd_ == -1` 却从不重置 → 析构与守护线程重连会 close 同一 fd 两次；而 `close(fd)` 后该 fd 号会被**立刻复用**，第二次可能关掉无关的 socket/文件。`write()` 是 const 方法直接 `::write(socket_fd_, ...)`，成员留着旧值会把 CAN 帧写到不相干目标。

- **改法**：先把 `socket_fd_`/`epoll_fd_` 置 -1 再真正 close；`open()` 里 bind 失败的清理由裸 `::close()` 改为 `close()`。
- **范围**：只消除"重复 close / 关错 fd"。`write()` 与重连线程的**竞态**（需锁或状态机）不在本次内。

### 2.11 MPC 入口并发

**commit `4c0768c`**

| 文件                       | 竞争                                                                                                                                                                           | 改法                                                                                                                    |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `standard_mpc.cpp`       | `nlohmann::json data` 是主线程栈对象，被 `plan_thread` 以 `[&]` 捕获；plan 线程写 `plan_yaw/plan_pitch/markers` 并 `plot(data)`，主线程写 `data["yaw"]`。json 是树形结构，并发改**不同 key** 也会触及共享 object 内部表 | 加 `data_mutex`；`plot()` 传锁内拷贝的 snapshot（持锁不做 IO）                                                                      |
| `auto_aim_debug_mpc.cpp` | 主线程 `armors = yolo.detect(img)` 整体赋值销毁旧节点，plan 线程同时 `for (const auto & a : armors)` 遍历 → 迭代器悬垂                                                                               | 加 `armors_mutex`；主线程在局部 `new_armors` 上完成 detect + `tracker.track`（接收非 const 引用会就地改），再在锁内 `std::move` 替换；plan 线程遍历整体进锁 |



---

## 3. 兼容性

- **对外接口无变更**：没有改任何函数签名、类定义（`Armor`/`Target`/`Aimer`/`Detector` 的公开接口原样）。
- **配置文件无需改动**：本次不涉及 yaml 键。
- **默认行为不变**：所有改动都是"非法输入时提前返回/丢弃"或"修正明显写错的常量"。正常输入下的数值路径未变（`limit_rad`、`trajectory` 有专门的等价性测试证明）。
- **新增一个头文件**：`tools/ov_tensor_utils.hpp`（header-only，`inline` 函数，无新增链接依赖）。

---

## 4. 验证状态与上机步骤

### 本机已验证（4 个测试全绿）

| 测试                                    | 覆盖                        |
| ------------------------------------- | ------------------------- |
| `tests/algo/ekf_update_check.cpp`     | EKF 与显式求逆参考实现一致（既有）       |
| `tests/algo/limit_rad_check.cpp`      | `limit_rad` 语义等价 + 非有限值安全 |
| `tests/algo/lambda_capture_check.cpp` | lambda 捕获生命周期             |
| `tests/algo/trajectory_nan_check.cpp` | 弹道 NaN/inf 拦截 + 正常弹道未变    |

均随 `tests/CMakeLists.txt` 的 `auto_add_executables` 自动构建。

### 本机无法验证（缺 OpenCV / OpenVINO / fmt / nlohmann / 相机 SDK / `linux/can.h`）

`cboard.cpp`、`mt_detector.cpp`、5 个 yolo 文件、`aimer.cpp`、`detector.cpp`、`decider.cpp`、`hikrobot.*`、`mindvision.cpp`、`target.cpp`、`socketcan.hpp`、2 个 MPC 入口 —— 改动已逐行人工核对，**必须在目标机编译确认**：

```bash
cmake -B build && make -C build -j$(nproc)
```

### 上机验收建议（按收益排序）

1. **相机不存在场景**（`hikrobot` 那组改动）：拔掉相机启动，确认不再崩、日志出现 `Not found camera!` 且守护线程持续重试。
2. **CAN 脏数据**：用 `cansend` 发一帧 `data[2]=99` 的弹速帧，确认日志出现 `invalid mode=99 ... frame dropped` 且不崩。
3. **主线程卡死**：跑 30 min 静态录制（含目标丢失/重捕获），观察是否还有 100% CPU 单核占满。这是 `limit_rad` 那条的验收点。
4. **线程竞争**：用 `-fsanitize=thread` 编一版跑 MPC 入口，确认无 race 报告。
5. **整体**：用 `-fsanitize=address,undefined` 编 Debug 跑同一份录包 —— ASan 能把 `mt_detector` 的 UAF 直接报到源码行。

---

## 5. 回滚

```bash
# 整体回滚到修复前
git reset --hard d5b53ed

# 只回滚某一项（例如相机改动）
git revert --no-commit d3037f6 && git commit
```


改动前另有一份文件级备份：`D:/my/ckyf_vision/segfix_backup_20261020_1641`（18 个待改文件的原版）。

---

## 6. 明确未改的项（及理由）

| 项                                             | 理由                                                                                                   |
| --------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `armor.cpp` 6 个构造函数索引 `armor_keypoints[0..3]` | 属**构造契约**（调用方必须传 4 点），YOLO 后处理保证满足。修它要改 6 处构造函数的初始化列表语义（`points` 在初始化列表里已拷贝，函数体内 resize 不生效），风险大于收益。 |
| `thread_safe_queue` 无 close/cancel 语义         | 挂死类问题，需引入关闭协议并改所有生产/消费端，属架构改动。                                                                       |
| `usbcamera` 采集线程持锁 + 析构取同一把锁                  | 同上，需重构锁粒度。                                                                                           |
| `exiter` 信号处理用普通 `bool`                       | 应改 `volatile std::sig_atomic_t`，但影响面广且当前只影响能否干净退出。                                                   |
| `gimbal` 串口 open/close 与 write 不同锁            | 需给串口加读写锁或改状态机。                                                                                       |
| `dm_imu` packed 结构体 `*(float*)&u32` 强转        | 需按字段 `memcpy` 重写解析，改动面较大且当前平台未报错。                                                                    |
| `commandgener` 忙等 / `socketcan` 10µs 轮询       | 性能问题，非崩溃。                                                                                            |

以上仍在 `segfault_audit.md` 的 §4 与 §2 中留有完整分析，可按需另行处理。
