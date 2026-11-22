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

struct EkfParams {
  double q_xyz_acc_var;  // Q 矩阵：位置/速度加速度方差
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
  Target(
    const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
    Eigen::VectorXd P0_dig, EkfParams ekf_params);
  Target(double x, double vyaw, double radius, double h);

  void predict(std::chrono::steady_clock::time_point t);
  void predict(double dt);
  void update(const Armor & armor);

  Eigen::VectorXd ekf_x() const;
  const tools::ExtendedKalmanFilter & ekf() const;

  /// 设置角速度状态（索引 7）。
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