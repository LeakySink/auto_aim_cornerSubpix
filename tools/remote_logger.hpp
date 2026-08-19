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

/// RemoteLogger — 主线程只入队，三个 worker 分工：
///
///   plot / log  ──→ var_buf_   ──→ var_worker_   ──→ .rlog (json) + UDP
///   plot_image  ──→ img_ring_  ──→ img_worker_   ──→ .rlog (jpeg) + UDP
///                   （仅 Mat 头，零拷贝；worker 取最新帧）
///   (enable_remote) ctrl_worker_ ──→ 注册 / 心跳 / 重试（独立控制线程）
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
    size_t img_buffer_size = 10;
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

  bool try_register();
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

  bool encode_and_dispatch_image(const ImgEntry & entry);
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

  // ── 图像环形缓冲（主线程只入队 Mat 头，img_worker 异步编码最新帧）──
  class ImgRingBuffer
  {
  public:
    void reset(size_t cap);
    bool push(uint64_t ts, nlohmann::json meta, const cv::Mat & img);
    ImgEntry * acquire_latest();
    void release();
    bool wait_not_empty(int timeout_ms, const std::atomic<bool> & running);
    void wake();
    void clear();

  private:
    void drop_slot(size_t idx);

    std::vector<ImgEntry> slots_;
    size_t cap_{0};
    size_t tail_{0};
    size_t count_{0};
    bool busy_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
  };

  ImgRingBuffer img_ring_;
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
  std::chrono::steady_clock::time_point last_hb_{};
  int sock_{-1};
  sockaddr_in addr_{};

  std::string sender_name_;
};

}  // namespace tools

#endif  // TOOLS__REMOTE_LOGGER_HPP
