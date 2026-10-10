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
  tx_fps_.reset();
  mailbox_.reset();
  tx_.reset();
  img_tx_ok_ = 0;
  last_tx_stats_ = {};
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

  {
    std::lock_guard<std::mutex> lock(disk_mtx_);
    disk_q_.clear();
  }

  control_.control_port = cfg_.control_port;
  control_.beacon_port = cfg_.beacon_port;
  control_.beacon_interval_ms = cfg_.beacon_interval_ms;
  control_.head_timeout_ms = cfg_.head_timeout_ms;
  control_.sender_name = cfg_.sender_name;
  control_.app = cfg_.app.empty() ? "normal" : cfg_.app;
  data_.set_sender(cfg_.sender_name);

  remote_ok_ = false;
  if (cfg_.enable_remote) remote_ok_ = control_.start();

  running_ = true;
  disk_open_ = cfg_.enable_local;
  if (disk_open_) disk_worker_ = std::thread(&Engine::disk_loop, this);
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

  // 生产者已停：等待落盘队列排空，再停 disk_worker 并强制 sync。
  {
    std::unique_lock<std::mutex> lock(disk_mtx_);
    disk_cv_.notify_all();
    if (disk_open_.load()) {
      disk_idle_cv_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return disk_q_.empty(); });
    }
  }
  disk_open_ = false;
  disk_cv_.notify_all();
  if (disk_worker_.joinable()) disk_worker_.join();
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
  uint8_t prio = 1;
  if (j.contains("level") && j["level"].is_string())
    prio = level_prio(j["level"].get<std::string>());
  {
    std::lock_guard<std::mutex> lock(var_mtx_);
    if (var_buf_.size() >= cfg_.var_buffer_size) var_buf_.erase(var_buf_.begin());
    var_buf_.push_back({ts, j.dump(), prio});
  }
  var_cv_.notify_one();
}

