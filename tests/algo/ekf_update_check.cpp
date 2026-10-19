// EKF 数值回归测试：
//   A. 状态/协方差更新与"显式求逆"参考实现一致（保护 LDLT 改动）
//   B. NIS 必须是"先验新息 + 先验 S"的卡方统计量（防止有人改回后验收敛量）
//   C. 阈值 9.488 是 χ²(4) 的 95% 分位数、旧值 0.711 是 5% 分位数（闭式验证，无需外部库）
//   D. 蒙特卡洛：NIS 的样本均值 ≈ 4、P(NIS > 9.488) ≈ 5%（证明阈值确实给出 5% 误报率）
//
// 用法（项目根目录）：g++ -std=c++17 -I. -I<eigen> \
//     tests/algo/ekf_update_check.cpp tools/extended_kalman_filter.cpp
#include <cmath>
#include <cstdio>
#include <random>

#include "tools/extended_kalman_filter.hpp"

namespace
{
// χ²(dof=4) 的生存函数有闭式：P(X > x) = e^{-x/2} (1 + x/2)
double chi2_4_sf(double x) { return std::exp(-x / 2.0) * (1.0 + x / 2.0); }

int failures = 0;
void check(bool ok, const char * what, double lhs, double rhs, double tol)
{
  std::printf("%-50s %s  (%.6f vs %.6f, tol %.1e)\n", what, ok ? "OK  " : "FAIL", lhs, rhs, tol);
  if (!ok) failures++;
}
}  // namespace

