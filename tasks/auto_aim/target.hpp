#ifndef AUTO_AIM__TARGET_HPP
#define AUTO_AIM__TARGET_HPP

#include <Eigen/Dense>
#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "armor.hpp"
#include "tools/extended_kalman_filter.hpp"

namespace auto_aim
{

/// 状态向量的下标（**唯一权威定义**）。
///
/// 为什么要集中定义：2026-11 之前所有地方都写裸下标 `x[7]`、`ekf_x()[8]`，
/// 一旦状态布局变化就要改几十处，而且**漏改不会报错**——只会静默读错分量。
/// 历史上已经踩过两次：
///   ① `aimer.cpp` 用 `[8]`（半径）当角速度判据，`|r|<=2` 恒真，
///      导致小陀螺分支对普通车**从未生效**
///   ② 前哨站板高符号改动只翻了公式没同步雅可比
///
/// 布局（14 维，常加速 CA 模型）：
///   x  vx ax | y  vy ay | z  vz az | a   w   r   l   h
///   0  1  2    3  4  5    6  7  8    9   10  11  12  13
///
/// 相比旧的 11 维 CV 布局，平移三轴各多一个加速度状态；`a` 及之后的
/// 所有下标整体后移 3。
namespace idx
{
// ---- 平移：每轴 (位置, 速度, 加速度) ----
constexpr int X = 0, VX = 1, AX = 2;
constexpr int Y = 3, VY = 4, AY = 5;
constexpr int Z = 6, VZ = 7, AZ = 8;

// ---- 旋转与几何 ----
constexpr int A = 9;   ///< yaw 相位（旧 [6]）
constexpr int W = 10;  ///< 角速度（旧 [7]）
constexpr int R = 11;  ///< 板到旋转中心距离（旧 [8]）
constexpr int L = 12;  ///< 奇偶面半径差，仅 4 板（旧 [9]）
constexpr int H = 13;  ///< 4 板=奇偶高度差；3 板=相邻板高差（旧 [10]）

/// 状态维数。所有 `x.size() < N` 的守卫都必须用它，不要写裸数字。
constexpr int DIM = 14;
}  // namespace idx

struct EkfParams {
  /// 平移过程噪声 [m²/s⁶] —— **CA 模型下是 jerk（加加速度）方差**，
  /// 不是加速度方差。旧的 CV 模型里同名参数是加速度方差 [m²/s⁴]，
  /// **两者量纲不同，数值不可直接搬运**。
  ///
  /// CV → CA 的换算（稳态加速度方差近似）：
  ///   CA 中 E[a²] ≈ q_jerk · dt / 3，而 CV 中直接就是 q_acc
  ///   取 dt=1/60：要让 E[a²] 与旧 q_acc=70 相当，需 q_jerk ≈ 70·3/dt ≈ 12600
  /// 实际取值应以仿真/实车拟合为准，上面的换算只说明"10 不是 70 的替代"。
  double q_xyz_acc_var;

  double q_yaw_acc_var;  // Q 矩阵：角加速度方差
  double r_yaw;          // R 矩阵：yaw 观测噪声
  double r_pitch;        // R 矩阵：pitch 观测噪声
  double r_dist_base;    // R 矩阵：距离观测噪声基础值

  /// 几何状态（r/l/h）的随机游走强度 [m²/s]，**默认 0（冻结）**。
  ///
  /// ⚠️ 2026-11-23 撤回一次错误改动：曾把默认值设为 1e-4
  /// （理由"P 只减不增，偏了回不来"），但这是**误诊**——
  /// `r`/`h` 是**固定物理常数**（前哨站是官方场地道具，尺寸不变；
  /// 4 板机器人的板距也是设计值）。常参数不需要过程噪声：
  /// P 递减是"不确定性在降低"，不是缺陷。
  ///
  /// 加噪声的实测代价（`tests/algo/outpost_h_noise_check.cpp`，
  /// 5 m / 2 cm 位置噪声 / w=0.8π）：
  ///   q_geo=1e-4 → h 估计峰峰值 8.2 mm（且呈 10 s 级慢漂）
  ///   q_geo=0    → 1.5 mm
  /// 8 mm 已占竖直命中窗口（±19.6 mm）的 41%，会直接影响能不能打中。
  ///
  /// 收敛性由 `P0` 控制而不是这里：先验错 48 mm 时，q_geo=0 且
  /// P0(h)=1e-2 在 120 s 内收敛到 0.0 mm 残差。
  double q_geo = 0.0;

  /// 转速冻结阈值 [rad/s]：|w| 低于它时按 |w|/阈值 线性缩小 q_geo。
  /// 物理依据：目标不转时，r/l/h 与旋转中心位置在观测上共线（不可观测），
  /// 此时让几何参数随机游走只会把误差灌进位置状态。
  double omega_freeze_thresh = 0.5;

