#ifndef AUTO_AIM__PLANNER_HPP
#define AUTO_AIM__PLANNER_HPP

#include <Eigen/Dense>
#include <list>
#include <mutex>
#include <optional>

#include "tasks/auto_aim/target.hpp"
#include "tinympc/tiny_api.hpp"

namespace auto_aim
{
constexpr double DT = 0.01;
constexpr int HALF_HORIZON = 50;
constexpr int HORIZON = HALF_HORIZON * 2;

using Trajectory = Eigen::Matrix<double, 4, HORIZON>;  // yaw, yaw_vel, pitch, pitch_vel

struct Plan
{
  bool control;
  bool fire;
  float target_yaw;
  float target_pitch;
  float yaw;
  float yaw_vel;
  float yaw_acc;
  float pitch;
  float pitch_vel;
  float pitch_acc;
  double yaw0;        // MPC 解算前的理想瞄准 yaw
  double pitch0;      // MPC 解算前的理想发射 pitch
};

class Planner
{
public:
  /// 当前帧"理想瞄准点"（世界系 xyz + 装甲板 yaw），调试可视化用它反投影成红框。
  /// 修过的三个坑：
  ///   1) 只在 plan() 算完**当前帧**后写一次 —— 以前 aim() 每次都写，而 get_trajectory()
  ///      内部会循环调用 aim() 上百次（把目标预测到未来 0.01s 之后），于是红框显示的是
  ///      "未来某个时刻"的瞄准点，一秒内扫过目标整段位移，看起来就是乱飘；
  ///   2) 读写都在互斥量里 —— 以前主线程无锁读 plan 线程写的 Eigen::Vector4d（32 字节，
  ///      非原子），可能读到撕裂值（x 来自新帧、y/z 来自旧帧），位置凭空乱跳；
  ///   3) plan() 失败（无目标/无可用装甲板/弹道无解）时清空 validity，红框随之消失而不是
  ///      停在上一帧的旧位置。
  Eigen::Vector4d debug_xyza() const;
  bool debug_xyza_valid() const;

  Planner(const std::string & config_path);

  Plan plan(Target target, double bullet_speed);
  Plan plan(std::optional<Target> target, double bullet_speed);

  double fire_thresh() const { return fire_thresh_; }

private:
  double yaw_offset_;
  double pitch_offset_;

  double fire_thresh_;
  double low_speed_delay_time_, high_speed_delay_time_, decision_speed_;
  // 与 Solver::min_valid_dist 同名的可选键：过滤掉预测位置离云台过近的"幽灵装甲板"。
  // 正常交战中不会有 0.5m 以内的目标；出现即说明 EKF 状态被带跑（见 tracker 的空间门控）。
  double min_valid_dist_ = 0.5;

  // 调试瞄准点（跨线程读写，见 public 里的说明）
  mutable std::mutex debug_mutex_;
  Eigen::Vector4d debug_xyza_ = Eigen::Vector4d::Zero();
  bool debug_valid_ = false;
  void set_debug_aim(const Eigen::Vector4d & xyza);
  void clear_debug_aim();

  TinySolver * yaw_solver_;
  TinySolver * pitch_solver_;

  void setup_yaw_solver(const std::string & config_path);
  void setup_pitch_solver(const std::string & config_path);

  Eigen::Matrix<double, 2, 1> aim(
    const Target & target, double bullet_speed, Eigen::Vector4d * debug_out = nullptr) const;
  Trajectory get_trajectory(Target & target, double yaw0, double bullet_speed);
};

}  // namespace auto_aim

#endif  // AUTO_AIM__PLANNER_HPP