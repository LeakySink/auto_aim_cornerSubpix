#ifndef AUTO_AIM__SOLVER_HPP
#define AUTO_AIM__SOLVER_HPP

#include <Eigen/Dense>  // 必须在opencv2/core/eigen.hpp上面
#include <Eigen/Geometry>
#include <opencv2/core/eigen.hpp>

#include "armor.hpp"

namespace auto_aim
{
class Solver
{
public:
  explicit Solver(const std::string & config_path);

  Eigen::Matrix3d R_gimbal2world() const;

  void set_R_gimbal2world(const Eigen::Quaterniond & q);

  /// 解算装甲板位姿（PnP + 坐标变换）。
  /// 返回 false 表示本次位姿被判定为不可信（非有限值 / 相机系深度非法 / 距离越界），
  /// 此时 armor 上的 xyz_* / ypr_* / ypd_* 会被清零；调用方应当丢弃这块装甲板
  /// （Tracker 已按此处理）。不可信的来源主要是远距离低置信度框 —— 角点噪声会让
  /// PnP 解出几十米甚至 NaN 的位姿，一旦进 EKF 就会污染 11 维状态。
  bool solve(Armor & armor) const;

  std::vector<cv::Point2f> reproject_armor(
    const Eigen::Vector3d & xyz_in_world, double yaw, ArmorType type, ArmorName name) const;

  double oupost_reprojection_error(Armor armor, const double & picth);

  std::vector<cv::Point2f> world2pixel(const std::vector<cv::Point3f> & worldPoints);

private:
  cv::Mat camera_matrix_;
  cv::Mat distort_coeffs_;
  Eigen::Matrix3d R_gimbal2imubody_;
  Eigen::Matrix3d R_camera2gimbal_;
  Eigen::Vector3d t_camera2gimbal_;
  Eigen::Matrix3d R_gimbal2world_;

  // 关键点语义校验开关（yaml 可选键 reject_bad_keypoints，缺省 false）
  bool reject_bad_keypoints_ = false;

  // 位姿合理性门限（yaml 可选键 min_valid_dist / max_valid_dist）。
  // 下限取 0.5 m：与弹道拟合表的覆盖下限一致，且 0.5 m 以内的"敌人"物理上不存在
  // ——现场出现过 d=0.30 m 的脏解（虚假框被 IPPE 解成"贴脸"），0.2 m 拦不住。
  double min_valid_dist_ = 0.5;
  double max_valid_dist_ = 50.0;

  void optimize_yaw(Armor & armor) const;

  double armor_reprojection_error(const Armor & armor, double yaw, const double & inclined) const;
  double SJTU_cost(
    const std::vector<cv::Point2f> & cv_refs, const std::vector<cv::Point2f> & cv_pts,
    const double & inclined) const;
};

}  // namespace auto_aim

#endif  // AUTO_AIM__SOLVER_HPP