  /// 切板帧的板朝向观测噪声放大倍数。
  ///
  /// 动机：换一块板时，`angle = x[A] + id·2π/armor_num` 会**整跳一个板间距**
  /// （4 板 90°、3 板 120°）。若仍按普通帧的 r_yaw 采信这次观测，
  /// 新息会以很大权重把 `a`/`w` 拽一下，连带把 `h` 拉偏。
  ///
  /// 做法（参考 ckyf 的 `r_yaw_adaptive_factor`）：检测到切板时，
  /// 把**板朝向通道**（R 的第 4 个对角元）放大这个倍数，让它"少信一点"。
  /// 注意必须用 `is_switch_`（在 `last_id` 更新前算好的），
  /// 不能在 `update_ypda` 里比 `id != last_id` —— 那时 last_id 已经是新值了。
  double r_yaw_adaptive_factor = 50.0;

  /// 几何协方差发散阈值 [m²]：P(r)/P(l)/P(h) 任一超过它即判为发散。
  /// 对应 σ = 1 m，而 r≈0.28 m、h≈0.10 m —— 到这个量级说明几何参数
  /// 已经彻底失去约束（低可观测下的慢漂移），继续用只会越错越远。
  double max_geo_cov = 1.0;
};

/// 几何量（r / l / h）的初值。
///
/// 用途：重捕目标时用上次收敛的值做初值，省掉十几帧重新估 r/h
/// （ckyf 的 `geo_cache_` 思路）。任一分量 < 0 表示"没缓存，用配置默认"。
///
/// 注意 h 的语义随板数变：4 板 = 奇偶面高度差；3 板 = 相邻板高差。
struct GeoInit {
  double r = -1.0;
  double l = -1.0;
  double h = -1.0;

  bool valid() const { return r >= 0.0; }
};

class Target
{
public:
  ArmorName name;
  ArmorType armor_type;
  ArmorPriority priority;
  bool jumped;
  int last_id;  // debug only

  Target() = default;
  /// @param id_hypothesis 首帧观测到的板是 id 几（仅 3 板/前哨站使用）。
  ///
  /// 前哨站只有一块板同时可见，而 `id` 的编号由首次观测决定 ——
  /// 第一帧根本不知道看到的是第几块。而模型里
  ///   `z_i = x[Z] - i·h`、`angle_i = x[A] + i·2π/3`
  /// 一旦假设错，`x[Z]` 就差 `k·h`（最大 204 mm），EKF 只能用 `h < 0`
  /// 去吸收这个错位，表现为 h 收敛到负值、三块板预测高度全错。
  ///
  /// 解决：对 k = 0/1/2 各建一个候选，并行比较后选优（见 Tracker）。
  /// 4 板时该参数被忽略（4 板总能同时看到两块，不存在这个歧义）。
  Target(
    const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
    Eigen::VectorXd P0_dig, EkfParams ekf_params, GeoInit geo = {}, int id_hypothesis = 0);
  Target(double x, double vyaw, double radius, double h);

  void predict(std::chrono::steady_clock::time_point t);
  void predict(double dt);
  void update(const Armor & armor);

  Eigen::VectorXd ekf_x() const;
  const tools::ExtendedKalmanFilter & ekf() const;

  /// 当前几何量估计（r/l/h），供 Tracker 写 geo_cache。
  GeoInit geometric_params() const;

  /// 本帧是否发生了切板（当前观测的板 id 与上一帧不同）。
  /// 用途：①调试/rdbg 观察切板频率；②单测断言 R(3,3) 的放大条件成立。
  bool is_switching() const { return is_switch_; }

  /// 设置角速度状态（`idx::W`）。
  /// 主要给单元测试用：几何过程噪声按 |w| 缩放，测试必须能把 w 摆到指定值。
  /// 也让调试时能人为注入一个已知转速做对照。
  void set_w(double w);
  std::vector<Eigen::Vector4d> armor_xyza_list() const;

  bool diverged() const;

  bool convergened();

  bool isinit = false;

  bool checkinit();

private:
  int armor_num_;
  int switch_count_;
  int update_count_;

  bool is_switch_, is_converged_;

  tools::ExtendedKalmanFilter ekf_;
  std::chrono::steady_clock::time_point t_;
  EkfParams ekf_params_;

  void update_ypda(const Armor & armor, int id);  // yaw pitch distance angle

  Eigen::Vector3d h_armor_xyz(const Eigen::VectorXd & x, int id) const;
  Eigen::MatrixXd h_jacobian(const Eigen::VectorXd & x, int id) const;
};

}  // namespace auto_aim

#endif  // AUTO_AIM__TARGET_HPP
