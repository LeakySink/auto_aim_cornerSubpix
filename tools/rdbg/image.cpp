#include "image.hpp"

#include "proto.hpp"

#include <chrono>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace tools
{
namespace rdbg
{

std::string stream_name(const nlohmann::json & meta)
{
  if (meta.contains("name") && meta["name"].is_string()) {
    auto s = meta["name"].get<std::string>();
    if (!s.empty()) return s;
  }
  return "default";
}

void FpsGate::reset()
{
  std::lock_guard<std::mutex> lock(mtx_);
  due_ns_.clear();
}

bool FpsGate::select(uint64_t ts, const std::string & name)
{
  return select(ts, name, kImgSaveFps);
}

bool FpsGate::select(uint64_t ts, const std::string & name, int fps)
{
  if (fps < 1) fps = 1;
  const uint64_t period = 1000000000ULL / static_cast<uint64_t>(fps);
  std::lock_guard<std::mutex> lock(mtx_);
  uint64_t & due = due_ns_[name];
  if (due != 0 && ts < due) return false;
  due = (due == 0) ? ts + period
                   : due + ((ts - due) / period + 1) * period;
  return true;
}

void Mailbox::reset() { clear(); }

void Mailbox::publish(uint64_t ts, nlohmann::json meta, const cv::Mat & img)
{
  const auto name = stream_name(meta);
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

bool Mailbox::take(ImgFrame & out, int timeout_ms, const std::atomic<bool> & running)
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

void Mailbox::wake() { cv_.notify_all(); }

void Mailbox::clear()
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

bool encode_jpeg(const cv::Mat & img, int width, int quality, std::vector<uint8_t> & jpeg)
{
  if (img.empty() || img.cols <= 0) return false;
  cv::Mat work;
  const cv::Mat * src = &img;
  if (width > 0 && img.cols > width) {
    double scale = static_cast<double>(width) / img.cols;
    int new_h = static_cast<int>(img.rows * scale);
    if (new_h < 1) new_h = 1;
    cv::resize(img, work, cv::Size(width, new_h));
    src = &work;
  }
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
  if (!cv::imencode(".jpg", *src, jpeg, params) || jpeg.empty()) return false;
  return true;
}

}  // namespace rdbg
}  // namespace tools
