#ifndef TOOLS_RDBG_MARKERS_VIZ_MARKERS_HPP
#define TOOLS_RDBG_MARKERS_VIZ_MARKERS_HPP

#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

namespace tools::viz
{

/// Watch 通用 3D Marker（plot.markers / marker_v1）。
/// 仅定义与结构；业务转换（如 Target→markers）放在各 task，不放本头文件。
/// frame_id 可选，默认 "world"；按 ns 在前端整组替换合并。
class MarkerArray
{
public:
  std::string frame_id = "world";

  MarkerArray() = default;
  explicit MarkerArray(std::string frame) : frame_id(std::move(frame))
  {
    if (frame_id.empty()) frame_id = "world";
  }

  /// 下一条 marker 覆盖 frame_id（空 = 继承 array）
  MarkerArray & with_frame(std::string frame)
  {
    next_frame_ = std::move(frame);
    return *this;
  }

  MarkerArray & sphere(
    std::string ns, std::string id, const Eigen::Vector3d & p, double radius,
    const Eigen::Vector4d & color = {1, 0.45, 0.1, 1})
  {
    nlohmann::json m = base(std::move(ns), std::move(id), "sphere");
    m["pose"] = {{"p", vec3(p)}, {"q", {1, 0, 0, 0}}};
    m["scale"] = {radius, radius, radius};
    m["color"] = rgba(color);
    items_.push_back(std::move(m));
    return *this;
  }

  MarkerArray & arrow(
    std::string ns, std::string id, const Eigen::Vector3d & p, const Eigen::Vector3d & dir,
    double shaft_len, const Eigen::Vector4d & color = {0.2, 0.9, 0.3, 1})
  {
    nlohmann::json m = base(std::move(ns), std::move(id), "arrow");
    m["pose"] = {{"p", vec3(p)}, {"q", {1, 0, 0, 0}}};
    m["dir"] = vec3(dir);
    m["shaft_len"] = shaft_len;
    m["scale"] = {shaft_len, 1, 1};
    m["color"] = rgba(color);
    items_.push_back(std::move(m));
    return *this;
  }

  MarkerArray & box(
    std::string ns, std::string id, const Eigen::Vector3d & p, const Eigen::Quaterniond & q,
    const Eigen::Vector3d & scale, const Eigen::Vector4d & color = {0.25, 0.55, 1, 0.9})
  {
    nlohmann::json m = base(std::move(ns), std::move(id), "box");
    Eigen::Quaterniond qq = q.normalized();
    m["pose"] = {{"p", vec3(p)}, {"q", {qq.w(), qq.x(), qq.y(), qq.z()}}};
    m["scale"] = vec3(scale);
    m["color"] = rgba(color);
    items_.push_back(std::move(m));
    return *this;
  }

  MarkerArray & line_list(
    std::string ns, std::string id, const std::vector<Eigen::Vector3d> & pts,
    double width = 0.005, const Eigen::Vector4d & color = {0.7, 0.7, 0.7, 0.8})
  {
    nlohmann::json m = base(std::move(ns), std::move(id), "line_list");
    nlohmann::json points = nlohmann::json::array();
    for (const auto & p : pts) points.push_back(vec3(p));
    m["points"] = std::move(points);
    m["scale"] = {width, 1, 1};
    m["color"] = rgba(color);
    m["pose"] = {{"p", {0, 0, 0}}, {"q", {1, 0, 0, 0}}};
    items_.push_back(std::move(m));
    return *this;
  }

  nlohmann::json to_json() const
  {
    std::string fid = frame_id.empty() ? "world" : frame_id;
    return {
      {"schema", "marker_v1"},
      {"frame_id", fid},
      {"items", items_},
    };
  }

private:
  nlohmann::json items_ = nlohmann::json::array();
  std::string next_frame_;

  static nlohmann::json vec3(const Eigen::Vector3d & v) { return {v.x(), v.y(), v.z()}; }

  static nlohmann::json rgba(const Eigen::Vector4d & c)
  {
    return {c.x(), c.y(), c.z(), c.w()};
  }

  nlohmann::json base(std::string ns, std::string id, const char * type)
  {
    nlohmann::json m{{"ns", std::move(ns)}, {"id", std::move(id)}, {"type", type}};
    if (!next_frame_.empty()) {
      m["frame_id"] = next_frame_;
      next_frame_.clear();
    }
    return m;
  }
};

}  // namespace tools::viz

#endif  // TOOLS_RDBG_MARKERS_VIZ_MARKERS_HPP
