#include <fmt/format.h>

#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"
#include "tools/trajectory.hpp"

// 定义命令行参数
const std::string keys =
  "{help h usage ? | | 输出命令行参数说明}"
  "{@config-path   | | yaml配置文件路径 }";

int main(int argc, char * argv[])
{
  // 读取命令行参数
  cv::CommandLineParser cli(argc, argv, keys);
  auto config_path = cli.get<std::string>(0);
  if (cli.has("help") || config_path.empty()) {
    cli.printMessage();
    return 0;
  }

  tools::Exiter exiter;

  tools::RemoteLogger::instance().init(config_path);

  // 初始化云台
  io::Gimbal gimbal(config_path);
  io::VisionToGimbal plan{};
  auto last_t = std::chrono::steady_clock::now();
  plan.yaw = 0;
  plan.pitch = 0;
  plan.distance = 3.0f;
  plan.w_yaw = 0;
  plan.w_pitch = 0;
  plan.jmp_time = 0;
  plan.target_rate = 20;
  plan.target_number = 0;

  while (!exiter.exit()) {
    auto now = std::chrono::steady_clock::now();
    auto gs = gimbal.state();
    if (tools::delta_time(now, last_t) > 1.600) {
      plan.success = 2;  // 控制+开火
      tools::RemoteLogger::instance().log("DEBUG", "fire!");
      last_t = now;
    } else {
      plan.success = 1;  // 控制不开火
    }

    gimbal.send(plan);

    // -------------- 调试输出 --------------

    nlohmann::json data;

    if (plan.success != 0) {
      data["shoot"] = plan.success == 2 ? 1 : 0;
    }

    tools::RemoteLogger::instance().plot(data);

    auto key = cv::waitKey(1);
    if (key == 'q') break;
  }

  return 0;
}
