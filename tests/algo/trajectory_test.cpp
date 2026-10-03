//
// Created by ckyf on 2026/10/4.
//
#include <cstdio>
#include <initializer_list>

#include "tools/trajectory.hpp"

int main()
{
  const double v0 = 22.0;
  const double d = 18.0;

  for (double h : {-3.0, 0.0, 3.0, 5.0}) {
    tools::Trajectory no_drag(v0, d, h, 0.0);
    tools::Trajectory with_drag(v0, d, h, 0.02);

    std::printf(
      "h=%+.1f  no_drag=%.2f deg (t=%.3f)  "
      "drag=%.2f deg (t=%.3f)\n",
      h,
      no_drag.pitch * 57.2958,
      no_drag.fly_time,
      with_drag.pitch * 57.2958,
      with_drag.fly_time);
  }

  return 0;
}