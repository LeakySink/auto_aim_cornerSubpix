#include "session.hpp"

#include "clock.hpp"
#include "proto.hpp"

#include <chrono>
#include <cstdio>
#include <sys/stat.h>

namespace tools
{
namespace rdbg
{

void Session::open(const std::string & log_dir)
{
  close();
  log_dir_ = log_dir;
  if (!log_dir_.empty()) ::mkdir(log_dir_.c_str(), 0755);
}

void Session::close()
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!fp_) return;
  std::fflush(fp_);
  std::fclose(fp_);
  fp_ = nullptr;
  path_.clear();
}

bool Session::ensure_file()
{
  if (fp_) return true;
  if (log_dir_.empty()) return false;
  path_ = log_dir_ + "/run_" + std::to_string(now_ns()) + ".rlog";
  fp_ = std::fopen(path_.c_str(), "wb");
  if (!fp_) {
    std::fprintf(stderr, "[RemoteLogger] Failed to open %s\n", path_.c_str());
    path_.clear();
    return false;
  }
  std::setvbuf(fp_, nullptr, _IOFBF, kSessionIoBuf);
  uint32_t magic = kFileMagicV2;
  if (std::fwrite(&magic, sizeof(magic), 1, fp_) != 1) {
    std::fprintf(stderr, "[RemoteLogger] Failed to write magic to %s\n", path_.c_str());
    std::fclose(fp_);
    fp_ = nullptr;
    path_.clear();
    return false;
  }
  std::fprintf(stderr, "[RemoteLogger] local session %s\n", path_.c_str());
  return true;
}

void Session::flush_maybe(bool force)
{
  if (!fp_) return;
  auto now = std::chrono::steady_clock::now();
  if (!force && last_flush_.time_since_epoch().count() != 0) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_);
    if (ms.count() < static_cast<int64_t>(kSessionFlushMs)) return;
  }
  std::fflush(fp_);
  last_flush_ = now;
}

void Session::write_json(uint64_t ts, const std::string & json)
{
  write_jsons({{ts, json}});
}

void Session::write_jsons(const std::vector<std::pair<uint64_t, std::string>> & entries)
{
  if (entries.empty()) return;
  std::lock_guard<std::mutex> lock(mtx_);
  if (!ensure_file()) return;
  for (const auto & e : entries) {
    uint8_t type = kRecJson;
    uint32_t len = static_cast<uint32_t>(e.second.size());
    if (std::fwrite(&type, sizeof(type), 1, fp_) != 1 ||
        std::fwrite(&e.first, sizeof(e.first), 1, fp_) != 1 ||
        std::fwrite(&len, sizeof(len), 1, fp_) != 1 ||
        std::fwrite(e.second.data(), 1, len, fp_) != len) {
      std::fprintf(stderr, "[RemoteLogger] Failed to write var record\n");
      return;
    }
  }
  flush_maybe(false);
}

void Session::write_image(uint64_t ts, const nlohmann::json & meta,
                         const std::vector<uint8_t> & jpeg)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!ensure_file()) return;
  std::string meta_str = meta.dump();
  uint8_t type = kRecImg;
  uint32_t meta_len = static_cast<uint32_t>(meta_str.size());
  uint32_t jpg_len = static_cast<uint32_t>(jpeg.size());
  if (std::fwrite(&type, sizeof(type), 1, fp_) != 1 ||
      std::fwrite(&ts, sizeof(ts), 1, fp_) != 1 ||
      std::fwrite(&meta_len, sizeof(meta_len), 1, fp_) != 1 ||
      std::fwrite(meta_str.data(), 1, meta_len, fp_) != meta_len ||
      std::fwrite(&jpg_len, sizeof(jpg_len), 1, fp_) != 1 ||
      std::fwrite(jpeg.data(), 1, jpg_len, fp_) != jpg_len) {
    std::fprintf(stderr, "[RemoteLogger] Failed to write img record\n");
    return;
  }
  flush_maybe(false);
}

}  // namespace rdbg
}  // namespace tools
