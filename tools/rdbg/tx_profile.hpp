#ifndef TOOLS_RDBG_TX_PROFILE_HPP
#define TOOLS_RDBG_TX_PROFILE_HPP

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace tools
{
namespace rdbg
{

// 远程 JPEG 档位：只影响 UDP，本地 .rlog 仍用 yaml。
struct TxProfile
{
  int width{640};
  int quality{50};
  int fps{30};
  int level{0};
};

struct TxCap
{
  int max_width{0};    // 0 = 不限制
  int max_quality{0};
  int max_fps{0};
  int max_level{-1};   // >=0 时限制最高档（数字越大画质越低）
};

inline TxProfile profile_for_level(int level, int yaml_w, int yaml_q)
{
  if (level < 0) level = 0;
  if (level > 3) level = 3;
  TxProfile p;
  p.level = level;
  p.width = yaml_w;
  p.quality = yaml_q;
  p.fps = 30;
  if (level >= 1) {
    if (p.width <= 0 || p.width > 480) p.width = 480;
    if (p.quality <= 0 || p.quality > 40) p.quality = 40;
    p.fps = 20;
  }
  if (level >= 2) {
    if (p.width <= 0 || p.width > 320) p.width = 320;
    if (p.quality <= 0 || p.quality > 30) p.quality = 30;
    p.fps = 15;
  }
  if (level >= 3) {
    if (p.width <= 0 || p.width > 320) p.width = 320;
    if (p.quality <= 0 || p.quality > 25) p.quality = 25;
    p.fps = 10;
  }
  return p;
}

inline void apply_cap(TxProfile & p, const TxCap & cap)
{
  if (cap.max_width > 0 && (p.width <= 0 || p.width > cap.max_width))
    p.width = cap.max_width;
  if (cap.max_quality > 0 && (p.quality <= 0 || p.quality > cap.max_quality))
    p.quality = cap.max_quality;
  if (cap.max_fps > 0 && p.fps > cap.max_fps) p.fps = cap.max_fps;
}

// 根据非阻塞发送成败自适应 level；Host 上限单独叠加。
class TxAdaptor
{
public:
  void reset()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    level_ = 0;
    host_cap_ = {};
    ok_ = 0;
    fail_ = 0;
    window_start_ = {};
    good_secs_ = 0;
  }

  void set_host_cap(TxCap cap)
  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_cap_ = cap;
  }

  TxCap host_cap() const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return host_cap_;
  }

  int level() const { return level_.load(); }

  TxProfile effective(int yaml_w, int yaml_q) const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    int lvl = level_.load();
    // max_level：Host 要求「至少」降到该档（数字越大越糊）。
    if (host_cap_.max_level >= 0 && lvl < host_cap_.max_level)
      lvl = host_cap_.max_level;
    TxProfile p = profile_for_level(lvl, yaml_w, yaml_q);
    apply_cap(p, host_cap_);
    return p;
  }

  void note_send(bool ok)
  {
    auto now = std::chrono::steady_clock::now();
    int new_level = -1;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (window_start_.time_since_epoch().count() == 0) window_start_ = now;
      if (ok) ++ok_;
      else ++fail_;
      auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_);
      if (ms.count() < 1000) return;
      const int total = ok_ + fail_;
      const int fails = fail_;
      ok_ = 0;
      fail_ = 0;
      window_start_ = now;
      if (total <= 0) return;
      // 失败占比高 → 降画质；接近零失败若干秒 → 慢恢复。
      if (fails * 5 >= total && fails > 0) {
        if (level_ < 3) {
          ++level_;
          good_secs_ = 0;
          new_level = level_;
        }
      } else if (fails == 0) {
        ++good_secs_;
        if (good_secs_ >= 3 && level_ > 0) {
          --level_;
          good_secs_ = 0;
          new_level = level_;
        }
      } else {
        good_secs_ = 0;
      }
    }
    if (new_level >= 0) {
      std::fprintf(stderr, "[RemoteLogger] tx_level -> %d\n", new_level);
    }
  }

private:
  mutable std::mutex mtx_;
  std::atomic<int> level_{0};
  TxCap host_cap_{};
  int ok_{0};
  int fail_{0};
  int good_secs_{0};
  std::chrono::steady_clock::time_point window_start_{};
};

}  // namespace rdbg
}  // namespace tools

#endif
