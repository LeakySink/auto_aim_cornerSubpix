#include "remote_logger.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "tools/yaml.hpp"

namespace tools
{

namespace
{

constexpr uint32_t kFileMagicV2 = 0x32474C52;  // "RLG2"
constexpr uint8_t kRecJson = 0x00;
constexpr uint8_t kRecImg = 0x01;
constexpr uint8_t kImgMarker = 0xFF;
constexpr size_t kMaxUdpPayload = 60000;
constexpr size_t kSessionIoBuf = 256 * 1024;
constexpr uint64_t kImgMinIntervalNs = 33000000ULL;  // ~30 fps，仅 worker 侧限流
constexpr uint32_t kSessionFlushMs = 200;
constexpr int kVarWorkerPollMs = 50;
constexpr int kImgWorkerPollMs = 5;
constexpr int kImgEncodeBudgetMs = 25;  // 每轮 img_worker 最多编码时长
constexpr int kCtrlWorkerPollMs = 200;  // 控制线程轮询间隔

}  // namespace

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

  sender_name_ = resolve_sender();
  registered_ = false;
  last_register_ts_ = {};
  last_hb_ = {};
  img_ring_.reset(cfg_.img_buffer_size);
  {
    std::lock_guard<std::mutex> lock(session_mtx_);
    close_session_file_unlocked();
  }
  session_file_.clear();

  if (cfg_.enable_remote && cfg_.control_port == 0) {
    cfg_.enable_remote = false;
    std::fprintf(stderr, "[RemoteLogger] control_port required, remote disabled\n");
  }

  if (cfg_.enable_remote) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
      cfg_.enable_remote = false;
      std::fprintf(stderr, "[RemoteLogger] socket() failed\n");
    } else {
      addr_.sin_family = AF_INET;
      addr_.sin_port = 0;
      addr_.sin_addr.s_addr = ::inet_addr(cfg_.remote_host.c_str());
    }
  }

  running_ = true;
  if (cfg_.enable_remote && sock_ >= 0) {
    ctrl_worker_ = std::thread(&RemoteLogger::ctrl_worker_loop, this);
  }
  var_worker_ = std::thread(&RemoteLogger::var_worker_loop, this);
  img_worker_ = std::thread(&RemoteLogger::img_worker_loop, this);
}

void RemoteLogger::init(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto node = yaml["remote_logger"];
  if (!node) {
    log("ERROR", "[YAML] remote_logger not found!");
    exit(1);
  }

  Config cfg;
  cfg.remote_host = tools::read<std::string>(node, "remote_host");
  cfg.control_port = tools::read<uint16_t>(node, "control_port");
  cfg.enable_remote = tools::read<bool>(node, "enable_remote");
  cfg.enable_local = tools::read<bool>(node, "enable_local");
  cfg.log_dir = tools::read<std::string>(node, "log_dir");
  cfg.var_buffer_size = tools::read<size_t>(node, "var_buffer_size");
  cfg.img_buffer_size = tools::read<size_t>(node, "img_buffer_size");
  cfg.img_width = tools::read<int>(node, "img_width");
  cfg.img_quality = tools::read<int>(node, "img_quality");
  cfg.heartbeat_interval_ms = tools::read<uint32_t>(node, "heartbeat_interval_ms");
  cfg.sender_name = tools::read<std::string>(node, "sender_name");
  cfg.register_retry_ms = tools::read<uint32_t>(node, "register_retry_ms");

  init(cfg);
}

