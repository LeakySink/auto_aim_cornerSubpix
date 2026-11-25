#include "aimer.hpp"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <vector>

#include "tools/ballistics_poly.hpp"
#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/trajectory.hpp"

namespace auto_aim
{
Aimer::Aimer(const std::string & config_path)
: left_yaw_offset_(std::nullopt), right_yaw_offset_(std::nullopt)
{
  // 同 Planner：按配置装载弹道模型（poly / 内置 RK4），幂等
  tools::install_ballistics_from_config(config_path);

  auto yaml = YAML::LoadFile(config_path);

  yaw_offset_ = yaml["yaw_offset"].as<double>() / 57.3;      // degree to rad
  pitch_offset_ = yaml["pitch_offset"].as<double>() / 57.3;  // degree to rad

  comming_angle_ = yaml["comming_angle"].as<double>() / 57.3;  // degree to rad
  leaving_angle_ = yaml["leaving_angle"].as<double>() / 57.3;  // degree to rad

  high_speed_delay_time_ = yaml["high_speed_delay_time"].as<double>();
  low_speed_delay_time_ = yaml["low_speed_delay_time"].as<double>();
  decision_speed_ = yaml["decision_speed"].as<double>();
  // 可选键（旧配置没有）：缺失时保持头文件里的默认值
  if (yaml["delay_blend_width"] && yaml["delay_blend_width"].IsDefined()) {
    delay_blend_width_ = yaml["delay_blend_width"].as<double>();
  }
  if (yaml["nominal_pipeline_delay"] && yaml["nominal_pipeline_delay"].IsDefined()) {
    nominal_pipeline_delay_ = yaml["nominal_pipeline_delay"].as<double>();
  }
  if (yaml["lock_hold_margin"] && yaml["lock_hold_margin"].IsDefined()) {
    lock_hold_margin_ = yaml["lock_hold_margin"].as<double>();
  }

  // 兜底弹速：缺键保持旧行为（22 / 10 / 25），换弹种时用 yaml 覆盖。
  // 重装 42mm 应配 bullet_speed_default: 16.5。
  if (yaml["bullet_speed_default"].IsDefined())
    bullet_speed_default_ = yaml["bullet_speed_default"].as<double>();
  if (yaml["bullet_speed_min"].IsDefined())
    bullet_speed_min_ = yaml["bullet_speed_min"].as<double>();
  if (yaml["bullet_speed_max"].IsDefined())
    bullet_speed_max_ = yaml["bullet_speed_max"].as<double>();

  if (yaml["left_yaw_offset"].IsDefined() && yaml["right_yaw_offset"].IsDefined()) {
    left_yaw_offset_ = yaml["left_yaw_offset"].as<double>() / 57.3;    // degree to rad
    right_yaw_offset_ = yaml["right_yaw_offset"].as<double>() / 57.3;  // degree to rad
    tools::RemoteLogger::instance().log("INFO", "[Aimer] successfully loading shootmode");
  }
}

io::Command Aimer::aim(
  std::list<Target> targets, std::chrono::steady_clock::time_point timestamp, double bullet_speed,
  bool to_now)
{
  if (targets.empty()) return {false, false, 0, 0};

  auto target = targets.front();

  auto ekf = target.ekf();

  // ---- 延迟补偿：两档 -> 连续过渡 ----
  // 原来 `|w| > decision_speed_ ? 高速档 : 低速档` 在门限处硬跳变。
  // delay_blend_width_ > 0 时，在 [门限·(1-k), 门限·(1+k)] 内线性插值；
  // = 0 时退化为原来的硬二值（保持旧行为，便于 A/B）。
  const double abs_w = std::abs(target.ekf_x()[idx::W]);
  double delay_time;
  if (delay_blend_width_ > 1e-6 && decision_speed_ > 1e-6) {
    const double lo = decision_speed_ * (1.0 - delay_blend_width_);
    const double hi = decision_speed_ * (1.0 + delay_blend_width_);
    double ratio;
    if (abs_w <= lo) {
      ratio = 0.0;
    } else if (abs_w >= hi) {
      ratio = 1.0;
    } else {
      ratio = (abs_w - lo) / (hi - lo);   // 线性过渡
    }
    delay_time = low_speed_delay_time_ + ratio * (high_speed_delay_time_ - low_speed_delay_time_);
  } else {
    delay_time = abs_w > decision_speed_ ? high_speed_delay_time_ : low_speed_delay_time_;
  }

  // 测速不可用时不要凭空猜一个"够快"的值：按配置的兜底弹速，并与同一套
  // 上下限判据一致（否则 yaml 写了 16.5 又被 <14 的旧判据改写成 23）。
  if (!std::isfinite(bullet_speed) || bullet_speed < bullet_speed_min_ ||
      bullet_speed > bullet_speed_max_) {
    tools::RemoteLogger::instance().log(
      "WARN", "[Aimer] 弹速异常（{:.2f} m/s，允许 {:.1f}~{:.1f}），改用兜底 {:.2f} m/s",
      bullet_speed, bullet_speed_min_, bullet_speed_max_, bullet_speed_default_);
    bullet_speed = bullet_speed_default_;
  }

  // 考虑detector和tracker所消耗的时间，此外假设aimer的用时可忽略不计
  auto future = timestamp;
  if (to_now) {
    double dt = tools::delta_time(std::chrono::steady_clock::now(), timestamp) + delay_time;
    future += std::chrono::microseconds(int(dt * 1e6));
    target.predict(future);
  } else {
    auto dt = nominal_pipeline_delay_ + delay_time;  // 管线耗时 + 发弹延时
    future += std::chrono::microseconds(int(dt * 1e6));
    target.predict(future);
  }

  auto aim_point0 = choose_aim_point(target);
  debug_aim_point = aim_point0;
  if (!aim_point0.valid) {
    return {false, false, 0, 0};
  }

  Eigen::Vector3d xyz0 = aim_point0.xyza.head(3);
  auto d0 = std::sqrt(xyz0[0] * xyz0[0] + xyz0[1] * xyz0[1]);

  // 弹道：优先拟合表，超出网格回退 RK4（阻力系数为编译期常量，见 tools/trajectory.hpp）
  tools::Trajectory trajectory0(bullet_speed, d0, xyz0[2]);
  if (trajectory0.unsolvable) {
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Aimer] Unsolvable trajectory0: {:.2f} {:.2f} {:.2f}",
      bullet_speed, d0, xyz0[2]);
    debug_aim_point.valid = false;
    return {false, false, 0, 0};
  }

  // 迭代求解飞行时间 (最多10次，收敛条件：相邻两次fly_time差 <0.001)
  bool converged = false;
  double prev_fly_time = trajectory0.fly_time;
  tools::Trajectory current_traj = trajectory0;
  // 按需构造目标副本：通常 2~3 次即可收敛，避免每帧固定 10 次 Target 深拷贝
  std::vector<Target> iteration_target;
  iteration_target.reserve(10);

  for (int iter = 0; iter < 10; ++iter) {
    iteration_target.emplace_back(target);

    // 预测目标在 future + prev_fly_time 时刻的位置
    auto predict_time =
      future + std::chrono::microseconds(static_cast<int>(prev_fly_time * 1e6));
    iteration_target[iter].predict(predict_time);

    // 计算瞄准点
    auto aim_point = choose_aim_point(iteration_target[iter]);
    debug_aim_point = aim_point;
    if (!aim_point.valid) {
      return {false, false, 0, 0};
    }

    // 计算新弹道
    Eigen::Vector3d xyz = aim_point.xyza.head(3);
    double d = std::sqrt(xyz.x() * xyz.x() + xyz.y() * xyz.y());

    current_traj = tools::Trajectory(bullet_speed, d, xyz.z());

    // 检查弹道是否可解
    if (current_traj.unsolvable) {
      tools::RemoteLogger::instance().log(
        "DEBUG",
        "[Aimer] Unsolvable trajectory in iter {}: speed={:.2f}, d={:.2f}, z={:.2f}",
        iter + 1, bullet_speed, d, xyz.z());
      debug_aim_point.valid = false;
      return {false, false, 0, 0};
    }

    // 检查收敛条件
    if (std::abs(current_traj.fly_time - prev_fly_time) < 0.001) {
      converged = true;
      break;
    }
    prev_fly_time = current_traj.fly_time;
  }

  if (!converged) {
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Aimer] fly_time iteration not converged in 10 steps, use last result");
  }

  // 计算最终角度
  Eigen::Vector3d final_xyz = debug_aim_point.xyza.head(3);
  // 限幅到 [-pi, pi]，防止 |yaw| > 3.2767 rad 时 CAN int16 打包溢出
  double yaw = tools::limit_rad(std::atan2(final_xyz.y(), final_xyz.x()) + yaw_offset_);
  double pitch = -(current_traj.pitch + pitch_offset_);  // 世界坐标系下pitch向上为负

  return {true, false, yaw, pitch};
}

