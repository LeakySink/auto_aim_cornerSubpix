#ifndef TOOLS_RDBG_ENGINE_HPP
#define TOOLS_RDBG_ENGINE_HPP

#include "control.hpp"
#include "data.hpp"
#include "image.hpp"
#include "session.hpp"
#include "tx_profile.hpp"

#include "tools/remote_logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tools
{
namespace rdbg
{

// 主线程只入队。worker：先落盘再非阻塞 UDP；disk_worker 异步写 .rlog。
class Engine
{
public:
  void init(const RemoteLogger::Config & cfg);
  void shutdown();
  void plot(const nlohmann::json & data);
  void log(const std::string & level, const std::string & msg);
  void plot_image(const cv::Mat & img, const nlohmann::json & meta);
  bool poll_calib_cmd(std::string & cmd);
  bool poll_json(nlohmann::json & data);
  // Host/IO 调试指令设置的远程画质上限（不改本地落盘）。
  void apply_tx_cap(const TxCap & cap);
  int tx_level() const { return tx_.level(); }

private:
  struct VarEntry
  {
    uint64_t ts;
    std::string json_str;
    uint8_t prio{1};  // 0=image 1=normal 2=warn 3=error
  };

  struct DiskJob
  {
    bool image{false};
    bool urgent{false};  // 写后立刻 fdatasync
    uint8_t prio{1};
    uint64_t ts{0};
    std::string meta;
    std::vector<uint8_t> jpeg;
    std::vector<std::pair<uint64_t, std::string>> jsons;
  };

  void var_loop();
  void img_loop();
  void ctrl_loop();
  void disk_loop();
  void enqueue_disk(DiskJob job);
  static uint8_t level_prio(const std::string & level);
  void note_stream(const std::string & name);
  void maybe_send_catalog();
  void maybe_plot_tx_stats();
  static std::string resolve_sender(const std::string & name);

  RemoteLogger::Config cfg_;
  std::atomic<bool> running_{false};
  bool remote_ok_{false};

  ControlPlane control_;
  DataPlane data_{control_};
  Session session_;
  FpsGate fps_;
  FpsGate tx_fps_;
  Mailbox mailbox_;
  TxAdaptor tx_;
  std::atomic<uint32_t> img_tx_ok_{0};
  std::chrono::steady_clock::time_point last_tx_stats_{};

  std::vector<VarEntry> var_buf_;
  std::mutex var_mtx_;
  std::condition_variable var_cv_;
  std::mutex var_wake_mtx_;

  std::mutex stream_mtx_;
  std::unordered_set<std::string> known_streams_;
  std::chrono::steady_clock::time_point last_catalog_{};

  std::deque<DiskJob> disk_q_;
  std::mutex disk_mtx_;
  std::condition_variable disk_cv_;
  std::condition_variable disk_idle_cv_;
  std::atomic<bool> disk_open_{false};

  std::thread var_worker_;
  std::thread img_worker_;
  std::thread ctrl_worker_;
  std::thread disk_worker_;
  std::chrono::steady_clock::time_point last_hb_{};
};

}  // namespace rdbg
}  // namespace tools

#endif
