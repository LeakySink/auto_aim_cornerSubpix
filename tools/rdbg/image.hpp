#ifndef TOOLS_RDBG_IMAGE_HPP
#define TOOLS_RDBG_IMAGE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace tools
{
namespace rdbg
{

struct ImgFrame
{
  uint64_t ts{0};
  nlohmann::json meta;
  cv::Mat img;
};

std::string stream_name(const nlohmann::json & meta);

// 按 name 相位锁 ~30fps，未入选无拷贝。
class FpsGate
{
public:
  void reset();
  bool select(uint64_t ts, const std::string & name);
  // fps<=0 时用默认 kImgSaveFps。
  bool select(uint64_t ts, const std::string & name, int fps);

private:
  std::mutex mtx_;
  std::unordered_map<std::string, uint64_t> due_ns_;
};

// 每路深度 1 邮箱。publish 必须 clone。
class Mailbox
{
public:
  void reset();
  void publish(uint64_t ts, nlohmann::json meta, const cv::Mat & img);
  bool take(ImgFrame & out, int timeout_ms, const std::atomic<bool> & running);
  void wake();
  void clear();

private:
  struct Slot
  {
    ImgFrame entry;
    bool has{false};
  };
  std::unordered_map<std::string, Slot> slots_;
  std::string rr_key_;
  std::mutex mtx_;
  std::condition_variable cv_;
};

bool encode_jpeg(const cv::Mat & img, int width, int quality, std::vector<uint8_t> & jpeg);

}  // namespace rdbg
}  // namespace tools

#endif