void RemoteLogger::shutdown()
{
  if (!running_) return;

  if (registered_) {
    int reg_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (reg_sock >= 0) {
      sockaddr_in ctrl_addr{};
      ctrl_addr.sin_family = AF_INET;
      ctrl_addr.sin_port = ::htons(cfg_.control_port);
      ctrl_addr.sin_addr.s_addr = ::inet_addr(cfg_.remote_host.c_str());

      nlohmann::json dreg;
      dreg["type"] = "deregister";
      dreg["name"] = sender_name_;
      std::string payload = dreg.dump();
      ::sendto(reg_sock, payload.data(), payload.size(), 0,
               reinterpret_cast<sockaddr *>(&ctrl_addr), sizeof(ctrl_addr));

      timeval tv{};
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      ::setsockopt(reg_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      char buf[512];
      ::recvfrom(reg_sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
      ::close(reg_sock);
    }
  }
  registered_ = false;

  running_ = false;
  var_cv_.notify_all();
  img_ring_.wake();
  if (ctrl_worker_.joinable()) ctrl_worker_.join();
  if (var_worker_.joinable()) var_worker_.join();
  if (img_worker_.joinable()) img_worker_.join();

  close_session_file();
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
    if (var_buf_.size() >= cfg_.var_buffer_size) {
      var_buf_.erase(var_buf_.begin());
    }
    var_buf_.push_back({ts, data.dump()});
  }
  var_cv_.notify_one();
}

void RemoteLogger::log(const std::string & level, const std::string & msg)
{
  auto ts = now_ns();
  auto t = static_cast<time_t>(ts / 1000000000ULL);
  auto ms = (ts / 1000000ULL) % 1000;
  char tbuf[32];
  std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", std::localtime(&t));
  std::fprintf(stderr, "%s.%03lu [%s] %s\n", tbuf,
               static_cast<unsigned long>(ms), level.c_str(), msg.c_str());

  if (!running_) return;
  nlohmann::json entry = {{"level", level}, {"msg", msg}};
  plot(entry);
}

void RemoteLogger::plot_image(const cv::Mat & img, const nlohmann::json & meta)
{
  if (!running_) return;
  if (!cfg_.enable_local && !cfg_.enable_remote) return;
  if (img.empty()) return;

  uint64_t ts;
  if (meta.contains("ts") && meta["ts"].is_number()) {
    ts = meta["ts"].get<uint64_t>();
  } else {
    ts = now_ns();
  }

  img_ring_.push(ts, meta, img);
}

uint64_t RemoteLogger::now_ns() const
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

// ── var_worker：JSON 变量 / 日志 → 本地 + UDP ─────────────────────────

void RemoteLogger::var_worker_loop()
{
  std::vector<VarEntry> pending;

  while (running_) {
    {
      std::unique_lock<std::mutex> lock(var_wake_mtx_);
      var_cv_.wait_for(lock, std::chrono::milliseconds(kVarWorkerPollMs));
    }
    if (!running_) break;

    {
      std::lock_guard<std::mutex> lock(var_mtx_);
      if (!var_buf_.empty()) var_buf_.swap(pending);
    }

    if (!pending.empty()) {
      if (cfg_.enable_local) flush_var_local(pending);
      if (cfg_.enable_remote && registered_) {
        for (const auto & e : pending) try_send_var(e);
      }
      pending.clear();
    }
  }

  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    if (!var_buf_.empty()) var_buf_.swap(pending);
  }
  if (!pending.empty()) {
    if (cfg_.enable_local) flush_var_local(pending);
    if (cfg_.enable_remote && registered_) {
      for (const auto & e : pending) try_send_var(e);
    }
  }
}

// ── ctrl_worker：注册 / 心跳 / 重试（与 var/img 完全分离）────────────

void RemoteLogger::ctrl_worker_loop()
{
  try_register();

  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kCtrlWorkerPollMs));
    if (!running_) break;

    if (!registered_) {
      try_register();
    } else if (cfg_.heartbeat_interval_ms > 0) {
      send_heartbeat();
    }
  }
}

// ── img_ring：主线程只钉 Mat 头（零拷贝），worker 取最新帧编码 ─────────

void RemoteLogger::ImgRingBuffer::reset(size_t cap)
{
  std::lock_guard<std::mutex> lock(mtx_);
  slots_.clear();
  cap_ = cap;
  tail_ = 0;
  count_ = 0;
  busy_ = false;
  if (cap_ > 0) slots_.resize(cap_);
}

void RemoteLogger::ImgRingBuffer::drop_slot(size_t idx)
{
  slots_[idx].meta = nlohmann::json{};
  slots_[idx].img.release();
}

