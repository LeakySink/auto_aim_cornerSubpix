#ifndef TOOLS__YAML_HPP
#define TOOLS__YAML_HPP

#include <filesystem>
#include <string>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

#include "tools/remote_logger.hpp"

namespace tools
{
inline YAML::Node load_file(const std::string & path)
{
  try {
    return YAML::LoadFile(path);
  } catch (const YAML::BadFile & e) {
    tools::RemoteLogger::instance().log("ERROR", "[YAML] Failed to load file: {}", e.what());
    exit(1);
  } catch (const YAML::ParserException & e) {
    tools::RemoteLogger::instance().log("ERROR", "[YAML] Parser error: {}", e.what());
    exit(1);
  }
}

// 当前文件 camera_dir 下的同名 yaml，例如 configs/sentry.yaml → configs/camera/sentry.yaml
inline std::string camera_yaml_path(const std::string & config_path)
{
  auto yaml = load_file(config_path);
  if (!yaml["camera_dir"]) return {};
  const auto dir = yaml["camera_dir"].as<std::string>();
  const auto name = std::filesystem::path(config_path).filename();
  return (std::filesystem::path(dir) / name).string();
}

namespace detail
{
inline void merge_map(YAML::Node & dst, const YAML::Node & src)
{
  for (auto it = src.begin(); it != src.end(); ++it) {
    const auto key = it->first.as<std::string>();
    if (key == "camera_dir") continue;
    dst[key] = YAML::Clone(it->second);
  }
}

// 沿 camera_dir 递归合并：A → camera_dir/A.yaml → …；深层先合并，近层覆盖远层
inline YAML::Node load_recursive(
  const std::string & path, std::unordered_set<std::string> & visited)
{
  const auto canon = std::filesystem::weakly_canonical(std::filesystem::absolute(path)).string();
  if (!visited.insert(canon).second) {
    tools::RemoteLogger::instance().log(
      "ERROR", "[YAML] camera_dir cycle detected at: {}", path);
    exit(1);
  }

  auto yaml = load_file(path);
  if (!yaml["camera_dir"]) return yaml;

  const auto dir = yaml["camera_dir"].as<std::string>();
  const auto child =
    (std::filesystem::path(dir) / std::filesystem::path(path).filename()).string();
  if (!std::filesystem::exists(child)) {
    tools::RemoteLogger::instance().log(
      "ERROR", "[YAML] camera_dir file not found: {}", child);
    exit(1);
  }

  auto nested = load_recursive(child, visited);
  merge_map(yaml, nested);
  return yaml;
}
}  // namespace detail

// 加载 yaml，并按 camera_dir 递归合并同名相机参数文件
inline YAML::Node load(const std::string & path)
{
  std::unordered_set<std::string> visited;
  return detail::load_recursive(path, visited);
}

template <typename T>
inline T read(const YAML::Node & yaml, const std::string & key)
{
  if (yaml[key]) return yaml[key].as<T>();
  tools::RemoteLogger::instance().log("ERROR", "[YAML] {} not found!", key);
  exit(1);
}

}  // namespace tools

#endif  // TOOLS__YAML_HPP
