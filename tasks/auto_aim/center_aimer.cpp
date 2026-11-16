#include "center_aimer.hpp"

#include <cmath>

#include <yaml-cpp/yaml.h>

#include "tools/ballistics_poly.hpp"
#include "tools/math_tools.hpp"
#include "tools/trajectory.hpp"

namespace auto_aim
{
namespace
{
constexpr double kRad2Deg = 57.29577951308232;

double yaml_or(const YAML::Node & n, const char * key, double fallback)
{
  return (n[key] && n[key].IsDefined()) ? n[key].as<double>() : fallback;
}

}  // namespace

CenterAimer::CenterAimer(const std::string & config_path)
{
  // 与 Aimer / Planner 一样：按配置装弹道模型（poly / 内置 RK4），幂等
  tools::install_ballistics_from_config(config_path);

  const auto yaml = YAML::LoadFile(config_path);

  // 板面尺寸：优先 armor_geometry 段（与 Solver 共用同一份来源）
  const auto geo = yaml["armor_geometry"];
  if (geo && geo.IsDefined()) {
    cfg_.plate_width = yaml_or(geo, "small_width", cfg_.plate_width);
    cfg_.plate_height = yaml_or(geo, "lightbar_length", cfg_.plate_height);
  }

  cfg_.yaw_offset = yaml_or(yaml, "yaw_offset", 0.0) / kRad2Deg;
  cfg_.pitch_offset = yaml_or(yaml, "pitch_offset", 0.0) / kRad2Deg;

  const auto ca = yaml["center_aim"];
  if (ca && ca.IsDefined()) {
    cfg_.hit_margin = yaml_or(ca, "hit_margin", cfg_.hit_margin);
    cfg_.gimbal_tol = yaml_or(ca, "gimbal_tol_deg", cfg_.gimbal_tol);
    cfg_.require_settled = (ca["require_settled"] && ca["require_settled"].IsDefined())
                             ? ca["require_settled"].as<bool>()
                             : cfg_.require_settled;
    cfg_.max_iter = static_cast<int>(yaml_or(ca, "max_iter", cfg_.max_iter));
  }

  // 防御：hit_margin 写错（比如 1.5）会让窗口变成负数，永远不开火
  if (!(cfg_.hit_margin >= 0.0 && cfg_.hit_margin < 0.95)) cfg_.hit_margin = 0.30;
  if (cfg_.max_iter < 1) cfg_.max_iter = 1;
}

bool CenterAimer::aim_center(
  const Target & target, std::chrono::steady_clock::time_point /*t*/, double bullet_speed,
  CenterAimResult * out) const
{
  const Eigen::VectorXd x0 = target.ekf_x();
  // 11 维是最低要求：短状态（默认构造的 Target）取 [0]/[2]/[4] 会越界
  if (x0.size() < 11 || !x0.allFinite()) return false;
  if (!std::isfinite(bullet_speed) || bullet_speed <= 0.0) return false;

  // 飞行时间与距离互相依赖：距离 -> traj.fly_time -> 外推更远的中心 -> 距离再变。
  // 一般 2~3 次收敛；不收敛就用最后一次（误差在毫米级）。
  double fly = 0.0;
  for (int i = 0; i < cfg_.max_iter; ++i) {
    Target pred = target;  // 值拷贝：predict() 会改状态，不能动调用方的 target
    pred.predict(fly);
    const Eigen::VectorXd xp = pred.ekf_x();
    if (xp.size() < 11 || !xp.allFinite()) return false;

    const double d = std::hypot(xp[0], xp[2]);
    if (!(d > 0.2) || !std::isfinite(d)) return false;  // 贴脸/脏解，物理上不存在

    const tools::Trajectory traj(bullet_speed, d, xp[4]);
    if (traj.unsolvable) return false;

    const double new_fly = traj.fly_time;
    if (std::abs(new_fly - fly) < cfg_.fly_time_eps) {
      fly = new_fly;
      break;
    }
    fly = new_fly;
  }

  // 用最终 fly_time 再外推一次，保证 aim / pitch 与 fly_time 严格对应
  Target pred = target;
  pred.predict(fly);
  const Eigen::VectorXd xp = pred.ekf_x();
  if (xp.size() < 11 || !xp.allFinite()) return false;

  const Eigen::Vector3d aim{xp[0], xp[2], xp[4]};
  const double d = std::hypot(aim.x(), aim.y());
  if (!(d > 0.2)) return false;

  const tools::Trajectory traj(bullet_speed, d, aim.z());
  if (traj.unsolvable) return false;

  if (out != nullptr) {
    out->valid = true;
    out->fly_time = fly;
    out->aim_xyz = aim;
    out->aim_pitch_abs = traj.pitch;
    out->yaw = tools::limit_rad(std::atan2(aim.y(), aim.x()) + cfg_.yaw_offset);
    out->pitch = -(traj.pitch + cfg_.pitch_offset);  // 世界系向上为负
  }
  return true;
}

CenterAimResult CenterAimer::solve(
  const Target & target, std::chrono::steady_clock::time_point t, double gimbal_yaw,
  double gimbal_pitch, double bullet_speed) const
{
  CenterAimResult r;
  if (!aim_center(target, t, bullet_speed, &r)) return CenterAimResult{};

  if (!std::isfinite(gimbal_yaw) || !std::isfinite(gimbal_pitch)) return CenterAimResult{};

  // 到达时刻的目标状态：板的位置要按"那时"算，而不是"现在"
  Target pred = target;
  pred.predict(r.fly_time);

  const auto plates = pred.armor_xyza_list();
  if (plates.empty()) return CenterAimResult{};

  const double yaw_los = std::atan2(r.aim_xyz.y(), r.aim_xyz.x());
  // pitch 判据必须比"到瞄准点的直线仰角"，不能比 bullet_traj.pitch（那是弹道抬角，
  // 含重力补偿，比直线仰角大 1~2°）。用弹道角去比会把所有板判成"高出去"。
  const double elev_aim = std::atan2(r.aim_xyz.z(), std::hypot(r.aim_xyz.x(), r.aim_xyz.y()));
  // 判据用板面中间的一部分，吸收 PnP 与超前量误差
  const double eff_w = cfg_.plate_width * (1.0 - cfg_.hit_margin);
  const double eff_h = cfg_.plate_height * (1.0 - cfg_.hit_margin);

  int best_id = -1;
  double best_score = 1e9;
  double best_err_yaw = 180.0 * kRad2Deg;  // 占位
  double best_win_yaw = 0.0, best_err_pitch = 0.0, best_win_pitch = 0.0;

  for (std::size_t i = 0; i < plates.size(); ++i) {
    const Eigen::Vector3d p = plates[i].head<3>();
    if (!p.allFinite()) continue;
    const double range = p.norm();
    const double horiz = std::hypot(p.x(), p.y());
    if (!(range > 0.2) || !(horiz > 0.1)) continue;

    const double phi = std::atan2(p.y(), p.x());
    const double elev = std::atan2(p.z(), horiz);
    const double err_yaw = std::abs(tools::limit_rad(phi - yaw_los));
    const double err_pitch = std::abs(elev - elev_aim);

    const double half_yaw = std::atan2(eff_w / 2.0, horiz);
    const double half_pitch = std::atan2(eff_h / 2.0, range);
    if (half_yaw <= 0.0 || half_pitch <= 0.0) continue;

    // 归一化误差：yaw 与 pitch 各占一半权重，取最小的那块板
    const double score = err_yaw / half_yaw + err_pitch / half_pitch;
    if (score < best_score) {
      best_score = score;
      best_id = static_cast<int>(i);
      best_err_yaw = err_yaw;
      best_win_yaw = 2.0 * half_yaw;
      best_err_pitch = err_pitch;
      best_win_pitch = 2.0 * half_pitch;
    }
  }

  if (best_id < 0) return CenterAimResult{};

  r.plate_id = best_id;
  r.align_err_deg = best_err_yaw * kRad2Deg;
  r.hit_window_deg = best_win_yaw * kRad2Deg;
  r.pitch_err_deg = best_err_pitch * kRad2Deg;
  r.pitch_window_deg = best_win_pitch * kRad2Deg;

  // 到达时刻这块板是否压在瞄准线上（yaw 与 pitch 都要在窗口内）
  const bool in_window = best_err_yaw <= best_win_yaw / 2.0 && best_err_pitch <= best_win_pitch / 2.0;

  // 云台到位判据：预测得再准，枪口没到也是白搭
  bool settled = true;
  if (cfg_.require_settled) {
    const double tol = cfg_.gimbal_tol / kRad2Deg;
    settled = std::abs(tools::limit_rad(gimbal_yaw - r.yaw)) < tol &&
              std::abs(gimbal_pitch - r.pitch) < tol;
  }

  r.fire = in_window && settled;

  // 距穿越还有多久：给调试看"是不是快到了"，不参与判据
  const double w = target.ekf_x()[7];
  r.t_to_cross = (std::abs(w) > 1e-3) ? (best_err_yaw / std::abs(w)) : 0.0;

  return r;
}

}  // namespace auto_aim
