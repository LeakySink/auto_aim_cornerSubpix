#ifndef TOOLS__REMOTE_LOGGER_HPP
#define TOOLS__REMOTE_LOGGER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace tools
{

/// 车上调试日志。实现在 tools/rdbg/（传输 / 控制 / 数据 / 本地会话）。
///
///   plot / log     → 本地 .rlog + UDP（仅队首）
///   plot_image     → 30fps 入选 clone → JPEG → .rlog + UDP（仅队首）
///   enable_remote  → beacon + host 队列（host 来注册）
class RemoteLogger
{
public:
  struct Config
  {
    uint16_t control_port = 15000;
    uint16_t beacon_port = 15999;
    bool enable_remote = true;
    bool enable_local = true;
    std::string log_dir = "./logs";
    size_t var_buffer_size = 1024;
    size_t img_buffer_size = 10;  // yaml 兼容保留
    int img_width = 640;
    int img_quality = 50;
    uint32_t heartbeat_interval_ms = 0;
    std::string sender_name;
    uint32_t beacon_interval_ms = 1000;
    uint32_t head_timeout_ms = 2000;
  };

  static RemoteLogger & instance();

  void init(const Config & cfg);
  void init(const std::string & config_path);
  void plot(const nlohmann::json & data);
  void log(const std::string & level, const std::string & msg);

  template <typename... Args>
  void log(const std::string & level, const std::string & fmt_str, Args &&... args)
  {
    log(level, fmt::format(fmt::runtime(fmt_str), std::forward<Args>(args)...));
  }

  void plot_image(const cv::Mat & img, const nlohmann::json & meta);
  /// host 网页下发的标定指令（add/calibrate/save/drop/reset/undistort/quit）
  bool poll_calib_cmd(std::string & cmd);
  void shutdown();

private:
  RemoteLogger();
  ~RemoteLogger();
  RemoteLogger(const RemoteLogger &) = delete;
  RemoteLogger & operator=(const RemoteLogger &) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tools

#endif  // TOOLS__REMOTE_LOGGER_HPP
