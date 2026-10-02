#include "tf_auto_pub.hpp"

#include <filesystem>
#include <mutex>
#include <vector>

#include "tools/remote_logger.hpp"

namespace tools::rdbg
{
namespace
{
struct Cache
{
  bool loaded = false;
  std::string config_path;
  std::string config_name;
  Eigen::Matrix3d R_gimbal2imubody = Eigen::Matrix3d::Identity();
  Eigen::Matrix3d R_camera2gimbal = Eigen::Matrix3d::Identity();
  Eigen::Vector3d t_camera2gimbal = Eigen::Vector3d::Zero();
};

std::mutex g_mu;
Cache g_cache;

std::vector<double> mat3_row_major(const Eigen::Matrix3d & R)
{
  std::vector<double> out(9);
  Eigen::Matrix<double, 3, 3, Eigen::RowMajor> Rm = R;
  for (int i = 0; i < 9; ++i) out[i] = Rm.data()[i];
  return out;
}

std::vector<double> vec3(const Eigen::Vector3d & v) { return {v.x(), v.y(), v.z()}; }

Eigen::Matrix3d compute_R_gimbal2world(
  const Eigen::Quaterniond & q, const Eigen::Matrix3d & R_gimbal2imubody)
{
  Eigen::Matrix3d R_imubody2imuabs = q.normalized().toRotationMatrix();
  return R_gimbal2imubody.transpose() * R_imubody2imuabs * R_gimbal2imubody;
}

nlohmann::json build_tf(const Cache & c, const Eigen::Quaterniond & q)
{
  const Eigen::Matrix3d R_g2w = compute_R_gimbal2world(q, c.R_gimbal2imubody);
  const Eigen::Matrix3d R_c2w = R_g2w * c.R_camera2gimbal;
  const Eigen::Vector3d t_c2w = R_g2w * c.t_camera2gimbal;

  nlohmann::json tf;
  tf["config_path"] = c.config_path;
  tf["config_name"] = c.config_name;
  tf["q"] = {q.w(), q.x(), q.y(), q.z()};
  tf["R_gimbal2imubody"] = mat3_row_major(c.R_gimbal2imubody);
  tf["R_camera2gimbal"] = mat3_row_major(c.R_camera2gimbal);
  tf["t_camera2gimbal"] = vec3(c.t_camera2gimbal);
  tf["R_gimbal2world"] = mat3_row_major(R_g2w);
  tf["R_camera2world"] = mat3_row_major(R_c2w);
  tf["t_camera2world"] = vec3(t_c2w);
  return tf;
}

void plot_tf(const nlohmann::json & tf)
{
  tools::RemoteLogger::instance().plot({{"tf", tf}});
}
}  // namespace

void publish_tf_extrinsics_once(const YAML::Node & root, const std::string & config_path)
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_cache.loaded) return;
  if (!root["R_gimbal2imubody"] || !root["R_camera2gimbal"] || !root["t_camera2gimbal"]) return;

  try {
    auto Rg = root["R_gimbal2imubody"].as<std::vector<double>>();
    auto Rc = root["R_camera2gimbal"].as<std::vector<double>>();
    auto tc = root["t_camera2gimbal"].as<std::vector<double>>();
    if (Rg.size() != 9 || Rc.size() != 9 || tc.size() != 3) return;

    g_cache.R_gimbal2imubody = Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(Rg.data());
    g_cache.R_camera2gimbal = Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(Rc.data());
    g_cache.t_camera2gimbal = Eigen::Vector3d(tc.data());
    g_cache.config_path = config_path;
    g_cache.config_name = std::filesystem::path(config_path).filename().string();
    g_cache.loaded = true;
  } catch (...) {
    return;
  }

  plot_tf(build_tf(g_cache, Eigen::Quaterniond::Identity()));
}

void publish_tf_attitude(const Eigen::Quaterniond & q)
{
  Cache snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_cache.loaded) return;
    snap = g_cache;
  }
  plot_tf(build_tf(snap, q));
}

}  // namespace tools::rdbg
