#ifndef IO__HIKROBOT_HPP
#define IO__HIKROBOT_HPP

#include <atomic>
#include <chrono>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>

#include "MvCameraControl.h"
#include "io/camera.hpp"
#include "tools/thread_safe_queue.hpp"

namespace io
{
class HikRobot : public CameraBase
{
public:
  HikRobot(double exposure_ms, double gain, const std::string & vid_pid, bool auto_gain = false);
  ~HikRobot() override;
  void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp) override;

private:
  struct CameraData
  {
    cv::Mat img;
    std::chrono::steady_clock::time_point timestamp;
  };

  double exposure_us_;
  double gain_;
  bool auto_gain_;

  std::thread daemon_thread_;
  std::atomic<bool> daemon_quit_;

  // 必须显式初始化为 nullptr：capture_start() 有 4 条提前返回的失败路径
  // （EnumDevices 失败 / 没找到相机 / CreateHandle 失败 / OpenDevice 失败），
  // 其中前三条走完 handle_ 仍是"从未赋值"的状态。守护线程紧接着会调用
  // capture_stop()，那里要拿 handle_ 去调 MVS SDK，未初始化指针等于把
  // 栈垃圾传给闭源库。
  void * handle_ = nullptr;
  std::thread capture_thread_;
  std::atomic<bool> capturing_;
  std::atomic<bool> capture_quit_;
  tools::ThreadSafeQueue<CameraData> queue_;

  int vid_, pid_;

  void capture_start();
  void capture_stop();

  void set_float_value(const std::string & name, double value);
  void set_enum_value(const std::string & name, unsigned int value);
  void set_int_value(const std::string & name, int64_t value);
  bool try_set_enum(const std::string & name, unsigned int value);

  void set_vid_pid(const std::string & vid_pid);
  void reset_usb() const;
};

}  // namespace io

#endif  // IO__HIKROBOT_HPP