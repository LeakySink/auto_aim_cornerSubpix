#ifndef IO__REMOTE_DEBUG_HPP
#define IO__REMOTE_DEBUG_HPP

#include "tools/remote_logger.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace io
{

/// Host→车调试指令入口（RemoteLogger 控制面，不是电控 CAN Command）。
///
/// 主循环每帧调用 `poll()`：消费 `set_img_tx` 等；并可用 `on_frame` 上报 loop/cam fps。
class RemoteDebug
{
public:
  /// 排空 poll_json；对 `cmd=set_img_tx` 调用 RemoteLogger::apply_tx_cap。
  static void poll()
  {
    nlohmann::json data;
    auto & rl = tools::RemoteLogger::instance();
    while (rl.poll_json(data)) {
      if (!data.is_object()) continue;
      if (data.value("cmd", "") != "set_img_tx") continue;
      const int max_width =
        data.contains("max_width") && data["max_width"].is_number_integer()
          ? data["max_width"].get<int>()
          : 0;
      const int max_quality =
        data.contains("max_quality") && data["max_quality"].is_number_integer()
          ? data["max_quality"].get<int>()
          : 0;
      const int max_fps =
        data.contains("max_fps") && data["max_fps"].is_number_integer()
          ? data["max_fps"].get<int>()
          : 0;
      const int level =
        data.contains("level") && data["level"].is_number_integer()
          ? data["level"].get<int>()
          : -1;
      rl.apply_tx_cap(max_width, max_quality, max_fps, level);
    }
  }

  /// 主循环每帧调用；约 1Hz plot `loop_fps`，若 cam_fps>=0 则附带 `cam_fps`。
  static void on_frame(double cam_fps = -1.0)
  {
    static thread_local int count = 0;
    static thread_local auto window =
      std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    ++count;
    if (window.time_since_epoch().count() == 0) {
      window = now;
      return;
    }
    const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - window);
    if (ms.count() < 1000) return;
    const double loop_fps = count * 1000.0 / static_cast<double>(ms.count());
    count = 0;
    window = now;
    nlohmann::json j = {{"loop_fps", loop_fps}};
    if (cam_fps >= 0.0) j["cam_fps"] = cam_fps;
    tools::RemoteLogger::instance().plot(j);
  }
};

}  // namespace io

#endif
