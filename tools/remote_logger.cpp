#include "remote_logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
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
constexpr int kImgSaveFps = 30;
constexpr uint64_t kImgSavePeriodNs = 1000000000ULL / kImgSaveFps;
constexpr uint32_t kSessionFlushMs = 200;
constexpr int kVarWorkerPollMs = 50;
constexpr int kImgWorkerPollMs = 50;
constexpr int kCtrlWorkerPollMs = 200;  // 控制线程轮询间隔

std::string img_stream_name(const nlohmann::json & meta)
{
  if (meta.contains("name") && meta["name"].is_string()) {
    auto s = meta["name"].get<std::string>();
    if (!s.empty()) return s;
  }
  return "default";
}

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
  last_beacon_ts_ = {};
  last_ack_ts_ = {};
  last_hb_ = {};
  {
    std::lock_guard<std::mutex> lock(remote_mtx_);
    queue_.clear();
    addr_ = sockaddr_in{};
    addr_.sin_family = AF_INET;
  }
  {
    std::lock_guard<std::mutex> lock(img_due_mtx_);
    img_due_ns_.clear();
  }
  img_mbox_.reset();
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
      int yes = 1;
      ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      ::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
      sockaddr_in local{};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      local.sin_port = htons(cfg_.control_port);
      if (::bind(sock_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        ::close(sock_);
        sock_ = -1;
        cfg_.enable_remote = false;
        std::fprintf(stderr, "[RemoteLogger] bind() control %u failed\n",
                     cfg_.control_port);
      }
    }
  }

  if (cfg_.enable_remote && sock_ >= 0) {
    beacon_sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (beacon_sock_ >= 0) {
      int yes = 1;
      ::setsockopt(beacon_sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
      ::setsockopt(beacon_sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
      ::setsockopt(beacon_sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
      sockaddr_in baddr{};
      baddr.sin_family = AF_INET;
      baddr.sin_addr.s_addr = htonl(INADDR_ANY);
      baddr.sin_port = htons(cfg_.beacon_port);
      if (::bind(beacon_sock_, reinterpret_cast<sockaddr *>(&baddr), sizeof(baddr)) < 0) {
        std::fprintf(stderr, "[RemoteLogger] bind() beacon %u failed, who disabled\n",
                     cfg_.beacon_port);
        ::close(beacon_sock_);
        beacon_sock_ = -1;
      }
    }
  }

  running_ = true;
  if (cfg_.enable_remote && sock_ >= 0) {
    std::fprintf(stderr, "[RemoteLogger] '%s' control :%u beacon :%u\n",
                 sender_name_.c_str(), cfg_.control_port, cfg_.beacon_port);
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
  if (node["beacon_interval_ms"])
    cfg.beacon_interval_ms = node["beacon_interval_ms"].as<uint32_t>();
  if (node["head_timeout_ms"])
    cfg.head_timeout_ms = node["head_timeout_ms"].as<uint32_t>();
  if (node["beacon_port"])
    cfg.beacon_port = node["beacon_port"].as<uint16_t>();

  init(cfg);
}

void RemoteLogger::shutdown()
{
  if (!running_) return;

  running_ = false;
  var_cv_.notify_all();
  img_mbox_.wake();
  if (ctrl_worker_.joinable()) ctrl_worker_.join();

  registered_ = false;
  {
    std::lock_guard<std::mutex> lock(remote_mtx_);
    queue_.clear();
  }

  if (var_worker_.joinable()) var_worker_.join();
  if (img_worker_.joinable()) img_worker_.join();

  close_session_file();
  if (beacon_sock_ >= 0) {
    ::close(beacon_sock_);
    beacon_sock_ = -1;
  }
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

  if (!try_select_img(ts, img_stream_name(meta))) return;
  img_mbox_.publish(ts, meta, img);
}

bool RemoteLogger::try_select_img(uint64_t ts, const std::string & name)
{
  std::lock_guard<std::mutex> lock(img_due_mtx_);
  uint64_t & due = img_due_ns_[name];
  if (due != 0 && ts < due) return false;
  due = (due == 0) ? ts + kImgSavePeriodNs
                   : due + ((ts - due) / kImgSavePeriodNs + 1) * kImgSavePeriodNs;
  return true;
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

// ── ctrl_worker：beacon / host 队列 / 队首心跳（与 var/img 完全分离）──

void RemoteLogger::ctrl_worker_loop()
{
  send_beacon();

  while (running_) {
    poll_ctrl();
    poll_beacon();

    if (registered_) {
      auto now = std::chrono::steady_clock::now();
      std::chrono::steady_clock::time_point ack_ts;
      {
        std::lock_guard<std::mutex> lock(remote_mtx_);
        ack_ts = last_ack_ts_;
      }
      auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - ack_ts);
      auto limit = static_cast<int64_t>(cfg_.head_timeout_ms);
      if (limit < 500) limit = 500;
      if (ack_ts.time_since_epoch().count() > 0 && silent.count() > limit) {
        drop_head("head_alive timeout");
      }
    }

    send_beacon();
    if (registered_ && cfg_.heartbeat_interval_ms > 0) send_heartbeat();

    std::this_thread::sleep_for(std::chrono::milliseconds(kCtrlWorkerPollMs));
  }
}

// ── img_mbox：深度 1，在飞编码 + 等待最新 ────────────────────────────

void RemoteLogger::ImgMailbox::reset() { clear(); }

void RemoteLogger::ImgMailbox::publish(uint64_t ts, nlohmann::json meta,
                                       const cv::Mat & img)
{
  const auto name = img_stream_name(meta);
  // 必须深拷贝：Mat 赋值只拷 header。worker 异步 JPEG 时，调用方会改同一块
  // 像素（下一帧 camera.read、同帧多路 overlay、resize 显示），多路就会编成同一张图。
  cv::Mat owned = img.clone();
  {
    std::lock_guard<std::mutex> lock(mtx_);
    auto & slot = slots_[name];
    slot.entry.ts = ts;
    slot.entry.meta = std::move(meta);
    slot.entry.img = std::move(owned);
    slot.has = true;
  }
  cv_.notify_one();
}

bool RemoteLogger::ImgMailbox::take(ImgEntry & out, int timeout_ms,
                                    const std::atomic<bool> & running)
{
  std::unique_lock<std::mutex> lock(mtx_);
  cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    if (!running.load()) return true;
    for (const auto & kv : slots_) {
      if (kv.second.has) return true;
    }
    return false;
  });

  auto pick = slots_.end();
  auto start = slots_.find(rr_key_);
  if (start != slots_.end()) ++start;
  else start = slots_.begin();

  auto it = start;
  for (size_t n = 0; n < slots_.size(); ++n) {
    if (it == slots_.end()) it = slots_.begin();
    if (it->second.has) {
      pick = it;
      break;
    }
    ++it;
  }
  if (pick == slots_.end()) return false;

  out.ts = pick->second.entry.ts;
  out.meta = std::move(pick->second.entry.meta);
  out.img = std::move(pick->second.entry.img);
  pick->second.entry.meta = nlohmann::json{};
  pick->second.entry.img.release();
  pick->second.has = false;
  rr_key_ = pick->first;
  return true;
}

void RemoteLogger::ImgMailbox::wake() { cv_.notify_all(); }

void RemoteLogger::ImgMailbox::clear()
{
  std::lock_guard<std::mutex> lock(mtx_);
  for (auto & kv : slots_) {
    kv.second.entry.meta = nlohmann::json{};
    kv.second.entry.img.release();
    kv.second.has = false;
  }
  slots_.clear();
  rr_key_.clear();
}

// ── img_worker：邮箱取帧 → resize 放全分辨率 → JPEG + 本地 + UDP ──

void RemoteLogger::img_worker_loop()
{
  ImgEntry entry;
  while (true) {
    if (!img_mbox_.take(entry, kImgWorkerPollMs, running_)) {
      if (!running_.load()) break;
      continue;
    }
    encode_and_dispatch_image(entry);
    entry.meta = nlohmann::json{};
    entry.img.release();
  }
  img_mbox_.clear();
}

bool RemoteLogger::encode_and_dispatch_image(ImgEntry & entry)
{
  if (entry.img.empty() || entry.img.cols <= 0) return false;

  cv::Mat work;
  if (cfg_.img_width > 0 && entry.img.cols > cfg_.img_width) {
    double scale = static_cast<double>(cfg_.img_width) / entry.img.cols;
    int new_h = static_cast<int>(entry.img.rows * scale);
    if (new_h < 1) new_h = 1;
    cv::resize(entry.img, work, cv::Size(cfg_.img_width, new_h));
    entry.img.release();
  } else {
    work = std::move(entry.img);
  }

  std::vector<uint8_t> jpeg;
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg_.img_quality};
  if (!cv::imencode(".jpg", work, jpeg, params) || jpeg.empty()) {
    return false;
  }
  work.release();

  if (cfg_.enable_local) flush_img_local(entry.ts, entry.meta, jpeg);
  if (cfg_.enable_remote && registered_) {
    try_send_img(jpeg, entry.ts, entry.meta);
  }
  return true;
}

