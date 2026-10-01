#include "control.hpp"

#include "clock.hpp"
#include "proto.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tools
{
namespace rdbg
{

bool ControlPlane::start()
{
  stop();
  has_head_ = false;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
    calib_cmds_.clear();
    inbound_json_.clear();
    img_subs_.clear();
    img_union_.clear();
    head_addr_ = sockaddr_in{};
    head_addr_.sin_family = AF_INET;
  }
  last_beacon_ = {};
  last_alive_ = {};

  if (control_port == 0) {
    std::fprintf(stderr, "[RemoteLogger] control_port required, remote disabled\n");
    return false;
  }
  if (!ctrl_.bind(control_port, true, false)) {
    std::fprintf(stderr, "[RemoteLogger] bind() control %u failed\n", control_port);
    return false;
  }
  if (!beacon_.bind(beacon_port, true, true)) {
    std::fprintf(stderr, "[RemoteLogger] bind() beacon %u failed, who disabled\n",
                 beacon_port);
  }
  std::fprintf(stderr, "[RemoteLogger] '%s' control :%u beacon :%u\n",
               sender_name.c_str(), control_port, beacon_port);
  return true;
}

void ControlPlane::stop()
{
  has_head_ = false;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
    calib_cmds_.clear();
    inbound_json_.clear();
    img_subs_.clear();
    img_union_.clear();
  }
  beacon_.close();
  ctrl_.close();
}

bool ControlPlane::send_to_head(const void * data, size_t len)
{
  if (!has_head_ || !ctrl_.valid()) return false;
  std::lock_guard<std::mutex> lock(mtx_);
  if (!has_head_) return false;
  return ctrl_.sendto(data, len, head_addr_);
}

bool ControlPlane::send_to_head(const struct iovec * iov, int iovcnt)
{
  if (!has_head_ || !ctrl_.valid()) return false;
  std::lock_guard<std::mutex> lock(mtx_);
  if (!has_head_) return false;
  return ctrl_.sendmsg(iov, iovcnt, head_addr_);
}

bool ControlPlane::image_subscribed(const std::string & stream) const
{
  std::lock_guard<std::mutex> lock(mtx_);
  return img_union_.count(stream) > 0;
}

bool ControlPlane::any_image_subscribed() const
{
  std::lock_guard<std::mutex> lock(mtx_);
  return !img_union_.empty();
}

void ControlPlane::clear_img_sub_locked(const std::string & host_id)
{
  img_subs_.erase(host_id);
  rebuild_img_union_locked();
}

void ControlPlane::rebuild_img_union_locked()
{
  img_union_.clear();
  for (const auto & kv : img_subs_) {
    for (const auto & s : kv.second) img_union_.insert(s);
  }
}

void ControlPlane::send_json(in_addr ip, uint16_t port, const nlohmann::json & j)
{
  auto payload = j.dump();
  ctrl_.sendto(payload.data(), payload.size(), ip, port);
}

nlohmann::json ControlPlane::make_beacon() const
{
  nlohmann::json j;
  j["v"] = 1;
  j["type"] = "beacon";
  j["name"] = sender_name;
  const std::string app_s = app.empty() ? "normal" : app;
  j["app"] = app_s;
  // 可选：门户直接按 feature 开页（calibrate / tfviz / watch …）；缺省由 host 从 app 映射
  if (app_s == "calibrate" || app_s == "tfviz") j["feature"] = app_s;
  else if (app_s == "normal") j["feature"] = "watch";
  j["ip"] = local_ipv4();
  j["control"] = control_port;
  j["ts"] = now_ns();
  return j;
}

void ControlPlane::send_beacon_to(const sockaddr_in & dest)
{
  auto payload = make_beacon().dump();
  if (beacon_.valid()) beacon_.sendto(payload.data(), payload.size(), dest);
  else ctrl_.sendto(payload.data(), payload.size(), dest);
}

void ControlPlane::maybe_beacon()
{
  auto now = std::chrono::steady_clock::now();
  uint32_t interval = beacon_interval_ms < 1 ? 1 : beacon_interval_ms;
  if (last_beacon_.time_since_epoch().count() > 0) {
    auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_beacon_);
    if (elapsed.count() < static_cast<int64_t>(interval)) return;
  }
  last_beacon_ = now;
  send_beacon_to(broadcast_addr(beacon_port));
}

nlohmann::json ControlPlane::queue_ids_locked() const
{
  nlohmann::json ids = nlohmann::json::array();
  for (const auto & h : queue_) ids.push_back(h.host_id);
  return ids;
}

