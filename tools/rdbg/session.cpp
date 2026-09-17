#include "session.hpp"

#include "clock.hpp"
#include "proto.hpp"

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

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
  if (fd_ < 0) return;
  ::close(fd_);
  fd_ = -1;
  path_.clear();
}

bool Session::ensure_file()
{
  if (fd_ >= 0) return true;
  if (log_dir_.empty()) return false;
  path_ = log_dir_ + "/run_" + std::to_string(now_ns()) + ".rlog";
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) {
    std::fprintf(stderr, "[RemoteLogger] Failed to open %s\n", path_.c_str());
    path_.clear();
    return false;
  }
  uint32_t magic = kFileMagicV2;
  if (::write(fd_, &magic, sizeof(magic)) != static_cast<ssize_t>(sizeof(magic))) {
    std::fprintf(stderr, "[RemoteLogger] Failed to write magic to %s\n", path_.c_str());
    ::close(fd_);
    fd_ = -1;
    path_.clear();
    return false;
  }
  std::fprintf(stderr, "[RemoteLogger] local session %s\n", path_.c_str());
  return true;
}

bool Session::writev_all(const struct ::iovec * iov, int iovcnt)
{
  if (fd_ < 0 || !iov || iovcnt <= 0) return false;
  size_t total = 0;
  for (int i = 0; i < iovcnt; ++i) total += iov[i].iov_len;
  ssize_t n = ::writev(fd_, iov, iovcnt);
  return n >= 0 && static_cast<size_t>(n) == total;
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
    uint64_t ts = e.first;
    uint32_t len = static_cast<uint32_t>(e.second.size());
    ::iovec iov[4];
    iov[0].iov_base = &type;
    iov[0].iov_len = sizeof(type);
    iov[1].iov_base = &ts;
    iov[1].iov_len = sizeof(ts);
    iov[2].iov_base = &len;
    iov[2].iov_len = sizeof(len);
    iov[3].iov_base = const_cast<char *>(e.second.data());
    iov[3].iov_len = e.second.size();
    if (!writev_all(iov, 4)) {
      std::fprintf(stderr, "[RemoteLogger] Failed to write var record\n");
      return;
    }
  }
}

void Session::write_image(uint64_t ts, const std::string & meta_json,
                          const std::vector<uint8_t> & jpeg)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!ensure_file()) return;
  uint8_t type = kRecImg;
  uint32_t meta_len = static_cast<uint32_t>(meta_json.size());
  uint32_t jpg_len = static_cast<uint32_t>(jpeg.size());
  ::iovec iov[6];
  iov[0].iov_base = &type;
  iov[0].iov_len = sizeof(type);
  iov[1].iov_base = &ts;
  iov[1].iov_len = sizeof(ts);
  iov[2].iov_base = &meta_len;
  iov[2].iov_len = sizeof(meta_len);
  iov[3].iov_base = const_cast<char *>(meta_json.data());
  iov[3].iov_len = meta_json.size();
  iov[4].iov_base = &jpg_len;
  iov[4].iov_len = sizeof(jpg_len);
  iov[5].iov_base = const_cast<uint8_t *>(jpeg.data());
  iov[5].iov_len = jpeg.size();
  if (!writev_all(iov, 6)) {
      std::fprintf(stderr, "[RemoteLogger] Failed to write img record\n");
  }
}

}  // namespace rdbg
}  // namespace tools
