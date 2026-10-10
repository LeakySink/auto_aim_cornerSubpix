// 临时验证：新版 EKF update（LDLT 求解）与显式求逆参考实现的数值一致性
// 用法：g++ -std=c++17 -Itools -I<eigen> tests/ekf_update_check.cpp tools/extended_kalman_filter.cpp
#include <cmath>
#include <iostream>

#include "tools/extended_kalman_filter.hpp"

int main()
{
  constexpr int NX = 11, NZ = 4;

  Eigen::VectorXd x0(NX);
  x0 << 2.0, 0.5, 1.0, -0.3, 0.2, 0.0, 0.7, 1.2, 0.25, 0.0, 0.0;

  // 构造对称正定 P0
  Eigen::MatrixXd A = Eigen::MatrixXd::Random(NX, NX);
  Eigen::MatrixXd P0 = A.transpose() * A + Eigen::MatrixXd::Identity(NX, NX) * 0.1;

  Eigen::MatrixXd H = Eigen::MatrixXd::Zero(NZ, NX);
  H(0, 0) = 1.0; H(1, 2) = 1.0; H(2, 4) = 1.0; H(3, 6) = 1.0;
  Eigen::MatrixXd R = Eigen::MatrixXd::Identity(NZ, NZ) * 0.04;

  Eigen::VectorXd z(NZ);
  z << 2.1, 0.9, 0.25, 0.65;

  // 新版（LDLT）更新
  tools::ExtendedKalmanFilter ekf(x0, P0);
  auto x_new = ekf.update(z, H, R);

  // 参考：显式求逆（原实现公式，x_add 默认为 a+b，h 默认为 Hx）
  Eigen::MatrixXd K_ref = P0 * H.transpose() * (H * P0 * H.transpose() + R).inverse();
  Eigen::VectorXd x_ref = x0 + K_ref * (z - H * x0);
  Eigen::MatrixXd I = Eigen::MatrixXd::Identity(NX, NX);
  Eigen::MatrixXd P_ref =
    (I - K_ref * H) * P0 * (I - K_ref * H).transpose() + K_ref * R * K_ref.transpose();

  double dx = (x_new - x_ref).norm();
  double dP = (ekf.P - P_ref).norm();
  double nis = ekf.data["nis"];
  double nees = ekf.data["nees"];

  std::cout << "|x_new - x_ref| = " << dx << "\n";
  std::cout << "|P_new - P_ref| = " << dP << "\n";
  std::cout << "nis = " << nis << ", nees = " << nees << "\n";
  std::cout << "nis_fail = " << ekf.data["nis_fail"] << " (threshold 9.488)\n";

  bool ok = dx < 1e-10 && dP < 1e-10 && std::isfinite(nis) && std::isfinite(nees);
  std::cout << (ok ? "EKF_UPDATE_CHECK_PASS" : "EKF_UPDATE_CHECK_FAIL") << "\n";
  return ok ? 0 : 1;
}
