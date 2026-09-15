#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "tasks/auto_aim/planner/planner.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/target.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"
#include "tools/thread_safe_queue.hpp"

using namespace std::chrono_literals;

namespace
{

struct SimGimbal
{
  float yaw = 0;
  float yaw_vel = 0;
  float pitch = 0;
  float pitch_vel = 0;
  float bullet_speed = 22;
  uint16_t bullet_count = 0;

  void track(const auto_aim::Plan & plan, double dt)
  {
    constexpr double kTrack = 8.0;
    const double alpha = 1.0 - std::exp(-kTrack * dt);
    yaw += static_cast<float>((plan.yaw - yaw) * alpha + plan.yaw_vel * dt);
    pitch += static_cast<float>((plan.pitch - pitch) * alpha + plan.pitch_vel * dt);
    yaw_vel = plan.yaw_vel;
    pitch_vel = plan.pitch_vel;

    if (plan.fire && std::abs(plan.yaw - yaw) < 0.04 && std::abs(plan.pitch - pitch) < 0.03) {
      bullet_count++;
    }
  }

  Eigen::Quaterniond orientation() const
  {
    return Eigen::Quaterniond(tools::rotation_matrix(Eigen::Vector3d(yaw, pitch, 0)));
  }
};

}  // namespace

const std::string keys =
  "{help h usage ? |                        | 输出命令行参数说明}"
  "{d              | 3.0                    | 模拟目标距离(m)       }"
  "{w              | 5.0                    | 模拟目标角速度(rad/s) }"
  "{v              | 22.0                   | 模拟弹速(m/s)         }"
  "{fps            | 30                     | 主循环帧率            }"
  "{@config-path   | configs/sentry.yaml    | yaml 配置文件路径     }";

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  auto config_path = cli.get<std::string>("@config-path");
  if (cli.has("help") || config_path.empty()) {
    cli.printMessage();
    return 0;
  }

  const double d = cli.get<double>("d");
  const double w = cli.get<double>("w");
  const double bullet_speed = cli.get<double>("v");
  const int fps = cli.get<int>("fps");
  const auto frame_interval = std::chrono::microseconds(1000000 / std::max(fps, 1));

  tools::Exiter exiter;
  tools::RemoteLogger::instance().init(config_path);
  tools::RemoteLogger::instance().log(
    "INFO", "mpc offline started d={:.1f}m w={:.1f}rad/s v={:.1f}m/s fps={} config={}", d, w,
    bullet_speed, fps, config_path);

  auto_aim::Solver solver(config_path);
  auto_aim::Planner planner(config_path);
  auto_aim::Target target(d, w, 0.2, 0.1);
  target.name = auto_aim::ArmorName::three;
  target.armor_type = auto_aim::ArmorType::small;

  // 画布与相机主点对齐：宽=2*cx、高=2*cy，否则重投影会偏出 640x480
  const auto camera_matrix = tools::load(config_path)["camera_matrix"].as<std::vector<double>>();
  const int img_w = std::max(640, static_cast<int>(std::lround(2.0 * camera_matrix[2])));
  const int img_h = std::max(480, static_cast<int>(std::lround(2.0 * camera_matrix[5])));

  SimGimbal gimbal;
  gimbal.bullet_speed = static_cast<float>(bullet_speed);

  tools::ThreadSafeQueue<std::optional<auto_aim::Target>, true> target_queue(1);
  target_queue.push(target);

  std::atomic<bool> quit{false};
  auto plan_thread = std::thread([&]() {
    auto t0 = std::chrono::steady_clock::now();
    auto last_tick = t0;
    auto last_debug = t0 - 1s;
    auto last_info = t0 - 2s;
    auto last_warn_periodic = t0 - 6s;
    auto last_error = t0 - 10s;
    auto last_fatal = t0 - 22s;
    auto last_lag_warn = t0;
    uint16_t last_bullet_count = 0;
    bool last_control = true;

    while (!quit) {
      const auto now = std::chrono::steady_clock::now();
      const double dt = tools::delta_time(now, last_tick);
      last_tick = now;
      const double t = tools::delta_time(now, t0);

      auto target_opt = target_queue.front();
      auto plan = planner.plan(target_opt, gimbal.bullet_speed);
      if (plan.control) {
        gimbal.track(plan, dt);
      }

      const bool fired = gimbal.bullet_count > last_bullet_count;
      last_bullet_count = gimbal.bullet_count;
      const double yaw_err = std::abs(plan.yaw - gimbal.yaw);
      const double pitch_err = std::abs(plan.pitch - gimbal.pitch);

      if (fired) {
        tools::RemoteLogger::instance().log(
          "INFO", "fired bullet={} t={:.2f}s yaw={:.3f} pitch={:.3f}", gimbal.bullet_count, t,
          gimbal.yaw, gimbal.pitch);
      }

      if (now - last_debug >= 500ms) {
        last_debug = now;
        tools::RemoteLogger::instance().log(
          "DEBUG", "t={:.2f}s yaw={:.3f} pitch={:.3f} yaw_err={:.3f} fire={} control={}", t,
          gimbal.yaw, gimbal.pitch, yaw_err, plan.fire, plan.control);
      }

      if (now - last_info >= 3s) {
        last_info = now;
        tools::RemoteLogger::instance().log(
          "INFO", "tracker ok t={:.1f}s bullets={} d={:.1f}m w={:.1f}rad/s", t,
          gimbal.bullet_count, d, w);
      }

      if (last_control && !plan.control) {
        tools::RemoteLogger::instance().log("WARN", "planner lost control at t={:.2f}s", t);
      }
      last_control = plan.control;

      if (plan.control && yaw_err > 0.12 && now - last_lag_warn >= 1s) {
        last_lag_warn = now;
        tools::RemoteLogger::instance().log(
          "WARN", "tracking lag yaw_err={:.3f} pitch_err={:.3f} t={:.2f}s", yaw_err, pitch_err, t);
      }

      if (now - last_warn_periodic >= 7s) {
        last_warn_periodic = now;
        tools::RemoteLogger::instance().log(
          "WARN", "gimbal near software limit check t={:.1f}s yaw={:.3f}", t, gimbal.yaw);
      }

      if (now - last_error >= 11s) {
        last_error = now;
        tools::RemoteLogger::instance().log(
          "ERROR", "simulated detector drop t={:.1f}s, retrying", t);
      }

      if (now - last_fatal >= 23s) {
        last_fatal = now;
        tools::RemoteLogger::instance().log(
          "FATAL", "simulated serial timeout t={:.1f}s (watch maps this to ERROR)", t);
      }

      nlohmann::json data;
      data["t"] = t;

      data["gimbal_yaw"] = gimbal.yaw;
      data["gimbal_yaw_vel"] = gimbal.yaw_vel;
      data["gimbal_pitch"] = gimbal.pitch;
      data["gimbal_pitch_vel"] = gimbal.pitch_vel;

      data["plan_yaw"] = plan.yaw;
      data["plan_yaw_vel"] = plan.yaw_vel;
      data["plan_yaw_acc"] = plan.yaw_acc;

      data["plan_pitch"] = plan.pitch;
      data["plan_pitch_vel"] = plan.pitch_vel;
      data["plan_pitch_acc"] = plan.pitch_acc;

      data["fire"] = plan.fire ? 1 : 0;
      data["fired"] = fired ? 1 : 0;

      if (target_opt.has_value()) {
        const auto & x = target_opt->ekf_x();
        data["target_x"] = x[0];
        data["target_vx"] = x[1];
        data["target_y"] = x[2];
        data["target_vy"] = x[3];
        data["target_z"] = x[4];
        data["target_vz"] = x[5];
        data["target_yaw"] = x[6];
        data["target_yaw_vel"] = x[7];
        data["w"] = x[7];
        data["measure_yaw"] = x[6];
      } else {
        data["w"] = 0.0;
      }

      tools::RemoteLogger::instance().plot(data);
      std::this_thread::sleep_for(10ms);
    }
  });

  auto loop_t0 = std::chrono::steady_clock::now();
  auto last_frame = loop_t0;
  while (!exiter.exit()) {
    const auto tick = std::chrono::steady_clock::now();
    const double sim_t = tools::delta_time(tick, loop_t0);

    target.predict(tools::delta_time(tick, last_frame));
    last_frame = tick;
    target_queue.push(target);

    solver.set_R_gimbal2world(gimbal.orientation());

    cv::Mat raw(img_h, img_w, CV_8UC3, cv::Scalar(20, 20, 30));
    tools::draw_text(
      raw, fmt::format("sim t={:.2f}s  d={:.1f}m  w={:.1f}rad/s", sim_t, d, w), {10, 24});
    tools::draw_point(raw, {img_w / 2, img_h / 2}, {60, 60, 80}, 3);

    std::vector<Eigen::Vector4d> armor_xyza_list = target.armor_xyza_list();
    cv::Mat armor_img = raw.clone();
    for (const Eigen::Vector4d & xyza : armor_xyza_list) {
      auto image_points = solver.reproject_armor(
        xyza.head(3), xyza[3], target.armor_type, target.name);
      tools::draw_points(armor_img, image_points, {0, 255, 0});
    }

    cv::Mat reproj = armor_img.clone();
    Eigen::Vector4d aim_xyza = planner.debug_xyza;
    auto aim_points = solver.reproject_armor(
      aim_xyza.head(3), aim_xyza[3], target.armor_type, target.name);
    tools::draw_points(reproj, aim_points, {0, 0, 255});

    cv::Mat hud(240, 400, CV_8UC3, cv::Scalar(16, 16, 24));
    tools::draw_text(hud, fmt::format("t={:.2f}s", sim_t), {12, 28}, {0, 255, 255}, 0.7, 1);
    tools::draw_text(
      hud, fmt::format("yaw={:.3f}  pitch={:.3f}", gimbal.yaw, gimbal.pitch), {12, 56},
      {180, 180, 200}, 0.6, 1);
    tools::draw_text(
      hud, fmt::format("yaw_vel={:.2f}  pitch_vel={:.2f}", gimbal.yaw_vel, gimbal.pitch_vel),
      {12, 84}, {180, 180, 200}, 0.6, 1);
    tools::draw_text(
      hud, fmt::format("bullets={}  speed={:.1f}m/s", gimbal.bullet_count, gimbal.bullet_speed),
      {12, 112}, {120, 200, 120}, 0.6, 1);
    const int bar_x = 12, bar_w = 376, bar_h = 14;
    cv::rectangle(hud, {bar_x, 140}, {bar_x + bar_w, 140 + bar_h}, {40, 40, 50}, -1);
    const int yaw_px = std::clamp(
      static_cast<int>((gimbal.yaw / 3.14 + 0.5) * bar_w), 0, bar_w);
    cv::rectangle(hud, {bar_x, 140}, {bar_x + yaw_px, 140 + bar_h}, {80, 160, 220}, -1);
    tools::draw_text(hud, "yaw", {12, 176}, {100, 100, 120}, 0.5, 1);
    cv::rectangle(hud, {bar_x, 188}, {bar_x + bar_w, 188 + bar_h}, {40, 40, 50}, -1);
    const int pitch_px = std::clamp(
      static_cast<int>((gimbal.pitch / 0.6 + 0.5) * bar_w), 0, bar_w);
    cv::rectangle(hud, {bar_x, 188}, {bar_x + pitch_px, 188 + bar_h}, {80, 200, 140}, -1);
    tools::draw_text(hud, "pitch", {12, 224}, {100, 100, 120}, 0.5, 1);

    auto & rl = tools::RemoteLogger::instance();
    rl.plot_image(raw, {{"name", "raw"}});
    rl.plot_image(armor_img, {{"name", "armor"}});
    rl.plot_image(reproj, {{"name", "reprojection"}});
    rl.plot_image(hud, {{"name", "hud"}});

    const auto elapsed = std::chrono::steady_clock::now() - tick;
    if (elapsed < frame_interval) {
      std::this_thread::sleep_for(frame_interval - elapsed);
    }
  }

  quit = true;
  if (plan_thread.joinable()) plan_thread.join();
  tools::RemoteLogger::instance().log(
    "INFO", "mpc offline stopped t={:.2f}s bullets={}",
    tools::delta_time(std::chrono::steady_clock::now(), loop_t0), gimbal.bullet_count);
  tools::RemoteLogger::instance().shutdown();

  return 0;
}
