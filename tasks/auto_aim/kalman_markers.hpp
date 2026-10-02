#ifndef AUTO_AIM__KALMAN_MARKERS_HPP
#define AUTO_AIM__KALMAN_MARKERS_HPP

#include <nlohmann/json.hpp>

#include "target.hpp"

namespace tools::viz
{

/// Target EKF → Watch plot.markers（转换层，依赖 tools/rdbg/markers 定义）。
/// ns: kalman.center / kalman.vel / kalman.armor / kalman.spin
nlohmann::json kalman_markers(const auto_aim::Target & target);

}  // namespace tools::viz

#endif  // AUTO_AIM__KALMAN_MARKERS_HPP
