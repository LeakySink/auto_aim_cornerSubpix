#include "extended_kalman_filter.hpp"

#include <numeric>

namespace tools
{
ExtendedKalmanFilter::ExtendedKalmanFilter(
  const Eigen::VectorXd & x0, const Eigen::MatrixXd & P0,
  std::function<Eigen::VectorXd(const Eigen::VectorXd &, const Eigen::VectorXd &)> x_add)
: x(x0), P(P0), I(Eigen::MatrixXd::Identity(x0.rows(), x0.rows())), x_add(x_add)
{
  data["residual_yaw"] = 0.0;
  data["residual_pitch"] = 0.0;
  data["residual_distance"] = 0.0;
  data["residual_angle"] = 0.0;
  data["nis"] = 0.0;
  data["nees"] = 0.0;
  data["nis_fail"] = 0.0;
  data["nees_fail"] = 0.0;
  data["recent_nis_failures"] = 0.0;
}

Eigen::VectorXd ExtendedKalmanFilter::predict(const Eigen::MatrixXd & F, const Eigen::MatrixXd & Q)
{
  return predict(F, Q, [&](const Eigen::VectorXd & x) { return F * x; });
}

Eigen::VectorXd ExtendedKalmanFilter::predict(
  const Eigen::MatrixXd & F, const Eigen::MatrixXd & Q,
  std::function<Eigen::VectorXd(const Eigen::VectorXd &)> f)
{
  P = F * P * F.transpose() + Q;
  x = f(x);
  return x;
}

Eigen::VectorXd ExtendedKalmanFilter::update(
  const Eigen::VectorXd & z, const Eigen::MatrixXd & H, const Eigen::MatrixXd & R,
  std::function<Eigen::VectorXd(const Eigen::VectorXd &, const Eigen::VectorXd &)> z_subtract)
{
  return update(z, H, R, [&](const Eigen::VectorXd & x) { return H * x; }, z_subtract);
}

Eigen::VectorXd ExtendedKalmanFilter::update(
  const Eigen::VectorXd & z, const Eigen::MatrixXd & H, const Eigen::MatrixXd & R,
  std::function<Eigen::VectorXd(const Eigen::VectorXd &)> h,
  std::function<Eigen::VectorXd(const Eigen::VectorXd &, const Eigen::VectorXd &)> z_subtract)
{
  Eigen::VectorXd x_prior = x;
  // 先验新息协方差 S = H P⁻ Hᵀ + R（卡方检验必须用"先验"量，原因见下）
  Eigen::MatrixXd S_prior = H * P * H.transpose() + R;
  // 用 LDLT 求解代替显式求逆：K = P Hᵀ S⁻¹  <=>  Kᵀ = S⁻¹ H P，数值更稳且更快
  Eigen::MatrixXd K = S_prior.ldlt().solve(H * P).transpose();

  /// 卡方检验：必须在 x / P 更新之前完成
  // 新息 v = z − h(x⁻) 的协方差恰好是 S_prior，因此 NIS = vᵀ S_prior⁻¹ v ~ χ²(观测维数)。
  // 若改用更新后的 x/P（后验残差 + 后验 S），统计量会被"吸收"掉一部分，不再服从卡方分布：
  // 实测（REVIEW_OPTIMIZATION_20261018.md）其期望只有 3.3~3.7（应为 4），
  // 阈值 9.488 的实际误报率只剩 2%~4%，且对目标跳变/误匹配不敏感。
  Eigen::VectorXd innovation = z_subtract(z, h(x));
  double nis = (innovation.transpose() * S_prior.ldlt().solve(innovation)).value();

  // Stable Compution of the Posterior Covariance
  // https://github.com/rlabbe/Kalman-and-Bayesian-Filters-in-Python/blob/master/07-Kalman-Filter-Math.ipynb
  P = (I - K * H) * P * (I - K * H).transpose() + K * R * K.transpose();

  // K * innovation 与原实现的 K * z_subtract(z, h(x)) 完全等价（h 取的都是更新前的 x），状态更新不变
  x = x_add(x, K * innovation);

  // NEES：注意这不是标准卡方统计量。
  //   标准 NEES = (x⁺ − x_true)ᵀ P⁺⁻¹ (x⁺ − x_true) ~ χ²(状态维数)，而真值不可得。
  //   这里用状态修正量 (x⁺ − x⁻)ᵀ P⁺⁻¹ (x⁺ − x⁻) 代替，其协方差是 P⁻ − P⁺（秩 ≤ 观测维数），
  //   并不服从 χ²；且 yaw 分量在 x_add 回绕 ±π 时会产生约 2π 的伪跳变
  //   （实测 99.9% 的 NEES 爆表帧都来自这个回绕，见 REVIEW_OPTIMIZATION_20261018.md）。
  //   因此 19.675 只能算保守上限；nees_fail 仅用于日志，不参与任何逻辑判断。
  double nees = ((x - x_prior).transpose() * P.ldlt().solve(x - x_prior)).value();

  // 卡方检验阈值（显著性水平 5%，即名义误报率 5%）
  // NIS：自由度 = 观测维数 4，χ²(4) 的 95% 分位数 = 9.4877。
  //      （原值 0.711 是 χ²(4) 的 5% 分位数，即 95% 的正常更新都会被判失败，必然触发误重置）
  // NEES：见上方说明，19.675 为保守上限，无严格统计含义。
  constexpr double nis_threshold = 9.488;
  constexpr double nees_threshold = 19.675;

  if (nis > nis_threshold) nis_count_++, data["nis_fail"] = 1;
  if (nees > nees_threshold) nees_count_++, data["nees_fail"] = 1;
  total_count_++;
  last_nis = nis;

  recent_nis_failures.push_back(nis > nis_threshold ? 1 : 0);

  if (recent_nis_failures.size() > window_size) {
    recent_nis_failures.pop_front();
  }

  int recent_failures = std::accumulate(recent_nis_failures.begin(), recent_nis_failures.end(), 0);
  double recent_rate = static_cast<double>(recent_failures) / recent_nis_failures.size();

  // 注：这四个量现在是"新息"（先验残差），比原来的后验残差更有诊断价值
  // ——后验残差已被本次更新吸收，天然偏小，看不出滤波器是否偏离观测。
  data["residual_yaw"] = innovation[0];
  data["residual_pitch"] = innovation[1];
  data["residual_distance"] = innovation[2];
  data["residual_angle"] = innovation[3];
  data["nis"] = nis;
  data["nees"] = nees;
  data["recent_nis_failures"] = recent_rate;

  return x;
}

}  // namespace tools