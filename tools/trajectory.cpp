#include "trajectory.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "ballistics_poly.hpp"  // 可选的"多项式弹道"模型（ballistics-main 离线拟合）

namespace tools
{
namespace
{
constexpr double g = 9.7833;
constexpr double PI = 3.14159265358979323846;

constexpr double kMinVx = 1e-6;
constexpr double kMaxAbsPitch = 80.0 * PI / 180.0;

bool solve_no_drag(
  double v0, double d, double h, double & pitch, double & fly_time)
{
  auto a = g * d * d / (2.0 * v0 * v0);
  auto b = -d;
  auto c = a + h;
  auto delta = b * b - 4.0 * a * c;

  if (delta < 0.0) return false;

  auto tan_pitch_1 = (-b + std::sqrt(delta)) / (2.0 * a);
  auto tan_pitch_2 = (-b - std::sqrt(delta)) / (2.0 * a);

  auto pitch_1 = std::atan(tan_pitch_1);
  auto pitch_2 = std::atan(tan_pitch_2);

  auto t_1 = d / (v0 * std::cos(pitch_1));
  auto t_2 = d / (v0 * std::cos(pitch_2));

  if (t_1 < t_2) {
    pitch = pitch_1;
    fly_time = t_1;
  } else {
    pitch = pitch_2;
    fly_time = t_2;
  }

  return true;
}

struct State
{
  double vx;  // 水平速度
  double z;   // 高度
  double vz;  // 垂直速度
  double t;   // 飞行时间
};

bool deriv(const State & s, double k, State & ds)
{
  if (s.vx <= kMinVx) return false;

  double v = std::hypot(s.vx, s.vz);

  // 以水平距离 x 为自变量：
  // dvx/dx = -k * |v|
  // dz /dx = vz / vx
  // dvz/dx = (-g - k * |v| * vz) / vx
  // dt /dx = 1 / vx
  ds.vx = -k * v;
  ds.z = s.vz / s.vx;
  ds.vz = (-g - k * v * s.vz) / s.vx;
  ds.t = 1.0 / s.vx;
  return true;
}

State add_scaled(const State & s, const State & ds, double scale)
{
  return {
    s.vx + scale * ds.vx,
    s.z + scale * ds.z,
    s.vz + scale * ds.vz,
    s.t + scale * ds.t};
}

// 以 theta 角度发射，积分到水平距离 d，返回该处高度 z 和飞行时间 t
bool simulate_z_at_x(
  double v0, double theta, double d, double k, double & z_out, double & t_out)
{
  if (std::abs(theta) > kMaxAbsPitch) return false;

  // 0.1m 步长。若 CPU 性能紧张可以改成 0.2m；追求精度可以改成 0.05m。
  constexpr double dx_target = 0.1;
  int steps = std::max(1, static_cast<int>(std::ceil(d / dx_target)));
  double dx = d / steps;

  State s{
    v0 * std::cos(theta),
    0.0,
    v0 * std::sin(theta),
    0.0};

  for (int i = 0; i < steps; ++i) {
    State k1, k2, k3, k4;
    if (!deriv(s, k, k1)) return false;

    auto s2 = add_scaled(s, k1, 0.5 * dx);
    if (!deriv(s2, k, k2)) return false;

    auto s3 = add_scaled(s, k2, 0.5 * dx);
    if (!deriv(s3, k, k3)) return false;

    auto s4 = add_scaled(s, k3, dx);
    if (!deriv(s4, k, k4)) return false;

    s.vx += dx / 6.0 * (k1.vx + 2.0 * k2.vx + 2.0 * k3.vx + k4.vx);
    s.z  += dx / 6.0 * (k1.z  + 2.0 * k2.z  + 2.0 * k3.z  + k4.z);
    s.vz += dx / 6.0 * (k1.vz + 2.0 * k2.vz + 2.0 * k3.vz + k4.vz);
    s.t  += dx / 6.0 * (k1.t  + 2.0 * k2.t  + 2.0 * k3.t  + k4.t);
  }

  if (!std::isfinite(s.z) || !std::isfinite(s.t)) return false;

  z_out = s.z;
  t_out = s.t;
  return true;
}

bool solve_drag(
  double v0, double d, double h, double k, double & pitch, double & fly_time)
{
  // 先用无阻力解析解作为初值，并天然选择低伸弹道
  double theta = 0.0;
  double t = 0.0;
  if (!solve_no_drag(v0, d, h, theta, t)) return false;

  constexpr int MAX_ITER = 12;
  constexpr double Z_TOL = 1e-4;  // 0.1mm
  constexpr double MAX_STEP = 10.0 * PI / 180.0;

  for (int iter = 0; iter < MAX_ITER; ++iter) {
    double z = 0.0;
    if (!simulate_z_at_x(v0, theta, d, k, z, t)) return false;

    double err = h - z;
    if (std::abs(err) < Z_TOL) {
      pitch = theta;
      fly_time = t;
      return true;
    }

    // 用无阻力模型的解析导数近似 Jacobian，避免有限差分额外积分
    double cos_t = std::cos(theta);
    double tan_t = std::tan(theta);
    double sec2 = 1.0 / (cos_t * cos_t);

    // dz_no_drag / dtheta
    double dhdtheta = d * sec2 * (1.0 - g * d * tan_t / (v0 * v0));

    double step;
    if (std::abs(dhdtheta) < 1e-9) {
      step = (err > 0.0 ? 1.0 : -1.0) * 1e-3;
    } else {
      step = err / dhdtheta;
    }

    step = std::max(-MAX_STEP, std::min(MAX_STEP, step));

    double theta_new = theta + step;
    if (std::abs(theta_new) > kMaxAbsPitch) return false;

    theta = theta_new;
  }

  // 达到最大迭代次数后，若已经足够接近也返回
  double z = 0.0;
  if (!simulate_z_at_x(v0, theta, d, k, z, t)) return false;

  if (std::abs(z - h) < 1e-3) {
    pitch = theta;
    fly_time = t;
    return true;
  }

  return false;
}
}  // namespace

Trajectory::Trajectory(
  const double v0, const double d, const double h, const double drag_coefficient)
{
  unsolvable = false;
  fly_time = 0.0;
  pitch = 0.0;
  used_poly = false;

  // 判据写成 `!(isfinite(x) && x > 0)`，一次覆盖两类非法输入：
  //   - NaN：x > 0 为 false，且 isfinite 为 false -> 拦截。
  //     （原写法 `x <= 0` 对 NaN 是 false，**放行**，是本次要修的核心问题）
  //   - +inf：x > 0 为 true，但 isfinite 为 false -> 拦截。
  //     （只写 `!(x > 0)` 拦不住 +inf，会漏进求解器算出一堆 NaN）
  //
  // 原写法之所以致命：NaN 一路穿过 solve_no_drag（那里的 `delta < 0.0`
  // 对 NaN 同样是 false），得到 NaN 的 pitch / fly_time 且 unsolvable
  // 仍为 false。下游 aimer.cpp 的 static_cast<int>(fly_time * 1e6) 对 NaN
  // 是 UB（x86 上得 INT_MIN），时间戳被回拨约 35 分钟 -> 下一帧 dt 变成
  // 巨大值 -> EKF 的 dt^4 项爆炸 -> 状态推成 inf -> limit_rad(inf) 死循环。
  if (
    !(std::isfinite(v0) && v0 > 0.0) || !(std::isfinite(d) && d > 0.0) ||
    !std::isfinite(h) || !std::isfinite(drag_coefficient)) {
    unsolvable = true;
    return;
  }

  // ---------------------------------------------------------------------------
  // 弹道模型选择（2026-10 新增）：
  //   1) 若配置里装了 ballistics-main 的离线拟合多项式（ballistics_model: poly），
  //      优先用它 —— 它是用真实气动参数（Cd/S/m/ρ）积分后拟合的 (x,y)->(pitch,time)，
  //      比这里的单标量 k 更接近实弹；
  //   2) 多项式覆盖不到（d/h 超出拟合网格、表为空、结果非法）时，**回退**本文件
  //      原有的解析解 + RK4，行为与改动前完全一致；
  //   3) 未配置（缺省）时直接走 RK4，等于没改。
  // 回退原因带 2s 限流记一次日志，避免每帧刷屏。
  // ---------------------------------------------------------------------------
  if (auto model = ballistics_model(); model && !model->empty()) {
    std::string note;
    if (model->solve(v0, d, h, pitch, fly_time, &note)) {
      used_poly = true;
      return;  // solve() 已把 pitch 从"度"转成弧度，且抬头为正，与本结构体约定一致
    }
    ballistics_log("Ballistics:poly 回退 RK4", note);
  }

  if (drag_coefficient <= 1e-9) {
    if (!solve_no_drag(v0, d, h, pitch, fly_time)) unsolvable = true;
  } else if (!solve_drag(v0, d, h, drag_coefficient, pitch, fly_time)) {
    unsolvable = true;
  }

  // 最后一道闸：无论走哪条分支，结果只要不是有限值就判为无解，
  // 并把输出清零。仅置 unsolvable 不够 —— 结构体里的 fly_time / pitch
  // 仍会是 NaN，一旦有调用方忘了先查 unsolvable 就直接用，仍是 UB。
  if (!std::isfinite(pitch) || !std::isfinite(fly_time)) {
    unsolvable = true;
    pitch = 0.0;
    fly_time = 0.0;
  }
}

}  // namespace tools