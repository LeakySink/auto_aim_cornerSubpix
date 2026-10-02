#ifndef IO__FPS_METER_HPP
#define IO__FPS_METER_HPP

#include <chrono>
#include <mutex>

namespace io
{

/// 滑动窗口帧率计。不依赖 RemoteLogger；由 Camera::read / 主循环 tick。
class FpsMeter
{
public:
  void tick()
  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    ++count_;
    if (window_start_.time_since_epoch().count() == 0) {
      window_start_ = now;
      return;
    }
    const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_);
    if (ms.count() >= 1000) {
      fps_ = count_ * 1000.0 / static_cast<double>(ms.count());
      count_ = 0;
      window_start_ = now;
    }
  }

  double fps() const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return fps_;
  }

private:
  mutable std::mutex mtx_;
  int count_{0};
  double fps_{0};
  std::chrono::steady_clock::time_point window_start_{};
};

}  // namespace io

#endif
