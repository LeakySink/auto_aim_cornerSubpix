#include "kalman_markers.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "armor.hpp"
#include "tools/rdbg/markers/viz_markers.hpp"

namespace tools::viz
{
namespace
{
constexpr double LIGHTBAR_LENGTH = 56e-3;
constexpr double BIG_ARMOR_WIDTH = 230e-3;
constexpr double SMALL_ARMOR_WIDTH = 135e-3;
constexpr double ARMOR_THICKNESS = 0.01;

Eigen::Quaterniond armor_quat(double yaw, auto_aim::ArmorName name)
{
  const double pitch =
    (name == auto_aim::ArmorName::outpost) ? -15.0 * CV_PI / 180.0 : 15.0 * CV_PI / 180.0;
  const double sy = std::sin(yaw);
  const double cy = std::cos(yaw);
  const double sp = std::sin(pitch);
  const double cp = std::cos(pitch);
  // clang-format off
  Eigen::Matrix3d R;
  R << cy * cp, -sy, cy * sp,
       sy * cp,  cy, sy * sp,
           -sp,   0,      cp;
  // clang-format on
  return Eigen::Quaterniond(R);
}
}  // namespace

nlohmann::json kalman_markers(const auto_aim::Target & target)
{
  MarkerArray arr;  // frame_id default world
  const Eigen::VectorXd x = target.ekf_x();
  if (x.size() < 11) return arr.to_json();

  const Eigen::Vector3d center{x[0], x[2], x[4]};
  const Eigen::Vector3d vel{x[1], x[3], x[5]};
  const double w = x[7];

  arr.sphere("kalman.center", "c", center, 0.04, {1.0, 0.45, 0.1, 1.0});

  const double vnorm = vel.norm();
  if (vnorm > 1e-6) {
    const double shaft = std::min(0.8, std::max(0.05, vnorm * 0.15));
    arr.arrow("kalman.vel", "v", center, vel.normalized(), shaft, {0.2, 0.9, 0.3, 1.0});
  }

  if (std::abs(w) > 1e-4) {
    // 角速度绕世界 Z 示意（短箭头沿 +Z 或 -Z）
    Eigen::Vector3d spin_dir{0, 0, w >= 0 ? 1.0 : -1.0};
    const double shaft = std::min(0.4, 0.08 + std::abs(w) * 0.05);
    arr.arrow("kalman.spin", "w", center, spin_dir, shaft, {0.9, 0.3, 0.9, 1.0});
  }

  const auto plates = target.armor_xyza_list();
  const double width =
    (target.armor_type == auto_aim::ArmorType::big) ? BIG_ARMOR_WIDTH : SMALL_ARMOR_WIDTH;
  const Eigen::Vector3d box_scale{ARMOR_THICKNESS, width, LIGHTBAR_LENGTH};

  std::vector<Eigen::Vector3d> spokes;
  spokes.reserve(plates.size() * 2);
  for (std::size_t i = 0; i < plates.size(); ++i) {
    const Eigen::Vector3d p{plates[i][0], plates[i][1], plates[i][2]};
    const double yaw = plates[i][3];
    const auto q = armor_quat(yaw, target.name);
    arr.box("kalman.armor", "a" + std::to_string(i), p, q, box_scale, {0.25, 0.55, 1.0, 0.9});
    spokes.push_back(center);
    spokes.push_back(p);
  }
  if (!spokes.empty()) {
    arr.line_list("kalman.armor", "spokes", spokes, 0.004, {0.6, 0.6, 0.65, 0.75});
  }

  return arr.to_json();
}

}  // namespace tools::viz
