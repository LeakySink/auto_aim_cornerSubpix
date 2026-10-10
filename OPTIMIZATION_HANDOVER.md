# auto_aim_cornerSubpix-main 修改交接报告

- 日期：2026-10-11
- 目标：阅读理解项目中弹道解算相关实现（ballistic-main / rm_auto_aim-main / auto_aim_cornerSubpix-main）后，对 `auto_aim_cornerSubpix-main` 做定点优化
- 原则：**保持原有功能与对外接口兼容，默认行为不变**，不重写未验证逻辑
- 改动前完整备份：`D:/my/ckyf_vision/auto_aim_cornerSubpix_backup_20261011_0005`（回滚方式见文末）

---

## 1. 改动清单总览

| # | 文件 | 类型 | 一句话说明 |
|---|---|---|---|
| 1 | `tools/extended_kalman_filter.cpp` | 修 bug + 性能 | 卡方阈值 0.711 → 9.488/19.675；3 次显式矩阵求逆 → LDLT 求解 |
| 2 | `tasks/auto_aim/detector.cpp` | 精度 + 性能 + 能力 | 复用灰度图、亚像素漂移防护、灯条端点同步、去整像素截断、新增可选 ROI 亚像素 |
| 3 | `tasks/auto_aim/detector.hpp` | 接口同步 | 成员函数签名/新增声明（**唯一的对外可见签名变更**，见 §4） |
| 4 | `tasks/auto_aim/aimer.cpp` | 性能 + 鲁棒 | 惰性构造迭代副本、未收敛诊断、yaw 限幅防 CAN 溢出 |
| 5 | `io/cboard.cpp` | 鲁棒 | IMU 插值除零（NaN 四元数）防护 + 插值系数限幅 |
| 6 | `configs/standard3.yaml` | 配置显式化 | 补 `use_corner_subpix`、`drag_coefficient_up/down/boundary` |
| 7 | `tests/algo/ekf_update_check.cpp` | **新增** | EKF 数值回归测试（自动纳入 CMake 构建） |

除上述文件外无任何其他文件被修改（`diff -rq` 比对备份目录已确认）。

---

## 2. 逐文件修改明细

### 2.1 `tools/extended_kalman_filter.cpp`（推荐优先 review）

**① 卡方检验阈值错误（原 L65-67）→ 现 L67-71**

```cpp
// 原：注释写"自由度=4，取置信水平95%"，实际取 0.711
constexpr double nis_threshold = 0.711;   // 实为自由度4的 50% 分位数
constexpr double nees_threshold = 0.711;
// 现：
constexpr double nis_threshold = 9.488;   // NIS：自由度=观测维数4，95%
constexpr double nees_threshold = 19.675; // NEES：自由度=状态维数11，95%
```

影响链路：`nis_fail` → `recent_nis_failures` → `tracker.cpp:104-110` 失败率 >40% → 判 "Bad Converge" → **重置目标为 lost**。旧阈值下约一半更新会被误判为失败，存在把正常目标误丢的风险。

**② 三次显式求逆 → LDLT 求解（原 L50, L62-63）→ 现 L49-52, L64-65**

```cpp
// 原：K 用 (HPHᵀ+R).inverse()；NIS 再求 S.inverse()；NEES 再求 P.inverse()（11×11）
// 现：
Eigen::MatrixXd S_prior = H * P * H.transpose() + R;
Eigen::MatrixXd K = S_prior.ldlt().solve(H * P).transpose();   // K = P Hᵀ S⁻¹（数学等价）
double nis  = (residual.transpose() * S.ldlt().solve(residual)).value();
double nees = ((x - x_prior).transpose() * P.ldlt().solve(x - x_prior)).value();
```

**其余逻辑（Joseph 协方差更新、`x_add`、残差定义、后验协方差取 S）完全未动**，仅替换求解方式。

### 2.2 `tasks/auto_aim/detector.cpp`

