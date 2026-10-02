#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <opencv2/opencv.hpp>

#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

const std::string keys =
  "{help h usage ? | | 输出命令行参数说明}"
  "{@config-path   | | yaml配置文件路径 }";

using namespace std::chrono_literals;

namespace
{
std::vector<double> mat3_row_major(const Eigen::Matrix3d & R)
{
  std::vector<double> out(9);
  Eigen::Matrix<double, 3, 3, Eigen::RowMajor> Rm = R;
  for (int i = 0; i < 9; ++i) out[i] = Rm.data()[i];
  return out;
}

std::vector<double> vec3(const Eigen::Vector3d & v) { return {v.x(), v.y(), v.z()}; }

// 与 auto_aim::Solver::set_R_gimbal2world 一致：
// R_gimbal2world = R_gimbal2imubody^T * R_imubody2imuabs * R_gimbal2imubody
Eigen::Matrix3d compute_R_gimbal2world(
  const Eigen::Quaterniond & q, const Eigen::Matrix3d & R_gimbal2imubody)
{
  Eigen::Matrix3d R_imubody2imuabs = q.normalized().toRotationMatrix();
  return R_gimbal2imubody.transpose() * R_imubody2imuabs * R_gimbal2imubody;
}

void init_remote_logger_tfviz(const YAML::Node & yaml)
{
  auto node = yaml["remote_logger"];
  if (!node) {
    tools::RemoteLogger::instance().log("ERROR", "[YAML] remote_logger not found!");
    exit(1);
  }

  tools::RemoteLogger::Config cfg;
  cfg.control_port = tools::read<uint16_t>(node, "control_port");
  cfg.enable_remote = tools::read<bool>(node, "enable_remote");
  cfg.enable_local = tools::read<bool>(node, "enable_local");
  cfg.log_dir = tools::read<std::string>(node, "log_dir");
  cfg.var_buffer_size = tools::read<size_t>(node, "var_buffer_size");
  cfg.img_buffer_size = tools::read<size_t>(node, "img_buffer_size");
  cfg.img_width = tools::read<int>(node, "img_width");
  cfg.img_quality = tools::read<int>(node, "img_quality");
  cfg.heartbeat_interval_ms = tools::read<uint32_t>(node, "heartbeat_interval_ms");
  cfg.sender_name = tools::read<std::string>(node, "sender_name");
  if (cfg.sender_name.empty()) cfg.sender_name = "tf_pub";
  cfg.app = "tfviz";  // 强制：门户车辆列表映射到 TF Viz
  if (node["beacon_interval_ms"])
    cfg.beacon_interval_ms = node["beacon_interval_ms"].as<uint32_t>();
  if (node["head_timeout_ms"]) cfg.head_timeout_ms = node["head_timeout_ms"].as<uint32_t>();
  if (node["beacon_port"]) cfg.beacon_port = node["beacon_port"].as<uint16_t>();

  tools::RemoteLogger::instance().init(cfg);
}
}  // namespace

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  auto config_path_arg = cli.get<std::string>("@config-path");
  if (cli.has("help") || !cli.has("@config-path")) {
    cli.printMessage();
    return 0;
  }

  const std::string config_path = tools::project_path(config_path_arg);
  const std::string config_name = std::filesystem::path(config_path).filename().string();

  auto yaml = tools::load(config_path_arg);
  auto R_gimbal2imubody_data = yaml["R_gimbal2imubody"].as<std::vector<double>>();
  auto R_camera2gimbal_data = yaml["R_camera2gimbal"].as<std::vector<double>>();
  auto t_camera2gimbal_data = yaml["t_camera2gimbal"].as<std::vector<double>>();
  Eigen::Matrix3d R_gimbal2imubody =
    Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(R_gimbal2imubody_data.data());
  Eigen::Matrix3d R_camera2gimbal =
    Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(R_camera2gimbal_data.data());
  Eigen::Vector3d t_camera2gimbal(t_camera2gimbal_data.data());

  tools::Exiter exiter;
  init_remote_logger_tfviz(yaml);
  io::Gimbal gimbal(config_path_arg);

  while (!exiter.exit()) {
    auto t = std::chrono::steady_clock::now();
    auto q = gimbal.q(t);
    auto ypr = tools::eulers(q, 2, 1, 0);

    Eigen::Matrix3d R_gimbal2world = compute_R_gimbal2world(q, R_gimbal2imubody);
    Eigen::Vector3d t_camera2world = R_gimbal2world * t_camera2gimbal;
    Eigen::Matrix3d R_camera2world = R_gimbal2world * R_camera2gimbal;

    nlohmann::json tf;
    tf["config_path"] = config_path;
    tf["config_name"] = config_name;
    tf["q"] = {q.w(), q.x(), q.y(), q.z()};
    tf["R_gimbal2imubody"] = mat3_row_major(R_gimbal2imubody);
    tf["R_camera2gimbal"] = mat3_row_major(R_camera2gimbal);
    tf["t_camera2gimbal"] = vec3(t_camera2gimbal);
    tf["R_gimbal2world"] = mat3_row_major(R_gimbal2world);
    tf["R_camera2world"] = mat3_row_major(R_camera2world);
    tf["t_camera2world"] = vec3(t_camera2world);

    nlohmann::json data;
    data["tf"] = tf;
    data["gimbal_yaw"] = ypr[0];
    data["gimbal_pitch"] = ypr[1];
    data["gimbal_roll"] = ypr[2];
    data["cam_x"] = t_camera2world.x();
    data["cam_y"] = t_camera2world.y();
    data["cam_z"] = t_camera2world.z();
    tools::RemoteLogger::instance().plot(data);

    std::this_thread::sleep_for(20ms);
  }

  return 0;
}
