#ifndef AUTO_AIM__KEYPOINT_CHECK_HPP
#define AUTO_AIM__KEYPOINT_CHECK_HPP

// 关键点语义校验：换模型/换装甲板样式时，**第一道防线**。
//
// 动机：如果新模型的关键点顺序变了（例如 TL,TR,BR,BL 变成 TR,TL,BL,BR）、或者模型
// 把点给到了错误的特征上，PnP 不会报错 —— 它会安静地解出一个错误位姿，表现为"框看着
// 没问题但就是打不准"。这类问题靠肉眼看很难发现，靠几何约束则一眼可辨。
//
// 判据（只做"必然不合法"的检查，避免误伤大角度/透视畸变）：
//   1) 恰好 4 个点、全部有限
//   2) 凸四边形（相邻边叉积同号）—— 顺序错/点崩会立刻违反
//   3) 对边长度比接近 1（上≈下、左≈右）；透视会让它们略有差异，故容差放宽到 ±40%
//   4) 相邻边不能退化（任一边 < 板宽的 3%）
//   5) 宽高比落在合理区间（同样是宽松区间，只挡量级错误）
//
// 用法：Solver::solve() 里每帧调用；默认只记 WARN（reject_bad_keypoints=true 时才拒收），
//       因此开启它不会改变现有行为，只是把"错误静默发生"变成"日志里看得见"。

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "armor_geometry.hpp"

namespace auto_aim
{

struct KeypointReport
{
  bool ok = true;
  const char * reason = "";
  double aspect = 0.0;            ///< 上/下边均值 ÷ 左/右边均值
  double top_bottom_ratio = 0.0;  ///< 上边 ÷ 下边
  double left_right_ratio = 0.0;  ///< 左边 ÷ 右边
};

inline double kp_len(const cv::Point2f & a, const cv::Point2f & b)
{
  return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y));
}

inline double kp_cross(const cv::Point2f & o, const cv::Point2f & a, const cv::Point2f & b)
{
  return static_cast<double>(a.x - o.x) * (b.y - o.y) - static_cast<double>(a.y - o.y) * (b.x - o.x);
}

/// edge_ratio_tol: |log(ratio)| 上限（0.35 ≈ ±40%）；aspect 用几何推导的期望值放宽 0.25x~4x
inline KeypointReport check_keypoints(
  const std::vector<cv::Point2f> & pts, const ArmorGeometry & g, bool is_big = false,
  double edge_ratio_tol = 0.35)
{
  KeypointReport r;
  if (pts.size() != 4) {
    r.ok = false;
    r.reason = "关键点数不是 4";
    return r;
  }
  for (const auto & p : pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      r.ok = false;
      r.reason = "关键点含非有限值";
      return r;
    }
  }

  // 凸性：四个顶点同向
  const double c0 = kp_cross(pts[0], pts[1], pts[2]);
  const double c1 = kp_cross(pts[1], pts[2], pts[3]);
  const double c2 = kp_cross(pts[2], pts[3], pts[0]);
  const double c3 = kp_cross(pts[3], pts[0], pts[1]);
  const bool all_pos = c0 > 0 && c1 > 0 && c2 > 0 && c3 > 0;
  const bool all_neg = c0 < 0 && c1 < 0 && c2 < 0 && c3 < 0;
  if (!all_pos && !all_neg) {
    r.ok = false;
    r.reason = "不是凸四边形（关键点顺序或位置错乱）";
    return r;
  }

  const double top = kp_len(pts[0], pts[1]);
  const double right = kp_len(pts[1], pts[2]);
  const double bottom = kp_len(pts[2], pts[3]);
  const double left = kp_len(pts[3], pts[0]);
  if (bottom < 1e-6 || right < 1e-6) {
    r.ok = false;
    r.reason = "存在退化边";
    return r;
  }

  r.top_bottom_ratio = top / bottom;
  r.left_right_ratio = left / right;
  r.aspect = ((top + bottom) / 2.0) / ((left + right) / 2.0);

  if (std::abs(std::log(r.top_bottom_ratio)) > edge_ratio_tol ||
      std::abs(std::log(r.left_right_ratio)) > edge_ratio_tol) {
    r.ok = false;
    r.reason = "对边长度比异常（上≠下 或 左≠右 超过 40%）";
    return r;
  }

  // 期望宽高比 = 板宽 / 灯条长；宽松区间只挡量级错误
  const double expect = g.width(is_big ? ArmorType::big : ArmorType::small) / g.lightbar_length;
  if (r.aspect < expect * 0.25 || r.aspect > expect * 4.0) {
    r.ok = false;
    r.reason = "宽高比超出合理区间";
    return r;
  }

  const double min_edge = g.width(is_big ? ArmorType::big : ArmorType::small) * 0.03;
  if (std::min(std::min(top, bottom), std::min(left, right)) < min_edge) {
    r.ok = false;
    r.reason = "存在过短边（点集中在一条线上）";
    return r;
  }
  return r;
}

}  // namespace auto_aim

#endif  // AUTO_AIM__KEYPOINT_CHECK_HPP
