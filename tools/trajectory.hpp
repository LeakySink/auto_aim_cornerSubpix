#ifndef TOOLS__TRAJECTORY_HPP
#define TOOLS__TRAJECTORY_HPP

namespace tools
{
/// RK4 回退路径使用的默认阻力系数 k（单位 1/m），模型 a_drag = -k|v|v。
///   k = ρ·Cd·S/(2M)，取 17mm 弹丸物理参数
///   （M=3.2 g, d=16.8 mm, Cd=0.47, ρ=1.15 @30℃）-> 0.01875
/// 历史：这个值原本由 yaml 的 drag_coefficient_up / drag_coefficient_down +
/// drag_height_boundary 按高度分档配置，其中 sentry.yaml 长期填了 0.2415（12.9 倍
/// 偏大），导致 22 m/s 下 d>7.2 m 弹道无解、云台不受控。2026-10 起：
///   - 首选 ballistics-main 拟合表（ballistics_model: poly，按弹速插值）
///   - 只有超出拟合网格才回退 RK4，此时用这个编译期常量，不再由 yaml 配置
constexpr double kDefaultDragCoefficient = 0.01875;

struct Trajectory
{
  bool unsolvable;
  double fly_time;
  double pitch;      // 抬头为正
  bool used_poly = false;  // true = 本次走的是 ballistics-main 拟合多项式；false = 内置 RK4/解析解

  // v0  : 子弹初速度大小，单位：m/s
  // d   : 目标水平距离，单位：m
  // h   : 目标竖直高度，单位：m
  // drag_coefficient: 空气阻力系数 k，单位 1/m，a_drag = -k|v|v
  //   k = 0 时退化为无阻力抛物线（仅测试/对照用）。
  //   生产链路不再从 yaml 传参：见上面的 kDefaultDragCoefficient 说明。
  Trajectory(
    const double v0, const double d, const double h,
    const double drag_coefficient = kDefaultDragCoefficient);
};

}  // namespace tools

#endif  // TOOLS__TRAJECTORY_HPP