void RemoteLogger::send_beacon()
{
  auto now = std::chrono::steady_clock::now();
  {
    uint32_t interval = cfg_.beacon_interval_ms;
    if (interval < 1) interval = 1;
    if (last_beacon_ts_.time_since_epoch().count() > 0) {
      auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_beacon_ts_);
      if (elapsed.count() < static_cast<int64_t>(interval)) return;
    }
    last_beacon_ts_ = now;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(cfg_.beacon_port);
  dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  send_beacon_to(dest);
}

void RemoteLogger::send_beacon_to(const sockaddr_in & dest)
{
  auto payload = make_beacon().dump();
  int fd = beacon_sock_ >= 0 ? beacon_sock_ : sock_;
  if (fd < 0) return;
  ::sendto(fd, payload.data(), payload.size(), 0,
           reinterpret_cast<const sockaddr *>(&dest), sizeof(dest));
}

nlohmann::json RemoteLogger::make_beacon() const
{
  nlohmann::json j;
  j["v"] = 1;
  j["type"] = "beacon";
  j["name"] = sender_name_;
  j["ip"] = local_ipv4();
  j["control"] = cfg_.control_port;
  j["ts"] = now_ns();
  return j;
}

std::string RemoteLogger::local_ipv4() const
{
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    ::inet_aton("8.8.8.8", &dest.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) == 0) {
      sockaddr_in src{};
      socklen_t n = sizeof(src);
      if (::getsockname(fd, reinterpret_cast<sockaddr *>(&src), &n) == 0) {
        ::close(fd);
        char buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf));
        if (buf[0] && std::strcmp(buf, "0.0.0.0") != 0) return buf;
      }
    }
    ::close(fd);
  }

  ifaddrs * ifa = nullptr;
  if (::getifaddrs(&ifa) != 0) return {};
  std::string out;
  for (ifaddrs * p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
    if (p->ifa_flags & IFF_LOOPBACK) continue;
    if (!(p->ifa_flags & IFF_UP)) continue;
    auto * in = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
    if (buf[0]) {
      out = buf;
      break;
    }
  }
  ::freeifaddrs(ifa);
  return out;
}

