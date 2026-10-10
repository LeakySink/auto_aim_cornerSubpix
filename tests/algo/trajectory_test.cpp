#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "tools/trajectory.hpp"

namespace
{
constexpr double RAD2DEG = 57.29577951308232;

void print_trajectory(const char * name, const tools::Trajectory & traj)
{
  if (traj.unsolvable) {
    std::printf("  %s=UNSOLVABLE", name);
    return;
  }

  std::printf(
    "  %s=%.2f deg (t=%.3f)",
    name, traj.pitch * RAD2DEG, traj.fly_time);
}
}  // namespace

int main(int argc, char ** argv)
{
  double v0 = 22.0;   // 弹速 m/s
  double d = 18.0;    // 水平距离 m
  double k = 0.02;    // 空气阻力系数 1/m

  // 可选参数：
  // ./trajectory_test [v0] [d] [drag_coefficient]
  if (argc > 1) v0 = std::atof(argv[1]);
  if (argc > 2) d = std::atof(argv[2]);
  if (argc > 3) k = std::atof(argv[3]);

  std::printf("v0=%.2f m/s, d=%.2f m, drag_coefficient=%.4f 1/m\n", v0, d, k);
  std::printf("------------------------------------------------------------\n");

  const double h_list[] = {-5.0, -3.0, -1.0, 0.0, 1.0, 3.0, 5.0};

  for (double h : h_list) {
    tools::Trajectory no_drag(v0, d, h, 0.0);
    tools::Trajectory with_drag(v0, d, h, k);

    std::printf("h=%+.1f", h);
    print_trajectory("no_drag", no_drag);
    print_trajectory("drag", with_drag);

    if (!no_drag.unsolvable && !with_drag.unsolvable) {
      const double delta_deg = (with_drag.pitch - no_drag.pitch) * RAD2DEG;
      std::printf("  delta=%+.2f deg", delta_deg);
    }

    std::printf("\n");
  }

  return 0;
}