int main()
{
  constexpr int NX = 11, NZ = 4;

  // ---------------- A. LDLT 实现 vs 显式求逆参考实现 ----------------
  {
    Eigen::VectorXd x0(NX);
    x0 << 2.0, 0.5, 1.0, -0.3, 0.2, 0.0, 0.7, 1.2, 0.25, 0.0, 0.0;

    Eigen::MatrixXd A(NX, NX);
    for (int i = 0; i < NX; i++)
      for (int j = 0; j < NX; j++) A(i, j) = std::sin(0.7 * i + 1.3 * j);
    Eigen::MatrixXd P0 = A.transpose() * A + Eigen::MatrixXd::Identity(NX, NX) * 0.1;

    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(NZ, NX);
    H(0, 0) = 1.0; H(1, 2) = 1.0; H(2, 4) = 1.0; H(3, 6) = 1.0;
    Eigen::MatrixXd R = Eigen::MatrixXd::Identity(NZ, NZ) * 0.04;

    Eigen::VectorXd z(NZ);
    z << 2.1, 0.9, 0.25, 0.65;

    tools::ExtendedKalmanFilter ekf(x0, P0);
    auto x_new = ekf.update(z, H, R);

    Eigen::MatrixXd K_ref = P0 * H.transpose() * (H * P0 * H.transpose() + R).inverse();
    Eigen::VectorXd x_ref = x0 + K_ref * (z - H * x0);
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(NX, NX);
    Eigen::MatrixXd P_ref =
      (I - K_ref * H) * P0 * (I - K_ref * H).transpose() + K_ref * R * K_ref.transpose();

    check((x_new - x_ref).norm() < 1e-10, "A1 状态更新(LDLT) == 显式求逆", (x_new - x_ref).norm(),
          0.0, 1e-10);
    check((ekf.P - P_ref).norm() < 1e-10, "A2 协方差更新(LDLT) == 显式求逆",
          (ekf.P - P_ref).norm(), 0.0, 1e-10);

    // ---------------- B. NIS 必须是先验定义 ----------------
    // 若实现改回"后验收敛量"，data["nis"] 会明显偏离下面的手工计算结果
    Eigen::VectorXd v = z - H * x0;
    Eigen::MatrixXd S_prior = H * P0 * H.transpose() + R;
    double nis_manual = (v.transpose() * S_prior.ldlt().solve(v)).value();
    check(std::abs(ekf.data["nis"] - nis_manual) < 1e-9, "B  data[\"nis\"] == 先验新息卡方统计量",
          ekf.data["nis"], nis_manual, 1e-9);
  }

  // ---------------- C. 阈值本身（χ²(4) 闭式） ----------------
  // 9.488 应为 95% 分位数：P(X > 9.488) = 0.05
  check(std::abs(chi2_4_sf(9.488) - 0.05) < 5e-4, "C1 9.488 是 χ²(4) 95% 分位(误报率5%)",
        chi2_4_sf(9.488), 0.05, 5e-4);
  // 0.711 应为 5% 分位数：P(X <= 0.711) = 0.05 —— 旧阈值下 95% 的正常更新会被判失败
  check(std::abs((1.0 - chi2_4_sf(0.711)) - 0.05) < 1e-3, "C2 0.711 是 χ²(4) 5% 分位(误报率95%)",
        1.0 - chi2_4_sf(0.711), 0.05, 1e-3);

  // ---------------- D. 蒙特卡洛：NIS 真的服从 χ²(4) ----------------
  {
    const double dt = 1.0 / 60.0, v1 = 100.0, v2 = 75.0;
    const int REP = 300, STEPS = 300, BURN = 60;

    Eigen::MatrixXd F = Eigen::MatrixXd::Identity(NX, NX);
    F(0, 1) = dt; F(2, 3) = dt; F(4, 5) = dt; F(6, 7) = dt;

    const double a = dt * dt * dt * dt / 4, b = dt * dt * dt / 2, c = dt * dt;
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(NX, NX);
    const int idx[4][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
    for (int k = 0; k < 4; k++) {
      const double v = (k == 3) ? v2 : v1;
      const int i = idx[k][0], j = idx[k][1];
      Q(i, i) = a * v; Q(i, j) = b * v; Q(j, i) = b * v; Q(j, j) = c * v;
    }

    // 确定性构造 H（避免 Eigen::Random 的跨实现差异），并保证行满秩
    Eigen::MatrixXd H(NZ, NX);
    for (int i = 0; i < NZ; i++)
      for (int j = 0; j < NX; j++) H(i, j) = std::cos(1.1 * (i + 1) + 0.37 * (j + 1));
    for (int i = 0; i < NZ; i++) H(i, i * 2) += 1.0;

    Eigen::VectorXd r_dig(NZ);
    r_dig << 4e-2, 4e-2, 1.0, 2e-2;
    Eigen::MatrixXd R = r_dig.asDiagonal();

    std::mt19937 gen(20261018);
    std::normal_distribution<double> g(0.0, 1.0);

    double sum = 0;
    int over_9488 = 0, over_0711 = 0, n = 0;

    for (int rep = 0; rep < REP; rep++) {
      Eigen::VectorXd xt(NX);
      xt << 5.0, 0.8, 1.5, -0.5, 0.3, 0.0, 0.2, 1.2, 0.2, 0.0, 0.0;
      Eigen::VectorXd x0 = xt;
      for (int i = 0; i < NX; i++) x0[i] += g(gen);
      tools::ExtendedKalmanFilter ekf(x0, Eigen::MatrixXd::Identity(NX, NX));

      for (int k = 0; k < STEPS; k++) {
        Eigen::VectorXd w = Eigen::VectorXd::Zero(NX);
        w[0] = 0.5 * dt * dt * g(gen) * std::sqrt(v1); w[1] = dt * g(gen) * std::sqrt(v1);
        w[2] = 0.5 * dt * dt * g(gen) * std::sqrt(v1); w[3] = dt * g(gen) * std::sqrt(v1);
        w[4] = 0.5 * dt * dt * g(gen) * std::sqrt(v1); w[5] = dt * g(gen) * std::sqrt(v1);
        w[6] = 0.5 * dt * dt * g(gen) * std::sqrt(v2); w[7] = dt * g(gen) * std::sqrt(v2);
        xt = F * xt + w;

        Eigen::VectorXd noise(NZ);
        for (int i = 0; i < NZ; i++) noise[i] = g(gen) * std::sqrt(r_dig[i]);
        Eigen::VectorXd z = H * xt + noise;

        ekf.predict(F, Q);
        ekf.update(z, H, R);

        if (k >= BURN) {
          const double nis = ekf.data["nis"];
          sum += nis;
          if (nis > 9.488) over_9488++;
          if (nis > 0.711) over_0711++;
          n++;
        }
      }
    }

    const double mean = sum / n;
    const double p9488 = static_cast<double>(over_9488) / n;
    const double p0711 = static_cast<double>(over_0711) / n;
    std::printf("    [蒙特卡洛] 样本 %d, E[NIS]=%.4f, P(NIS>9.488)=%.4f, P(NIS>0.711)=%.4f\n", n,
                mean, p9488, p0711);
    check(std::abs(mean - 4.0) < 0.15, "D1 E[NIS] ≈ 4 (χ²(4) 均值)", mean, 4.0, 0.15);
    check(std::abs(p9488 - 0.05) < 0.01, "D2 P(NIS>9.488) ≈ 5% (阈值正确)", p9488, 0.05, 0.01);
    check(p0711 > 0.90, "D3 旧阈值 0.711 误报率 > 90% (必须修改)", p0711, 0.90, 0.0);
  }

  std::printf("\n%s (%d 项失败)\n", failures ? "EKF_UPDATE_CHECK_FAIL" : "EKF_UPDATE_CHECK_PASS",
              failures);
  return failures ? 1 : 0;
}
