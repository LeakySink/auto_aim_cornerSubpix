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

  bool start();
  void stop();
  void poll();
  void maybe_beacon();
  void check_head_timeout();

  bool has_head() const { return has_head_.load(); }
  bool send_to_head(const void * data, size_t len);

  // host → 车：标定网页按钮（仅已入队 host）。返回 false 表示队列空。
  bool poll_calib_cmd(std::string & cmd);

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
  void poll_ctrl();
  void poll_beacon();

  UdpSocket ctrl_;
  UdpSocket beacon_;
  std::mutex mtx_;
  std::atomic<bool> has_head_{false};
  std::vector<HostSlot> queue_;
  std::deque<std::string> calib_cmds_;
  sockaddr_in head_addr_{};
  std::chrono::steady_clock::time_point last_beacon_{};
  std::chrono::steady_clock::time_point last_alive_{};
};

}  // namespace rdbg
}  // namespace tools

#endif