| 位置（现） | 修改 |
|---|---|
| L31-33 | 构造函数新增可选读取 `use_corner_subpix`（缺省 false） |
| L48 | `cv::imshow("binary_img", ...)` 收进 `if (debug_)`，非调试模式不再弹窗/阻塞 |
| L123-124 | `refine_corners_subpix(bgr_img, armors)` → `refine_corners_subpix(gray_img, armors)`，**复用 detect 内已生成的灰度图**，省一次全帧 `cvtColor` |
| L151-152 | `std::vector<cv::Point> points` → `std::vector<cv::Point2f>`，ROI 构造不再把浮点角点截断为整像素 |
| L228-247 | ROI 二次检测回写逻辑重构：先按 ROI 坐标回写 4 点 → 可选亚像素精化 → 再统一加 `roi_offset` 平移回原图。结果与原实现等价（原行为保留） |
| L249-279 | **新增** `Detector::refine_armor_subpix()`：单块装甲板的 cornerSubPix 精化，在原有 `isfinite` 校验之外新增 **3 px 漂移防护**（`MAX_SHIFT`）——输入角点是灯条边中点而非鞍点角，异常漂移时拒绝本笔精化并保留原角点 |
| L281-303 | `refine_corners_subpix()` 改为遍历调用上述 helper；成功后除更新 `armor.center` 外，**新增同步 `left/right` 灯条的 `top/bottom/top2bottom/points`**，消除"精炼角点与灯条端点不一致" |

cornerSubPix 参数完全未变（winSize 5×5、zeroZone (-1,-1)、TermCriteria EPS+MAX_ITER 30/0.01）。

### 2.3 `tasks/auto_aim/detector.hpp`

- L24-25：注释与形参名改为 `gray_img`（**语义变更，见 §4**）
- L42：新增成员 `bool use_corner_subpix_ = false;`
- L45-46：新增私有方法声明 `bool refine_armor_subpix(const cv::Mat & gray_img, Armor & armor) const;`

### 2.4 `tasks/auto_aim/aimer.cpp`

| 位置（现） | 修改 |
|---|---|
| L105-107, L110 | `std::vector<Target> iteration_target(10, target)`（每帧固定 10 次深拷贝）→ `reserve(10)` + 循环内按需 `emplace_back(target)`；通常 2~3 次即收敛 |
| L153-156 | 新增：迭代 10 次仍未收敛时打印 DEBUG 日志（原 `converged` 变量只赋值不使用，无诊断） |
| L160-161 | `yaw` 输出增加 `tools::limit_rad(...)`，限幅 ±π，防止 CAN 侧 `(int16_t)(yaw*1e4)` 在 \|yaw\|>3.2767 rad 时溢出 |
| L181 | 带 `ShootMode` 的重载同样 `tools::limit_rad(command.yaw - yaw_offset_ + yaw_offset)` |

弹道模型、俯仰公式、射击模式逻辑均**未改动**。

### 2.5 `io/cboard.cpp`

- L3：新增 `#include <algorithm>`（`std::clamp`）
- L43-50：`imu_at()` 中 `k = t_ac / t_ab` **除零**（相邻两帧 IMU 时间戳相同）会产生 NaN 四元数，污染整条解算链 → `t_ab <= 0` 时直接返回最新姿态；正常路径 `k` 限幅 [0,1]，避免时间戳轻微乱序时外插

### 2.6 `configs/standard3.yaml`

- L13-14：新增 `use_corner_subpix: false`（默认关，行为不变）
- L73-78：显式补 `drag_coefficient_up/down/boundary: 0.0` 并注释。**注意：取值仍为 0**，与原先"缺键→静默回退 0"的实际行为完全一致，只是把陷阱显性化；标定后可直接填值生效（参考 `sentry.yaml` 的 `up=0.2415, down=0.0, boundary=-0.3`）

### 2.7 新增 `tests/algo/ekf_update_check.cpp`

自建 11 维状态 / 4 维观测的小用例，对比"新 LDLT 实现"与"显式求逆参考实现"的状态更新与协方差更新结果，并检查 NIS/NEES 有限性与阈值逻辑。会被 `tests/CMakeLists.txt` 的 `auto_add_executables` 自动构造成同名可执行目标（仅依赖 Eigen + `tools`）。

---

## 3. 验证状态