void RemoteLogger::send_json_to(in_addr ip, uint16_t port, const nlohmann::json & j)
{
  if (sock_ < 0) return;
  std::string payload = j.dump();
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr = ip;
  dest.sin_port = htons(port);
  ::sendto(sock_, payload.data(), payload.size(), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
}

nlohmann::json RemoteLogger::queue_ids_locked() const
{
  nlohmann::json ids = nlohmann::json::array();
  for (const auto & h : queue_) ids.push_back(h.host_id);
  return ids;
}

RemoteLogger::HostSlot * RemoteLogger::find_host_locked(const std::string & id)
{
  auto it = std::find_if(queue_.begin(), queue_.end(),
                         [&](const HostSlot & h) { return h.host_id == id; });
  if (it == queue_.end()) return nullptr;
  return &(*it);
}

void RemoteLogger::apply_head_locked()
{
  if (queue_.empty()) {
    registered_ = false;
    addr_ = sockaddr_in{};
    addr_.sin_family = AF_INET;
    return;
  }
  const auto & h = queue_.front();
  addr_.sin_family = AF_INET;
  addr_.sin_addr = h.ip;
  addr_.sin_port = htons(h.data_port);
  registered_ = true;
  last_ack_ts_ = std::chrono::steady_clock::now();
}

void RemoteLogger::notify_head_change(const HostSlot & head,
                                      const std::vector<HostSlot> & rest)
{
  nlohmann::json ids = nlohmann::json::array();
  ids.push_back(head.host_id);
  for (const auto & h : rest) ids.push_back(h.host_id);

  nlohmann::json promote;
  promote["v"] = 1;
  promote["type"] = "promote";
  promote["robot"] = sender_name_;
  promote["role"] = "head";
  promote["queue"] = ids;
  send_json_to(head.ip, head.peer_port, promote);

  nlohmann::json upd;
  upd["v"] = 1;
  upd["type"] = "queue_update";
  upd["robot"] = sender_name_;
  upd["head"] = {
    {"host_id", head.host_id},
    {"ip", inet_ntoa(head.ip)},
    {"peer_port", head.peer_port},
    {"data_port", head.data_port},
  };
  upd["queue"] = ids;
  for (const auto & h : rest) send_json_to(h.ip, h.peer_port, upd);
}

void RemoteLogger::drop_head(const char * why)
{
  HostSlot new_head;
  std::vector<HostSlot> rest;
  bool has_new = false;
  {
    std::lock_guard<std::mutex> lock(remote_mtx_);
    if (queue_.empty()) return;
    auto gone = queue_.front();
    queue_.erase(queue_.begin());
    std::fprintf(stderr, "[RemoteLogger] drop head '%s' (%s)\n",
                 gone.host_id.c_str(), why ? why : "");
    if (!queue_.empty()) {
      has_new = true;
      new_head = queue_.front();
      rest.assign(queue_.begin() + 1, queue_.end());
    }
    apply_head_locked();
  }
  if (has_new) {
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &new_head.ip, ip, sizeof(ip));
    std::fprintf(stderr, "[RemoteLogger] new head '%s' -> %s:%u\n",
                 new_head.host_id.c_str(), ip, new_head.data_port);
    notify_head_change(new_head, rest);
  }
}

