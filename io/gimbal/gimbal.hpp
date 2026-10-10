#ifndef IO__GIMBAL_HPP
#define IO__GIMBAL_HPP

#include <Eigen/Geometry>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

#include "serial/serial.h"
#include "tools/thread_safe_queue.hpp"

namespace io
{

// 帧头：header(0xFF) + length(总帧长) + id
struct __attribute__((packed)) FrameHead
{
  uint8_t header = 0xFF;
  uint8_t length = 0;
  uint8_t id = 0;
};

// 帧尾：CRC8（固定值 0x0D，与下位机约定）
struct __attribute__((packed)) FrameTail
{
  uint8_t crc8 = 0x0d;
};

// 上行帧 payload：云台 → 视觉，ID = 0x14，共 13 字节
struct __attribute__((packed)) GimbalToVision
{
  uint8_t mode;  // 0:空闲 1:自瞄 2:小符 3:大符
  float roll;    // 度
  float pitch;   // 度
  float yaw;     // 度
};

// 下行帧 payload：视觉 → 云台，ID = 0x81，共 27 字节
struct __attribute__((packed)) VisionToGimbal
{
  uint8_t success;      // 0:不控制 1:控制不开火 2:控制+开火
  float pitch;          // 度
  float yaw;            // 度
  float distance;       // m
  float w_pitch;        // 度/s
  float w_yaw;          // 度/s
  float jmp_time;       // s
  uint8_t target_rate;
  uint8_t target_number;
};

static_assert(sizeof(GimbalToVision) == 13);
static_assert(sizeof(VisionToGimbal) == 27);

enum class GimbalMode
{
  IDLE,
  AUTO_AIM,
  SMALL_BUFF,
  BIG_BUFF
};

struct GimbalState
{
  float yaw;             // rad（由度转换）
  float yaw_vel;         // rad/s（新协议不提供，恒为 0）
  float pitch;           // rad（由度转换）
  float pitch_vel;       // rad/s（新协议不提供，恒为 0）
  float bullet_speed;    // m/s（新协议不提供，恒为 0）
  uint16_t bullet_count; // 新协议不提供，恒为 0
};

class Gimbal
{
public:
  Gimbal(const std::string & config_path);

  ~Gimbal();

  GimbalMode mode() const;
  GimbalState state() const;
  std::string str(GimbalMode mode) const;
  Eigen::Quaterniond q(std::chrono::steady_clock::time_point t);

  // 8 参数重载：保持与上层调用方兼容，内部转换为新协议格式
  // yaw/pitch/yaw_vel/pitch_vel 单位为 rad/rad·s⁻¹，内部转换为度
  // yaw_acc/pitch_acc 新协议无对应字段，忽略
  void send(
    bool control, bool fire, float yaw, float yaw_vel, float yaw_acc, float pitch, float pitch_vel,
    float pitch_acc);

  void send(io::VisionToGimbal msg);

private:
  serial::Serial serial_;

  std::thread thread_;
  std::atomic<bool> quit_ = false;
  mutable std::mutex mutex_;

  GimbalToVision rx_data_;
  VisionToGimbal tx_data_;

  GimbalMode mode_ = GimbalMode::IDLE;
  GimbalState state_;
  tools::ThreadSafeQueue<std::tuple<Eigen::Quaterniond, std::chrono::steady_clock::time_point>>
    queue_{1000};

  bool read_bytes(uint8_t * buffer, size_t size);
  void read_thread();
  void reconnect();
};

}  // namespace io

#endif  // IO__GIMBAL_HPP
