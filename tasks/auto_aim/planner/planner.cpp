#include "planner.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "tools/ballistics_poly.hpp"
#include "tools/math_tools.hpp"
#include "tools/trajectory.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono_literals;

namespace auto_aim
{
namespace
{
// 降级日志限流：plan() 是 10ms 周期的热路径，"无解"每帧都可能发生，
// 原来每帧一条 WARN 会把日志刷爆、也掩盖真正的问题。
void log_degraded(const std::string & msg)
{
  static std::chrono::steady_clock::time_point last{};
  static bool inited = false;
  const auto now = std::chrono::steady_clock::now();
  if (inited && now - last < std::chrono::seconds(2)) return;
  inited = true;
  last = now;
  tools::RemoteLogger::instance().log("WARN", "{}", msg);
}
}  // namespace
Planner::Planner(const std::string & config_path)
{
  // 弹道模型：配置里 ballistics_model: poly 时装载 ballistic-main 的离线拟合表，
  // 否则（缺省）保持内置解析解+RK4。tup/yolov5 等入口都通过 Planner/Aimer 构造，
  // 所以在这里装一次即可全局生效（幂等）。
  tools::install_ballistics_from_config(config_path);

  auto yaml = tools::load(config_path);

  yaw_offset_ = tools::read<double>(yaml, "yaw_offset") / 57.3;
  pitch_offset_ = tools::read<double>(yaml, "pitch_offset") / 57.3;

  // 可选键，与 Solver 用同一个名字（缺省 0.5 m）
  if (yaml["min_valid_dist"] && yaml["min_valid_dist"].IsDefined()) {
    min_valid_dist_ = yaml["min_valid_dist"].as<double>();
  }

  auto old_node = yaml["drag_coefficient"];
  auto up_node = yaml["drag_coefficient_up"];
  auto down_node = yaml["drag_coefficient_down"];
  auto boundary_node = yaml["drag_height_boundary"];

  drag_coefficient_up_ =
    up_node ? up_node.as<double>() : (old_node ? old_node.as<double>() : 0.0);

  drag_coefficient_down_ =
    down_node ? down_node.as<double>() : drag_coefficient_up_;

  drag_height_boundary_ =
    boundary_node ? boundary_node.as<double>() : 0.0;

  fire_thresh_ = tools::read<double>(yaml, "fire_thresh");
  decision_speed_ = tools::read<double>(yaml, "decision_speed");
  high_speed_delay_time_ = tools::read<double>(yaml, "high_speed_delay_time");
  low_speed_delay_time_ = tools::read<double>(yaml, "low_speed_delay_time");

  setup_yaw_solver(config_path);
  setup_pitch_solver(config_path);
}

Plan Planner::plan(Target target, double bullet_speed)
{
  // 0. Check bullet speed
  if (bullet_speed < 10 || bullet_speed > 25) {
    bullet_speed = 22;
  }

  // 1. Predict fly_time
  //    xyz / min_dist 必须显式初始化：armor_xyza_list() 为空、或里面全是非有限值
  //    （EKF 被脏观测污染后的表现）时下面的循环一次都不会进，读未初始化的 Eigen
  //    向量是 UB，而且 d = 1e10 必然让弹道无解。
  Eigen::Vector3d xyz = Eigen::Vector3d::Zero();
  auto min_dist = std::numeric_limits<double>::infinity();
  for (auto & xyza : target.armor_xyza_list()) {
    if (!xyza.allFinite()) continue;  // 跳过被污染的装甲板
    auto dist = xyza.head<2>().norm();
    // 幽灵板过滤：EKF 状态被带跑时，armor_xyza_list() 会给出离云台很近的假位置，
    // 而下面取的是**最近**的板，正好会被它劫持（现场 d=0.30m 就是这么来的）。
    if (dist < min_valid_dist_) continue;
    if (dist < min_dist) {
      min_dist = dist;
      xyz = xyza.head<3>();
    }
  }

  // 没有任何可用装甲板：本帧直接放弃控制，不再往下算弹道与 MPC
  if (!std::isfinite(min_dist) || min_dist <= 0.0) {
    log_degraded("[Planner] 无可用装甲板（armor_xyza_list 为空或全为非有限值），本帧不控制");
    return {false};
  }

  const double k0 =
  (xyz.z() >= drag_height_boundary_)
    ? drag_coefficient_up_
    : drag_coefficient_down_;

  auto bullet_traj = tools::Trajectory(bullet_speed, min_dist, xyz.z(), k0);
  target.predict(bullet_traj.fly_time);

  // 2. Get trajectory
  double yaw0, pitch0;
  Trajectory traj;
  try {
    auto aim_ref = aim(target, bullet_speed);
    yaw0 = aim_ref(0);
    pitch0 = aim_ref(1);
    traj = get_trajectory(target, yaw0, bullet_speed);
  } catch (const std::exception & e) {
    // 保留原来"弹速"这个关键信息，但改成限流 + 带上原因：无解有两个来源 ——
    // 找不到可用装甲板（no valid armor）/ 本帧弹道无解（Unsolvable bullet trajectory）
    log_degraded(
      std::string("[Planner] 弹道/目标无解，本帧不控制：") + e.what() +
      "（bullet_speed=" + std::to_string(bullet_speed) + "）");
    return {false};
  }

  // 3. Solve yaw
  Eigen::VectorXd x0(2);
  x0 << traj(0, 0), traj(1, 0);
  tiny_set_x0(yaw_solver_, x0);

  yaw_solver_->work->Xref = traj.block(0, 0, 2, HORIZON);
  tiny_solve(yaw_solver_);

  // 4. Solve pitch
  x0 << traj(2, 0), traj(3, 0);
  tiny_set_x0(pitch_solver_, x0);

  pitch_solver_->work->Xref = traj.block(2, 0, 2, HORIZON);
  tiny_solve(pitch_solver_);

  Plan plan;
  plan.control = true;
  plan.yaw0 = yaw0;
  plan.pitch0 = pitch0;

  plan.target_yaw = tools::limit_rad(traj(0, HALF_HORIZON) + yaw0);
  plan.target_pitch = traj(2, HALF_HORIZON);

  plan.yaw = yaw_solver_->work->x(0, HALF_HORIZON) + yaw0;
  plan.yaw_vel = yaw_solver_->work->x(1, HALF_HORIZON);
  plan.yaw_acc = yaw_solver_->work->u(0, HALF_HORIZON);

  plan.pitch = pitch_solver_->work->x(0, HALF_HORIZON);
  plan.pitch = -plan.pitch;
  plan.pitch_vel = pitch_solver_->work->x(1, HALF_HORIZON);
  plan.pitch_acc = pitch_solver_->work->u(0, HALF_HORIZON);

  auto shoot_offset_ = 2;
  plan.fire =
    std::hypot(
      traj(0, HALF_HORIZON + shoot_offset_) -
        yaw_solver_->work->x(0, HALF_HORIZON + shoot_offset_),
      traj(2, HALF_HORIZON + shoot_offset_) -
        pitch_solver_->work->x(0, HALF_HORIZON + shoot_offset_)) < fire_thresh_;

  return plan;
}

Plan Planner::plan(std::optional<Target> target, double bullet_speed)
{
  if (!target.has_value()) return {false};

  double delay_time =
    std::abs(target->ekf_x()[7]) > decision_speed_ ? high_speed_delay_time_
                                                   : low_speed_delay_time_;

  auto future =
    std::chrono::steady_clock::now() + std::chrono::microseconds(int(delay_time * 1e6));

  target->predict(future);

  return plan(*target, bullet_speed);
}

void Planner::setup_yaw_solver(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto max_yaw_acc = tools::read<double>(yaml, "max_yaw_acc");
  auto Q_yaw = tools::read<std::vector<double>>(yaml, "Q_yaw");
  auto R_yaw = tools::read<std::vector<double>>(yaml, "R_yaw");

  Eigen::MatrixXd A{{1, DT}, {0, 1}};
  Eigen::MatrixXd B{{0}, {DT}};
  Eigen::VectorXd f{{0, 0}};
  Eigen::Matrix<double, 2, 1> Q(Q_yaw.data());
  Eigen::Matrix<double, 1, 1> R(R_yaw.data());

  tiny_setup(
    &yaw_solver_, A, B, f, Q.asDiagonal(), R.asDiagonal(), 1.0, 2, 1, HORIZON, 0);

  Eigen::MatrixXd x_min = Eigen::MatrixXd::Constant(2, HORIZON, -1e17);
  Eigen::MatrixXd x_max = Eigen::MatrixXd::Constant(2, HORIZON, 1e17);
  Eigen::MatrixXd u_min = Eigen::MatrixXd::Constant(1, HORIZON - 1, -max_yaw_acc);
  Eigen::MatrixXd u_max = Eigen::MatrixXd::Constant(1, HORIZON - 1, max_yaw_acc);
  tiny_set_bound_constraints(yaw_solver_, x_min, x_max, u_min, u_max);

  yaw_solver_->settings->max_iter = 10;
}

void Planner::setup_pitch_solver(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto max_pitch_acc = tools::read<double>(yaml, "max_pitch_acc");
  auto Q_pitch = tools::read<std::vector<double>>(yaml, "Q_pitch");
  auto R_pitch = tools::read<std::vector<double>>(yaml, "R_pitch");

  Eigen::MatrixXd A{{1, DT}, {0, 1}};
  Eigen::MatrixXd B{{0}, {DT}};
  Eigen::VectorXd f{{0, 0}};
  Eigen::Matrix<double, 2, 1> Q(Q_pitch.data());
  Eigen::Matrix<double, 1, 1> R(R_pitch.data());

  tiny_setup(
    &pitch_solver_, A, B, f, Q.asDiagonal(), R.asDiagonal(), 1.0, 2, 1, HORIZON, 0);

  Eigen::MatrixXd x_min = Eigen::MatrixXd::Constant(2, HORIZON, -1e17);
  Eigen::MatrixXd x_max = Eigen::MatrixXd::Constant(2, HORIZON, 1e17);
  Eigen::MatrixXd u_min = Eigen::MatrixXd::Constant(1, HORIZON - 1, -max_pitch_acc);
  Eigen::MatrixXd u_max = Eigen::MatrixXd::Constant(1, HORIZON - 1, max_pitch_acc);
  tiny_set_bound_constraints(pitch_solver_, x_min, x_max, u_min, u_max);

  pitch_solver_->settings->max_iter = 10;
}

Eigen::Matrix<double, 2, 1> Planner::aim(const Target & target, double bullet_speed)
{
  // 与 plan() 同样：显式初始化 + 跳过被污染的装甲板（读未初始化内存是 UB）
  Eigen::Vector3d xyz = Eigen::Vector3d::Zero();
  double yaw = 0.0;
  auto min_dist = std::numeric_limits<double>::infinity();

  for (auto & xyza : target.armor_xyza_list()) {
    if (!xyza.allFinite()) continue;
    auto dist = xyza.head<2>().norm();
    if (dist < min_valid_dist_) continue;  // 幽灵板过滤，同上
    if (dist < min_dist) {
      min_dist = dist;
      xyz = xyza.head<3>();
      yaw = xyza[3];
    }
  }

  if (!std::isfinite(min_dist) || min_dist <= 0.0) {
    throw std::runtime_error("no valid armor");
  }

  debug_xyza = Eigen::Vector4d(xyz.x(), xyz.y(), xyz.z(), yaw);

  auto azim = std::atan2(xyz.y(), xyz.x());

  const double k =
  (xyz.z() >= drag_height_boundary_)
    ? drag_coefficient_up_
    : drag_coefficient_down_;

  auto bullet_traj = tools::Trajectory(bullet_speed, min_dist, xyz.z(), k);

  if (bullet_traj.unsolvable) {
    throw std::runtime_error("Unsolvable bullet trajectory!");
  }

  return {tools::limit_rad(azim + yaw_offset_), -bullet_traj.pitch - pitch_offset_};
}

Trajectory Planner::get_trajectory(Target & target, double yaw0, double bullet_speed)
{
  Trajectory traj;

  target.predict(-DT * (HALF_HORIZON + 1));
  auto yaw_pitch_last = aim(target, bullet_speed);

  target.predict(DT);  // [0] = -HALF_HORIZON * DT -> [HHALF_HORIZON] = 0
  auto yaw_pitch = aim(target, bullet_speed);

  for (int i = 0; i < HORIZON; i++) {
    target.predict(DT);
    auto yaw_pitch_next = aim(target, bullet_speed);

    auto yaw_vel =
      tools::limit_rad(yaw_pitch_next(0) - yaw_pitch_last(0)) / (2 * DT);
    auto pitch_vel = (yaw_pitch_next(1) - yaw_pitch_last(1)) / (2 * DT);

    traj.col(i) << tools::limit_rad(yaw_pitch(0) - yaw0), yaw_vel, yaw_pitch(1),
      pitch_vel;

    yaw_pitch_last = yaw_pitch;
    yaw_pitch = yaw_pitch_next;
  }

  return traj;
}

}  // namespace auto_aim
