#ifndef TOOLS_RDBG_SESSION_HPP
#define TOOLS_RDBG_SESSION_HPP

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tools
{
namespace rdbg
{

// L3：本地 .rlog（RLG2）。不知道 UDP。
class Session
{
public:
  void open(const std::string & log_dir);
  void close();
  void write_json(uint64_t ts, const std::string & json);
  void write_jsons(const std::vector<std::pair<uint64_t, std::string>> & entries);
  void write_image(uint64_t ts, const nlohmann::json & meta,
                   const std::vector<uint8_t> & jpeg);

private:
  bool ensure_file();  // 已持有 mtx_
  void flush_maybe(bool force);

  std::mutex mtx_;
  std::string log_dir_;
  std::string path_;
  FILE * fp_{nullptr};
  std::chrono::steady_clock::time_point last_flush_{};
};

}  // namespace rdbg
}  // namespace tools

#endif
