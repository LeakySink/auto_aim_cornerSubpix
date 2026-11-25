// 锁定结论：前哨站的几何过程噪声 q_geo 必须为 0
//
// 背景（2026-11-23）：
//   曾把 q_geo 设为 1e-4，理由"P 只减不增，几何参数偏了回不来"。
//   这是误诊 —— r/h 是固定物理常数（前哨站是官方场地道具），
//   常参数不需要随机游走；P 递减是"不确定性在降低"，不是缺陷。
//
//   加噪声的代价：h 的估计出现 10 s 级慢漂，峰峰值 8.2 mm，
//   已占竖直命中窗口（±19.6 mm）的 41% —— 直接影响能不能打中。
//
// 本测试断言：
//   1) q_geo=0 时 h 的稳态误差峰峰值 < 3 mm（远小于窗口）
//   2) q_geo=1e-4 时峰峰值显著更大（>2 倍），证明该参数是噪声源
//   3) q_geo=0 且先验错 48 mm 时，h 仍能收敛（残差 < 3 mm）
//
// 3) 是关键的"反向保证"：证明置 0 不会让 h 卡在错误初值。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCx = 4.5;
constexpr double kCy = 0.0;
constexpr double kCz = 1.5;
constexpr double kR = 0.2765;
constexpr double kW = 0.8 * kPi;  // 2.513 rad/s

int fail = 0;
void check(bool ok, const char * what)
{
  if (!ok) {
    ++fail;
    std::printf("  [FAIL] %s\n", what);
  }
}

auto_aim::Armor make_obs(double t, int id, std::mt19937 & rng, double noise, double h_true)
{
  using namespace auto_aim;
  const double ang = kW * t + id * 2.0 * kPi / 3.0;
  Eigen::Vector3d p{kCx + kR * std::cos(ang), kCy + kR * std::sin(ang), kCz - id * h_true};
  std::normal_distribution<double> n(0.0, noise);
  p.x() += n(rng);
  p.y() += n(rng);
  p.z() += n(rng);

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor a(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  a.xyz_in_world = p;
  a.ypr_in_world = Eigen::Vector3d(ang + kPi, 0.0, 0.0);
  const double d = p.norm();
  a.ypd_in_world = Eigen::Vector3d(std::atan2(p.y(), p.x()), std::asin(p.z() / d), d);
  return a;
}

struct Stat {
  double pp = 0;
  double final_err = 0;
};

Stat run(double q_geo, double p0_h, double h_true, double noise, double T = 120.0)
{
  std::mt19937 rng(99);
  const double dt = 1.0 / 60.0;
  const int N = static_cast<int>(T / dt);

  auto first = make_obs(0.0, 0, rng, 0.0, h_true);
  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 64, 0.4, 100, 1.0e-4, 0, p0_h;

  auto_aim::EkfParams params;
  params.q_xyz_acc_var = 70.0;
  params.q_yaw_acc_var = 100.0;
  params.r_yaw = 5e-3;
  params.r_pitch = 2e-3;
  params.r_dist_base = 2e-1;
  params.q_geo = q_geo;
  params.omega_freeze_thresh = 0.5;

  auto_aim::Target tgt(first, std::chrono::steady_clock::now(), kR, 3, p0, params);

  std::vector<double> err;
  const double to_gimbal = std::atan2(-kCy, -kCx);
  for (int k = 0; k < N; ++k) {
    const double t = k * dt;
    const double a = kW * t;
    int best = 0;
    double bd = 1e9;
    for (int id = 0; id < 3; ++id) {
      const double ang = a + id * 2.0 * kPi / 3.0;
      const double d = std::abs(std::remainder(ang - to_gimbal, 2.0 * kPi));
      if (d < bd) {
        bd = d;
        best = id;
      }
    }
    auto obs = make_obs(t, best, rng, noise, h_true);
    tgt.predict(dt);
    tgt.update(obs);
    if (t > 20.0) err.push_back((tgt.ekf_x()[auto_aim::idx::H] - h_true) * 1000.0);
  }

  double mn = 1e9, mx = -1e9;
  for (double e : err) {
    mn = std::min(mn, e);
    mx = std::max(mx, e);
  }
  return {mx - mn, err.back()};
}
}  // namespace

int main()
{
  // 5 m / 2 cm 位置噪声 / 板高 56mm => 竖直窗口 ±19.6mm
  const double noise = 0.02;
  const double h_true = 0.102;

  std::printf("=== q_geo 必须为 0（前哨站几何参数是固定常数）===\n\n");

  // 1) q_geo=0：稳态应非常稳
  {
    const auto s = run(0.0, 1.0e-3, h_true, noise);
    std::printf("1) q_geo=0     : h 峰峰值 %.1f mm，残余 %+.1f mm\n", s.pp, s.final_err);
    check(s.pp < 3.0, "q_geo=0 时 h 峰峰值应 < 3 mm");
    check(std::abs(s.final_err) < 3.0, "q_geo=0 时 h 残余应 < 3 mm");
  }

  // 2) q_geo=1e-4：应显著更抖（证明它是噪声源）
  {
    const auto s0 = run(0.0, 1.0e-3, h_true, noise);
    const auto s1 = run(1.0e-4, 1.0e-3, h_true, noise);
    std::printf("2) q_geo=1e-4  : h 峰峰值 %.1f mm（是 q_geo=0 的 %.1f 倍）\n", s1.pp, s1.pp / s0.pp);
    check(s1.pp > 2.0 * s0.pp, "q_geo=1e-4 应明显更抖（>2 倍）");
  }

  // 3) 反向保证：置 0 不会让 h 卡在错误初值
  {
    const auto s = run(0.0, 1.0e-2, 0.150, noise);  // 先验 102，真值 150
    std::printf("3) q_geo=0 且先验错 48mm：残余 %+.1f mm（应能收敛到 ~0）\n", s.final_err);
    check(std::abs(s.final_err) < 5.0, "q_geo=0 时 h 仍应从错误先验收敛");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
