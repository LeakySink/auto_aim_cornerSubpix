// 重装自瞄入口：**瞄旋转中心，等板转过来再开火**。
//
// 与 auto_aim_debug_mpc 的区别：
//   - 不跑 MPC：直接把"瞄中心"的 yaw/pitch 发给云台（重装云台惯量大，
//     中心是稳定点，不需要每帧换瞄准点）；
//   - 开火判据不是"云台靠近指令"，而是"弹丸到达时刻有板压在瞄准线上"
//     （见 tasks/auto_aim/center_aimer.hpp 的推导）；
//   - 弹速优先用下发的真值，缺失/越界时用 yaml 的 bullet_speed_default。
//
// 用法：
//   ./build/src/heavy_aim configs/sentry.yaml
//   ./build/src/heavy_aim configs/sentry.yaml --bullet-speed 16.5
#include <chrono>
#include <cmath>

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "io/remote_debug.hpp"
#include "tasks/auto_aim/center_aimer.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                        | 输出命令行参数说明 }"
  "{@config-path   | configs/sentry.yaml    | 位置参数：yaml 配置文件路径 }"
  "{bullet-speed b | 0                      | 弹速 m/s（<=0 时用 yaml 的 bullet_speed_default）}";

int main(int argc, char * argv[])
{
  tools::Exiter exiter;

  cv::CommandLineParser cli(argc, argv, keys);
  const auto config_path = cli.get<std::string>(0);
  if (cli.has("help") || config_path.empty()) {
    cli.printMessage();
    return 0;
  }
  const double cli_speed = cli.get<double>("bullet-speed");

  tools::RemoteLogger::instance().init(config_path);
  io::RemoteDebug::install();

  io::Gimbal gimbal(config_path);
  io::Camera camera(config_path);
  auto_aim::YOLO yolo(config_path, false);
  auto_aim::Solver solver(config_path);
  auto_aim::Tracker tracker(config_path, solver);
  auto_aim::CenterAimer center_aimer(config_path);

  // 弹速兜底：与 Aimer/Planner 同名同义（缺键 = 22 / 10 / 25）
  double speed_default = 22.0, speed_min = 10.0, speed_max = 25.0;
  {
    const auto yaml = tools::load(config_path);
    if (yaml["bullet_speed_default"] && yaml["bullet_speed_default"].IsDefined())
      speed_default = yaml["bullet_speed_default"].as<double>();
    if (yaml["bullet_speed_min"] && yaml["bullet_speed_min"].IsDefined())
      speed_min = yaml["bullet_speed_min"].as<double>();
    if (yaml["bullet_speed_max"] && yaml["bullet_speed_max"].IsDefined())
      speed_max = yaml["bullet_speed_max"].as<double>();
  }
  const double speed_fixed = (cli_speed > 0.0) ? cli_speed : 0.0;

  const auto t0 = std::chrono::steady_clock::now();
  cv::Mat img;
  std::chrono::steady_clock::time_point t;

  while (!exiter.exit()) {
    camera.read(img, t);

    // 世界系姿态：PnP 与 EKF 都用它把相机系搬到世界系
    const auto q = gimbal.q(t);
    solver.set_R_gimbal2world(q);

    auto armors = yolo.detect(img);
    auto targets = tracker.track(armors, t);

    const auto gs = gimbal.state();
    const double bullet_speed =
      (speed_fixed > 0.0) ? speed_fixed
                          : ((gs.bullet_speed >= speed_min && gs.bullet_speed <= speed_max)
                               ? gs.bullet_speed
                               : speed_default);

    auto_aim::CenterAimResult res;
    if (!targets.empty()) {
      res = center_aimer.solve(targets.front(), t, gs.yaw, gs.pitch, bullet_speed);
    }

    if (res.valid) {
      gimbal.send(
        true, res.fire, static_cast<float>(res.yaw), 0, 0, static_cast<float>(res.pitch), 0, 0);
    } else {
      gimbal.send(false, false, 0, 0, 0, 0, 0, 0);
    }

    // ---- 调试图 + rdbg 兼容字段 ----
    if (res.valid && !targets.empty()) {
      const auto & target = targets.front();
      for (const auto & xyza : target.armor_xyza_list()) {
        const auto pts = solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
        tools::draw_points(img, pts, {0, 255, 0});
      }
    }

    nlohmann::json data;
    data["t"] = tools::delta_time(std::chrono::steady_clock::now(), t0);
    data["gimbal_yaw"] = gs.yaw;
    data["gimbal_pitch"] = gs.pitch;
    data["bullet_speed"] = bullet_speed;

    data["heavy_valid"] = res.valid ? 1 : 0;
    data["fire"] = (res.valid && res.fire) ? 1 : 0;
    data["heavy_plate_id"] = res.plate_id;
    data["heavy_align_err_deg"] = res.align_err_deg;
    data["heavy_window_deg"] = res.hit_window_deg;
    data["heavy_pitch_err_deg"] = res.pitch_err_deg;
    data["heavy_pitch_window_deg"] = res.pitch_window_deg;
    data["heavy_fly_time"] = res.fly_time;
    data["heavy_t_to_cross"] = res.t_to_cross;

    if (!targets.empty()) {
      const auto x = targets.front().ekf_x();
      data["target_x"] = x[0];
      data["target_vx"] = x[1];
      data["target_y"] = x[2];
      data["target_vy"] = x[3];
      data["target_z"] = x[4];
      data["target_vz"] = x[5];
      data["target_yaw"] = x[6];
      data["w"] = x[7];
      data["armor_id"] = targets.front().last_id;
      data["heavy_aim_x"] = res.aim_xyz.x();
      data["heavy_aim_y"] = res.aim_xyz.y();
      data["heavy_aim_z"] = res.aim_xyz.z();
    }

    tools::RemoteLogger::instance().plot(data);
    tools::RemoteLogger::instance().plot_image(img, {{"name", "heavy_aim"}});
    io::RemoteDebug::on_frame(camera.fps());
  }

  gimbal.send(false, false, 0, 0, 0, 0, 0, 0);
  return 0;
}
