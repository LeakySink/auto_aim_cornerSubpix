#ifndef IO__CAMERA_HPP
#define IO__CAMERA_HPP

#include <chrono>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>

#include "fps_meter.hpp"

namespace io
{
class CameraBase
{
public:
  virtual ~CameraBase() = default;
  virtual void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp) = 0;
};

class Camera
{
public:
  Camera(const std::string & config_path);
  void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp);
  /// 相机 read 滑动窗口帧率（Hz）；供 RemoteDebug / plot 上报。
  double fps() const { return cam_meter_.fps(); }

private:
  std::unique_ptr<CameraBase> camera_;
  FpsMeter cam_meter_;
};

}  // namespace io

#endif  // IO__CAMERA_HPP