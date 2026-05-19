#include "gimbal.hpp"

#include <cmath>

#include "tools/logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/yaml.hpp"

static constexpr float DEG2RAD = static_cast<float>(M_PI / 180.0);
static constexpr float RAD2DEG = static_cast<float>(180.0 / M_PI);

namespace io
{
Gimbal::Gimbal(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto com_port = tools::read<std::string>(yaml, "com_port");

  try {
    serial_.setPort(com_port);
    serial_.setBaudrate(115200);
    serial_.open();
  } catch (const std::exception & e) {
    tools::logger()->error("[Gimbal] Failed to open serial: {}", e.what());
    exit(1);
  }

  thread_ = std::thread(&Gimbal::read_thread, this);

  queue_.pop();
  tools::logger()->info("[Gimbal] First q received.");
}

Gimbal::~Gimbal()
{
  quit_ = true;
  if (thread_.joinable()) thread_.join();
  serial_.close();
}

GimbalMode Gimbal::mode() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

GimbalState Gimbal::state() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::string Gimbal::str(GimbalMode mode) const
{
  switch (mode) {
    case GimbalMode::IDLE:       return "IDLE";
    case GimbalMode::AUTO_AIM:   return "AUTO_AIM";
    case GimbalMode::SMALL_BUFF: return "SMALL_BUFF";
    case GimbalMode::BIG_BUFF:   return "BIG_BUFF";
    default:                     return "INVALID";
  }
}

Eigen::Quaterniond Gimbal::q(std::chrono::steady_clock::time_point t)
{
  while (true) {
    auto [q_a, t_a] = queue_.pop();
    auto [q_b, t_b] = queue_.front();
    auto t_ab = tools::delta_time(t_a, t_b);
    auto t_ac = tools::delta_time(t_a, t);
    auto k = t_ac / t_ab;
    Eigen::Quaterniond q_c = q_a.slerp(k, q_b).normalized();
    if (t < t_a) return q_c;
    if (!(t_a < t && t <= t_b)) continue;
    return q_c;
  }
}

void Gimbal::send(io::VisionToGimbal msg)
{
  FrameHead head;
  head.header = 0xFF;
  head.length = static_cast<uint8_t>(sizeof(FrameHead) + sizeof(VisionToGimbal) + sizeof(FrameTail));
  head.id = 0x81;

  FrameTail tail;
  tail.crc8 = 0x0d;

  uint8_t frame[sizeof(FrameHead) + sizeof(VisionToGimbal) + sizeof(FrameTail)];
  memcpy(frame, &head, sizeof(FrameHead));
  memcpy(frame + sizeof(FrameHead), &msg, sizeof(VisionToGimbal));
  memcpy(frame + sizeof(FrameHead) + sizeof(VisionToGimbal), &tail, sizeof(FrameTail));

  try {
    serial_.write(frame, sizeof(frame));
  } catch (const std::exception & e) {
    tools::logger()->warn("[Gimbal] Failed to write serial: {}", e.what());
  }
}

// yaw/pitch/yaw_vel/pitch_vel 单位为 rad，转换为度后发送
// yaw_acc/pitch_acc 新协议无对应字段，忽略
void Gimbal::send(
  bool control, bool fire, float yaw, float yaw_vel, float yaw_acc, float pitch, float pitch_vel,
  float pitch_acc)
{
  (void)yaw_acc;
  (void)pitch_acc;

  VisionToGimbal msg{};
  msg.success       = control ? (fire ? 2 : 1) : 0;
  msg.yaw           = yaw   * RAD2DEG;
  msg.pitch         = pitch * RAD2DEG;
  msg.distance      = 3.0f;
  msg.w_yaw         = yaw_vel   * RAD2DEG;
  msg.w_pitch       = pitch_vel * RAD2DEG;
  msg.jmp_time      = 0.0f;
  msg.target_rate   = 20;
  msg.target_number = 0;

  send(msg);
}

bool Gimbal::read_bytes(uint8_t * buffer, size_t size)
{
  try {
    return serial_.read(buffer, size) == size;
  } catch (const std::exception &) {
    return false;
  }
}

void Gimbal::read_thread()
{
  tools::logger()->info("[Gimbal] read_thread started.");
  int error_count = 0;

  while (!quit_) {
    if (error_count > 5000) {
      error_count = 0;
      tools::logger()->warn("[Gimbal] Too many errors, attempting to reconnect...");
      reconnect();
      continue;
    }

    uint8_t header_byte;
    if (!read_bytes(&header_byte, 1)) {
      error_count++;
      continue;
    }
    if (header_byte != 0xFF) continue;

    uint8_t length_byte, id_byte;
    if (!read_bytes(&length_byte, 1) || !read_bytes(&id_byte, 1)) {
      error_count++;
      continue;
    }

    int payload_size = static_cast<int>(length_byte) - static_cast<int>(sizeof(FrameHead)) -
                       static_cast<int>(sizeof(FrameTail));
    if (payload_size <= 0 || payload_size > 256) {
      error_count++;
      continue;
    }

    uint8_t payload[256];
    if (!read_bytes(payload, static_cast<size_t>(payload_size))) {
      error_count++;
      continue;
    }

    uint8_t crc8_byte;
    if (!read_bytes(&crc8_byte, 1)) {
      error_count++;
      continue;
    }
    if (crc8_byte != 0x0D) {
      tools::logger()->debug("[Gimbal] CRC8 check failed: 0x{:02X}", crc8_byte);
      continue;
    }

    error_count = 0;

    if (id_byte != 0x14) continue;
    if (payload_size != static_cast<int>(sizeof(GimbalToVision))) {
      tools::logger()->warn(
        "[Gimbal] Payload size mismatch: got {}, expect {}", payload_size,
        sizeof(GimbalToVision));
      continue;
    }

    memcpy(&rx_data_, payload, sizeof(GimbalToVision));
    auto t = std::chrono::steady_clock::now();

    double roll_rad  = static_cast<double>(rx_data_.roll)  * DEG2RAD;
    double pitch_rad = static_cast<double>(rx_data_.pitch) * DEG2RAD;
    double yaw_rad   = static_cast<double>(rx_data_.yaw)   * DEG2RAD;

    Eigen::AngleAxisd roll_aa(roll_rad,   Eigen::Vector3d::UnitX());
    Eigen::AngleAxisd pitch_aa(pitch_rad, Eigen::Vector3d::UnitY());
    Eigen::AngleAxisd yaw_aa(yaw_rad,     Eigen::Vector3d::UnitZ());
    Eigen::Quaterniond q = (yaw_aa * pitch_aa * roll_aa).normalized();
    queue_.push({q, t});

    std::lock_guard<std::mutex> lock(mutex_);

    state_.yaw          = static_cast<float>(yaw_rad);
    state_.pitch        = static_cast<float>(pitch_rad);
    state_.yaw_vel      = 0.0f;
    state_.pitch_vel    = 0.0f;
    state_.bullet_speed = 0.0f;
    state_.bullet_count = 0;

    switch (rx_data_.mode) {
      case 0:  mode_ = GimbalMode::IDLE;       break;
      case 1:  mode_ = GimbalMode::AUTO_AIM;   break;
      case 2:  mode_ = GimbalMode::SMALL_BUFF; break;
      case 3:  mode_ = GimbalMode::BIG_BUFF;   break;
      default:
        mode_ = GimbalMode::IDLE;
        tools::logger()->warn("[Gimbal] Invalid mode: {}", rx_data_.mode);
        break;
    }
  }

  tools::logger()->info("[Gimbal] read_thread stopped.");
}

void Gimbal::reconnect()
{
  for (int i = 0; i < 10 && !quit_; ++i) {
    tools::logger()->warn("[Gimbal] Reconnecting serial, attempt {}/10...", i + 1);
    try {
      serial_.close();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    } catch (...) {
    }
    try {
      serial_.open();
      queue_.clear();
      tools::logger()->info("[Gimbal] Reconnected serial successfully.");
      break;
    } catch (const std::exception & e) {
      tools::logger()->warn("[Gimbal] Reconnect failed: {}", e.what());
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

}  // namespace io
