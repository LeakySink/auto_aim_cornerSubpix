#ifndef TOOLS__TRAJECTORY_HPP
#define TOOLS__TRAJECTORY_HPP

namespace tools
{
struct Trajectory
{
  bool unsolvable;
  double fly_time;
  double pitch;  // 抬头为正

  // v0  : 子弹初速度大小，单位：m/s
  // d   : 目标水平距离，单位：m
  // h   : 目标竖直高度，单位：m
  // drag_coefficient: 空气阻力系数 k，单位 1/m
  //   a_drag = -k * |v| * v
  //   k = 0 时退化为无阻力抛物线
  Trajectory(
    const double v0, const double d, const double h,
    const double drag_coefficient = 0.0);
};

}  // namespace tools

#endif  // TOOLS__TRAJECTORY_HPP