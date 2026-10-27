#ifndef AUTO_AIM__ARMOR_GEOMETRY_HPP
#define AUTO_AIM__ARMOR_GEOMETRY_HPP

// 装甲板几何参数：**全部可配置**，1 月换装甲板样式时只改 yaml，不动代码。
//
// 历史：这些量原先硬编码在两处（solver.cpp 的 SMALL/BIG_ARMOR_WIDTH、
// LIGHTBAR_LENGTH、±15° 倾角；kalman_markers.cpp 又抄了一遍），换样式要改代码 +
// 两处同步，容易漏。现在统一到一个单例，由 Solver 构造时从 yaml 装载。
//
// yaml（顶层键，全部可选；缺省 = 历史硬编码值，行为零变化）：
//   armor_geometry:
//     small_width:      0.135    # m，小装甲板宽（两灯条外缘间距）
//     big_width:        0.230    # m，大装甲板宽
//     lightbar_length:  0.056    # m，灯条长（= 板高）
//     tilt_deg:         15.0     # 普通装甲板俯仰（度）
//     outpost_tilt_deg: -15.0    # 前哨站装甲板俯仰（度）
//     thickness:        0.01     # m，仅可视化用（3D marker 的板厚）

#include <vector>

#include <opencv2/core.hpp>
#include <yaml-cpp/yaml.h>

#include "armor.hpp"

namespace auto_aim
{

struct ArmorGeometry
{
  double small_width = 0.135;      ///< m
  double big_width = 0.230;        ///< m
  double lightbar_length = 0.056;  ///< m
  double tilt_deg = 15.0;          ///< 普通板俯仰（度）
  double outpost_tilt_deg = -15.0; ///< 前哨站俯仰（度）
  double thickness = 0.01;         ///< m，可视化用

  double width(ArmorType type) const
  {
    return (type == ArmorType::big) ? big_width : small_width;
  }

  /// 板面俯仰（rad）：正常板 +tilt，前哨站 outpost_tilt
  double tilt_rad(ArmorName name) const
  {
    return ((name == ArmorName::outpost) ? outpost_tilt_deg : tilt_deg) * CV_PI / 180.0;
  }

  /// PnP 物体点（m），顺序必须与检测器给出的 4 个关键点一一对应：
  /// [右上, 左上, 左下, 右下]（沿用历史顺序，改动会让 PnP 解出镜像姿态）
  std::vector<cv::Point3f> points(ArmorType type) const
  {
    const float hw = static_cast<float>(width(type) / 2.0);
    const float hl = static_cast<float>(lightbar_length / 2.0);
    return {{0, hw, hl}, {0, -hw, hl}, {0, -hw, -hl}, {0, hw, -hl}};
  }
};

/// 进程内单例：Solver 构造时装载；viz 等直接读（避免多处各抄一份常量）
inline ArmorGeometry & mutable_armor_geometry()
{
  static ArmorGeometry g;
  return g;
}

inline const ArmorGeometry & armor_geometry() { return mutable_armor_geometry(); }

inline double yaml_or(const YAML::Node & n, const char * key, double fallback)
{
  return (n[key] && n[key].IsDefined()) ? n[key].as<double>() : fallback;
}

/// 从 yaml 读装甲几何（键缺失时保留现值）
inline void load_armor_geometry(const YAML::Node & yaml)
{
  auto & g = mutable_armor_geometry();
  const YAML::Node n = yaml["armor_geometry"];
  if (!n || !n.IsDefined()) return;
  g.small_width = yaml_or(n, "small_width", g.small_width);
  g.big_width = yaml_or(n, "big_width", g.big_width);
  g.lightbar_length = yaml_or(n, "lightbar_length", g.lightbar_length);
  g.tilt_deg = yaml_or(n, "tilt_deg", g.tilt_deg);
  g.outpost_tilt_deg = yaml_or(n, "outpost_tilt_deg", g.outpost_tilt_deg);
  g.thickness = yaml_or(n, "thickness", g.thickness);
}

}  // namespace auto_aim

#endif  // AUTO_AIM__ARMOR_GEOMETRY_HPP
