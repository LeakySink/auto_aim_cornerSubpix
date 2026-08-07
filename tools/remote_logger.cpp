#include "remote_logger.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace tools
{

constexpr uint32_t kFileMagic = 0x524C4F47;
constexpr uint8_t kImgMarker = 0xFF;
constexpr size_t kMaxUdpPayload = 60000;

RemoteLogger & RemoteLogger::instance()
{
  static RemoteLogger inst;
  return inst;
}

RemoteLogger::~RemoteLogger() { shutdown(); }

void RemoteLogger::init(const Config & cfg)
{
  if (running_) shutdown();

  cfg_ = cfg;

  if (cfg_.enable_local && !cfg_.log_dir.empty()) {
    ::mkdir(cfg_.log_dir.c_str(), 0755);
  }

  if (cfg_.enable_remote) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
      cfg_.enable_remote = false;
      std::fprintf(stderr, "[RemoteLogger] socket() failed\n");
    } else {
      addr_.sin_family = AF_INET;
      addr_.sin_port = ::htons(cfg_.remote_port);
      addr_.sin_addr.s_addr = ::inet_addr(cfg_.remote_host.c_str());
    }
  }

  running_ = true;
  worker_ = std::thread(&RemoteLogger::worker, this);
}

void RemoteLogger::shutdown()
{
  if (!running_) return;
  running_ = false;
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (sock_ >= 0) {
    ::close(sock_);
    sock_ = -1;
  }
}

void RemoteLogger::plot(const nlohmann::json & data)
{
  if (!running_) return;

  uint64_t ts;
  if (data.contains("ts") && data["ts"].is_number()) {
    ts = data["ts"].get<uint64_t>();
  } else {
    ts = now_ns();
  }

  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    var_buf_.push_back({ts, data.dump()});
    if (var_buf_.size() >= cfg_.var_buffer_size) cv_.notify_one();
  }
}

void RemoteLogger::log(const std::string & level, const std::string & msg)
{
  if (!running_) return;

  nlohmann::json entry = {{"level", level}, {"msg", msg}};
  plot(entry);
}

void RemoteLogger::plot_image(cv::Mat img, const nlohmann::json & meta)
{
  if (!running_) return;

  uint64_t ts;
  if (meta.contains("ts") && meta["ts"].is_number()) {
    ts = meta["ts"].get<uint64_t>();
  } else {
    ts = now_ns();
  }

  {
    std::lock_guard<std::mutex> lock(img_mtx_);
    if (img_buf_.size() >= cfg_.img_buffer_size) img_buf_.erase(img_buf_.begin());
    img_buf_.push_back({ts, meta, std::move(img)});
    cv_.notify_one();
  }
}

uint64_t RemoteLogger::now_ns() const
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

void RemoteLogger::worker()
{
  std::vector<VarEntry> var_pending;
  std::vector<ImgEntry> img_pending;

  while (running_) {
    {
      std::unique_lock<std::mutex> lock(wake_mtx_);
      cv_.wait_for(lock, std::chrono::milliseconds(200));
    }

    if (!running_) break;

    {
      std::lock_guard<std::mutex> lock(var_mtx_);
      if (!var_buf_.empty()) var_buf_.swap(var_pending);
    }

    if (!var_pending.empty()) {
      if (cfg_.enable_local) flush_var_local(var_pending);
      if (cfg_.enable_remote) {
        for (const auto & e : var_pending) try_send_var(e);
      }
      var_pending.clear();
    }

    {
      std::lock_guard<std::mutex> lock(img_mtx_);
      if (!img_buf_.empty()) img_buf_.swap(img_pending);
    }

    if (!img_pending.empty()) {
      for (auto & e : img_pending) {
        double scale = static_cast<double>(cfg_.img_width) / e.img.cols;
        int new_h = static_cast<int>(e.img.rows * scale);
        cv::Mat resized;
        cv::resize(e.img, resized, cv::Size(cfg_.img_width, new_h));

        std::vector<uint8_t> jpeg;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg_.img_quality};
        cv::imencode(".jpg", resized, jpeg, params);

        if (cfg_.enable_remote) try_send_img(jpeg, e.ts, e.meta);
      }
      img_pending.clear();
    }
  }

  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    if (!var_buf_.empty()) var_buf_.swap(var_pending);
  }
  if (!var_pending.empty() && cfg_.enable_local) flush_var_local(var_pending);
}

void RemoteLogger::flush_var_local(const std::vector<VarEntry> & entries)
{
  if (entries.empty()) return;

  if (session_file_.empty()) {
    session_file_ = cfg_.log_dir + "/var_" + std::to_string(now_ns()) + ".rlog";
  }

  FILE * f = std::fopen(session_file_.c_str(), "ab");
  if (!f) {
    std::fprintf(stderr, "[RemoteLogger] Failed to open %s\n", session_file_.c_str());
    return;
  }

  std::fseek(f, 0, SEEK_END);
  if (std::ftell(f) == 0) {
    uint32_t magic = kFileMagic;
    std::fwrite(&magic, sizeof(magic), 1, f);
  }

  for (const auto & e : entries) {
    std::fwrite(&e.ts, sizeof(e.ts), 1, f);
    uint32_t len = static_cast<uint32_t>(e.json_str.size());
    std::fwrite(&len, sizeof(len), 1, f);
    std::fwrite(e.json_str.data(), 1, len, f);
  }

  std::fclose(f);
}

void RemoteLogger::try_send_var(const VarEntry & entry)
{
  try {
    auto j = nlohmann::json::parse(entry.json_str);
    if (!j.contains("ts")) j["ts"] = entry.ts;
    std::string payload = j.dump();
    send_udp(payload.data(), payload.size());
  } catch (...) {
    std::string payload =
      "{\"ts\":" + std::to_string(entry.ts) + ",\"raw\":\"" + entry.json_str + "\"}";
    send_udp(payload.data(), payload.size());
  }
}

void RemoteLogger::try_send_img(const std::vector<uint8_t> & jpeg, uint64_t ts,
                                const nlohmann::json & meta)
{
  std::string meta_str = meta.dump();
  size_t total = 1 + 8 + 4 + meta_str.size() + 4 + jpeg.size();
  if (total > kMaxUdpPayload) {
    std::fprintf(stderr, "[RemoteLogger] image too large %zu bytes, skipped\n", total);
    return;
  }

  std::vector<uint8_t> pkt;
  pkt.reserve(total);
  pkt.push_back(kImgMarker);
  pkt.insert(pkt.end(), reinterpret_cast<const uint8_t *>(&ts),
             reinterpret_cast<const uint8_t *>(&ts) + 8);
  uint32_t meta_len = static_cast<uint32_t>(meta_str.size());
  pkt.insert(pkt.end(), reinterpret_cast<const uint8_t *>(&meta_len),
             reinterpret_cast<const uint8_t *>(&meta_len) + 4);
  pkt.insert(pkt.end(), meta_str.begin(), meta_str.end());
  uint32_t jpg_len = static_cast<uint32_t>(jpeg.size());
  pkt.insert(pkt.end(), reinterpret_cast<const uint8_t *>(&jpg_len),
             reinterpret_cast<const uint8_t *>(&jpg_len) + 4);
  pkt.insert(pkt.end(), jpeg.begin(), jpeg.end());

  send_udp(pkt.data(), pkt.size());
}

void RemoteLogger::send_udp(const void * data, size_t len)
{
  if (sock_ < 0) return;
  ::sendto(sock_, data, len, 0, reinterpret_cast<sockaddr *>(&addr_), sizeof(addr_));
}

}  // namespace tools