void RemoteLogger::poll_ctrl()
{
  if (sock_ < 0) return;
  char buf[2048];
  int icmp_n = 0;
  for (;;) {
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = ::recvfrom(sock_, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr *>(&from), &flen);
    if (n > 0) {
      buf[n] = '\0';
      handle_ctrl_payload(buf, static_cast<size_t>(n), from);
      icmp_n = 0;
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == ECONNREFUSED || errno == ECONNRESET) {
      if (++icmp_n > 64) break;
      continue;
    }
    break;
  }
}

void RemoteLogger::poll_beacon()
{
  if (beacon_sock_ < 0) return;
  char buf[2048];
  for (;;) {
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = ::recvfrom(beacon_sock_, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr *>(&from), &flen);
    if (n <= 0) break;
    buf[n] = '\0';
    try {
      auto msg = nlohmann::json::parse(buf, buf + n);
      if (msg.value("type", "") == "who") send_beacon_to(from);
    } catch (...) {
    }
  }
}

void RemoteLogger::handle_ctrl_payload(const char * buf, size_t n, const sockaddr_in & from)
{
  nlohmann::json msg;
  try {
    msg = nlohmann::json::parse(buf, buf + n);
  } catch (...) {
    return;
  }
  const auto type = msg.value("type", "");
  const auto host_id = msg.value("host_id", "");

  if (type == "who") {
    send_beacon_to(from);
    return;
  }

  if (type == "register") {
    uint16_t data_port = msg.value("data_port", 0);
    uint16_t peer_port = msg.value("peer_port", 0);
    if (host_id.empty() || data_port == 0 || peer_port == 0) {
      nlohmann::json err;
      err["v"] = 1;
      err["type"] = "register_ack";
      err["status"] = "error";
      err["message"] = "host_id/data_port/peer_port required";
      send_json_to(from.sin_addr, peer_port ? peer_port : ntohs(from.sin_port), err);
      return;
    }

    bool is_head = false;
    nlohmann::json ids;
    HostSlot head_copy;
    {
      std::lock_guard<std::mutex> lock(remote_mtx_);
      auto * existing = find_host_locked(host_id);
      if (existing) {
        existing->name = msg.value("name", existing->name);
        existing->ip = from.sin_addr;
        existing->data_port = data_port;
        existing->peer_port = peer_port;
      } else {
        if (queue_.size() >= 32) {
          nlohmann::json err;
          err["v"] = 1;
          err["type"] = "register_ack";
          err["status"] = "error";
          err["message"] = "host queue full";
          send_json_to(from.sin_addr, peer_port, err);
          return;
        }
        HostSlot slot;
        slot.host_id = host_id;
        slot.name = msg.value("name", "");
        slot.ip = from.sin_addr;
        slot.data_port = data_port;
        slot.peer_port = peer_port;
        queue_.push_back(std::move(slot));
        std::fprintf(stderr, "[RemoteLogger] host '%s' queued (%zu)\n",
                     host_id.c_str(), queue_.size());
      }
      apply_head_locked();
      is_head = !queue_.empty() && queue_.front().host_id == host_id;
      ids = queue_ids_locked();
      if (!queue_.empty()) head_copy = queue_.front();
    }

    nlohmann::json ack;
    ack["v"] = 1;
    ack["type"] = "register_ack";
    ack["status"] = "ok";
    ack["robot"] = sender_name_;
    ack["queue"] = ids;
    if (is_head) {
      ack["role"] = "head";
    } else {
      ack["role"] = "follower";
      ack["head"] = {
        {"host_id", head_copy.host_id},
        {"ip", inet_ntoa(head_copy.ip)},
        {"peer_port", head_copy.peer_port},
        {"data_port", head_copy.data_port},
      };
    }
    send_json_to(from.sin_addr, peer_port, ack);
    return;
  }

  if (type == "deregister") {
    bool was_head = false;
    HostSlot new_head;
    std::vector<HostSlot> rest;
    bool has_new = false;
    {
      std::lock_guard<std::mutex> lock(remote_mtx_);
      auto * existing = find_host_locked(host_id);
      if (!existing) {
        nlohmann::json ack;
        ack["v"] = 1;
        ack["type"] = "deregister_ack";
        ack["status"] = "ok";
        uint16_t port = msg.value("peer_port", ntohs(from.sin_port));
        send_json_to(from.sin_addr, port, ack);
        return;
      }
      was_head = queue_.front().host_id == host_id;
      uint16_t reply_port = existing->peer_port;
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                  [&](const HostSlot & h) { return h.host_id == host_id; }),
                   queue_.end());
      apply_head_locked();
      if (was_head && !queue_.empty()) {
        has_new = true;
        new_head = queue_.front();
        rest.assign(queue_.begin() + 1, queue_.end());
      }
      nlohmann::json ack;
      ack["v"] = 1;
      ack["type"] = "deregister_ack";
      ack["status"] = "ok";
      send_json_to(from.sin_addr, reply_port, ack);
    }
    std::fprintf(stderr, "[RemoteLogger] host '%s' deregistered\n", host_id.c_str());
    if (has_new) notify_head_change(new_head, rest);
    return;
  }

  if (type == "head_alive") {
    std::lock_guard<std::mutex> lock(remote_mtx_);
    if (queue_.empty() || queue_.front().host_id != host_id) return;
    auto & h = queue_.front();
    h.ip = from.sin_addr;
    if (msg.contains("data_port")) h.data_port = msg.value("data_port", h.data_port);
    if (msg.contains("peer_port")) h.peer_port = msg.value("peer_port", h.peer_port);
    apply_head_locked();
    return;
  }

  if (type == "query_head") {
    bool in_queue = false;
    bool is_head = false;
    nlohmann::json ids;
    HostSlot head_copy;
    uint16_t reply_port = msg.value("peer_port", 0);
    {
      std::lock_guard<std::mutex> lock(remote_mtx_);
      auto * existing = find_host_locked(host_id);
      in_queue = existing != nullptr;
      if (existing && existing->peer_port) reply_port = existing->peer_port;
      ids = queue_ids_locked();
      if (!queue_.empty()) {
        head_copy = queue_.front();
        is_head = head_copy.host_id == host_id;
      }
    }
    if (!reply_port) reply_port = ntohs(from.sin_port);
    nlohmann::json ack;
    ack["v"] = 1;
    ack["type"] = "register_ack";
    ack["robot"] = sender_name_;
    ack["queue"] = ids;
    if (!in_queue) {
      ack["status"] = "error";
      ack["message"] = "not in queue";
    } else {
      ack["status"] = "ok";
      ack["role"] = is_head ? "head" : "follower";
      if (!is_head) {
        ack["head"] = {
          {"host_id", head_copy.host_id},
          {"ip", inet_ntoa(head_copy.ip)},
          {"peer_port", head_copy.peer_port},
          {"data_port", head_copy.data_port},
        };
      }
    }
    send_json_to(from.sin_addr, reply_port, ack);
  }
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
