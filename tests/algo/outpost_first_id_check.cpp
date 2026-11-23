// 前哨站首板歧义常驻回归（对应仿真 S5/S5b）
//
// 为什么必须有这条回归：
//   首帧实际是 id1/id2 时，单假设模型只能靠 `h < 0` 去吸收 k·h 的错位，
//   结果 h 收敛到负值、三块板预测高度全错、俯仰必然打空；
//   而**位置误差只有约 10 mm，日志上看不出任何异常** —— 典型静默失效。
//   这类错误不会以"崩溃/报错"的形式出现，只能靠断言兜住。
//
// 断言：
//   1) k=0 的新构造函数与旧公式完全等价（改动不改变原行为）
//   2) 首帧为 id0/id1/id2 时，三假设的累积误差都能选中正确的 k
//   3) 三种首帧下 h 都收敛到 +0.102 m（**不落负值**），z 误差 < 5 mm
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
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
constexpr double kH = 0.102;          // 真值：相邻板高差
constexpr double kW = 0.8 * kPi;      // 2.513 rad/s
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
  p.q_geo = 0.0;              // 与 configs/sentry.yaml 一致（固定几何常数）
  p.omega_freeze_thresh = 0.5;
  return p;
}

/// 真值：板 i 在世界系的位置与法向
/// @param phase 让 t=0 时正对的板号可控：angle = 0 表示该板正对相机。
///              令 id=first_id 正对，取 phase = -first_id·120°。
void truth_plate(double t, int i, double phase, Eigen::Vector3d * pos, double * yaw)
{
  const double ang = kW * t + i * 2.0 * kPi / 3.0 + phase;
  // 与 h_armor_xyz 同一套公式：板 = 中心 - r·(cos,sin)，z = cz - i·h
  *pos = Eigen::Vector3d(kCx - kR * std::cos(ang), kCy - kR * std::sin(ang), kCz - i * kH);
  *yaw = ang;   // "板→中心"方向
}

