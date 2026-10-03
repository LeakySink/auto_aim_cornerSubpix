#include "trajectory.hpp"

#include <algorithm>
#include <cmath>

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

  if (v0 <= 0.0 || d <= 0.0) {
    unsolvable = true;
    return;
  }

  if (drag_coefficient <= 1e-9) {
    if (!solve_no_drag(v0, d, h, pitch, fly_time)) unsolvable = true;
    return;
  }

  if (!solve_drag(v0, d, h, drag_coefficient, pitch, fly_time)) {
    unsolvable = true;
  }
}

}  // namespace tools