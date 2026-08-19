#ifndef TOOLS__REMOTE_LOGGER_HPP
#define TOOLS__REMOTE_LOGGER_HPP

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fmt/format.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

namespace tools
{

/// RemoteLogger — 主线程只入队，worker 分工：
///
///   plot / log  ──→ var_buf_    ──→ var_worker_  ──→ .rlog (json) + UDP
///   plot_image  ──→ 相位锁选 30fps ──→ mailbox(1) ──→ img_worker_
///                   未入选帧直接 return              resize 后放全分辨率
///                                                   JPEG + .rlog + UDP
///   (enable_remote) ctrl_worker_ ──→ 注册 / 心跳 / 重试
class RemoteLogger
{
public:
  struct Config
  {
    std::string remote_host = "127.0.0.1";
    uint16_t control_port = 15000;
    bool enable_remote = true;
    bool enable_local = true;
    std::string log_dir = "./logs";
    size_t var_buffer_size = 1024;
    size_t img_buffer_size = 10;  // yaml 兼容保留，图像侧不再用深队列
    int img_width = 640;
    int img_quality = 50;
    uint32_t heartbeat_interval_ms = 0;
    std::string sender_name;
    uint32_t register_retry_ms = 3000;
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

  void var_worker_loop();
  void img_worker_loop();
  void ctrl_worker_loop();

  bool try_select_img(uint64_t ts);
  void send_register();
  void poll_ctrl();
  void handle_ctrl_payload(const char * buf, size_t n);
  void send_heartbeat();
  std::string resolve_sender() const;
  void inject_sender(nlohmann::json & j) const;

  bool ensure_session_file();  // 调用方须已持有 session_mtx_
  void close_session_file();
  void close_session_file_unlocked();
  void maybe_flush_session(bool force = false);
  void flush_var_local(const std::vector<VarEntry> & entries);
  void flush_img_local(uint64_t ts, const nlohmann::json & meta,
                       const std::vector<uint8_t> & jpeg);

  bool encode_and_dispatch_image(ImgEntry & entry);
  void try_send_var(const VarEntry & entry);
  void try_send_img(const std::vector<uint8_t> & jpeg, uint64_t ts,
                    const nlohmann::json & meta);
  void send_udp(const void * data, size_t len);

  uint64_t now_ns() const;

  Config cfg_;
  std::atomic<bool> running_{false};

  // ── 变量队列（主线程写，var_worker 读）────────────────────────────
  std::vector<VarEntry> var_buf_;
  std::mutex var_mtx_;
  std::condition_variable var_cv_;
  std::mutex var_wake_mtx_;
  std::thread var_worker_;

  // ── 图像：相位锁 30fps + 深度 1 邮箱（在飞 1 + 等待 1）──────────
  class ImgMailbox
  {
  public:
    void reset();
    void publish(uint64_t ts, nlohmann::json meta, const cv::Mat & img);
    bool take(ImgEntry & out, int timeout_ms, const std::atomic<bool> & running);
    void wake();
    void clear();

  private:
    ImgEntry slot_;
    bool has_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
  };

  std::atomic<uint64_t> img_due_ns_{0};
  ImgMailbox img_mbox_;
  std::thread img_worker_;

  // ── 远程控制（注册 / 心跳，独立线程）────────────────────────────
  std::thread ctrl_worker_;

  // ── 本地 .rlog 会话文件（var/img worker 写，session_mtx_ 保护）──
  std::mutex session_mtx_;
  std::string session_file_;
  FILE * session_fp_{nullptr};
  std::chrono::steady_clock::time_point last_session_flush_{};

  // ── 远程 UDP（注册状态 + sendto）────────────────────────────────
  std::mutex remote_mtx_;
  std::atomic<bool> registered_{false};
  std::chrono::steady_clock::time_point last_register_ts_{};
  std::chrono::steady_clock::time_point last_ack_ts_{};
  std::chrono::steady_clock::time_point last_hb_{};
  int sock_{-1};
  sockaddr_in addr_{};

  std::string sender_name_;
};

}  // namespace tools

#endif  // TOOLS__REMOTE_LOGGER_HPP
