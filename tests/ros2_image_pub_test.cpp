#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>
#include <opencv2/opencv.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <yaml-cpp/yaml.h>

#include "io/camera.hpp"
#include "io/usbcamera/usbcamera.hpp"
#include "tools/exiter.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                          | 输出命令行参数说明}"
  "{config-path c  | configs/camera.yaml      | 主相机 yaml（io::Camera）}"
  "{name n         |                          | USB 设备名，如 video0；设置后走 USBCamera}"
  "{@usb-config    | configs/sentry.yaml      | USB 用的配置文件（仅 --name 时）}"
  "{topic t        | image_raw                | 图像话题名}"
  "{info-topic     | camera_info              | CameraInfo 话题名}"
  "{frame f        | camera                   | frame_id}"
  "{info i         |                          | 内参 yaml（含 camera_matrix）；可为空}"
  "{rate r         | 30                       | 发布上限 Hz}";

namespace
{

sensor_msgs::msg::Image to_image_msg(
  const cv::Mat & bgr, const std_msgs::msg::Header & header)
{
  sensor_msgs::msg::Image msg;
  msg.header = header;
  msg.height = static_cast<uint32_t>(bgr.rows);
  msg.width = static_cast<uint32_t>(bgr.cols);
  msg.encoding = "bgr8";
  msg.is_bigendian = false;
  msg.step = static_cast<uint32_t>(bgr.cols * bgr.elemSize());
  msg.data.assign(bgr.data, bgr.data + bgr.total() * bgr.elemSize());
  return msg;
}

bool fill_camera_info(
  sensor_msgs::msg::CameraInfo & info, const YAML::Node & y, int width, int height)
{
  if (!y || !y["camera_matrix"] || !y["camera_matrix"].IsSequence() ||
      y["camera_matrix"].size() != 9)
    return false;

  const auto K = y["camera_matrix"].as<std::vector<double>>();
  for (size_t i = 0; i < 9; ++i) info.k[i] = K[i];

  info.d.clear();
  if (y["distort_coeffs"] && y["distort_coeffs"].IsSequence())
    info.d = y["distort_coeffs"].as<std::vector<double>>();

  // 无旋转；投影矩阵 P = [K|0]
  info.r = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  info.p = {K[0], K[1], K[2], 0, K[3], K[4], K[5], 0, K[6], K[7], K[8], 0};

  info.width = static_cast<uint32_t>(width);
  info.height = static_cast<uint32_t>(height);
  info.distortion_model = "plumb_bob";
  return true;
}

YAML::Node load_info_yaml(const std::string & path)
{
  if (path.empty()) return {};

  try {
    return YAML::LoadFile(path);
  } catch (const std::exception & e) {
    tools::RemoteLogger::instance().log("WARN", "load info yaml failed: {}", e.what());
    return {};
  }
}

}  // namespace

int main(int argc, char ** argv)
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    fmt::print(
      "\n用途：把相机图发到 ROS2，便于用 camera_calibration / rqt_image_view 对比标定。\n"
      "示例：\n"
      "  ./build/ros2_image_pub_test -c configs/camera.yaml\n"
      "  ./build/ros2_image_pub_test -c configs/camera.yaml -i calibration/result.yaml\n"
      "  ./build/ros2_image_pub_test -n video0 -i configs/sentry.yaml\n"
      "  ros2 run camera_calibration cameracalibrator --size 11x8 --square 0.04 \\\n"
      "      image:=/image_raw camera:=/\n");
    return 0;
  }

  const auto config_path = cli.get<std::string>("config-path");
  const auto usb_name = cli.get<std::string>("name");
  const auto usb_config = cli.get<std::string>(0);
  const auto topic = cli.get<std::string>("topic");
  const auto info_topic = cli.get<std::string>("info-topic");
  const auto frame_id = cli.get<std::string>("frame");
  const auto info_path = cli.get<std::string>("info");
  const double rate_hz = std::max(1.0, cli.get<double>("rate"));
  const auto period = std::chrono::duration<double>(1.0 / rate_hz);

  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("ros2_image_pub_test");
  auto img_pub = node->create_publisher<sensor_msgs::msg::Image>(topic, 10);
  auto info_pub = node->create_publisher<sensor_msgs::msg::CameraInfo>(info_topic, 10);

  YAML::Node info_yaml = load_info_yaml(info_path);
  const bool have_info = info_yaml && info_yaml["camera_matrix"];
  if (!info_path.empty() && !have_info) {
    tools::RemoteLogger::instance().log(
      "WARN", "info yaml 无 camera_matrix，仅发布图像: {}", info_path);
  } else if (have_info) {
    tools::RemoteLogger::instance().log("INFO", "将同时发布 CameraInfo ← {}", info_path);
  }

  std::unique_ptr<io::Camera> main_cam;
  std::unique_ptr<io::USBCamera> usb_cam;
  if (!usb_name.empty()) {
    tools::RemoteLogger::instance().log(
      "INFO", "USB camera '{}' config={}", usb_name, usb_config);
    usb_cam = std::make_unique<io::USBCamera>(usb_name, usb_config);
  } else {
    tools::RemoteLogger::instance().log("INFO", "main camera config={}", config_path);
    main_cam = std::make_unique<io::Camera>(config_path);
  }

  tools::Exiter exiter;
  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;
  auto last_stamp = std::chrono::steady_clock::now();
  auto next_pub = std::chrono::steady_clock::now();

  tools::RemoteLogger::instance().log(
    "INFO", "publishing /{} (+ /{})  frame_id={}  max {:.0f} Hz", topic, info_topic, frame_id,
    rate_hz);

  while (!exiter.exit() && rclcpp::ok()) {
    if (usb_cam)
      usb_cam->read(img, stamp);
    else
      main_cam->read(img, stamp);

    if (img.empty()) {
      std::this_thread::sleep_for(5ms);
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_pub) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    next_pub = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

    const auto dt = tools::delta_time(stamp, last_stamp);
    last_stamp = stamp;
    if (dt > 1e-6)
      tools::RemoteLogger::instance().log("INFO", "{:.1f} fps  {}x{}", 1.0 / dt, img.cols, img.rows);

    std_msgs::msg::Header header;
    header.stamp = node->get_clock()->now();
    header.frame_id = frame_id;

    img_pub->publish(to_image_msg(img, header));

    if (have_info) {
      sensor_msgs::msg::CameraInfo cinfo;
      cinfo.header = header;
      if (fill_camera_info(cinfo, info_yaml, img.cols, img.rows)) info_pub->publish(cinfo);
    }

    rclcpp::spin_some(node);
  }

  rclcpp::shutdown();
  return 0;
}
