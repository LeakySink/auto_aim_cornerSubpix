// 前哨站停转回归：ω 必须能在停转后归零
//
// 背景：`target.cpp` 原有一个从同济祖先继承的硬钳位
//
//     if (convergened() && outpost && |w| > 2)  w = ±2.51;
//
// 这是**无条件钳死**。目标停转后（比赛 3 分钟 / 首次被击毁 / 对方护甲展开），
// EKF 想把 w 收敛到 0，但每帧都被钳回 2.51，形成"永远以为它在转"的死锁。
//
// 实测（修复前，10s 停转）：
//     停转后 2s w=2.4260，跑到 120s 仍是 2.4260
//     中心误差稳定在 64~69 mm，55 秒不自愈
//
// 后果：CenterAimer 用 EKF 外推算瞄准点，ω 错了上游全错——即使「静止直瞄板」
// 的分支修好了，外推到命中时刻的板位置仍然错。
//
// 修复：钳位从"|w|>2 就设为 ±2.51"改为"|w| > 1.5×0.8π(=3.77) 才拉回 0.8π"。
//   - 正常转速 2.513 不被干预
//   - 停转时让 w 自由收敛到 0
//   - 仍能拦住脏数据把 w 推飞
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCx = 4.5, kCy = 0.0, kCz = 1.5;
constexpr double kR = 0.2765, kH = 0.102, kDt = 0.01;
constexpr double kSpin = 0.8 * kPi;   // 2.5133

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
  p.q_geo = 0.0;
  p.omega_freeze_thresh = 0.5;
  return p;
}

double truth_angle(double t, double t_stop, int i)
{
  return kSpin * std::min(t, t_stop) + i * 2.0 * kPi / 3.0;
}

auto_aim::Armor make_obs(double t, double t_stop, int id)
{
  using namespace auto_aim;
  const double a = truth_angle(t, t_stop, id);
  const Eigen::Vector3d pos{kCx - kR * std::cos(a), kCy - kR * std::sin(a), kCz - id * kH};
  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor ar(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  ar.xyz_in_world = pos;
  ar.ypr_in_world = Eigen::Vector3d(a, 0.0, 0.0);
  ar.ypd_in_world =
    Eigen::Vector3d(std::atan2(pos.y(), pos.x()), std::atan2(pos.z(), std::hypot(pos.x(), pos.y())),
                    pos.norm());
  return ar;
}

struct Stats {
  double w_before_stop = 0.0;
  double w_at_stop_plus_2s = 0.0;
  double w_end = 0.0;
  double center_err_end = 0.0;
};

/// t_stop = 无穷大表示全程旋转
Stats run(double t_stop, double t_end)
{
  auto first = make_obs(0.0, t_stop, 0);
  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;
  auto_aim::Target tgt(first, std::chrono::steady_clock::now(), kR, 3, p0, default_params(), {}, 0);

  Stats st;
  double t = 0.0;
  for (int f = 0; f < static_cast<int>(t_end / kDt); ++f) {
    t += kDt;
    tgt.predict(kDt);

    int best = 0;
    double best_dot = -2.0;
    for (int i = 0; i < 3; ++i) {
      const double a = truth_angle(t, t_stop, i);
      const double px = kCx - kR * std::cos(a), py = kCy - kR * std::sin(a);
      const double dot = std::cos(a) * px + std::sin(a) * py;
      if (dot > best_dot) {
        best_dot = dot;
        best = i;
      }
    }
    tgt.update(make_obs(t, t_stop, best));

    if (std::abs(t - (t_stop - 1.0)) < kDt * 0.5) st.w_before_stop = tgt.ekf_x()[7];
    if (std::abs(t - (t_stop + 2.0)) < kDt * 0.5) st.w_at_stop_plus_2s = tgt.ekf_x()[7];
  }
  st.w_end = tgt.ekf_x()[7];
  st.center_err_end = std::hypot(tgt.ekf_x()[0] - kCx, tgt.ekf_x()[2] - kCy) * 1000.0;
  return st;
}
}  // namespace

int main()
{
  std::printf("=== 前哨站停转回归 ===\n\n");

  // ---------- 1) 全程旋转：ω 应稳定在 0.8π 附近 ----------
  {
    const auto st = run(1e9, 20.0);
    std::printf("1) 全程旋转：w_end=%.4f（真值 %.4f）\n", st.w_end, kSpin);
    check(std::abs(st.w_end - kSpin) < 0.15, "全程旋转时 w 应跟住 0.8π");
  }

  // ---------- 2) 10s 停转：ω 必须归零（这是原 bug 的核心）----------
  {
    const auto st = run(10.0, 20.0);
    std::printf("2) 10s 停转：停转前 w=%.4f  停转后2s w=%.4f  结束 w=%.4f  中心误差 %.1f mm\n",
                st.w_before_stop, st.w_at_stop_plus_2s, st.w_end, st.center_err_end);
    check(std::abs(st.w_before_stop - kSpin) < 0.15, "停转前应跟住 0.8π");
    check(std::abs(st.w_at_stop_plus_2s) < 0.2, "停转后 2 秒内 w 应归零（原实现会永久停在 2.43）");
    check(std::abs(st.w_end) < 0.05, "最终 w 应为 0");
    check(st.center_err_end < 10.0, "中心位置误差应 < 10 mm");
  }

  // ---------- 3) 长跑不自愈的问题：40s 停转后跑到 100s ----------
  {
    const auto st = run(40.0, 100.0);
    std::printf("3) 40s 停转、跑到 100s：结束 w=%.4f  中心误差 %.1f mm\n", st.w_end,
                st.center_err_end);
    check(std::abs(st.w_end) < 0.05, "长时间停转后 w 仍应为 0（不能缓慢漂回去）");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
