#include "engine.hpp"

#include "clock.hpp"
#include "proto.hpp"

#include <cstdio>
#include <ctime>

namespace tools
{
namespace rdbg
{

std::string Engine::resolve_sender(const std::string & name)
{
  if (!name.empty()) return name;
  auto ns = now_ns();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "dev_%04x", static_cast<uint32_t>(ns & 0xFFFF));
  return buf;
}

void Engine::init(const RemoteLogger::Config & cfg)
{
  shutdown();
  cfg_ = cfg;
  cfg_.sender_name = resolve_sender(cfg_.sender_name);

  if (cfg_.enable_local) session_.open(cfg_.log_dir);
  else session_.close();

  fps_.reset();
  mailbox_.reset();
  last_hb_ = {};
  last_catalog_ = {};
  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    var_buf_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(stream_mtx_);
    known_streams_.clear();
  }

  control_.control_port = cfg_.control_port;
  control_.beacon_port = cfg_.beacon_port;
  control_.beacon_interval_ms = cfg_.beacon_interval_ms;
  control_.head_timeout_ms = cfg_.head_timeout_ms;
  control_.sender_name = cfg_.sender_name;
  data_.set_sender(cfg_.sender_name);

  remote_ok_ = false;
  if (cfg_.enable_remote) remote_ok_ = control_.start();

  running_ = true;
  if (remote_ok_) ctrl_worker_ = std::thread(&Engine::ctrl_loop, this);
  var_worker_ = std::thread(&Engine::var_loop, this);
  img_worker_ = std::thread(&Engine::img_loop, this);
}

void Engine::shutdown()
{
  if (!running_) return;
  running_ = false;
  var_cv_.notify_all();
  mailbox_.wake();
  if (ctrl_worker_.joinable()) ctrl_worker_.join();
  if (var_worker_.joinable()) var_worker_.join();
  if (img_worker_.joinable()) img_worker_.join();
  control_.stop();
  remote_ok_ = false;
  session_.close();
}

void Engine::plot(const nlohmann::json & data)
{
  if (!running_) return;
  uint64_t ts = now_ns();
  nlohmann::json j = data;
  if (j.contains("ts") && j["ts"].is_number()) ts = j["ts"].get<uint64_t>();
  else j["ts"] = ts;
  if (!j.contains("_from")) j["_from"] = cfg_.sender_name;
  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    if (var_buf_.size() >= cfg_.var_buffer_size) var_buf_.erase(var_buf_.begin());
    var_buf_.push_back({ts, j.dump()});
  }
  var_cv_.notify_one();
}

void Engine::log(const std::string & level, const std::string & msg)
{
  auto ts = now_ns();
  auto t = static_cast<time_t>(ts / 1000000000ULL);
  auto ms = (ts / 1000000ULL) % 1000;
  char tbuf[32];
  std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", std::localtime(&t));
  std::fprintf(stderr, "%s.%03lu [%s] %s\n", tbuf,
               static_cast<unsigned long>(ms), level.c_str(), msg.c_str());
  if (!running_) return;
  plot({{"level", level}, {"msg", msg}});
}

void Engine::plot_image(const cv::Mat & img, const nlohmann::json & meta)
{
  if (!running_) return;
  if (!cfg_.enable_local && !cfg_.enable_remote) return;
  if (img.empty()) return;
  const auto name = stream_name(meta);
  note_stream(name);

  const bool want_remote =
    cfg_.enable_remote && remote_ok_ && control_.image_subscribed(name);
  if (!cfg_.enable_local && !want_remote) return;

  uint64_t ts = now_ns();
  if (meta.contains("ts") && meta["ts"].is_number()) ts = meta["ts"].get<uint64_t>();
  if (!fps_.select(ts, name)) return;
  mailbox_.publish(ts, meta, img);
}

void Engine::note_stream(const std::string & name)
{
  if (name.empty()) return;
  std::lock_guard<std::mutex> lock(stream_mtx_);
  known_streams_.insert(name);
}

void Engine::maybe_send_catalog()
{
  if (!cfg_.enable_remote || !remote_ok_ || !control_.has_head()) return;
  auto now = std::chrono::steady_clock::now();
  if (last_catalog_.time_since_epoch().count() != 0) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_catalog_);
    if (ms.count() < static_cast<int64_t>(kImgCatalogIntervalMs)) return;
  }
  std::vector<std::string> streams;
  {
    std::lock_guard<std::mutex> lock(stream_mtx_);
    if (known_streams_.empty()) return;
    streams.assign(known_streams_.begin(), known_streams_.end());
  }
  last_catalog_ = now;
  data_.send_img_catalog(streams);
}

void Engine::var_loop()
{
  std::vector<VarEntry> pending;
  auto flush = [&]() {
    if (pending.empty()) return;
    std::vector<std::pair<uint64_t, std::string>> rec;
    rec.reserve(pending.size());
    for (auto & e : pending) rec.emplace_back(e.ts, std::move(e.json_str));
    pending.clear();
    if (cfg_.enable_local) session_.write_jsons(rec);
    if (cfg_.enable_remote && remote_ok_ && control_.has_head()) {
      for (const auto & e : rec) data_.send_raw_json(e.second);
    }
  };

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
    flush();
  }
  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    if (!var_buf_.empty()) var_buf_.swap(pending);
  }
  flush();
}

void Engine::img_loop()
{
  ImgFrame entry;
  while (true) {
    if (!mailbox_.take(entry, kImgWorkerPollMs, running_)) {
      if (!running_.load()) break;
      continue;
    }
    const auto name = stream_name(entry.meta);
    const bool want_remote =
      cfg_.enable_remote && remote_ok_ && control_.has_head() &&
      control_.image_subscribed(name);
    if (!cfg_.enable_local && !want_remote) {
      entry.meta = nlohmann::json{};
      entry.img.release();
      continue;
    }

    std::vector<uint8_t> jpeg;
    if (encode_jpeg(entry.img, cfg_.img_width, cfg_.img_quality, jpeg)) {
      nlohmann::json meta = std::move(entry.meta);
      data_.inject(meta);
      if (!meta.contains("ts")) meta["ts"] = entry.ts;
      const std::string meta_str = meta.dump();
      if (cfg_.enable_local) session_.write_image(entry.ts, meta_str, jpeg);
      if (want_remote) data_.send_image(jpeg, entry.ts, meta_str);
    }
    entry.meta = nlohmann::json{};
    entry.img.release();
  }
  mailbox_.clear();
}

void Engine::ctrl_loop()
{
  control_.maybe_beacon();
  while (running_) {
    control_.poll();
    control_.check_head_timeout();
    control_.maybe_beacon();
    maybe_send_catalog();
    if (control_.has_head() && cfg_.heartbeat_interval_ms > 0) {
      auto now = std::chrono::steady_clock::now();
      if (last_hb_.time_since_epoch().count() == 0) {
        last_hb_ = now;
      } else {
        auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hb_);
        if (elapsed.count() >= static_cast<int64_t>(cfg_.heartbeat_interval_ms)) {
          last_hb_ = now;
          data_.send_heartbeat();
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kCtrlWorkerPollMs));
  }
}

}  // namespace rdbg
}  // namespace tools