uint8_t Engine::level_prio(const std::string & level)
{
  if (level == "ERROR" || level == "error" || level == "FATAL" || level == "fatal")
    return 3;
  if (level == "WARN" || level == "warn" || level == "WARNING" || level == "warning")
    return 2;
  return 1;
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

bool Engine::poll_calib_cmd(std::string & cmd) { return control_.poll_calib_cmd(cmd); }

bool Engine::poll_json(nlohmann::json & data) { return control_.poll_json(data); }

void Engine::set_json_handler(ControlPlane::JsonHandler handler)
{
  control_.set_json_handler(std::move(handler));
}

void Engine::apply_tx_cap(const TxCap & cap)
{
  control_.set_host_tx_cap(cap);
  tx_.set_host_cap(cap);
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

void Engine::maybe_plot_tx_stats()
{
  auto now = std::chrono::steady_clock::now();
  if (last_tx_stats_.time_since_epoch().count() != 0) {
    auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_stats_);
    if (ms.count() < 1000) return;
  }
  last_tx_stats_ = now;
  const uint32_t ok = img_tx_ok_.exchange(0);
  const int level = tx_.level();
  // 约 1Hz：远程出图帧率与档位（Watch 可画曲线）。
  plot({{"img_tx_fps", static_cast<double>(ok)},
        {"img_tx_level", level}});
}

void Engine::var_loop()
{
  std::vector<VarEntry> pending;
  auto flush = [&]() {
    if (pending.empty()) return;
    std::vector<std::pair<uint64_t, std::string>> rec;
    rec.reserve(pending.size());
    uint8_t prio = 1;
    for (auto & e : pending) {
      if (e.prio > prio) prio = e.prio;
      rec.emplace_back(e.ts, std::move(e.json_str));
    }
    pending.clear();
    // 落盘优先：弱网 UDP 失败不得拖住本地 .rlog。
    if (cfg_.enable_local) {
      DiskJob job;
      job.prio = prio;
      job.urgent = (prio >= 3);
      job.jsons = rec;
      enqueue_disk(std::move(job));
    }
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
      maybe_plot_tx_stats();
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

    // Host 上限与自适应档同步（subscribe 可能刚更新）。
    tx_.set_host_cap(control_.host_tx_cap());
    const TxProfile txp = tx_.effective(cfg_.img_width, cfg_.img_quality);

    nlohmann::json meta = std::move(entry.meta);
    data_.inject(meta);
    if (!meta.contains("ts")) meta["ts"] = entry.ts;
    const cv::Mat mat = std::move(entry.img);

    std::vector<uint8_t> local_jpeg;
    bool have_local = false;
    if (cfg_.enable_local) {
      have_local =
        encode_jpeg(mat, cfg_.img_width, cfg_.img_quality, local_jpeg);
      if (have_local) {
        DiskJob job;
        job.image = true;
        job.prio = 0;
        job.ts = entry.ts;
        job.meta = meta.dump();
        job.jpeg = local_jpeg;
        enqueue_disk(std::move(job));
      }
    }

    if (want_remote) {
      const std::string tx_key = name + "#tx";
      if (tx_fps_.select(entry.ts, tx_key, txp.fps)) {
        const bool same_as_local =
          have_local && txp.width == cfg_.img_width &&
          txp.quality == cfg_.img_quality;
        std::vector<uint8_t> tx_jpeg;
        bool have_tx = false;
        if (same_as_local) {
          tx_jpeg = std::move(local_jpeg);
          have_tx = true;
        } else {
          have_tx = encode_jpeg(mat, txp.width, txp.quality, tx_jpeg);
        }
        if (have_tx) {
          nlohmann::json tx_meta = meta;
          tx_meta["img_tx_level"] = txp.level;
          tx_meta["img_tx_w"] = txp.width;
          tx_meta["img_tx_q"] = txp.quality;
          const bool ok =
            data_.try_send_image(tx_jpeg, entry.ts, tx_meta.dump());
          tx_.note_send(ok);
          if (ok) img_tx_ok_.fetch_add(1);
        }
      }
    }
    maybe_plot_tx_stats();
    entry.meta = nlohmann::json{};
    entry.img.release();
  }
  mailbox_.clear();
}

void Engine::enqueue_disk(DiskJob job)
{
  if (!disk_open_) return;
  {
    std::lock_guard<std::mutex> lock(disk_mtx_);
    if (!disk_open_) return;
    while (disk_q_.size() >= kDiskQueueMax) {
      // 丢最低优先级；同级丢最旧。图像(0) < normal(1) < warn(2) < error(3)
      auto victim = disk_q_.begin();
      for (auto it = disk_q_.begin(); it != disk_q_.end(); ++it) {
        if (it->prio < victim->prio) victim = it;
        if (victim->prio == 0) break;
      }
      disk_q_.erase(victim);
    }
    disk_q_.push_back(std::move(job));
  }
  disk_cv_.notify_one();
}

void Engine::disk_loop()
{
  auto last_sync = std::chrono::steady_clock::now();
  while (true) {
    DiskJob job;
    {
      std::unique_lock<std::mutex> lock(disk_mtx_);
      disk_cv_.wait_for(lock, std::chrono::milliseconds(kDiskSyncIntervalMs),
                        [&] { return !disk_q_.empty() || !disk_open_.load(); });
      if (disk_q_.empty()) {
        disk_idle_cv_.notify_all();
        if (!disk_open_.load()) break;
        // 空闲也按周期 sync，避免长时间无写时残留页缓存。
        auto now = std::chrono::steady_clock::now();
        auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync);
        if (ms.count() >= static_cast<int64_t>(kDiskSyncIntervalMs)) {
          lock.unlock();
          session_.sync(true);
          last_sync = now;
        }
        continue;
      }
      job = std::move(disk_q_.front());
      disk_q_.pop_front();
      if (disk_q_.empty()) disk_idle_cv_.notify_all();
    }
    if (job.image) session_.write_image(job.ts, job.meta, job.jpeg);
    else if (!job.jsons.empty()) session_.write_jsons(job.jsons);
    if (job.urgent) {
      session_.sync(true);
      last_sync = std::chrono::steady_clock::now();
    } else {
      auto now = std::chrono::steady_clock::now();
      auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync);
      if (ms.count() >= static_cast<int64_t>(kDiskSyncIntervalMs)) {
        session_.sync(true);
        last_sync = now;
      }
    }
  }
  session_.sync(true);
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