io::Command Aimer::aim(
  std::list<Target> targets, std::chrono::steady_clock::time_point timestamp, double bullet_speed,
  io::ShootMode shoot_mode, bool to_now)
{
  double yaw_offset;
  if (shoot_mode == io::left_shoot && left_yaw_offset_.has_value()) {
    yaw_offset = left_yaw_offset_.value();
  } else if (shoot_mode == io::right_shoot && right_yaw_offset_.has_value()) {
    yaw_offset = right_yaw_offset_.value();
  } else {
    yaw_offset = yaw_offset_;
  }

  auto command = aim(targets, timestamp, bullet_speed, to_now);
  command.yaw = tools::limit_rad(command.yaw - yaw_offset_ + yaw_offset);

  return command;
}

AimPoint Aimer::choose_aim_point(const Target & target)
{
  std::vector<Eigen::Vector4d> armor_xyza_list = target.armor_xyza_list();

  // Target::armor_xyza_list() 是 `for (i = 0; i < armor_num_; ++i)` 构造的，
  // 当 armor_num_ <= 0 时返回**空 vector**（data() == nullptr）。下面 3 处
  // armor_xyza_list[0] / [lock_id_] / [id_list[0]] 都在无条件取下标，
  // 空容器上 operator[](0) 即解引用 nullptr -> SIGSEGV。
  //
  // 当前 configs/*.yaml 的 armor_num 只有 3 和 4，故现在不可达；
  // 但这是每帧每个目标都走的热路径，一旦配置写错（新增机器人类型漏填、
  // 脚本生成配置默认值）立刻崩。这里在入口挡住，向下返回"无解"。
  if (armor_xyza_list.empty()) return {false, Eigen::Vector4d::Zero()};

  Eigen::VectorXd ekf_x = target.ekf_x();
  auto armor_num = armor_xyza_list.size();

  // 如果装甲板未发生过跳变，则只有当前装甲板的位置已知
  if (!target.jumped) return {true, armor_xyza_list[0]};

  // 整车旋转中心的球坐标yaw
  auto center_yaw = std::atan2(ekf_x[idx::Y], ekf_x[idx::X]);

  // 如果delta_angle为0，则该装甲板中心和整车中心的连线在世界坐标系的xy平面过原点
  std::vector<double> delta_angle_list;
  for (int i = 0; i < armor_num; i++) {
    auto delta_angle = tools::limit_rad(armor_xyza_list[i][3] - center_yaw);
    delta_angle_list.emplace_back(delta_angle);
  }

  // 不考虑小陀螺
  //
  // ⚠️ 索引必须是 [7]（角速度 w），不能是 [8]（半径 r）。
  // 这里原来写的是 [8]：r 的量级是 0.2~0.3 m，`|r| <= 2` **恒为真**，
  // 于是这个条件实际退化成 `name != outpost` —— 小陀螺分支对普通车
  // **从未生效过**（只有前哨站会走到下面的 else）。
  //
  // 修正后：普通车 |w| > 2 rad/s 也会走"来板优先"分支（coming/leaving），
  // 这是本行注释的原意。属行为变更，影响 standard/infantry 等走 Aimer 的
  // 入口（重装走 CenterAimer，不受影响）。
  if (std::abs(ekf_x[idx::W]) <= 2 && target.name != ArmorName::outpost) {
    // 选择在可射击范围内的装甲板
    std::vector<int> id_list;
    for (int i = 0; i < armor_num; i++) {
      if (std::abs(delta_angle_list[i]) > 60 / 57.3) continue;
      id_list.push_back(i);
    }

    // 绝无可能
    if (id_list.empty()) {
      tools::RemoteLogger::instance().log("WARN", "Empty id list!");
      return {false, armor_xyza_list[0]};
    }

    // 锁定模式：防止在两个都呈45度的装甲板之间来回切换
    if (id_list.size() > 1) {
      int id0 = id_list[0], id1 = id_list[1];

      // 未处于锁定模式时，选择delta_angle绝对值较小的装甲板，进入锁定模式
      if (lock_id_ != id0 && lock_id_ != id1)
        lock_id_ = (std::abs(delta_angle_list[id0]) < std::abs(delta_angle_list[id1])) ? id0 : id1;

      return {true, armor_xyza_list[lock_id_]};
    }

    // 只有一个装甲板在可射击范围内时，退出锁定模式
    lock_id_ = -1;
    return {true, armor_xyza_list[id_list[0]]};
  }

  double coming_angle, leaving_angle;
  if (target.name == ArmorName::outpost) {
    coming_angle = 70 / 57.3;
    leaving_angle = 30 / 57.3;
  } else {
    coming_angle = comming_angle_;
    leaving_angle = leaving_angle_;
  }

  // ---- 施密特滞回：先看上一帧锁定的板能不能保持 ----
  // 原来每次重新扫描 `for` 取"第一个满足条件的"，目标转到两块板都接近
  // 45° 的过渡角时，会在两块板之间来回切，云台跟着抖。
  // 现在：只要锁定的板还在 `coming_angle + 裕度` 内，就继续用它；
  // 离开这个更宽的门限后才重新选板（enter/exit 双门限 = 滞回）。
  if (lock_id_ >= 0 && lock_id_ < armor_num) {
    const int locked = static_cast<int>(lock_id_);
    const double da = delta_angle_list[locked];
    const bool in_hold_window = std::abs(da) <= coming_angle + lock_hold_margin_;
    if (in_hold_window) return {true, armor_xyza_list[locked]};
  }

  // 在小陀螺时，一侧的装甲板不断出现，另一侧的装甲板不断消失，显然前者被打中的概率更高
  for (int i = 0; i < armor_num; i++) {
    if (std::abs(delta_angle_list[i]) > coming_angle) continue;
    const bool coming_in = (ekf_x[idx::W] > 0 && delta_angle_list[i] < leaving_angle) ||
                           (ekf_x[idx::W] < 0 && delta_angle_list[i] > -leaving_angle);
    if (coming_in) {
      lock_id_ = i;   // 记录锁定，供下一帧滞回
      return {true, armor_xyza_list[i]};
    }
  }

  lock_id_ = -1;   // 没有可打的板，解除锁定
  return {false, armor_xyza_list[0]};
}

}  // namespace auto_aim