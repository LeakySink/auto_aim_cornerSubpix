// 验证 P0-1（切板帧 R 放大）与 P0-2（几何协方差发散判据）
//
// 注意：前哨站 3 块板 120° 等分，**任意时刻只有一块板面向相机**
// （法向与视线夹角 < 90° 的范围正好 180°，3 块板覆盖 360°）。
// 所以"切板"只能通过让目标旋转自然发生，不能手工指定 id。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

namespace
{
int fail = 0;
void check(bool ok, const char * what)
{
  if (!ok) {
    ++fail;
    std::printf("  [FAIL] %s\n", what);
  }
}

auto_aim::EkfParams default_params()
{
  auto_aim::EkfParams p;
  p.q_xyz_acc_var = 70.0;
  p.q_yaw_acc_var = 100.0;
  p.r_yaw = 5e-3;
  p.r_pitch = 2e-3;
  p.r_dist_base = 2e-1;
  p.q_geo = 1.0e-4;
  p.omega_freeze_thresh = 0.5;
  p.r_yaw_adaptive_factor = 50.0;
  p.max_geo_cov = 1.0;
  return p;
}

auto_aim::Target make_target(double w, const auto_aim::EkfParams & params)
{
  using namespace auto_aim;
  const double radius = 0.2765;
  const double dist = 4.5;
  const double z = 1.5;

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor a(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  a.xyz_in_world = Eigen::Vector3d(dist - radius, 0.0, z);
  a.ypr_in_world = Eigen::Vector3d(0.0, 0.0, 0.0);
  a.ypd_in_world =
    Eigen::Vector3d(0.0, std::atan2(z, dist - radius), std::hypot(dist - radius, z));

  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;

  Target tgt(a, std::chrono::steady_clock::now(), radius, 3, p0, params);
  tgt.set_w(w);
  return tgt;
}

/// 用 target 预测里**最正对相机**的那块板造一个完美观测
auto_aim::Armor make_visible_obs(const auto_aim::Target & tgt, int * chosen_id)
{
  using namespace auto_aim;
  const auto plates = tgt.armor_xyza_list();

  int best = 0;
  double best_dot = -2.0;
  for (int i = 0; i < static_cast<int>(plates.size()); ++i) {
    const double yaw = plates[i][3];
    // n̂·p：板法向（板→中心）与"原点→板"的夹角余弦，>0 表示面向相机
    const double dot = std::cos(yaw) * plates[i][0] + std::sin(yaw) * plates[i][1];
    if (dot > best_dot) {
      best_dot = dot;
      best = i;
    }
  }
  if (chosen_id) *chosen_id = best;

  const auto & p = plates[best];
  const Eigen::Vector3d xyz{p[0], p[1], p[2]};

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor a(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  a.xyz_in_world = xyz;
  a.ypr_in_world = Eigen::Vector3d(p[3], 0.0, 0.0);
  a.ypd_in_world = Eigen::Vector3d(
    std::atan2(xyz.y(), xyz.x()), std::atan2(xyz.z(), std::hypot(xyz.x(), xyz.y())),
    xyz.norm());
  return a;
}
}  // namespace

int main()
{
  std::printf("=== P0-1 / P0-2 验证 ===\n\n");

  // ---------- P0-1：旋转过程中应发生切板 ----------
  {
    auto tgt = make_target(3.0, default_params());   // 3 rad/s，1 秒转 172°
    int switches = 0;
    int frames = 200;
    for (int i = 0; i < frames; ++i) {
      tgt.predict(0.01);
      auto obs = make_visible_obs(tgt, nullptr);
      tgt.update(obs);
      if (tgt.is_switching()) ++switches;
    }
    // 3 块板 120° 等分 -> 每转 120° 切一次；0.95 圈应切 2~3 次
    std::printf("1) w=3 rad/s 跑 %d 帧（%.2f 圈）：切板 %d 次（应 2~3）\n", frames,
                frames * 0.01 * 3.0 / (2 * M_PI), switches);
    check(switches >= 2, "旋转过程中应发生多次切板");
  }

  // ---------- P0-1：切板帧 R 放大 -> 对状态的修正更小 ----------
  //
  // 用"完美观测"测不出东西：新息恒为 0，NIS 也恒为 0。
  // 这里给观测的板朝向加一个固定偏置（~1.1°），制造可控的新息，
  // 再比较切板帧里 a 被拉动的幅度。
  {
    constexpr double kBiasRad = 0.02;
    auto run = [&](double factor) {
      auto params = default_params();
      params.r_yaw_adaptive_factor = factor;
      auto tgt = make_target(3.0, params);
      double sum_da = 0.0;
      int n_switch = 0;
      for (int i = 0; i < 300; ++i) {
        tgt.predict(0.01);
        auto obs = make_visible_obs(tgt, nullptr);
        obs.ypr_in_world[0] += kBiasRad;   // 制造新息
        const double a_before = tgt.ekf_x()[6];
        tgt.update(obs);
        if (tgt.is_switching()) {
          sum_da += std::abs(tgt.ekf_x()[6] - a_before);
          ++n_switch;
        }
      }
      return std::make_pair(sum_da / std::max(1, n_switch), n_switch);
    };
    const auto [da1, n1] = run(1.0);
    const auto [da50, n50] = run(50.0);
    std::printf("2) 切板帧 |Δa|：factor=1 -> %.6f rad (%d 次)，factor=50 -> %.6f rad (%d 次)\n",
                da1, n1, da50, n50);
    check(n1 > 0 && n50 > 0, "两种配置都应观察到切板");
    check(da50 < da1, "R 放大后切板帧对 a 的修正应更小");
  }

  // ---------- P0-2：几何协方差发散判据 ----------
  {
    auto params = default_params();
    params.q_geo = 1.0;   // 调大到 1.0，只 predict 不 update -> P(h) 单调涨
    auto tgt = make_target(3.0, params);
    const double p_before = tgt.ekf().P(10, 10);
    for (int i = 0; i < 200; ++i) tgt.predict(0.01);
    const double p_after = tgt.ekf().P(10, 10);
    std::printf("3) 200 帧后 P(h)：%.4f -> %.4f，diverged = %d（应 1）\n", p_before, p_after,
                tgt.diverged());
    check(p_after > params.max_geo_cov, "P(h) 应涨过阈值");
    check(tgt.diverged(), "几何协方差超阈值应判发散");
  }

  // ---------- P0-2：默认参数下不误判 ----------
  {
    auto tgt = make_target(3.0, default_params());
    for (int i = 0; i < 100; ++i) tgt.predict(0.01);
    const double p = tgt.ekf().P(10, 10);
    std::printf("4) 默认 q_geo 跑 100 帧：P(h) = %.6f（远离 1.0），diverged = %d（应 0）\n", p,
                tgt.diverged());
    check(p < 0.01, "默认参数下 P(h) 应远小于阈值");
    check(!tgt.diverged(), "默认参数下不应误判发散");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
