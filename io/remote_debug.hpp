#ifndef IO__REMOTE_DEBUG_HPP
#define IO__REMOTE_DEBUG_HPP

#include "tools/remote_logger.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace io
{

/// Host→车调试指令入口（RemoteLogger 控制面，不是电控 CAN Command）。
///
/// `install()` 注册 JSON 回调（收到即处理，无需主循环 poll）；
/// `on_frame` 上报 loop/cam fps。
class RemoteDebug
{
public:
  /// 在 `RemoteLogger::init` 之后调用一次：注册 Host `type=json` 回调（ctrl 线程）。
  /// 可重复调用（覆盖为同一处理函数）。`shutdown` 后再 `init` 需重新 install。
  static void install()
  {
    tools::RemoteLogger::instance().set_json_callback(&RemoteDebug::on_json);
  }

  /// @deprecated 等价于 `install()`；请改在 init 后调用一次 `install()`。
  static void poll() { install(); }

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

private:
  static void on_json(const nlohmann::json & data)
  {
    if (!data.is_object()) return;
    if (data.value("cmd", "") != "set_img_tx") return;
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
    tools::RemoteLogger::instance().apply_tx_cap(max_width, max_quality, max_fps,
                                                 level);
  }
};

}  // namespace io

#endif