/// 造一个"最正对相机"的真值观测（前哨站同时只有一块板可见）
auto_aim::Armor make_truth_obs(double t, double phase, std::mt19937 & rng, double noise)
{
  using namespace auto_aim;

  int best = 0;
  double best_dot = -2.0;
  Eigen::Vector3d best_pos;
  double best_yaw = 0.0;
  for (int i = 0; i < 3; ++i) {
    Eigen::Vector3d p;
    double yaw;
    truth_plate(t, i, phase, &p, &yaw);
    // n̂·p：法向（板→中心）与"原点→板"的点积，>0 表示面向相机
    const double dot = std::cos(yaw) * p.x() + std::sin(yaw) * p.y();
    if (dot > best_dot) {
      best_dot = dot;
      best = i;
      best_pos = p;
      best_yaw = yaw;
    }
  }
  (void)best;

  std::normal_distribution<double> n(0.0, noise);
  Eigen::Vector3d noisy = best_pos;
  noisy.x() += n(rng);
  noisy.y() += n(rng);
  noisy.z() += n(rng);

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor a(Color::red, ArmorName::outpost, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  a.xyz_in_world = noisy;
  a.ypr_in_world = Eigen::Vector3d(best_yaw, 0.0, 0.0);
  a.ypd_in_world = Eigen::Vector3d(
    std::atan2(noisy.y(), noisy.x()), std::atan2(noisy.z(), std::hypot(noisy.x(), noisy.y())),
    noisy.norm());
  return a;
}

auto_aim::Target make_cand(const auto_aim::Armor & first, int k)
{
  using namespace auto_aim;
  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 81, 0.4, 100, 1.0e-4, 0, 1.0e-3;
  return Target(
    first, std::chrono::steady_clock::now(), kR, 3, p0, default_params(), {}, k);
}

struct TriResult {
  int best_k = -1;
  double err[3] = {0.0, 0.0, 0.0};
  double h_final = 0.0;
  double cz_final = 0.0;

  // ---- 锁定过程复刻（与 Tracker::update_target_outpost 的判据一致）----
  bool locked = false;
  int lock_frame = -1;          // 第几帧锁定（-1 表示没锁）
  int lock_best_k = -1;         // 锁定时选中的 k
  bool locked_at_90 = false;    // 90 帧时是否已经锁定（应为 false）
  int seen_plates = 0;          // 真值上累计出现过的不同板号数
};

/// 复刻 Tracker::update_target_outpost 的判据（累积 pos*10 + ang，每帧选优）
TriResult run_tri(int first_id, int frames, double noise, unsigned seed)
{
  using namespace auto_aim;
  std::mt19937 rng(seed);

  // 让 t=0 时正对的板恰好是 id=first_id
  const double phase = -first_id * 2.0 * kPi / 3.0;

  const auto obs0 = make_truth_obs(0.0, phase, rng, noise);
  Target cand[3] = {make_cand(obs0, 0), make_cand(obs0, 1), make_cand(obs0, 2)};

  TriResult res;
  double t = 0.0;
  for (int f = 0; f < frames; ++f) {
    t += kDt;
    for (int k = 0; k < 3; ++k) cand[k].predict(kDt);

    const auto obs = make_truth_obs(t, phase, rng, noise);

    for (int k = 0; k < 3; ++k) {
      const auto pred = cand[k].armor_xyza_list();
      double min_err = 1e18;
      double d_min = 1e18;
      for (const auto & p : pred) {
        const double pos_err = (obs.xyz_in_world - p.head<3>()).norm();
        const double ang_err = std::abs(std::atan2(
          std::sin(obs.ypr_in_world[0] - p[3]), std::cos(obs.ypr_in_world[0] - p[3])));
        min_err = std::min(min_err, pos_err * 10.0 + ang_err);
        d_min = std::min(d_min, pos_err);
      }
      // 门控 1 + 0.2·d
      const double d_ref = pred.empty() ? 0.0 : pred.front().head<2>().norm();
      if (d_min < 1.0 + 0.2 * d_ref) {
        res.err[k] += min_err;
        cand[k].update(obs);
      }
    }
  }

  int best = 0;
  for (int k = 1; k < 3; ++k) {
    if (res.err[k] < res.err[best]) best = k;
  }
  res.best_k = best;
  res.h_final = cand[best].ekf_x()[10];
  res.cz_final = cand[best].ekf_x()[4];
  return res;
}

/// 复刻 Tracker 的锁定判据，返回"第几帧锁定、锁定时选了谁"。
///
/// 判据：`seen_all && (err[best] < kDiscriminate * err[second])`
/// 与 tracker.cpp 完全一致。90 帧时若区分度不足（比值≈1.00）就不该锁定 ——
/// 这正是原实现 `frames >= 90` 兜底会锁错的场景。
constexpr double kDiscriminate = 0.5;

TriResult run_with_locking(int first_id, int max_frames, double noise, unsigned seed)
{
  using namespace auto_aim;
  std::mt19937 rng(seed);
  const double phase = -first_id * 2.0 * kPi / 3.0;

  const auto obs0 = make_truth_obs(0.0, phase, rng, noise);
  Target cand[3] = {make_cand(obs0, 0), make_cand(obs0, 1), make_cand(obs0, 2)};

  // 每个假设"见过"哪些模型 id（与 Tracker 的 outpost_seen_ 同义）
  std::set<int> seen[3];
  for (int k = 0; k < 3; ++k) seen[k].insert(k);

  TriResult res;
  double err[3] = {0.0, 0.0, 0.0};
  std::set<int> seen_truth;
  double t = 0.0;

  for (int f = 0; f < max_frames; ++f) {
    t += kDt;
    for (int k = 0; k < 3; ++k) cand[k].predict(kDt);

    const auto obs = make_truth_obs(t, phase, rng, noise);

    // 真值上本帧有哪些板可见
    for (int i = 0; i < 3; ++i) {
      Eigen::Vector3d p;
      double yaw;
      truth_plate(t, i, phase, &p, &yaw);
      if (std::cos(yaw) * p.x() + std::sin(yaw) * p.y() > 0.0) seen_truth.insert(i);
    }

    for (int k = 0; k < 3; ++k) {
      const auto pred = cand[k].armor_xyza_list();
      double min_err = 1e18, d_min = 1e18;
      int best_pred_id = 0;
      for (int i = 0; i < static_cast<int>(pred.size()); ++i) {
        const double pos_err = (obs.xyz_in_world - pred[i].head<3>()).norm();
        const double ang_err = std::abs(std::atan2(
          std::sin(obs.ypr_in_world[0] - pred[i][3]), std::cos(obs.ypr_in_world[0] - pred[i][3])));
        const double e = pos_err * 10.0 + ang_err;
        if (e < min_err) {
          min_err = e;
          best_pred_id = i;
        }
        d_min = std::min(d_min, pos_err);
      }
      const double d_ref = pred.empty() ? 0.0 : pred.front().head<2>().norm();
      if (d_min < 1.0 + 0.2 * d_ref) {
        err[k] += min_err;
        seen[k].insert(best_pred_id);
        cand[k].update(obs);
      }
    }

    if (res.lock_frame < 0) {
      int best = 0;
      for (int k = 1; k < 3; ++k)
        if (err[k] < err[best]) best = k;

      double second = 1e18;
      for (int k = 0; k < 3; ++k)
        if (k != best) second = std::min(second, err[k]);

      const bool seen_all = seen[best].size() >= 3;
      const bool discriminative = err[best] < kDiscriminate * second;
      if (seen_all && discriminative) {
        res.lock_frame = f;
        res.lock_best_k = best;
      }
      if (f == 89) res.locked_at_90 = (res.lock_frame >= 0);
    }
  }

  int best = 0;
  for (int k = 1; k < 3; ++k)
    if (err[k] < err[best]) best = k;
  res.best_k = best;
  for (int k = 0; k < 3; ++k) res.err[k] = err[k];
  res.h_final = cand[best].ekf_x()[10];
  res.cz_final = cand[best].ekf_x()[4];
  res.seen_plates = static_cast<int>(seen_truth.size());
  res.locked = (res.lock_frame >= 0);
  return res;
}
}  // namespace