HostSlot * ControlPlane::find_locked(const std::string & id)
{
  auto it = std::find_if(queue_.begin(), queue_.end(),
                         [&](const HostSlot & h) { return h.host_id == id; });
  if (it == queue_.end()) return nullptr;
  return &(*it);
}

void ControlPlane::apply_head_locked()
{
  if (queue_.empty()) {
    has_head_ = false;
    head_addr_ = sockaddr_in{};
    head_addr_.sin_family = AF_INET;
    return;
  }
  const auto & h = queue_.front();
  head_addr_.sin_family = AF_INET;
  head_addr_.sin_addr = h.ip;
  head_addr_.sin_port = htons(h.data_port);
  has_head_ = true;
  last_alive_ = std::chrono::steady_clock::now();
}

void ControlPlane::notify_head_change(const HostSlot & head,
                                      const std::vector<HostSlot> & rest)
{
  nlohmann::json ids = nlohmann::json::array();
  ids.push_back(head.host_id);
  for (const auto & h : rest) ids.push_back(h.host_id);

  nlohmann::json promote;
  promote["v"] = 1;
  promote["type"] = "promote";
  promote["robot"] = sender_name;
  promote["role"] = "head";
  promote["queue"] = ids;
  send_json(head.ip, head.peer_port, promote);

  nlohmann::json upd;
  upd["v"] = 1;
  upd["type"] = "queue_update";
  upd["robot"] = sender_name;
  upd["head"] = {
    {"host_id", head.host_id},
    {"ip", ip_string(head.ip)},
    {"peer_port", head.peer_port},
    {"data_port", head.data_port},
  };
  upd["queue"] = ids;
  for (const auto & h : rest) send_json(h.ip, h.peer_port, upd);
}

void ControlPlane::drop_head(const char * why)
{
  HostSlot new_head;
  std::vector<HostSlot> rest;
  bool has_new = false;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return;
    auto gone = queue_.front();
    queue_.erase(queue_.begin());
    clear_img_sub_locked(gone.host_id);
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
    std::fprintf(stderr, "[RemoteLogger] new head '%s' -> %s:%u\n",
                 new_head.host_id.c_str(), ip_string(new_head.ip).c_str(),
                 new_head.data_port);
    notify_head_change(new_head, rest);
  }
}

void ControlPlane::check_head_timeout()
{
  if (!has_head_) return;
  auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point ack_ts;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    ack_ts = last_alive_;
  }
  auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - ack_ts);
  auto limit = static_cast<int64_t>(head_timeout_ms);
  if (limit < 500) limit = 500;
  if (ack_ts.time_since_epoch().count() > 0 && silent.count() > limit) {
    drop_head("head_alive timeout");
  }
}

