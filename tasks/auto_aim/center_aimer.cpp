#include "center_aimer.hpp"

#include <chrono>
#include <cmath>

#include <yaml-cpp/yaml.h>

#include "tools/ballistics_poly.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
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

/// 单个候选板的评估结果（选板与弹道解算共用）
struct PlateEval
{
  int id = -1;
  double z = 0.0;        ///< 板心高度（世界系，相对云台原点）
  double horiz = 0.0;    ///< 板心水平距离
  double elev = 0.0;     ///< 板心仰角（直线）
  double err_yaw = 0.0;  ///< 相对瞄准线的方位偏差
  double half_yaw = 0.0; ///< 该距离/朝向下板面的半角宽
  double cos_face = 0.0; ///< 朝向枪口的程度
  bool ok = false;
};

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
    cfg_.min_face_cos = yaml_or(ca, "min_face_cos", cfg_.min_face_cos);
  cfg_.static_w_thresh = yaml_or(ca, "static_w_thresh", cfg_.static_w_thresh);
    cfg_.gimbal_tol = yaml_or(ca, "gimbal_tol_deg", cfg_.gimbal_tol);
    cfg_.launch_delay = yaml_or(ca, "launch_delay", cfg_.launch_delay);
    cfg_.pitch_follow_plate =
      (ca["pitch_follow_plate"] && ca["pitch_follow_plate"].IsDefined())
        ? ca["pitch_follow_plate"].as<bool>()
        : cfg_.pitch_follow_plate;
    cfg_.require_settled = (ca["require_settled"] && ca["require_settled"].IsDefined())
                             ? ca["require_settled"].as<bool>()
                             : cfg_.require_settled;
    cfg_.max_iter = static_cast<int>(yaml_or(ca, "max_iter", cfg_.max_iter));

    // 热量模型（2027 规则 5.1.3；42mm 参数见比赛规则表 5-16）
    const auto ht = ca["heat"];
    if (ht && ht.IsDefined()) {
      cfg_.heat.enabled =
        (ht["enabled"] && ht["enabled"].IsDefined()) ? ht["enabled"].as<bool>() : cfg_.heat.enabled;
      cfg_.heat.limit = yaml_or(ht, "limit", cfg_.heat.limit);
      cfg_.heat.cool_rate = yaml_or(ht, "cool_rate", cfg_.heat.cool_rate);
      cfg_.heat.shot_cost = yaml_or(ht, "shot_cost", cfg_.heat.shot_cost);
      cfg_.heat.reserve = yaml_or(ht, "reserve", cfg_.heat.reserve);
      cfg_.heat.settle_hz = yaml_or(ht, "settle_hz", cfg_.heat.settle_hz);
      cfg_.heat.shots_per_pulse = yaml_or(ht, "shots_per_pulse", cfg_.heat.shots_per_pulse);
    }
  }

  // 防御：hit_margin 写错（比如 1.5）会让窗口变成负数，永远不开火
  if (!(cfg_.hit_margin >= 0.0 && cfg_.hit_margin < 0.95)) cfg_.hit_margin = 0.30;
  // min_face_cos 允许负值（放宽到"只要不是正背面"），但 >=1 会把所有板挡掉
  if (!(cfg_.min_face_cos > -1.0 && cfg_.min_face_cos < 0.99)) cfg_.min_face_cos = 0.15;
  if (cfg_.max_iter < 1) cfg_.max_iter = 1;
  if (!(cfg_.launch_delay >= 0.0) || cfg_.launch_delay > 1.0) cfg_.launch_delay = 0.0;

  // 热量参数防御：写错会直接导致"永远不开火"或"打爆热量"
  if (!(cfg_.heat.limit > 0.0)) cfg_.heat.limit = 300.0;
  if (!(cfg_.heat.cool_rate > 0.0)) cfg_.heat.cool_rate = 100.0;
  if (!(cfg_.heat.shot_cost > 0.0)) cfg_.heat.shot_cost = 100.0;
  if (!(cfg_.heat.settle_hz > 0.0)) cfg_.heat.settle_hz = 10.0;
  if (!(cfg_.heat.shots_per_pulse > 0.0)) cfg_.heat.shots_per_pulse = 1.0;
  // reserve 必须小于 limit-cost，否则这道闸永远不放行
  const double min_margin = cfg_.heat.shot_cost * cfg_.heat.shots_per_pulse;
  if (!(cfg_.heat.reserve >= 0.0) || cfg_.heat.reserve > cfg_.heat.limit - min_margin) {
    cfg_.heat.reserve = 0.0;
  }
}

