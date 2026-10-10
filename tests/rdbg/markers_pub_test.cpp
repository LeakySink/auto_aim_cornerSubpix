// 本地联调 Watch 3D Markers：用 RemoteLogger 发 plot.markers，不依赖实车/相机。
// 本文件默认不纳入 git（见 .gitignore）；构建：cmake --build build --target markers_pub_test
//
//   ./build/markers_pub_test
//   Host 开 Watch，选车辆 markers_pub，应自动出现 3D 格。

#include <chrono>
#include <cmath>
#include <thread>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include "tools/exiter.hpp"
#include "tools/rdbg/markers/viz_markers.hpp"
#include "tools/remote_logger.hpp"

using namespace std::chrono_literals;

int main()
{
  tools::RemoteLogger::Config cfg;
  cfg.control_port = 15000;
  cfg.enable_remote = true;
  cfg.enable_local = true;
  cfg.log_dir = "./logs";
  cfg.var_buffer_size = 1024;
  cfg.img_buffer_size = 10;
  cfg.img_width = 640;
  cfg.img_quality = 50;
  cfg.heartbeat_interval_ms = 500;
  cfg.sender_name = "markers_pub";
  cfg.app = "normal";  // 门户打开 Watch
  cfg.beacon_interval_ms = 1000;
  cfg.head_timeout_ms = 2000;
  tools::RemoteLogger::instance().init(cfg);

  tools::Exiter exiter;
  tools::RemoteLogger::instance().log("INFO", "[markers_pub] running; open Watch and bind markers_pub");

  const auto t0 = std::chrono::steady_clock::now();
  while (!exiter.exit()) {
    const double t =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // 绕原点缓慢转圈的假目标
    const double R = 1.2;
    const double omega = 0.4;
    const Eigen::Vector3d center{R * std::cos(omega * t), R * std::sin(omega * t), 0.35};
    const Eigen::Vector3d vel{-R * omega * std::sin(omega * t), R * omega * std::cos(omega * t), 0.0};
    const double yaw = omega * t + M_PI;  // 朝向切向外侧近似

    tools::viz::MarkerArray arr;  // frame_id 默认 world
    arr.sphere("demo.center", "c", center, 0.05, {1.0, 0.45, 0.1, 1.0});

    const double vnorm = vel.norm();
    if (vnorm > 1e-6) {
      arr.arrow(
        "demo.vel", "v", center, vel.normalized(), std::min(0.6, vnorm * 0.4),
        {0.2, 0.9, 0.3, 1.0});
    }

    // 四面「装甲」示意
    const double armor_r = 0.25;
    const Eigen::Vector3d box_scale{0.01, 0.135, 0.056};
    std::vector<Eigen::Vector3d> spokes;
    for (int i = 0; i < 4; ++i) {
      const double a = yaw + i * (M_PI / 2);
      const Eigen::Vector3d p{
        center.x() - armor_r * std::cos(a), center.y() - armor_r * std::sin(a), center.z()};
      Eigen::AngleAxisd aa(a, Eigen::Vector3d::UnitZ());
      Eigen::Quaterniond q(aa);
      arr.box("demo.armor", "a" + std::to_string(i), p, q, box_scale, {0.25, 0.55, 1.0, 0.9});
      spokes.push_back(center);
      spokes.push_back(p);
    }
    arr.line_list("demo.armor", "spokes", spokes, 0.004, {0.6, 0.6, 0.65, 0.75});

    nlohmann::json data;
    data["x"] = center.x();
    data["y"] = center.y();
    data["z"] = center.z();
    data["vx"] = vel.x();
    data["vy"] = vel.y();
    data["vz"] = vel.z();
    data["markers"] = arr.to_json();
    tools::RemoteLogger::instance().plot(data);

    std::this_thread::sleep_for(20ms);
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
