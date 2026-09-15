#ifndef TOOLS_RDBG_ENGINE_HPP
#define TOOLS_RDBG_ENGINE_HPP

#include "control.hpp"
#include "data.hpp"
#include "image.hpp"
#include "session.hpp"

#include "tools/remote_logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools
{
namespace rdbg
{

// 把 L0–L3 拼起来：主线程只入队，三条 worker 各管一层。
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

private:
  struct VarEntry
  {
    uint64_t ts;
    std::string json_str;
  };

  void var_loop();
  void img_loop();
  void ctrl_loop();
  static std::string resolve_sender(const std::string & name);

  RemoteLogger::Config cfg_;
  std::atomic<bool> running_{false};
  bool remote_ok_{false};

  ControlPlane control_;
  DataPlane data_{control_};
  Session session_;
  FpsGate fps_;
  Mailbox mailbox_;

  std::vector<VarEntry> var_buf_;
  std::mutex var_mtx_;
  std::condition_variable var_cv_;
  std::mutex var_wake_mtx_;

  std::thread var_worker_;
  std::thread img_worker_;
  std::thread ctrl_worker_;
  std::chrono::steady_clock::time_point last_hb_{};
};

}  // namespace rdbg
}  // namespace tools

#endif
