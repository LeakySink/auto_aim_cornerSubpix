// 验证 3 板（前哨站）的线性递降高度模型
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

int main()
{
  using auto_aim::Armor;
  using auto_aim::ArmorName;
  using auto_aim::Color;
  using auto_aim::Target;

  int fail = 0;
  auto check = [&](bool ok, const char * what) {
    if (!ok) {
      ++fail;
      std::printf("  [FAIL] %s\n", what);
    }
  };

  const double radius = 0.2765;
  const double dist = 4.5;
  // 与 target.cpp 的 kOutpostPlateHeight 保持一致（来源：参考实现 h_init = 0.102）
  const double h_mm = 0.102;

  // 构造前哨站观测：位于 (dist - r, 0, z0)，法向 +x
  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor armor(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);

  const double z_obs = 1.554;  // 假设看到的是最高那块（id0）；具体值不影响断言
  armor.xyz_in_world = Eigen::Vector3d(dist - radius, 0.0, z_obs);
  armor.ypr_in_world = Eigen::Vector3d(0.0, 0.0, 0.0);
  armor.ypd_in_world =
    Eigen::Vector3d(0.0, std::atan2(z_obs, dist - radius), std::hypot(dist - radius, z_obs));

  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 0;

  auto_aim::EkfParams params;
  params.q_xyz_acc_var = 70.0;
  params.q_yaw_acc_var = 100.0;
  params.r_yaw = 5e-3;
  params.r_pitch = 2e-3;
  params.r_dist_base = 2e-1;

  const auto t0 = std::chrono::steady_clock::now();
  Target tgt(armor, t0, radius, 3, p0, params);

  const auto plates = tgt.armor_xyza_list();
  std::printf("=== 3 板高度（观测 z=%.3f m, 期望相邻差 %.0f mm）===\n", z_obs, h_mm * 1000);
  check(plates.size() == 3, "应有 3 块板");
  for (std::size_t i = 0; i < plates.size(); ++i) {
    std::printf("  plate%zu  z = %.4f m   yaw = %+7.1f°\n", i, plates[i][2], plates[i][3] * 57.2958);
  }

  // 断言：z 逐块递减，步长 = h
  const double dh01 = plates[0][2] - plates[1][2];
  const double dh12 = plates[1][2] - plates[2][2];
  std::printf("  实测相邻差：%.1f mm, %.1f mm\n", dh01 * 1000, dh12 * 1000);
  check(std::abs(dh01 - h_mm) < 1e-6, "plate0-plate1 高度差应等于 h");
  check(std::abs(dh12 - h_mm) < 1e-6, "plate1-plate2 高度差应等于 h");
  check(plates[0][2] > plates[1][2] && plates[1][2] > plates[2][2], "高度应逐块递降（id0 最高）");
  check(std::abs(plates[0][2] - z_obs) < 1e-6, "id0 高度应等于观测高度（假设观测到 id0）");

  // 角度仍应 120° 等分
  // 注意 limit_rad 会把角度回绕到 [-pi,pi]，plate2 的 yaw 是 -240°（等价 +120°）。
  // 比较必须用"最短角差"，不能直接相减。
  const double d01 = std::atan2(
    std::sin(plates[1][3] - plates[0][3]), std::cos(plates[1][3] - plates[0][3]));
  const double d12 = std::atan2(
    std::sin(plates[2][3] - plates[1][3]), std::cos(plates[2][3] - plates[1][3]));
  std::printf("  相邻方位差：%.1f°, %.1f°（回绕后应 ±120°）\n", d01 * 57.2958, d12 * 57.2958);
  check(std::abs(std::abs(d01) - 2 * M_PI / 3) < 1e-6, "方位应 120° 等分");
  check(std::abs(std::abs(d12) - 2 * M_PI / 3) < 1e-6, "方位应 120° 等分");

  // 半径仍应相同（3 板不使用 l）
  for (std::size_t i = 0; i < plates.size(); ++i) {
    const double r_i = std::hypot(plates[i][0] - dist, plates[i][1]);
    std::printf("  plate%zu  半径 = %.4f m（应 %.4f）\n", i, r_i, radius);
    check(std::abs(r_i - radius) < 1e-6, "三块板半径应相同");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
