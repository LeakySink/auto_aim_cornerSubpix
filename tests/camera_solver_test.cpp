#include <fmt/core.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"

using namespace auto_aim;

const std::string keys =
  "{help h usage ? |                      | 输出命令行参数说明 }"
  "{@config-path   | configs/sentry.yaml  | yaml配置文件的路径}";

// 单相机测试: yolo检测 + solver位置解算, 不含tracker
// 无云台姿态输入, R_gimbal2world保持单位阵, 解算结果以零姿态云台坐标系为参考
// xyz/ypd 波形通过 ./host/watch.sh 查看 (t, x, y, z, yaw, pitch, dist, conf, armor_num)
// 与 auto_aim_debug_mpc 相同: ypd 来自 solver 内的 tools::xyz2ypd(armor.xyz_in_world)
int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }
  auto config_path = cli.get<std::string>(0);

  tools::Exiter exiter;
  tools::RemoteLogger::instance().init(config_path);

  io::Camera camera(config_path);
  YOLO yolo(config_path, true);
  Solver solver(config_path);

  auto t0 = std::chrono::steady_clock::now();

  cv::Mat img;
  std::chrono::steady_clock::time_point timestamp;

  while (!exiter.exit()) {
    camera.read(img, timestamp);
    if (img.empty()) break;

    auto armors = yolo.detect(img);

    nlohmann::json data;
    data["t"] = tools::delta_time(timestamp, t0);
    data["armor_num"] = armors.size();

    const Armor * best = nullptr;
    double best_conf = -1;
    for (auto & armor : armors) {
      solver.solve(armor);
      if (armor.confidence > best_conf) {
        best_conf = armor.confidence;
        best = &armor;
      }
    }

    if (best) {
      const auto & armor = *best;

      // 球坐标系: yaw/pitch/distance, 由 solver 内 tools::xyz2ypd(armor.xyz_in_world) 得到
      auto & ypd = armor.ypd_in_world;
      auto & xyz = armor.xyz_in_world;

      data["conf"] = armor.confidence;
      data["x"] = xyz[0];
      data["y"] = xyz[1];
      data["z"] = xyz[2];
      data["yaw"] = ypd[0];
      data["pitch"] = ypd[1];
      data["dist"] = ypd[2];

      auto info = fmt::format(
        "{} {} xyz=({:.2f},{:.2f},{:.2f}) yaw={:.1f} pitch={:.1f} dist={:.2f}",
        COLORS[armor.color], ARMOR_NAMES[armor.name], xyz[0], xyz[1], xyz[2], ypd[0] * 57.3,
        ypd[1] * 57.3, ypd[2]);
      fmt::print("{}\n", info);

      tools::draw_points(img, armor.points, {0, 255, 0});
      tools::draw_text(img, info, armor.center, {0, 255, 0});

      // 用解算结果重投影, 与检测角点对比
      auto reproj =
        solver.reproject_armor(armor.xyz_in_world, armor.ypr_in_world[0], armor.type, armor.name);
      tools::draw_points(img, reproj, {0, 0, 255});
    }

    tools::RemoteLogger::instance().plot(data);
    tools::RemoteLogger::instance().plot_image(img, {{"name", "camera"}});

    cv::resize(img, img, {}, 0.5, 0.5);
    cv::imshow("reprojection", img);
    auto key = cv::waitKey(30);
    if (key == 'q') break;
  }

  tools::RemoteLogger::instance().shutdown();

  return 0;
}