bool RemoteLogger::ImgRingBuffer::push(uint64_t ts, nlohmann::json meta,
                                       const cv::Mat & img)
{
  if (img.empty()) return false;

  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (cap_ == 0) return false;

    if (count_ == cap_) {
      if (busy_) {
        if (cap_ == 1) return false;
        const size_t newest = (tail_ + count_ - 1) % cap_;
        drop_slot(newest);
        auto & slot = slots_[newest];
        slot.ts = ts;
        slot.meta = std::move(meta);
        slot.img = img;
        return true;
      }
      drop_slot(tail_);
      tail_ = (tail_ + 1) % cap_;
      --count_;
    }

    const size_t idx = (tail_ + count_) % cap_;
    auto & slot = slots_[idx];
    slot.ts = ts;
    slot.meta = std::move(meta);
    slot.img = img;
    ++count_;
  }
  cv_.notify_one();
  return true;
}

RemoteLogger::ImgEntry * RemoteLogger::ImgRingBuffer::acquire_latest()
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (count_ == 0 || busy_) return nullptr;
  while (count_ > 1) {
    drop_slot(tail_);
    tail_ = (tail_ + 1) % cap_;
    --count_;
  }
  busy_ = true;
  return &slots_[tail_];
}

void RemoteLogger::ImgRingBuffer::release()
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!busy_ || count_ == 0) {
    busy_ = false;
    return;
  }
  drop_slot(tail_);
  tail_ = (tail_ + 1) % cap_;
  --count_;
  busy_ = false;
}

bool RemoteLogger::ImgRingBuffer::wait_not_empty(int timeout_ms,
                                                 const std::atomic<bool> & running)
{
  std::unique_lock<std::mutex> lock(mtx_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&] { return count_ > 0 || !running.load(); });
}

void RemoteLogger::ImgRingBuffer::wake() { cv_.notify_all(); }

void RemoteLogger::ImgRingBuffer::clear()
{
  std::lock_guard<std::mutex> lock(mtx_);
  for (auto & s : slots_) {
    s.meta = nlohmann::json{};
    s.img.release();
  }
  tail_ = 0;
  count_ = 0;
  busy_ = false;
}

// ── img_worker：JPEG 编码 → 本地 + UDP（与 var_worker 互不阻塞）────────

void RemoteLogger::img_worker_loop()
{
  uint64_t last_keep_ns = 0;

  auto consume_one = [&]() -> bool {
    ImgEntry * entry = img_ring_.acquire_latest();
    if (!entry) return false;
    const uint64_t ts = entry->ts;
    if (last_keep_ns == 0 || ts < last_keep_ns ||
        ts - last_keep_ns >= kImgMinIntervalNs) {
      last_keep_ns = ts;
      encode_and_dispatch_image(*entry);
    }
    img_ring_.release();
    return true;
  };

  while (running_) {
    img_ring_.wait_not_empty(kImgWorkerPollMs, running_);
    if (!running_) break;

    const auto budget_end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kImgEncodeBudgetMs);

    while (running_ && std::chrono::steady_clock::now() < budget_end) {
      if (!consume_one()) break;
    }
  }

  while (consume_one()) {
  }
  img_ring_.clear();
}

bool RemoteLogger::encode_and_dispatch_image(const ImgEntry & entry)
{
  if (entry.img.empty() || entry.img.cols <= 0) return false;

  cv::Mat resized;
  const cv::Mat * to_encode = &entry.img;
  if (cfg_.img_width > 0 && entry.img.cols > cfg_.img_width) {
    double scale = static_cast<double>(cfg_.img_width) / entry.img.cols;
    int new_h = static_cast<int>(entry.img.rows * scale);
    if (new_h < 1) new_h = 1;
    cv::resize(entry.img, resized, cv::Size(cfg_.img_width, new_h));
    to_encode = &resized;
  }

  std::vector<uint8_t> jpeg;
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg_.img_quality};
  if (!cv::imencode(".jpg", *to_encode, jpeg, params) || jpeg.empty()) {
    return false;
  }

  resized.release();

  if (cfg_.enable_local) flush_img_local(entry.ts, entry.meta, jpeg);
  if (cfg_.enable_remote && registered_) {
    try_send_img(jpeg, entry.ts, entry.meta);
  }
  return true;
}

