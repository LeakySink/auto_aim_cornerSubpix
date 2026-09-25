// 海康相机 → ROS2 /image_raw
// 用于对比本仓库标定与 ROS camera_calibration。
//
// 编译后：
//   source /opt/ros/humble/setup.bash
//   ./build/tests/ros2_image_pub_test -c configs/sentry.yaml
//
// 另开终端跑 ROS 标定：
//   ros2 run camera_calibration cameracalibrator --size 11x8 --square 0.04 \
//     image:=/image_raw camera:=/

#include <algorithm>
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

#include "io/hikrobot/hikrobot.hpp"
#include "tools/exiter.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                       | 输出命令行参数说明}"
  "{config-path c  | configs/sentry.yaml   | 海康 yaml（须 camera_name: hikrobot）}"
  "{topic t        | image_raw             | 图像话题}"
  "{info-topic     | camera_info           | CameraInfo 话题}"
  "{frame f        | camera                | frame_id}"
  "{info i         |                       | 可选：带 camera_matrix 的内参 yaml，一并发布 CameraInfo}"
  "{rate r         | 30                    | 发布上限 Hz}";

namespace
{

sensor_msgs::msg::Image to_image_msg(const cv::Mat & bgr, const std_msgs::msg::Header & header)
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

  info.r = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  info.p = {K[0], K[1], K[2], 0, K[3], K[4], K[5], 0, K[6], K[7], K[8], 0};
  info.width = static_cast<uint32_t>(width);
  info.height = static_cast<uint32_t>(height);
  info.distortion_model = "plumb_bob";
  return true;
}

std::unique_ptr<io::HikRobot> open_hikrobot(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  const auto name = tools::read<std::string>(yaml, "camera_name");
  if (name != "hikrobot") {
    throw std::runtime_error(
      fmt::format("{} 的 camera_name='{}'，本节点只支持 hikrobot", config_path, name));
  }

  const double exposure_ms = tools::read<double>(yaml, "exposure_ms");
  const double gain = tools::read<double>(yaml, "gain");
  const auto vid_pid = tools::read<std::string>(yaml, "vid_pid");

  tools::RemoteLogger::instance().log(
    "INFO", "HikRobot open  exposure_ms={:.2f} gain={:.1f} vid_pid={}", exposure_ms, gain, vid_pid);
  return std::make_unique<io::HikRobot>(exposure_ms, gain, vid_pid);
}

}  // namespace

int main(int argc, char ** argv)
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    fmt::print(
      "\n海康 → /image_raw，对比本仓库标定 vs ROS camera_calibration。\n"
      "  ./build/tests/ros2_image_pub_test -c configs/sentry.yaml\n"
      "  ./build/tests/ros2_image_pub_test -c configs/sentry.yaml -i configs/sentry.yaml\n"
      "  ros2 run camera_calibration cameracalibrator --size 11x8 --square 0.04 \\\n"
      "      image:=/image_raw camera:=/\n");
    return 0;
  }

  const auto config_path = cli.get<std::string>("config-path");
  const auto topic = cli.get<std::string>("topic");
  const auto info_topic = cli.get<std::string>("info-topic");
  const auto frame_id = cli.get<std::string>("frame");
  const auto info_path = cli.get<std::string>("info");
  const double rate_hz = std::max(1.0, cli.get<double>("rate"));
  const auto period = std::chrono::duration<double>(1.0 / rate_hz);

  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("ros2_image_pub_test");
  // 相对话题名 → 绝对 /image_raw（与 camera_calibration 默认一致）
  auto img_pub = node->create_publisher<sensor_msgs::msg::Image>(topic, rclcpp::SensorDataQoS());
  auto info_pub =
    node->create_publisher<sensor_msgs::msg::CameraInfo>(info_topic, rclcpp::SensorDataQoS());

  YAML::Node info_yaml;
  if (!info_path.empty()) {
    try {
      info_yaml = YAML::LoadFile(info_path);
    } catch (const std::exception & e) {
      tools::RemoteLogger::instance().log("WARN", "load info yaml failed: {}", e.what());
    }
  }
  const bool have_info = info_yaml && info_yaml["camera_matrix"];
  if (!info_path.empty() && !have_info)
    tools::RemoteLogger::instance().log("WARN", "info 无 camera_matrix，仅发图像: {}", info_path);
  else if (have_info)
    tools::RemoteLogger::instance().log("INFO", "同时发布 CameraInfo ← {}", info_path);

  std::unique_ptr<io::HikRobot> cam;
  try {
    cam = open_hikrobot(config_path);
  } catch (const std::exception & e) {
    tools::RemoteLogger::instance().log("ERROR", "{}", e.what());
    rclcpp::shutdown();
    return 1;
  }

  tools::Exiter exiter;
  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;
  auto last_stamp = std::chrono::steady_clock::now();
  auto next_pub = std::chrono::steady_clock::now();

  tools::RemoteLogger::instance().log(
    "INFO", "publishing /{} (+ /{}) frame_id={} max {:.0f} Hz", topic, info_topic, frame_id,
    rate_hz);

  while (!exiter.exit() && rclcpp::ok()) {
    cam->read(img, stamp);
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
      tools::RemoteLogger::instance().log(
        "INFO", "{:.1f} fps  {}x{}", 1.0 / dt, img.cols, img.rows);

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
