// 验证几何状态（r/l/h）的过程噪声已生效，且随 |w| 自适应
//
// 背景：2026-11 之前 Q 矩阵里 r/l/h 三行的对角元**恒为 0**，等于断言
// "几何参数永不改变"。后果是 P 只减不增，估值一旦偏了就再也纠正不回来。
// 前哨站只有一块板同时可见，可观测性本来就弱，这个问题在它身上最突出。
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

/// 建一个 3 板目标（前哨站），w 由调用方指定
auto_aim::Target make_target(double w, double q_geo, double thresh)
{
  using namespace auto_aim;
  const double radius = 0.2765;
  const double dist = 4.5;
  const double z = 1.5;

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor armor(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  armor.xyz_in_world = Eigen::Vector3d(dist - radius, 0.0, z);
  armor.ypr_in_world = Eigen::Vector3d(0.0, 0.0, 0.0);
  armor.ypd_in_world =
    Eigen::Vector3d(0.0, std::atan2(z, dist - radius), std::hypot(dist - radius, z));

  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;

  EkfParams params;
  params.q_xyz_acc_var = 70.0;
  params.q_yaw_acc_var = 100.0;
  params.r_yaw = 5e-3;
  params.r_pitch = 2e-3;
  params.r_dist_base = 2e-1;
  params.q_geo = q_geo;
  params.omega_freeze_thresh = thresh;

  Target tgt(armor, std::chrono::steady_clock::now(), radius, 3, p0, params);
  tgt.set_w(w);
  return tgt;
}

double run_predict(auto_aim::Target & tgt, int n = 100, double dt = 0.01)
{
  for (int i = 0; i < n; ++i) tgt.predict(dt);
  return tgt.ekf().P(auto_aim::idx::H, auto_aim::idx::H);
}
}  // namespace

int main()
{
  std::printf("=== 几何状态过程噪声（q_geo）行为验证 ===\n\n");

  // 1) q_geo = 0：旧行为，P(h) 不应增长
  {
    auto tgt = make_target(3.0, 0.0, 0.5);
    const double p0 = tgt.ekf().P(auto_aim::idx::H, auto_aim::idx::H);
    const double p1 = run_predict(tgt);
    std::printf("1) q_geo=0  |w|=3.0 : P(h) %.3e -> %.3e\n", p0, p1);
    check(std::abs(p1 - p0) < 1e-12, "q_geo=0 时 P(h) 不应变化");
  }

  // 2) q_geo > 0 且 |w| > 阈值：P(h) 应增长
  {
    auto tgt = make_target(3.0, 1.0e-4, 0.5);
    const double p0 = tgt.ekf().P(auto_aim::idx::H, auto_aim::idx::H);
    const double p1 = run_predict(tgt);
    std::printf("2) q_geo=1e-4 |w|=3.0 : P(h) %.3e -> %.3e\n", p0, p1);
    check(p1 > p0 * 1.001, "|w|>阈值时 P(h) 应增长");
  }

  // 3) |w| 低于阈值：增长按 |w|/阈值 线性缩小
  {
    auto hi = make_target(3.0, 1.0e-4, 0.5);
    auto lo = make_target(0.1, 1.0e-4, 0.5);
    const double hi0 = hi.ekf().P(auto_aim::idx::H, auto_aim::idx::H), lo0 = lo.ekf().P(auto_aim::idx::H, auto_aim::idx::H);
    const double hi1 = run_predict(hi), lo1 = run_predict(lo);
    const double hi_gain = hi1 - hi0, lo_gain = lo1 - lo0;
    const double ratio = (hi_gain > 0) ? lo_gain / hi_gain : -1.0;
    std::printf(
      "3) 增益: |w|=3.0 -> %.3e，|w|=0.1 -> %.3e，比值 %.3f（应≈0.2）\n", hi_gain, lo_gain,
      ratio);
    check(lo_gain > 0, "低转速仍应有少量噪声（不是全冻结）");
    check(std::abs(ratio - 0.2) < 0.02, "低转速增益应为高转速的 1/5");
  }

  // 4) |w| = 0：完全冻结（不可观测，不该随机游走）
  {
    auto tgt = make_target(0.0, 1.0e-4, 0.5);
    const double p0 = tgt.ekf().P(auto_aim::idx::H, auto_aim::idx::H);
    const double p1 = run_predict(tgt);
    std::printf("4) |w|=0 : P(h) %.3e -> %.3e\n", p0, p1);
    check(std::abs(p1 - p0) < 1e-12, "|w|=0 时几何参数应完全冻结");
  }

  // 5) r 也应有噪声（不只是 h）
  {
    auto tgt = make_target(3.0, 1.0e-4, 0.5);
    const double r0 = tgt.ekf().P(auto_aim::idx::R, auto_aim::idx::R);
    run_predict(tgt);
    const double r1 = tgt.ekf().P(auto_aim::idx::R, auto_aim::idx::R);
    std::printf("5) r 的 P : %.3e -> %.3e\n", r0, r1);
    check(r1 > r0 * 1.001, "r 的 P 也应增长");
  }

  // 6) l（索引 9）保持 0：3 板不用它，4 板的 l 也不该被噪声推动
  {
    auto tgt = make_target(3.0, 1.0e-4, 0.5);
    const double l0 = tgt.ekf().P(auto_aim::idx::L, auto_aim::idx::L);
    run_predict(tgt);
    const double l1 = tgt.ekf().P(auto_aim::idx::L, auto_aim::idx::L);
    std::printf("6) l 的 P : %.3e -> %.3e（保持不动）\n", l0, l1);
    check(std::abs(l1 - l0) < 1e-12, "l 不应获得过程噪声");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
