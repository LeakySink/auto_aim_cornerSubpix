// 海康 → /image_raw（测试用，参数写死）
//   source /opt/ros/humble/setup.bash && ./build/tests/ros2_image_pub_test
//   ros2 run camera_calibration cameracalibrator --size 11x8 --square 0.04 \
//     image:=/image_raw camera:=/

#include <chrono>
#include <memory>
#include <thread>

#include <opencv2/opencv.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include "io/hikrobot/hikrobot.hpp"
#include "tools/exiter.hpp"

using namespace std::chrono_literals;

int main(int argc, char ** argv)
{
  constexpr double kExposureMs = 32.0;
  constexpr double kGain = 10.0;
  const std::string kVidPid = "2bdf:0001";

  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("ros2_image_pub_test");
  auto pub = node->create_publisher<sensor_msgs::msg::Image>("image_raw", rclcpp::SensorDataQoS());

  io::HikRobot cam(kExposureMs, kGain, kVidPid);
  tools::Exiter exiter;
  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  while (!exiter.exit() && rclcpp::ok()) {
    cam.read(img, stamp);
    if (img.empty()) {
      std::this_thread::sleep_for(5ms);
      continue;
    }

    sensor_msgs::msg::Image msg;
    msg.header.stamp = node->get_clock()->now();
    msg.header.frame_id = "camera";
    msg.height = static_cast<uint32_t>(img.rows);
    msg.width = static_cast<uint32_t>(img.cols);
    msg.encoding = "bgr8";
    msg.is_bigendian = false;
    msg.step = static_cast<uint32_t>(img.cols * img.elemSize());
    msg.data.assign(img.data, img.data + img.total() * img.elemSize());
    pub->publish(msg);

    rclcpp::spin_some(node);
  }

  rclcpp::shutdown();
  return 0;
}
