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
#include <cstdio>

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "io/remote_debug.hpp"
#include "tasks/auto_aim/center_aimer.hpp"
#include "tasks/auto_aim/kalman_markers.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/rdbg/markers/viz_markers.hpp"
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

    // ---- 可视化：落点 + 到达时刻的板 + HUD ----
    // 不能实弹时，靠这张图回答"如果现在开火，弹丸会打在哪块板上"：
    //   绿细框 = 当前所有板（EKF 估计）
    //   红十   = **落点**（瞄准点 = 外推后的旋转中心）
    //   粗框   = **弹丸到达时刻**预计被命中的板：
    //            绿 = 会开火 / 黄 = 在窗口内但云台没到位 / 橙 = 那一刻没压线
    //   左上文字 = 两个半条件 + 角误差/窗口 + 距离/弹速/飞行时间/距穿越时间
    if (res.valid && !targets.empty()) {
      const auto & target = targets.front();

      // 1) 当前所有板
      for (const auto & xyza : target.armor_xyza_list()) {
        const auto pts =
          solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
        tools::draw_points(img, pts, {0, 255, 0});
      }

      // 2) 落点
      const std::vector<cv::Point3f> aim_pt{{static_cast<float>(res.aim_xyz.x()),
                                             static_cast<float>(res.aim_xyz.y()),
                                             static_cast<float>(res.aim_xyz.z())}};
      const auto aim_px = solver.world2pixel(aim_pt);
      if (!aim_px.empty()) {
        const cv::Point2f px = aim_px.front();
        if (std::isfinite(px.x) && std::isfinite(px.y) && px.x > 0 && px.y > 0 &&
            px.x < img.cols && px.y < img.rows) {
          cv::drawMarker(img, px, {0, 0, 255}, cv::MARKER_CROSS, 26, 2);
          cv::circle(img, px, 10, {0, 0, 255}, 1);
        }
      }

      // 3) 到达时刻预计被命中的板
      if (res.plate_id >= 0) {
        auto_aim::Target pred = target;
        pred.predict(res.total_time);
        const auto pred_plates = pred.armor_xyza_list();
        if (res.plate_id < static_cast<int>(pred_plates.size())) {
          const auto & xyza = pred_plates[res.plate_id];
          const auto pts =
            solver.reproject_armor(xyza.head(3), xyza[3], target.armor_type, target.name);
          if (pts.size() >= 4) {
            const cv::Scalar color = res.fire ? cv::Scalar(0, 255, 0)
                                              : (res.in_window ? cv::Scalar(0, 255, 255)
                                                               : cv::Scalar(0, 165, 255));
            cv::polylines(
              img, std::vector<std::vector<cv::Point>>{{pts[0], pts[1], pts[2], pts[3]}}, true,
              color, 3);
          }
        }
      }

      // 4) HUD
      char hud[256];
      std::snprintf(
        hud, sizeof(hud), "fire=%d in_win=%d settled=%d plate=%d | %s", res.fire ? 1 : 0,
        res.in_window ? 1 : 0, res.settled ? 1 : 0, res.plate_id,
        res.fire ? "WOULD FIRE" : (res.in_window ? "wait gimbal" : "wait plate"));
      cv::putText(
        img, hud, {10, 26}, cv::FONT_HERSHEY_SIMPLEX, 0.7,
        res.fire ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 255, 255), 2);
      std::snprintf(
        hud, sizeof(hud), "yaw %.2f/%.2f deg  pitch %.2f/%.2f deg", res.align_err_deg,
        res.hit_window_deg / 2.0, res.pitch_err_deg, res.pitch_window_deg / 2.0);
      cv::putText(img, hud, {10, 52}, cv::FONT_HERSHEY_SIMPLEX, 0.6, {255, 255, 255}, 1);
      std::snprintf(
        hud, sizeof(hud), "d %.2f m  v %.1f m/s  fly %.3f s  cross %.3f s", res.aim_xyz.norm(),
        bullet_speed, res.fly_time, res.t_to_cross);
      cv::putText(img, hud, {10, 76}, cv::FONT_HERSHEY_SIMPLEX, 0.6, {255, 255, 255}, 1);
    }

    nlohmann::json data;
    data["t"] = tools::delta_time(std::chrono::steady_clock::now(), t0);
    data["gimbal_yaw"] = gs.yaw;
    data["gimbal_pitch"] = gs.pitch;
    data["bullet_speed"] = bullet_speed;

    data["heavy_valid"] = res.valid ? 1 : 0;
    data["fire"] = (res.valid && res.fire) ? 1 : 0;
    // fire 一直是 0 时，先看这两个谁不满足：
    //   in_window=0 → 目标/预测问题（板没转到线上，或 h≠0 的板被竖直窗口排除）
    //   settled=0   → 云台没跟上（pitch 反馈抬头为正、指令抬头为负，误差是相加）
    data["heavy_in_window"] = res.in_window ? 1 : 0;
    data["heavy_settled"] = res.settled ? 1 : 0;
    data["heavy_plate_id"] = res.plate_id;
    data["heavy_align_err_deg"] = res.align_err_deg;
    data["heavy_window_deg"] = res.hit_window_deg;
    data["heavy_pitch_err_deg"] = res.pitch_err_deg;
    data["heavy_pitch_window_deg"] = res.pitch_window_deg;
    data["heavy_fly_time"] = res.fly_time;
    data["heavy_total_time"] = res.total_time;  // launch_delay + fly_time
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

    // ---- 3D 标记：当前状态（kalman_markers）+ 落点 / 到达时刻的板 ----
    // host 的 Watch 页只看 `markers` 这一个键，所以额外标记要并进同一个数组。
    if (!targets.empty() && res.valid) {
      const auto & target = targets.front();
      nlohmann::json markers = tools::viz::kalman_markers(target);

      tools::viz::MarkerArray extra;
      const float alpha = res.fire ? 1.0f : 0.55f;
      const Eigen::Vector4d c_aim{1.0, 0.2, 0.2, 1.0};
      const Eigen::Vector4d c_hit = res.fire ? Eigen::Vector4d{0.1, 1.0, 0.2, alpha}
                                             : Eigen::Vector4d{1.0, 0.8, 0.1, alpha};

      extra.sphere("heavy.aim", "a", res.aim_xyz, 0.05, c_aim);
      extra.line_list(
        "heavy.los", "l", {Eigen::Vector3d::Zero(), res.aim_xyz}, 0.006,
        {1.0, 0.2, 0.2, 0.6});

      if (res.plate_id >= 0) {
        auto_aim::Target pred = target;
        pred.predict(res.total_time);
        const auto pred_plates = pred.armor_xyza_list();
        if (res.plate_id < static_cast<int>(pred_plates.size())) {
          extra.sphere(
            "heavy.hit", "h", pred_plates[res.plate_id].head(3), 0.05,
            res.in_window ? c_hit : Eigen::Vector4d{1.0, 0.5, 0.1, alpha});
        }
      }

      for (auto & m : extra.to_json()) markers.push_back(m);
      data["markers"] = markers;
    }

    tools::RemoteLogger::instance().plot(data);
    tools::RemoteLogger::instance().plot_image(img, {{"name", "heavy_aim"}});
    io::RemoteDebug::on_frame(camera.fps());
  }

  gimbal.send(false, false, 0, 0, 0, 0, 0, 0);
  return 0;
}