bool RemoteLogger::try_register()
{
  if (sock_ < 0) return false;

  auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(remote_mtx_);
    if (last_register_ts_.time_since_epoch().count() > 0) {
      auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_register_ts_);
      if (elapsed.count() < static_cast<int64_t>(cfg_.register_retry_ms)) return false;
    }
    last_register_ts_ = now;
  }

  int reg_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (reg_sock < 0) return false;

  sockaddr_in ctrl_addr{};
  ctrl_addr.sin_family = AF_INET;
  ctrl_addr.sin_port = ::htons(cfg_.control_port);
  ctrl_addr.sin_addr.s_addr = ::inet_addr(cfg_.remote_host.c_str());

  nlohmann::json reg;
  reg["type"] = "register";
  reg["name"] = sender_name_;
  std::string payload = reg.dump();
  ::sendto(reg_sock, payload.data(), payload.size(), 0,
           reinterpret_cast<sockaddr *>(&ctrl_addr), sizeof(ctrl_addr));

  timeval tv{};
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  ::setsockopt(reg_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[2048];
  ssize_t n = ::recvfrom(reg_sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
  ::close(reg_sock);

  if (n <= 0) return false;

  buf[n] = '\0';
  try {
    auto resp = nlohmann::json::parse(buf);
    if (resp.value("status", "") == "ok" && resp.contains("port")) {
      uint16_t assigned = resp["port"].get<uint16_t>();
      {
        std::lock_guard<std::mutex> lock(remote_mtx_);
        addr_.sin_port = ::htons(assigned);
      }
      registered_ = true;
      std::fprintf(stderr, "[RemoteLogger] registered '%s' -> port %d\n",
                   sender_name_.c_str(), assigned);
      return true;
    }
  } catch (...) {
  }
  return false;
}

void RemoteLogger::send_heartbeat()
{
  auto now = std::chrono::steady_clock::now();
  if (last_hb_.time_since_epoch().count() == 0) {
    last_hb_ = now;
    return;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb_);
  if (elapsed.count() < static_cast<int64_t>(cfg_.heartbeat_interval_ms)) return;
  last_hb_ = now;

  nlohmann::json hb = {{"hb", 1}, {"_from", sender_name_}, {"ts", now_ns()}};
  std::string payload = hb.dump();
  send_udp(payload.data(), payload.size());
}

bool RemoteLogger::ensure_session_file()
{
  if (session_fp_) return true;
  if (cfg_.log_dir.empty()) return false;

  session_file_ = cfg_.log_dir + "/run_" + std::to_string(now_ns()) + ".rlog";
  session_fp_ = std::fopen(session_file_.c_str(), "wb");
  if (!session_fp_) {
    std::fprintf(stderr, "[RemoteLogger] Failed to open %s\n", session_file_.c_str());
    session_file_.clear();
    return false;
  }

  std::setvbuf(session_fp_, nullptr, _IOFBF, kSessionIoBuf);
  uint32_t magic = kFileMagicV2;
  if (std::fwrite(&magic, sizeof(magic), 1, session_fp_) != 1) {
    std::fprintf(stderr, "[RemoteLogger] Failed to write magic to %s\n", session_file_.c_str());
    close_session_file_unlocked();
    session_file_.clear();
    return false;
  }
  std::fprintf(stderr, "[RemoteLogger] local session %s\n", session_file_.c_str());
  return true;
}

void RemoteLogger::close_session_file_unlocked()
{
  if (!session_fp_) return;
  std::fflush(session_fp_);
  std::fclose(session_fp_);
  session_fp_ = nullptr;
}

void RemoteLogger::close_session_file()
{
  std::lock_guard<std::mutex> lock(session_mtx_);
  close_session_file_unlocked();
}

void RemoteLogger::maybe_flush_session(bool force)
{
  if (!session_fp_) return;
  auto now = std::chrono::steady_clock::now();
  if (!force && last_session_flush_.time_since_epoch().count() != 0) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_session_flush_);
    if (ms.count() < static_cast<int64_t>(kSessionFlushMs)) return;
  }
  std::fflush(session_fp_);
  last_session_flush_ = now;
}

