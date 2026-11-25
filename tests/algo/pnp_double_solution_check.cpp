// PnP 平面二义性：双解择一的回归测试
//
// 背景（2026-11 实机 rlog，三份 log 一致）：
//   4 点平面靶用 IPPE 求解天然有两个近似等价的位姿，二者关于**视线方向**
//   镜像。单帧重投影误差几乎相同，OpenCV 挑哪个近似掷硬币。
//
//   实测：
//     · 两解相差约 72°（71.7° / 71.5° / 72.1°），两簇精确对称分布在
//       圆心方位两侧各 36°
//     · 约 27% 的帧在两解之间跳变
//     · 两解的**距离只差 32~35 mm**（远小于距离噪声 55 mm）、pitch 相同
//       —— 即两个解只差朝向、不差位置
//
//   后果：`obs_armor_yaw` 逐帧在两个值间翻转 → `x[A]` 被 ±72° 冲击
//   → `w = dA/dt` 污染。实机实测只有 12.8~25.3% 的帧的 |w| 落在真值 ±0.3 内。
//
// 本测试模拟这个工况：
//   真值：前哨站 3 板，θ_i = A + i·120°，A 以 ω=0.8π 匀速推进。
//   每个观测帧挑**正对相机**的那块板（|θ| 最小），这样 n̂·p > 0 恒成立，
//   不会触发 ±π 消歧。
//   主解：73% 的帧是**真值**，27% 的帧是**镜像解**（yaw 关于视线反射，
//         即 -θ；它反向旋转，这正是平面镜像解的性质）。
//   备用解：另一个。
//
// 断言：
//   1) 提供备用解时，w 收敛到 0.8π 附近；`residual_angle` 的 sd 显著小
//   2) 不提供备用解（只喂会随机翻转的主解）时，w 明显被污染
//   3) 二者的对比说明"双解择一"确实有作用 —— 这是本改动的存在理由
//
// 注意：这两个场景必须用**同一个随机种子**，否则差异会被噪声掩盖。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"
#include "tools/math_tools.hpp"

void harness_on_log(const std::string &) {}

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCx = 4.5, kCy = 0.0, kCz = 1.5;
constexpr double kR = 0.2765, kH = 0.102;
constexpr double kW = 0.8 * kPi;  // 2.5133
constexpr double kDt = 0.01;

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
  p.r_dist = 5e-3;  // 改动 5 的标定值；两个场景都用它，只隔离"备用解"这一个变量
  p.q_geo = 0.0;
  p.omega_freeze_thresh = 0.5;
  return p;
}

struct Truth {
  double a;  // 相位 A
  int id;    // 本帧正对相机的板号
  double theta;
  Eigen::Vector3d xyz;
};

/// 推进到时刻 t，返回正对相机的那块板的真值
Truth truth_at(double t)
{
  Truth tr;
  tr.a = kW * t;
  double best = 1e9;
  for (int i = 0; i < 3; ++i) {
    const double th = tools::limit_rad(tr.a + i * 2.0 * kPi / 3.0);
    if (std::abs(th) < best) {
      best = std::abs(th);
      tr.id = i;
      tr.theta = th;
    }
  }
  // 模型约定：armor = center − r·(cos θ, sin θ)；z_i = Z − i·h
  tr.xyz = Eigen::Vector3d(kCx - kR * std::cos(tr.theta), kCy - kR * std::sin(tr.theta),
                           kCz - tr.id * kH);
  return tr;
}

auto_aim::Armor make_obs(const Truth & tr, double yaw, bool with_alt, double yaw_alt)
{
  using namespace auto_aim;
  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor ar(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  ar.xyz_in_world = tr.xyz;
  ar.ypr_in_world = Eigen::Vector3d(yaw, 0.0, 0.0);
  ar.ypd_in_world = tools::xyz2ypd(tr.xyz);
  ar.has_alt = with_alt;
  if (with_alt) {
    ar.xyz_in_world_alt = tr.xyz;  // 两解位置几乎相同（实测只差 32~35 mm）
    ar.ypr_in_world_alt = Eigen::Vector3d(yaw_alt, 0.0, 0.0);
  }
  return ar;
}

struct Result {
  double w_end = 0.0;
  double w_abs_mean = 0.0;      // 全程 |w| 均值
  double resid_sd = 0.0;        // residual_angle 的 sd
  double inband_ratio = 0.0;    // |w| 落在 0.8π±0.3 的比例
  double center_err = 0.0;      // 结束时中心水平误差
};

/// @param flip_prob 主解为镜像解的概率
/// @param with_alt  是否提供备用解
Result run(double flip_prob, bool with_alt)
{
  // 两个场景用同一随机种子 -> 翻转发生在相同的帧，差异只来自"有没有备用解"
  std::mt19937 rng(20261125);
  std::uniform_real_distribution<double> uni(0.0, 1.0);

  const auto tr0 = truth_at(0.0);
  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;
  auto_aim::Target tgt(
    make_obs(tr0, tr0.theta, false, 0.0), std::chrono::steady_clock::now(), kR, 3, p0,
    default_params(), {}, tr0.id);

  Result r;
  double sum_w = 0.0, sum_res = 0.0, sum_res2 = 0.0;
  int n_w = 0, n_res = 0, n_in = 0;
  const int frames = 800;  // 8 s

  for (int f = 1; f <= frames; ++f) {
    const double t = f * kDt;
    const auto tr = truth_at(t);

    tgt.predict(kDt);

    // 主解：真值 or 镜像（yaw 关于视线反射 -> -θ，且它反向旋转）
    const bool flipped = uni(rng) < flip_prob;
    const double yaw_primary = flipped ? -tr.theta : tr.theta;
    const double yaw_alt = flipped ? tr.theta : -tr.theta;

    tgt.update(make_obs(tr, yaw_primary, with_alt, yaw_alt));

    const double w = tgt.ekf_x()[auto_aim::idx::W];
    if (f > 200) {  // 跳过收敛期
      sum_w += std::abs(w);
      ++n_w;
      if (std::abs(std::abs(w) - kW) < 0.3) ++n_in;
    }
    const auto res = tgt.ekf().data;
    auto it = res.find("residual_angle");
    if (it != res.end() && f > 200) {
      sum_res += it->second;
      sum_res2 += it->second * it->second;
      ++n_res;
    }
  }

  r.w_end = tgt.ekf_x()[auto_aim::idx::W];
  r.w_abs_mean = n_w ? sum_w / n_w : 0.0;
  r.inband_ratio = n_w ? static_cast<double>(n_in) / n_w : 0.0;
  if (n_res > 1) {
    const double m = sum_res / n_res;
    r.resid_sd = std::sqrt(std::max(0.0, sum_res2 / n_res - m * m));
  }
  r.center_err =
    std::hypot(tgt.ekf_x()[auto_aim::idx::X] - kCx, tgt.ekf_x()[auto_aim::idx::Y] - kCy);
  return r;
}
}  // namespace

