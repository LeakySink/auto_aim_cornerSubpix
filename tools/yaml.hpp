#ifndef TOOLS__YAML_HPP
#define TOOLS__YAML_HPP

#include <cstddef>
#include <fstream>
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

/// 打印解析出错位置前后各一行并标出列（yaml-cpp 的 line/column 都是 0 基）。
/// 目的：车端只看到一句 "error at line 14, column 15" 时无从下手，这里直接把
/// 文件路径与原文打出来。注意列号按**字节**算，含中文的行光标会偏移。
inline void log_yaml_error_context(const std::string & file, std::size_t line0, std::size_t col0)
{
  std::ifstream ifs(file);
  if (!ifs) return;
  const std::size_t target = line0 + 1;  // 1 基行号
  std::string line;
  std::size_t no = 0;
  while (std::getline(ifs, line)) {
    ++no;
    if (no + 1 < target) continue;
    if (no > target + 1) break;
    tools::RemoteLogger::instance().log("ERROR", "[YAML] {:>4} | {}", no, line);
    if (no == target) {
      tools::RemoteLogger::instance().log(
        "ERROR", "[YAML]      | {}^  ← 第 {} 行第 {} 列（按字节）", std::string(col0, ' '), target,
        col0 + 1);
    }
  }
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
    // 带上文件路径：原来只打 e.what()，现场根本不知道是哪个 yaml 坏了
    tools::RemoteLogger::instance().log(
      "ERROR", "[YAML] {} 解析失败：{}", resolved, e.what());
    log_yaml_error_context(resolved, e.mark.line, e.mark.column);
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
