#include "cboard.hpp"

#include <algorithm>
#include <cstddef>

#include "tools/math_tools.hpp"
#include "tools/rdbg/tf_auto_pub.hpp"
#include "tools/yaml.hpp"

namespace io
{
CBoard::CBoard(const std::string & config_path)
: mode(Mode::idle),
  shoot_mode(ShootMode::left_shoot),
  bullet_speed(0),
  queue_(5000),
  can_(read_yaml(config_path), std::bind(&CBoard::callback, this, std::placeholders::_1))
// 注意: callback的运行会早于Cboard构造函数的完成
{
  tools::RemoteLogger::instance().log("INFO", "[Cboard] Waiting for q...");
  queue_.pop(data_ahead_);
  queue_.pop(data_behind_);
  tools::RemoteLogger::instance().log("INFO", "[Cboard] Opened.");
}

Eigen::Quaterniond CBoard::imu_at(std::chrono::steady_clock::time_point timestamp)
{
  if (data_behind_.timestamp < timestamp) data_ahead_ = data_behind_;

  while (true) {
    queue_.pop(data_behind_);
    if (data_behind_.timestamp > timestamp) break;
    data_ahead_ = data_behind_;
  }

  Eigen::Quaterniond q_a = data_ahead_.q.normalized();
  Eigen::Quaterniond q_b = data_behind_.q.normalized();
  auto t_a = data_ahead_.timestamp;
  auto t_b = data_behind_.timestamp;
  auto t_c = timestamp;
  std::chrono::duration<double> t_ab = t_b - t_a;
  std::chrono::duration<double> t_ac = t_c - t_a;

  // 相邻两帧 IMU 时间戳相同会导致除零产生 NaN 四元数，此时直接使用最新帧
  if (t_ab.count() <= 0) {
    tools::rdbg::publish_tf_attitude(q_b);
    return q_b;
  }

  // 四元数插值（k 限幅到 [0, 1]，避免时间戳轻微乱序时外插）
  auto k = std::clamp(t_ac / t_ab, 0.0, 1.0);
  Eigen::Quaterniond q_c = q_a.slerp(k, q_b).normalized();
  tools::rdbg::publish_tf_attitude(q_c);
  return q_c;
}

void CBoard::send(Command command) const
{
  can_frame frame;
  frame.can_id = send_canid_;
  frame.can_dlc = 8;
  frame.data[0] = (command.control) ? 1 : 0;
  frame.data[1] = (command.shoot) ? 1 : 0;
  frame.data[2] = (int16_t)(command.yaw * 1e4) >> 8;
  frame.data[3] = (int16_t)(command.yaw * 1e4);
  frame.data[4] = (int16_t)(command.pitch * 1e4) >> 8;
  frame.data[5] = (int16_t)(command.pitch * 1e4);
  frame.data[6] = (int16_t)(command.horizon_distance * 1e4) >> 8;
  frame.data[7] = (int16_t)(command.horizon_distance * 1e4);

  try {
    can_.write(&frame);
  } catch (const std::exception & e) {
    tools::RemoteLogger::instance().log("WARN", "{}", e.what());
  }
}

void CBoard::callback(const can_frame & frame)
{
  auto timestamp = std::chrono::steady_clock::now();

  if (frame.can_id == quaternion_canid_) {
    auto x = (int16_t)(frame.data[0] << 8 | frame.data[1]) / 1e4;
    auto y = (int16_t)(frame.data[2] << 8 | frame.data[3]) / 1e4;
    auto z = (int16_t)(frame.data[4] << 8 | frame.data[5]) / 1e4;
    auto w = (int16_t)(frame.data[6] << 8 | frame.data[7]) / 1e4;

    if (std::abs(x * x + y * y + z * z + w * w - 1) > 1e-2) {
      tools::RemoteLogger::instance().log("WARN", "Invalid q: {} {} {} {}", w, x, y, z);
      return;
    }

    queue_.push({{w, x, y, z}, timestamp});
  }

  else if (frame.can_id == bullet_speed_canid_) {
    // data 是下位机发来的**原始字节**，可能是脏数据：版本不匹配、总线噪声、
    // 或者短帧（can_dlc < 6）时复用到了上一次的残留字节。
    //
    // 不校验的后果：Mode(data[2]) 会得到一个不在枚举定义内的值，
    // 紧接着 MODES[mode] 越过 vector 末尾 —— 而 std::vector::operator[]
    // 在 Release（-DNDEBUG）下**不做边界检查**，于是把越界内存当成
    // std::string 解释，fmt 格式化时会拷贝这个"字符串"，从内部野指针
    // memcpy -> SIGSEGV。SHOOT_MODES 同理。
    //
    // 在入口统一挡住，保证 mode / shoot_mode 成员只可能被赋成合法枚举值，
    // 从而下游全部 5 处索引（cboard.cpp:108 与 4 个主循环日志）都不可能越界。
    if (frame.can_dlc < 6) {
      tools::RemoteLogger::instance().log(
        "WARN", "[CBoard] short bullet_speed frame, dlc={}", static_cast<int>(frame.can_dlc));
      return;
    }
    const std::size_t raw_mode = frame.data[2];
    const std::size_t raw_shoot_mode = frame.data[3];
    if (raw_mode >= MODES.size() || raw_shoot_mode >= SHOOT_MODES.size()) {
      tools::RemoteLogger::instance().log(
        "WARN", "[CBoard] invalid mode={} shoot_mode={}, frame dropped", raw_mode,
        raw_shoot_mode);
      return;
    }

    bullet_speed = (int16_t)(frame.data[0] << 8 | frame.data[1]) / 1e2;
    mode = Mode(raw_mode);
    shoot_mode = ShootMode(raw_shoot_mode);
    ft_angle = (int16_t)(frame.data[4] << 8 | frame.data[5]) / 1e4;

    // 限制日志输出频率为1Hz
    static auto last_log_time = std::chrono::steady_clock::time_point::min();
    auto now = std::chrono::steady_clock::now();

    if (bullet_speed > 0 && tools::delta_time(now, last_log_time) >= 1.0) {
      tools::RemoteLogger::instance().log("INFO", 
        "[CBoard] Bullet speed: {:.2f} m/s, Mode: {}, Shoot mode: {}, FT angle: {:.2f} rad",
        bullet_speed, MODES[mode], SHOOT_MODES[shoot_mode], ft_angle);
      last_log_time = now;
    }
  }
}

// 实现方式有待改进
std::string CBoard::read_yaml(const std::string & config_path)
{
  auto yaml = tools::load(config_path);

  quaternion_canid_ = tools::read<int>(yaml, "quaternion_canid");
  bullet_speed_canid_ = tools::read<int>(yaml, "bullet_speed_canid");
  send_canid_ = tools::read<int>(yaml, "send_canid");

  if (!yaml["can_interface"]) {
    throw std::runtime_error("Missing 'can_interface' in YAML configuration.");
  }

  return yaml["can_interface"].as<std::string>();
}

}  // namespace io