int main()
{
  std::printf("=== 前哨站首板歧义常驻回归（仿真 S5）===\n\n");

  // ---------- 1) k=0 与旧公式等价 ----------
  {
    std::mt19937 rng(1);
    const auto obs = make_truth_obs(0.0, 0.0, rng, 0.0);
    auto tgt = make_cand(obs, 0);
    const auto x = tgt.ekf_x();
    std::printf("1) k=0 等价性：x[4]=%.6f（应 %.6f）  x[6]=%.6f（应 %.6f）\n", x[4],
                obs.xyz_in_world.z(), x[6], obs.ypr_in_world[0]);
    check(std::abs(x[4] - obs.xyz_in_world.z()) < 1e-9, "k=0 时 center_z 应等于观测 z");
    check(std::abs(std::atan2(std::sin(x[6] - obs.ypr_in_world[0]),
                              std::cos(x[6] - obs.ypr_in_world[0]))) < 1e-9,
          "k=0 时 a 应等于观测 yaw");
    check(std::abs(x[10] - kH) < 1e-9, "h 初值应为 0.102");
  }

  // ---------- 2) 三种首帧都能选中正确的 k，且 h 不落负值 ----------
  for (int first_id = 0; first_id < 3; ++first_id) {
    const auto r = run_tri(first_id, 200, 0.005, 42 + first_id);
    const double z_err = (r.cz_final - kCz) * 1000.0;
    std::printf(
      "2.%d) 首帧 id%d：选中 k=%d（误差 %.3f/%.3f/%.3f，比值 %.2f）  h=%.1f mm  z 误差 %+.1f mm\n",
      first_id, first_id, r.best_k, r.err[0], r.err[1], r.err[2],
      [&] {
        double second = 1e18;
        for (int k = 0; k < 3; ++k)
          if (k != r.best_k) second = std::min(second, r.err[k]);
        return r.err[r.best_k] / std::max(1e-9, second);
      }(),
      r.h_final * 1000, z_err);

    check(r.best_k == first_id, "应选中与真值一致的首板假设");
    check(r.h_final > 0.0, "h 不得落到负值（否则三块板高度全错）");
    check(std::abs(r.h_final - kH) < 0.005, "h 应收敛到 +0.102 m");
    check(std::abs(z_err) < 5.0, "中心高度误差应小于 5 mm");

    // 区分度：正确假设的累积误差应显著小于次优者。
    // 若将来改动把区分度削弱了（比如误差度量退化），锁定就会不可靠，
    // 这条断言能提前发现 —— 光"选对了"不够，还要"选得稳"。
    double err_second = 1e18;
    for (int k = 0; k < 3; ++k) {
      if (k != r.best_k) err_second = std::min(err_second, r.err[k]);
    }
    const double ratio = r.err[r.best_k] / std::max(1e-9, err_second);
    check(ratio < 0.5, "正确假设的误差应不到次优者的一半（锁定才有余量）");
  }

  // ---------- 3) 锁定时机：90 帧内不得锁定（编号平移等价解无法区分）----------
  //
  // 这条防的是原实现的 bug：`seen_all || frames >= 90` 会在只看到 2 块板时
  // 就强制锁定，而此时三个假设的误差完全相同（比值 1.00），锁在谁身上是随机的。
  std::printf("\n3) 锁定时机（区分度判据，阈值 %.1f）\n", kDiscriminate);
  for (int first_id = 0; first_id < 3; ++first_id) {
    const auto r = run_with_locking(first_id, 300, 0.005, 100 + first_id);
    std::printf(
      "   首帧 id%d：锁定帧=%d 选中 k=%d（真值 %d）%s  90帧时已锁=%s\n", first_id, r.lock_frame,
      r.lock_best_k, first_id, (r.lock_best_k == first_id ? "OK" : "WRONG"),
      r.locked_at_90 ? "是" : "否");

    check(!r.locked_at_90, "90 帧内不应锁定（此时区分度不足，会锁错）");
    check(r.locked, "300 帧内应完成锁定");
    check(r.lock_best_k == first_id, "锁定时应选中正确的首板假设");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
