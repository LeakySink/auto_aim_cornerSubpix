#ifndef TOOLS_RDBG_TF_AUTO_PUB_HPP
#define TOOLS_RDBG_TF_AUTO_PUB_HPP

#include <string>

#include <Eigen/Geometry>
#include <yaml-cpp/yaml.h>

namespace tools::rdbg
{

/// 从整份 yaml 读外参，只发布一次 plot.tf（静态外参 + identity 姿态）。
/// 缺键则静默跳过。可重复调用，仅首次生效。
void publish_tf_extrinsics_once(const YAML::Node & root, const std::string & config_path);

/// 用已缓存外参 + 当前姿态发布 plot.tf（可多次）。外参未加载时无操作。
void publish_tf_attitude(const Eigen::Quaterniond & q);

}  // namespace tools::rdbg

#endif  // TOOLS_RDBG_TF_AUTO_PUB_HPP
