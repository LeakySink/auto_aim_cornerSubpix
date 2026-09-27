#ifndef TOOLS_RDBG_CONTROL_HPP
#define TOOLS_RDBG_CONTROL_HPP

#include "transport.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tools
{
namespace rdbg
{

struct HostSlot
{
  std::string host_id;
  std::string name;
  in_addr ip{};
  uint16_t data_port{0};
  uint16_t peer_port{0};
};

// L1：发现 + host 队列。只通过 UdpSocket 收发控制 JSON。
class ControlPlane
{
public:
  uint16_t control_port{15000};
  uint16_t beacon_port{15999};
  uint32_t beacon_interval_ms{1000};
  uint32_t head_timeout_ms{2000};
  std::string sender_name;
  std::string app{"normal"};  // normal | calibrate

  bool start();
  void stop();
  void poll();
  void maybe_beacon();
  void check_head_timeout();

  bool has_head() const { return has_head_.load(); }
  bool send_to_head(const void * data, size_t len);
  bool send_to_head(const struct iovec * iov, int iovcnt);

  // 任一台已入队 host 订阅的图像流并集；无人订阅则不发图 UDP。
  bool image_subscribed(const std::string & stream) const;
  bool any_image_subscribed() const;

  // host → 车：标定网页按钮（兼容旧协议；新代码优先用 poll_json）
  bool poll_calib_cmd(std::string & cmd);
  // host → 车：通用 JSON（type=json 的 data 字段）
  bool poll_json(nlohmann::json & data);

private:
  void send_beacon_to(const sockaddr_in & dest);
  nlohmann::json make_beacon() const;
  void send_json(in_addr ip, uint16_t port, const nlohmann::json & j);
  void handle(const char * buf, size_t n, const sockaddr_in & from);
  void apply_head_locked();
  void notify_head_change(const HostSlot & head, const std::vector<HostSlot> & rest);
  void drop_head(const char * why);
  nlohmann::json queue_ids_locked() const;
  HostSlot * find_locked(const std::string & id);
  void clear_img_sub_locked(const std::string & host_id);
  void rebuild_img_union_locked();
  void poll_ctrl();
  void poll_beacon();

  UdpSocket ctrl_;
  UdpSocket beacon_;
  mutable std::mutex mtx_;
  std::atomic<bool> has_head_{false};
  std::vector<HostSlot> queue_;
  std::deque<std::string> calib_cmds_;
  std::deque<nlohmann::json> inbound_json_;
  sockaddr_in head_addr_{};
  std::chrono::steady_clock::time_point last_beacon_{};
  std::chrono::steady_clock::time_point last_alive_{};

  std::unordered_map<std::string, std::unordered_set<std::string>> img_subs_;
  std::unordered_set<std::string> img_union_;
};

}  // namespace rdbg
}  // namespace tools

#endif