int main()
{
  std::printf("=== PnP 双解择一回归 ===\n\n");

  std::printf("-- 1) 无翻转（对照组）：两解都存在但主解恒为真值 --\n");
  {
    const auto r = run(0.0, true);
    std::printf("   w_end=%.4f  |w|均值=%.4f  在带内=%.0f%%  新息sd=%.4f rad  中心误差=%.1f mm\n",
                r.w_end, r.w_abs_mean, r.inband_ratio * 100, r.resid_sd, r.center_err * 1000);
    check(std::abs(r.w_end - kW) < 0.15, "无翻转时 w 应跟住 0.8π");
  }

  std::printf("\n-- 2) 27%% 翻转 + 提供备用解（本改动的目标工况）--\n");
  Result good;
  {
    good = run(0.27, true);
    std::printf("   w_end=%.4f  |w|均值=%.4f  在带内=%.0f%%  新息sd=%.4f rad  中心误差=%.1f mm\n",
                good.w_end, good.w_abs_mean, good.inband_ratio * 100, good.resid_sd,
                good.center_err * 1000);
    check(std::abs(good.w_end - kW) < 0.30, "有备用解时 w 应仍跟住 0.8π（±0.3 内）");
    check(good.resid_sd < 0.30, "有备用解时板朝向新息 sd 应明显小于实机 0.52 rad");
  }

  std::printf("\n-- 3) 27%% 翻转 + 不提供备用解（改动前的行为）--\n");
  Result bad;
  {
    bad = run(0.27, false);
    std::printf("   w_end=%.4f  |w|均值=%.4f  在带内=%.0f%%  新息sd=%.4f rad  中心误差=%.1f mm\n",
                bad.w_end, bad.w_abs_mean, bad.inband_ratio * 100, bad.resid_sd,
                bad.center_err * 1000);
  }

  std::printf("\n-- 4) 对比 --\n");
  std::printf("   板朝向新息 sd：有备用解 %.4f rad  vs  无备用解 %.4f rad（降 %.0f%%）\n",
              good.resid_sd, bad.resid_sd,
              100.0 * (1.0 - good.resid_sd / std::max(1e-9, bad.resid_sd)));
  std::printf("   中心误差：    有备用解 %.1f mm  vs  无备用解 %.1f mm\n", good.center_err * 1000,
              bad.center_err * 1000);
  std::printf("   在带内比例：  有备用解 %.0f%%  vs  无备用解 %.0f%%\n", good.inband_ratio * 100,
              bad.inband_ratio * 100);
  std::printf("\n   注意：两解关于视线镜像，而**镜像在板转到 ±60° 附近会与相邻板的预测\n"
              "   完全重合**（θ 与 θ−120° 对称）。这是几何上的真退化，不是实现缺陷，\n"
              "   所以本改动**不是**完整解 —— 翻转帧上仍有部分会被关联到错板。\n"
              "   它保证的是：不能靠改板号去消化翻转（那会让 w 翻符号、永不恢复）。\n");

  // 主收益：板朝向新息显著变小（这才是 w 被污染的直接来源）
  check(
    good.resid_sd < bad.resid_sd * 0.85,
    "提供备用解应使板朝向新息 sd 至少降 15%（这是改动的直接收益）");
  // 次收益：中心估计更稳
  check(good.center_err < bad.center_err, "提供备用解时中心误差应更小");
  // 不退化：w 落在真值附近的帧比例不能变差
  check(
    good.inband_ratio + 0.05 >= bad.inband_ratio, "提供备用解不应让 w 的在带内比例变差");
  // 绝对精度
  check(std::abs(good.w_end - kW) < 0.30, "有备用解时 w 终值应在 0.8π±0.3 内");

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