| 改动 | 验证方式 | 结果 |
|---|---|---|
| EKF（含阈值与 LDLT） | MinGW g++ + Eigen 3.4 编译并运行 `tests/algo/ekf_update_check.cpp` | **通过**：`\|x_new-x_ref\|=1.1e-16`、`\|P_new-P_ref\|=1.2e-15`，`nis=8.99e-05`、`nees=0.615`、`nis_fail=0` |
| EKF 独立语法检查 | `g++ -std=c++17 -fsyntax-only` | 通过 |
| detector / aimer / cboard / yaml | **本机无 OpenCV（依赖 Ubuntu/OpenVINO 环境）未能编译** | 需在目标机执行 `cmake -B build && make -C build -j$(nproc)` 验证；所用 OpenCV API（`cornerSubPix`/`minAreaRect(Point2f)`/`cv::norm(Point2f)`）与改动前一致 |

---

## 4. 兼容性与行为变更说明（重要）

1. **唯一的对外可见签名变更**：`Detector::refine_corners_subpix(bgr_img, armors)` 现要求传入**单通道灰度图**（函数名未变）。全仓唯一调用点为 `detector.cpp:123`，已同步为传入 `gray_img`。若后续有人从外部（如新建测试程序）再调用此函数，**必须传灰度图**，否则会被 `gray_img.channels() != 1` 判为无效而跳过精化（不会崩溃，但会静默失效）。
2. **默认行为不变的项**：`use_corner_subpix` 默认 false；yaml 阻力系数仍为 0；`Detector` 构造函数在不含新键的旧配置文件上也能正常工作（`IsDefined()` 判定）。因此**旧配置文件可直接使用**。
3. **行为有差异的三项（均为修复目的）**：非调试模式不再显示 `binary_img` 窗口；NIS/NEES 判定阈值纠正；缺失/重复 IMU 时间戳不再产生 NaN。
4. **已知未改动的风险点（不在本次范围）**：`solver.cpp:61` IPPE 双解未做重投影比较（单帧 yaw 可能错 180°，现靠 EKF 层 ±π 翻转兜底）；角点本质是灯条边中点而非真鞍点；`commandgener`/`socketcan` 轮询空转；`tracker.cpp` 图像中心硬编码 1440/1080；`yolov5.cpp:77-78` x/y scale 命名与 rows/cols 对调。详见上一条回答的"建议下一步"。

---

## 5. 回滚与恢复

```bash
# 整体回滚（把改动前的完整副本拷回）
rm -rf D:/my/ckyf_vision/auto_aim_cornerSubpix-main
cp -r  D:/my/ckyf_vision/auto_aim_cornerSubpix_backup_20261011_0005 \
       D:/my/ckyf_vision/auto_aim_cornerSubpix-main

# 单文件回滚示例（以 EKF 为例）
cp D:/my/ckyf_vision/auto_aim_cornerSubpix_backup_20261011_0005/tools/extended_kalman_filter.cpp \
   D:/my/ckyf_vision/auto_aim_cornerSubpix-main/tools/extended_kalman_filter.cpp
```

开关回滚（不想改代码、只想恢复旧行为）：把 `configs/standard3.yaml` 中 `use_corner_subpix` 置 `false`（已是默认）；阈值/NIS 修复需回退代码恢复。

---

## 6. 上机后的建议验收项

1. 编译：`cmake -B build && make -C build -j$(nproc)`（重点看 `tasks/auto_aim`、`io`、`tests` 三处目标）。
2. 跑 `tests/algo/detector_video_test.cpp`（传统全流程检测链路，含 cornerSubPix）与 `tests/algo/auto_aim_test.cpp`，确认无编译/运行回归。
3. 观察 remote_logger 中 `nis` / `recent_nis_failures`：修复后稳态应在低个位数百分比，不再出现持续的 "Bad Converge" 重置。
4. 需要亚像素能力时，将 `use_corner_subpix` 置 `true`，在同一段视频上对角点坐标/PnP 距离做 A/B 对比（建议先看 `>4 m` 远距离目标的距离抖动）。
5. 阻力系数标定后再填 `drag_coefficient_*`，并对比实际弹着/命中率，验证是否优于纯抛物线补偿。
