#ifndef AUTO_AIM__AIMER_HPP
#define AUTO_AIM__AIMER_HPP

#include <Eigen/Dense>
#include <chrono>
#include <limits>
#include <list>

#include "io/cboard.hpp"
#include "io/command.hpp"
#include "target.hpp"

namespace auto_aim
{

struct AimPoint
{
  bool valid;
  Eigen::Vector4d xyza;
};

class Aimer
{
public:
  AimPoint debug_aim_point;
  explicit Aimer(const std::string & config_path);

  io::Command aim(
    std::list<Target> targets, std::chrono::steady_clock::time_point timestamp, double bullet_speed,
    bool to_now = true);

  io::Command aim(
    std::list<Target> targets, std::chrono::steady_clock::time_point timestamp, double bullet_speed,
    io::ShootMode shoot_mode, bool to_now = true);

private:
  double yaw_offset_;
  std::optional<double> left_yaw_offset_, right_yaw_offset_;
  double pitch_offset_;


  double comming_angle_;
  double leaving_angle_;
  double lock_id_ = -1;
  double high_speed_delay_time_;
  double low_speed_delay_time_;
  double decision_speed_;

  // 兜底弹速（yaml: bullet_speed_default / bullet_speed_min / bullet_speed_max）。
  // 原来是写死的 `if (bullet_speed < 14) bullet_speed = 23;`，那是 17mm 时代的值；
  // 换 42mm（16.5 m/s）后，一旦 CAN 测速暂时不可用，就会按 23 m/s 解算 ——
  // 12 m 处抬枪角差 5° 以上，必然脱靶。
  // 缺省值与旧行为逐字等价：<14 → 23，且没有上界（max 取 +inf）。
  // 唯一的新增是 NaN/inf 也会落到兜底（旧写法 `NaN < 14` 为 false，会放行 NaN）。
  double bullet_speed_default_ = 23.0;
  double bullet_speed_min_ = 14.0;
  double bullet_speed_max_ = std::numeric_limits<double>::infinity();

  AimPoint choose_aim_point(const Target & target);
};

}  // namespace auto_aim

#endif  // AUTO_AIM__AIMER_HPP