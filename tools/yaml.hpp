#ifndef TOOLS__YAML_HPP
#define TOOLS__YAML_HPP

#include <string>
#include <yaml-cpp/yaml.h>

#include "tools/remote_logger.hpp"

#ifndef SP_PROJECT_SOURCE_DIR
#define SP_PROJECT_SOURCE_DIR "."
#endif

namespace tools
{
inline const std::string & project_root()
{
  static const std::string root{SP_PROJECT_SOURCE_DIR};
  return root;
}

/// 相对路径拼到 CMake PROJECT_SOURCE_DIR；绝对路径原样返回。
inline std::string project_path(const std::string & path)
{
  if (path.empty()) return path;
  if (path.front() == '/') return path;
  std::string rel = path;
  if (rel.size() >= 2 && rel[0] == '.' && rel[1] == '/') rel = rel.substr(2);
  return project_root() + "/" + rel;
}

inline YAML::Node load(const std::string & path)
{
  const auto resolved = project_path(path);
  try {
    return YAML::LoadFile(resolved);
  } catch (const YAML::BadFile & e) {
    tools::RemoteLogger::instance().log(
      "ERROR", "[YAML] Failed to load file: {} ({})", resolved, e.what());
    exit(1);
  } catch (const YAML::ParserException & e) {
    tools::RemoteLogger::instance().log("ERROR", "[YAML] Parser error: {}", e.what());
    exit(1);
  }
}

template <typename T>
inline T read(const YAML::Node & yaml, const std::string & key)
{
  if (yaml[key]) return yaml[key].as<T>();
  tools::RemoteLogger::instance().log("ERROR", "[YAML] {} not found!", key);
  exit(1);
}

/// 读取 YAML 中的路径字段，并解析为相对工程根的绝对路径。
inline std::string read_path(const YAML::Node & yaml, const std::string & key)
{
  return project_path(read<std::string>(yaml, key));
}

}  // namespace tools

#endif  // TOOLS__YAML_HPP