void CenterAimer::advance_heat(std::chrono::steady_clock::time_point t) const
{
  if (!cfg_.heat.enabled) {
    heat_q1_ = 0.0;
    heat_t_ = t;
    heat_valid_ = true;
    return;
  }

  if (!heat_valid_) {
    heat_q1_ = 0.0;
    heat_t_ = t;
    heat_valid_ = true;
    return;
  }

  const double dt = std::chrono::duration<double>(t - heat_t_).count();
  if (!(dt > 0.0)) {
    // 时间没前进（或回退）：不动热量，但把基准拉回来，避免累积出负 dt
    if (dt < 0.0) heat_t_ = t;
    return;
  }

  // 规则是按固定周期结算（10Hz），所以这里也按"完整周期数"扣，余量留到下次。
  // 直接按 dt 连续积分会在帧率与结算频率不同步时产生系统性偏差。
  const double period = 1.0 / cfg_.heat.settle_hz;
  const int n_periods = static_cast<int>(dt / period);
  if (n_periods > 0) {
    const double cool_per_period = cfg_.heat.cool_rate * period;
    heat_q1_ = std::max(0.0, heat_q1_ - cool_per_period * n_periods);
    heat_t_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(period * n_periods));
  }
}

bool CenterAimer::aim_center(
  const Target & target, std::chrono::steady_clock::time_point /*t*/, double bullet_speed,
  CenterAimResult * out) const
{
  const Eigen::VectorXd x0 = target.ekf_x();
  // 11 维是最低要求：短状态（默认构造的 Target）取 [0]/[2]/[4] 会越界
  if (x0.size() < 11 || !x0.allFinite()) return false;
  if (!std::isfinite(bullet_speed) || bullet_speed <= 0.0) return false;

  // 预测时刻 = launch_delay + fly_time：既要算弹丸飞行，也要算"判定→出膛"的延迟。
  // 距离与 fly_time 互相依赖，一般 2~3 次收敛；不收敛就用最后一次（误差在毫米级）。
  double fly = 0.0;
  double total = cfg_.launch_delay;
  for (int i = 0; i < cfg_.max_iter; ++i) {
    Target pred = target;  // 值拷贝：predict() 会改状态，不能动调用方的 target
    pred.predict(total);
    const Eigen::VectorXd xp = pred.ekf_x();
    if (xp.size() < 11 || !xp.allFinite()) return false;

    const double d = std::hypot(xp[0], xp[2]);
    if (!(d > 0.2) || !std::isfinite(d)) return false;  // 贴脸/脏解，物理上不存在

    const tools::Trajectory traj(bullet_speed, d, xp[4]);
    if (traj.unsolvable) return false;

    const double new_fly = traj.fly_time;
    const bool converged = std::abs(new_fly - fly) < cfg_.fly_time_eps;
    fly = new_fly;
    total = cfg_.launch_delay + new_fly;
    if (converged) break;
  }

  // 用最终 total 再外推一次，保证 aim / pitch / 板位置与 total 严格对应
  Target pred = target;
  pred.predict(total);
  const Eigen::VectorXd xp = pred.ekf_x();
  if (xp.size() < 11 || !xp.allFinite()) return false;

  Eigen::Vector3d aim{xp[0], xp[2], xp[4]};

  // ---- 静止目标：直接瞄可见的那块板 ----
  //
  // "瞄中心等板"依赖板会转过来。目标静止时板永远不动，而横向窗口只有
  // ±atan(板宽·0.7/2 / d)（4.5 m 处 ≈ ±0.60°），三块板 120° 分布，
  // 可打姿态占比约 1%（实测见 .deps/diag_static.cpp）。
  //
  // 这里改成瞄"最正对相机的那块板"，然后 yaw/pitch 都由这块板所在的位置
  // 重新解算（下方 traj 用的是 aim.z()，所以必须先把 aim 换掉）。
  //
  // 注意用 `xp[7]`（预测后的 w）而不是原始 target，保证与 total_time 一致。
  const double w_abs = std::abs(xp[7]);
  if (w_abs < cfg_.static_w_thresh) {
    const auto plates_now = pred.armor_xyza_list();
    if (plates_now.empty()) return false;

    int best = -1;
    double best_cos = -2.0;
    for (std::size_t i = 0; i < plates_now.size(); ++i) {
      const Eigen::Vector3d pi = plates_now[i].head<3>();
      if (!pi.allFinite()) continue;
      const double horiz_i = std::hypot(pi.x(), pi.y());
      if (!(horiz_i > 0.1)) continue;
      // n̂·p̂：正视 +1、侧棱 0、背面 −1
      const double ny = plates_now[i][3];
      const double cos_face = (std::cos(ny) * pi.x() + std::sin(ny) * pi.y()) / horiz_i;
      if (cos_face > best_cos) {
        best_cos = cos_face;
        best = static_cast<int>(i);
      }
    }
    // 没有正对相机的板（全在背面）-> 无解，交给上层判"不可打"
    if (best < 0 || best_cos < cfg_.min_face_cos) return false;

    aim = plates_now[best].head<3>();
  }

  const double d = std::hypot(aim.x(), aim.y());
  if (!(d > 0.2)) return false;

  const tools::Trajectory traj(bullet_speed, d, aim.z());
  if (traj.unsolvable) return false;

  if (out != nullptr) {
    out->valid = true;
    out->fly_time = fly;
    out->total_time = total;
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
  // t 目前不参与判据（连发冷却交给电控），保留参数是为了接口稳定与后续扩展
  (void)t;

  CenterAimResult r;
  if (!aim_center(target, t, bullet_speed, &r)) return CenterAimResult{};

  if (!std::isfinite(gimbal_yaw) || !std::isfinite(gimbal_pitch)) return CenterAimResult{};

  // 到达时刻的目标状态：板的位置要按"出膛后弹丸飞完"那一刻算（launch_delay + fly）
  Target pred = target;
  pred.predict(r.total_time);

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
  double best_cos_face = -1.0;
  int blocked_by_face = 0;  // 统计被"背面/侧棱"判据挡掉的板数（诊断用）

  for (std::size_t i = 0; i < plates.size(); ++i) {
    const Eigen::Vector3d p = plates[i].head<3>();
    if (!p.allFinite()) continue;
    const double range = p.norm();
    const double horiz = std::hypot(p.x(), p.y());
    if (!(range > 0.2) || !(horiz > 0.1)) continue;

    // ---- 可见性：只有朝向我们这块板才可能命中 ----
    // n̂ = 板→车体中心方向（plates[i][3] 就是这个角）。
    // cos_face = n̂·p̂：正视 +1、侧棱 0、背面 −1。
    // 不判这个的话，"瞄中心"会把**背面/远侧**那块也当成候选 —— 它同样压在瞄准线上，
    // 但子弹会先打在车身或前侧板上（2026-11 实测：4 板目标有 7/8 的相位会选中背面板）。
    const double n_yaw = plates[i][3];
    const double cos_face =
      (std::cos(n_yaw) * p.x() + std::sin(n_yaw) * p.y()) / horiz;
    if (cos_face < cfg_.min_face_cos) {
      ++blocked_by_face;
      continue;
    }

    const double phi = std::atan2(p.y(), p.x());
    const double elev = std::atan2(p.z(), horiz);
    const double err_yaw = std::abs(tools::limit_rad(phi - yaw_los));
    // pitch 偏差：跟随板高时，瞄准线的高度就是这块板的高度 → 偏差恒为 0；
    // 只瞄中心时，才需要比较板高与中心高度的差。
    const double elev_ref = cfg_.pitch_follow_plate ? elev : elev_aim;
    const double err_pitch = std::abs(elev - elev_ref);

    // 侧视时板在视线上被压扁，有效宽度按 cos_face 收缩（正视 1、侧棱 →0）
    const double half_yaw = std::atan2(eff_w * cos_face / 2.0, horiz);
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
      best_cos_face = cos_face;
    }
  }

  if (best_id < 0) {
    // 所有板都在背面/侧棱：本帧不该开火（不是"数据坏了"）。留个可查的痕迹。
    if (blocked_by_face > 0) {
      using clock = std::chrono::steady_clock;
      static clock::time_point last_all_hidden{};
      static bool logged{false};
      const auto now = clock::now();
      if (!logged || std::chrono::duration<double>(now - last_all_hidden).count() > 2.0) {
        last_all_hidden = now;
        logged = true;
        tools::RemoteLogger::instance().log(
          "DEBUG", "[CenterAim] {} 块板全部朝外（cos_face<{:.2f}），本帧无可见靶",
          blocked_by_face, cfg_.min_face_cos);
      }
    }
    return CenterAimResult{};
  }

  // ---- pitch 跟随选中板的高度 ----
  // 瞄准点 X/Y 仍然是**旋转中心**（板转过来才压线），只是把 Z 换成这块板的板高。
  // 这样高度差 h 再大（规范允许到 100mm）也不需要"整块板跳过一个窗口才能打"。
  if (cfg_.pitch_follow_plate) {
    const double plate_z = plates[best_id].z();
    const double horiz = std::hypot(r.aim_xyz.x(), r.aim_xyz.y());
    const tools::Trajectory traj_h(bullet_speed, horiz, plate_z);
    if (traj_h.unsolvable) return CenterAimResult{};

    r.aim_xyz.z() = plate_z;
    r.aim_pitch_abs = traj_h.pitch;
    r.pitch = -(traj_h.pitch + cfg_.pitch_offset);
    r.fly_time = traj_h.fly_time;
    r.total_time = cfg_.launch_delay + traj_h.fly_time;
    r.pitch_err_deg = 0.0;  // 瞄准线高度就是这块板的高度
  }

  r.plate_id = best_id;
  r.cos_face = best_cos_face;
  r.align_err_deg = best_err_yaw * kRad2Deg;
  r.hit_window_deg = best_win_yaw * kRad2Deg;
  r.pitch_err_deg = best_err_pitch * kRad2Deg;
  r.pitch_window_deg = best_win_pitch * kRad2Deg;

  // 到达时刻这块板是否压在瞄准线上（yaw 与 pitch 都要在窗口内）
  const bool in_window = best_err_yaw <= best_win_yaw / 2.0 && best_err_pitch <= best_win_pitch / 2.0;

  // 云台到位判据：预测得再准，枪口没到也是白搭。
  //
  // ⚠️ pitch 的符号是这里唯一的坑：
  //   - **反馈** `gs.pitch` 抬头为**正**（`gimbal.cpp` 里 `state_.pitch = rx.pitch`，
  //     构造四元数时才用 `-pitch` 绕 Y）；
  //   - **指令** `r.pitch` 抬头为**负**（和 `planner`/`aimer` 一样下发 `-traj.pitch`）。
  //   所以误差是两者**相加**（与 `planner` 的 `pitch_err = plan.pitch0 + gs.pitch` 同源）。
  //   2026-11 这里写成相减，导致稳态误差恒为 2×pitch（≥3°）→ `fire` 永远为 0。
  bool settled = true;
  double yaw_err = 0.0, pitch_err = 0.0;
  const double tol = cfg_.gimbal_tol / kRad2Deg;
  if (cfg_.require_settled) {
    yaw_err = std::abs(tools::limit_rad(gimbal_yaw - r.yaw));
    pitch_err = std::abs(gimbal_pitch + r.pitch);
    settled = yaw_err < tol && pitch_err < tol;
  }

  // "预测已命中但枪没到位"是最容易被误判成"算法不工作"的一种状态。
  // 限流打一条 WARN，把误差、容差和"实际在追的角度"一起打出来：
  //   - 若 yaw_err 恒≈|yaw_offset|、pitch_err 恒≈仰角+|pitch_offset|，
  //     说明**云台根本没在跟**（没切 AUTO_AIM、串口没通、使能没给），
  //     不是我们的解算有问题；
  //   - 若 pitch_err 恒≈2×瞄准仰角，才是符号写反。
  if (in_window && !settled) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    static bool logged = false;
    const auto now = clock::now();
    if (!logged || std::chrono::duration<double>(now - last).count() > 2.0) {
      last = now;
      logged = true;
      tools::RemoteLogger::instance().log(
        "WARN",
        "[CenterAim] 预测已命中但云台未到位：yaw_err={:.2f}° pitch_err={:.2f}°（容差 {:.2f}°）"
        " cmd(yaw={:.2f}° pitch={:.2f}°) fb(yaw={:.2f}° pitch={:.2f}°)。"
        "若 yaw_err≈|yaw_offset|、pitch_err≈仰角+|pitch_offset|，是云台没在跟（查 AUTO_AIM/串口）；"
        "若 pitch_err≈2×仰角，才是符号写反",
        yaw_err * kRad2Deg, pitch_err * kRad2Deg, cfg_.gimbal_tol, r.yaw * kRad2Deg,
        r.pitch * kRad2Deg, gimbal_yaw * kRad2Deg, gimbal_pitch * kRad2Deg);
    }
  }

  // 连发节奏由**热量**决定，依据规则 5.1.3：
  //   每发 42mm 弹丸 Q1 += 100，Q0 = 300，冷却 100/秒，10Hz 结算。
  //   Q1 > Q0 会锁发射机构（且图传断开），要等 Q1 = 0 才解锁，
  //   所以"多打一发"的代价是以秒计的，必须在开火前算清楚。
  //
  // ⚠️ **下位机按 fire 的上升沿算一发**（已确认）。
  //   所以热量必须只在**上升沿**记账，不能按帧累加：
  //   一个窗口里 fire 可能持续几十帧，那是同一发，只消耗 100 热量。
  //   按帧累加会在 3 帧内就把 Q1 打到 300，凭空锁死发射机构。
  advance_heat(t);
  const double cost = cfg_.heat.shot_cost * cfg_.heat.shots_per_pulse;
  const double after = heat_q1_ + cost;
  const double cap = cfg_.heat.limit - cfg_.heat.reserve;
  // Q1 == Q0 在规则上仍不算超限（锁的是 Q1 > Q0），所以用 <=。
  // 浮点：加个极小容差，避免 100+100+100 恰好等于 300 时因舍入被拒。
  const bool heat_ok = !cfg_.heat.enabled || (after <= cap + 1e-9);

  const bool want_fire = in_window && settled && heat_ok;
  const bool rising = want_fire && !last_fire_;  // 上升沿 = 下位机真正打出的那一发
  if (rising) {
    heat_q1_ = after;  // 只在上升沿记账
  }
  last_fire_ = want_fire;

  r.in_window = in_window;
  r.settled = settled;
  r.heat_ok = heat_ok;
  r.heat_q1 = heat_q1_;
  r.heat_limit = cfg_.heat.limit;
  r.heat_after = after;
  r.heat_shot = rising;  // 本帧是否是一个新的"发"
  r.fire = want_fire;

  // 距穿越还有多久：给调试看"是不是快到了"，不参与判据
  const double w = target.ekf_x()[7];
  r.t_to_cross = (std::abs(w) > 1e-3) ? (best_err_yaw / std::abs(w)) : 0.0;

  return r;
}

}  // namespace auto_aim
