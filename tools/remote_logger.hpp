#ifndef TOOLS__REMOTE_LOGGER_HPP
#define TOOLS__REMOTE_LOGGER_HPP

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

namespace tools
{

class RemoteLogger
{
public:
  struct Config
  {
    std::string remote_host = "127.0.0.1";
    uint16_t remote_port = 9871;
    std::string log_dir = "./logs";
    size_t var_buffer_size = 1024;
    size_t img_buffer_size = 10;
    int img_width = 640;
    int img_quality = 50;
    uint32_t heartbeat_interval_ms = 0;
    std::string sender_name;
    uint16_t control_port = 15000;
    uint32_t register_retry_ms = 3000;
    bool enable_remote = true;
    bool enable_local = true;
  };

  static RemoteLogger & instance();

  void init(const Config & cfg);
  void plot(const nlohmann::json & data);
  void log(const std::string & level, const std::string & msg);
  void plot_image(cv::Mat img, const nlohmann::json & meta);
  void shutdown();

private:
  RemoteLogger() = default;
  ~RemoteLogger();
  RemoteLogger(const RemoteLogger &) = delete;
  RemoteLogger & operator=(const RemoteLogger &) = delete;

  struct VarEntry
  {
    uint64_t ts;
    std::string json_str;
  };

  struct ImgEntry
  {
    uint64_t ts;
    nlohmann::json meta;
    cv::Mat img;
  };

  void worker();
  bool try_register();
  void send_heartbeat();
  std::string resolve_sender() const;
  void inject_sender(nlohmann::json & j) const;
  void flush_var_local(const std::vector<VarEntry> & entries);
  void try_send_var(const VarEntry & entry);
  void try_send_img(const std::vector<uint8_t> & jpeg, uint64_t ts,
                     const nlohmann::json & meta);
  void send_udp(const void * data, size_t len);
  uint64_t now_ns() const;

  Config cfg_;
  std::atomic<bool> running_{false};

  std::vector<VarEntry> var_buf_;
  std::mutex var_mtx_;

  std::vector<ImgEntry> img_buf_;
  std::mutex img_mtx_;

  std::condition_variable cv_;
  std::mutex wake_mtx_;
  std::thread worker_;
  std::chrono::steady_clock::time_point last_hb_{};
  std::atomic<bool> registered_{false};
  std::chrono::steady_clock::time_point last_register_ts_{};

  int sock_{-1};
  sockaddr_in addr_{};
  std::string session_file_;
  std::string sender_name_;
};

}  // namespace tools

#endif  // TOOLS__REMOTE_LOGGER_HPP