void RemoteLogger::flush_var_local(const std::vector<VarEntry> & entries)
{
  if (entries.empty()) return;

  std::lock_guard<std::mutex> lock(session_mtx_);
  if (!ensure_session_file()) return;

  for (const auto & e : entries) {
    uint8_t type = kRecJson;
    uint32_t len = static_cast<uint32_t>(e.json_str.size());
    if (std::fwrite(&type, sizeof(type), 1, session_fp_) != 1 ||
        std::fwrite(&e.ts, sizeof(e.ts), 1, session_fp_) != 1 ||
        std::fwrite(&len, sizeof(len), 1, session_fp_) != 1 ||
        std::fwrite(e.json_str.data(), 1, len, session_fp_) != len) {
      std::fprintf(stderr, "[RemoteLogger] Failed to write var record\n");
      return;
    }
  }
  maybe_flush_session(false);
}

void RemoteLogger::flush_img_local(uint64_t ts, const nlohmann::json & meta,
                                   const std::vector<uint8_t> & jpeg)
{
  std::lock_guard<std::mutex> lock(session_mtx_);
  if (!ensure_session_file()) return;

  nlohmann::json jmeta = meta;
  inject_sender(jmeta);
  if (!jmeta.contains("ts")) jmeta["ts"] = ts;
  std::string meta_str = jmeta.dump();

  uint8_t type = kRecImg;
  uint32_t meta_len = static_cast<uint32_t>(meta_str.size());
  uint32_t jpg_len = static_cast<uint32_t>(jpeg.size());
  if (std::fwrite(&type, sizeof(type), 1, session_fp_) != 1 ||
      std::fwrite(&ts, sizeof(ts), 1, session_fp_) != 1 ||
      std::fwrite(&meta_len, sizeof(meta_len), 1, session_fp_) != 1 ||
      std::fwrite(meta_str.data(), 1, meta_len, session_fp_) != meta_len ||
      std::fwrite(&jpg_len, sizeof(jpg_len), 1, session_fp_) != 1 ||
      std::fwrite(jpeg.data(), 1, jpg_len, session_fp_) != jpg_len) {
    std::fprintf(stderr, "[RemoteLogger] Failed to write img record\n");
    return;
  }
  maybe_flush_session(false);
}

void RemoteLogger::try_send_var(const VarEntry & entry)
{
  try {
    auto j = nlohmann::json::parse(entry.json_str);
    if (!j.contains("ts")) j["ts"] = entry.ts;
    inject_sender(j);
    std::string payload = j.dump();
    send_udp(payload.data(), payload.size());
  } catch (...) {
    std::string payload = "{\"ts\":" + std::to_string(entry.ts) + ",\"_from\":\"" +
                          sender_name_ + "\",\"raw\":\"" + entry.json_str + "\"}";
    send_udp(payload.data(), payload.size());
  }
}

void RemoteLogger::try_send_img(const std::vector<uint8_t> & jpeg, uint64_t ts,
                                const nlohmann::json & meta)
{
  auto jmeta = meta;
  inject_sender(jmeta);
  std::string meta_str = jmeta.dump();
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
  if (sock_ < 0 || !registered_) return;
  std::lock_guard<std::mutex> lock(remote_mtx_);
  ::sendto(sock_, data, len, 0, reinterpret_cast<sockaddr *>(&addr_), sizeof(addr_));
}

std::string RemoteLogger::resolve_sender() const
{
  if (!cfg_.sender_name.empty()) return cfg_.sender_name;
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "dev_%04x", static_cast<uint32_t>(ns & 0xFFFF));
  return buf;
}

void RemoteLogger::inject_sender(nlohmann::json & j) const
{
  if (!j.contains("_from")) j["_from"] = sender_name_;
}

}  // namespace tools
