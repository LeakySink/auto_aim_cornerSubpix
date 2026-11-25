// 静止目标直瞄板（CenterAimer 静止分支）常驻回归
//
// 背景：CenterAimer 原策略是"瞄旋转中心、等板转过来"，前提是**板会转**。
// 目标静止时板永远不动，而横向命中窗口只有 ±atan(板宽·0.7/2 / 距离)
// （4.5 m 处 ≈ ±0.60°），3 板 120° 分布 -> 可打姿态占比约 1%。
//
// 影响的是**所有静止目标**：前哨站（3 分钟后停转 / 重建后）、基地（永远不转）、
// 车（站桩/被卡/新手不转）。基地是终局目标（10000 血），打不中等于无法终结。
//
// 修复：aim_center 里按 |w| < static_w_thresh 走"直接瞄可见板"分支。
//
// 断言：
//   1) 静止时（w=0）扫描板相位 0~120°，绝大多数相位都应能开火
//   2) 转靶时（w=2.5 rad/s）行为不变（仍走瞄中心等板）
//
// 注意：测 fire 必须推进时间戳并关闭热量闸。否则循环调用 solve 时
// 时间戳不变 -> 热量不冷却 -> Q1 冲到 300 后永久封锁，会误判成"打不中"。
// 这一点在开发时踩过（第一版诊断因此得出错误结论）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tasks/auto_aim/center_aimer.hpp"
#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCx = 4.5;
constexpr double kCy = 0.0;
constexpr double kCz = 1.5;
constexpr double kR = 0.2765;
constexpr double kH = 0.102;

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

auto_aim::Target make_target(double a, double w)
{
  using namespace auto_aim;
  const Eigen::Vector3d pos{kCx - kR * std::cos(a), kCy - kR * std::sin(a), kCz};

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor ar(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  ar.xyz_in_world = pos;
  ar.ypr_in_world = Eigen::Vector3d(a, 0.0, 0.0);
  ar.ypd_in_world =
    Eigen::Vector3d(std::atan2(pos.y(), pos.x()), std::atan2(pos.z(), std::hypot(pos.x(), pos.y())),
                    pos.norm());

  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;
  Target tgt(ar, std::chrono::steady_clock::now(), kR, 3, p0, default_params(), {}, 0);
  tgt.set_w(w);
  return tgt;
}

/// 用"完美跟随云台"迭代若干帧，返回是否最终能 fire
bool can_fire(const auto_aim::Target & tgt, const auto_aim::CenterAimer & aimer, int iter = 50)
{
  auto tp = std::chrono::steady_clock::now();
  auto res = aimer.solve(tgt, tp, 0.0, 0.0, 16.5);
  for (int i = 0; i < iter; ++i) {
    tp += std::chrono::microseconds(10000);   // 推进时间，让热量能冷却
    res = aimer.solve(tgt, tp, res.yaw, -res.pitch, 16.5);
  }
  return res.fire;
}
}  // namespace

int main(int argc, char ** argv)
{
  std::printf("=== 静止目标直瞄板回归 ===\n\n");

  // 用 sentry.yaml（热量闸开着也没关系：can_fire 会推进时间戳让热量冷却，
  // 且只取最终一帧的 fire。这样测的是真实配置下的行为。
  // 配置从命令行传入（相对当前工作目录）。缺省用仓库里的 sentry.yaml。
  const std::string cfg_path = (argc > 1) ? std::string(argv[1]) : std::string("configs/sentry.yaml");
  auto_aim::CenterAimer aimer(cfg_path);

  // ---------- 1) 静止：扫描板相位，绝大多数应能开火 ----------
  int n_ok = 0, n_all = 0;
  for (int deg = 0; deg <= 120; deg += 10) {
    const double a = deg * kPi / 180.0;
    const auto tgt = make_target(a, 0.0);
    const bool ok = can_fire(tgt, aimer);
    ++n_all;
    if (ok) ++n_ok;
    std::printf("  静止 相位%4d°  fire=%d\n", deg, ok ? 1 : 0);
  }
  std::printf("  -> %d / %d 个相位能开火\n", n_ok, n_all);
  check(n_ok >= n_all - 2, "静止目标绝大多数板相位都应能开火（原实现只有约 1%）");

  // ---------- 2) 分支选择：静止瞄板 / 转靶瞄中心 ----------
  //
  // 直接看 aim_xyz 落在哪，比"能不能开火"更能证明分支生效：
  //   静止 -> aim_xyz 应等于某块板的位置
  //   转靶 -> aim_xyz 应等于旋转中心（离任一块板都有 r≈0.2765 的距离）
  {
    auto dist_to_nearest_plate = [](const auto_aim::Target & tgt, const Eigen::Vector3d & aim) {
      const auto plates = tgt.armor_xyza_list();
      double d = 1e18;
      for (const auto & pl : plates) d = std::min(d, (aim - pl.head<3>()).norm());
      return d;
    };

    // 取一个"板不在中心线上"的相位（30°），这样才能区分两种瞄准点
    const double a30 = 30.0 * kPi / 180.0;

    auto tgt_static = make_target(a30, 0.0);
    auto r_static = aimer.solve(tgt_static, std::chrono::steady_clock::now(), 0.0, 0.0, 16.5);
    const double d_static = dist_to_nearest_plate(tgt_static, r_static.aim_xyz);

    auto tgt_spin = make_target(a30, 2.5);
    auto r_spin = aimer.solve(tgt_spin, std::chrono::steady_clock::now(), 0.0, 0.0, 16.5);
    const double d_spin = dist_to_nearest_plate(tgt_spin, r_spin.aim_xyz);

    std::printf("  aim_xyz 到最近板的距离：静止 %.4f m（应≈0，瞄板）  转靶 %.4f m（应≈r，瞄中心）\n",
                d_static, d_spin);

    check(r_static.valid && d_static < 0.02, "静止时应直接瞄板（aim_xyz 落在板位置）");
    check(r_spin.valid && d_spin > 0.15, "转靶时应瞄旋转中心（aim_xyz 离板约一个半径）");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
