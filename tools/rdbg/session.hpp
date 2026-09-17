#ifndef TOOLS_RDBG_SESSION_HPP
#define TOOLS_RDBG_SESSION_HPP

#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <sys/uio.h>
#include <utility>
#include <vector>

namespace tools
{
namespace rdbg
{

// L3：本地 .rlog（RLG2）。fd + writev，避免 FILE 二次缓冲拷贝。
class Session
{
public:
  void open(const std::string & log_dir);
  void close();
  void write_json(uint64_t ts, const std::string & json);
  void write_jsons(const std::vector<std::pair<uint64_t, std::string>> & entries);
  void write_image(uint64_t ts, const std::string & meta_json,
                   const std::vector<uint8_t> & jpeg);

private:
  bool ensure_file();  // 已持有 mtx_
  bool writev_all(const struct ::iovec * iov, int iovcnt);

  std::mutex mtx_;
  std::string log_dir_;
  std::string path_;
  int fd_{-1};
};

}  // namespace rdbg
}  // namespace tools

#endif