void ControlPlane::poll_ctrl()
{
  if (!ctrl_.valid()) return;
  char buf[2048];
  int icmp_n = 0;
  for (;;) {
    sockaddr_in from{};
    ssize_t n = ctrl_.recvfrom_nb(buf, sizeof(buf) - 1, &from);
    if (n > 0) {
      buf[n] = '\0';
      handle(buf, static_cast<size_t>(n), from);
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

void ControlPlane::poll_beacon()
{
  if (!beacon_.valid()) return;
  char buf[2048];
  for (;;) {
    sockaddr_in from{};
    ssize_t n = beacon_.recvfrom_nb(buf, sizeof(buf) - 1, &from);
    if (n <= 0) break;
    buf[n] = '\0';
    try {
      auto msg = nlohmann::json::parse(buf, buf + n);
      if (msg.value("type", "") == "who") send_beacon_to(from);
    } catch (...) {
    }
  }
}

void ControlPlane::poll()
{
  poll_ctrl();
  poll_beacon();
}

void ControlPlane::handle(const char * buf, size_t n, const sockaddr_in & from)
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
      nlohmann::json err = {{"v", 1},
                            {"type", "register_ack"},
                            {"status", "error"},
                            {"message", "host_id/data_port/peer_port required"}};
      send_json(from.sin_addr, peer_port ? peer_port : ntohs(from.sin_port), err);
      return;
    }

    bool is_head = false;
    nlohmann::json ids;
    HostSlot head_copy;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto * existing = find_locked(host_id);
      if (existing) {
        existing->name = msg.value("name", existing->name);
        existing->ip = from.sin_addr;
        existing->data_port = data_port;
        existing->peer_port = peer_port;
      } else {
        if (queue_.size() >= kMaxHosts) {
          nlohmann::json err = {{"v", 1},
                                {"type", "register_ack"},
                                {"status", "error"},
                                {"message", "host queue full"}};
          send_json(from.sin_addr, peer_port, err);
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
    ack["robot"] = sender_name;
    ack["queue"] = ids;
    if (is_head) {
      ack["role"] = "head";
    } else {
      ack["role"] = "follower";
      ack["head"] = {
        {"host_id", head_copy.host_id},
        {"ip", ip_string(head_copy.ip)},
        {"peer_port", head_copy.peer_port},
        {"data_port", head_copy.data_port},
      };
    }
    send_json(from.sin_addr, peer_port, ack);
    return;
  }

  if (type == "deregister") {
    bool was_head = false;
    HostSlot new_head;
    std::vector<HostSlot> rest;
    bool has_new = false;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto * existing = find_locked(host_id);
      if (!existing) {
        nlohmann::json ack = {{"v", 1}, {"type", "deregister_ack"}, {"status", "ok"}};
        uint16_t port = msg.value("peer_port", ntohs(from.sin_port));
        send_json(from.sin_addr, port, ack);
        return;
      }
      was_head = queue_.front().host_id == host_id;
      uint16_t reply_port = existing->peer_port;
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                  [&](const HostSlot & h) { return h.host_id == host_id; }),
                   queue_.end());
      clear_img_sub_locked(host_id);
      apply_head_locked();
      if (was_head && !queue_.empty()) {
        has_new = true;
        new_head = queue_.front();
        rest.assign(queue_.begin() + 1, queue_.end());
      }
      nlohmann::json ack = {{"v", 1}, {"type", "deregister_ack"}, {"status", "ok"}};
      send_json(from.sin_addr, reply_port, ack);
    }
    std::fprintf(stderr, "[RemoteLogger] host '%s' deregistered\n", host_id.c_str());
    if (has_new) notify_head_change(new_head, rest);
    return;
  }

  if (type == "head_alive") {
    std::lock_guard<std::mutex> lock(mtx_);
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
      std::lock_guard<std::mutex> lock(mtx_);
      auto * existing = find_locked(host_id);
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
    ack["robot"] = sender_name;
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
          {"ip", ip_string(head_copy.ip)},
          {"peer_port", head_copy.peer_port},
          {"data_port", head_copy.data_port},
        };
      }
    }
    send_json(from.sin_addr, reply_port, ack);
  }

  if (type == "calib_cmd") {
    const auto cmd = msg.value("cmd", "");
    if (cmd.empty() || host_id.empty()) return;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (!find_locked(host_id)) return;
      if (calib_cmds_.size() >= 32) calib_cmds_.pop_front();
      calib_cmds_.push_back(cmd);
    }
    return;
  }

  if (type == "json") {
    if (host_id.empty() || !msg.contains("data")) return;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (!find_locked(host_id)) return;
      if (inbound_json_.size() >= 64) inbound_json_.pop_front();
      inbound_json_.push_back(msg["data"]);
    }
    return;
  }

  if (type == "img_subscribe") {
    if (host_id.empty()) return;
    std::unordered_set<std::string> streams;
    if (msg.contains("streams") && msg["streams"].is_array()) {
      for (const auto & s : msg["streams"]) {
        if (s.is_string()) {
          auto name = s.get<std::string>();
          if (!name.empty()) streams.insert(std::move(name));
        }
      }
    }
    nlohmann::json ack = {{"v", 1}, {"type", "img_subscribe_ack"}, {"status", "ok"}};
    uint16_t reply_port = msg.value("peer_port", 0);
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto * existing = find_locked(host_id);
      if (!existing) {
        ack["status"] = "error";
        ack["message"] = "not in queue";
        if (!reply_port) reply_port = ntohs(from.sin_port);
        send_json(from.sin_addr, reply_port, ack);
        return;
      }
      if (!reply_port) reply_port = existing->peer_port;
      if (streams.empty()) img_subs_.erase(host_id);
      else img_subs_[host_id] = std::move(streams);
      rebuild_img_union_locked();
      ack["streams"] = nlohmann::json::array();
      for (const auto & s : img_union_) ack["streams"].push_back(s);
      std::fprintf(stderr, "[RemoteLogger] img_subscribe host=%s union=%zu\n",
                   host_id.c_str(), img_union_.size());
    }
    send_json(from.sin_addr, reply_port, ack);
  }
}

bool ControlPlane::poll_calib_cmd(std::string & cmd)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (calib_cmds_.empty()) return false;
  cmd = std::move(calib_cmds_.front());
  calib_cmds_.pop_front();
  return true;
}

bool ControlPlane::poll_json(nlohmann::json & data)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (inbound_json_.empty()) return false;
  data = std::move(inbound_json_.front());
  inbound_json_.pop_front();
  return true;
}

}  // namespace rdbg
}  // namespace tools
