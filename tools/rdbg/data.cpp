#include "data.hpp"

#include "clock.hpp"
#include "control.hpp"
#include "proto.hpp"

#include <cstdio>

namespace tools
{
namespace rdbg
{

void DataPlane::inject(nlohmann::json & j) const
{
  if (!j.contains("_from")) j["_from"] = sender_;
}

void DataPlane::send_json(nlohmann::json j)
{
  inject(j);
  auto payload = j.dump();
  ctrl_.send_to_head(payload.data(), payload.size());
}

void DataPlane::send_raw_json(uint64_t ts, const std::string & json_str)
{
  try {
    auto j = nlohmann::json::parse(json_str);
    if (!j.contains("ts")) j["ts"] = ts;
    send_json(std::move(j));
  } catch (...) {
    std::string payload = "{\"ts\":" + std::to_string(ts) + ",\"_from\":\"" +
                          sender_ + "\",\"raw\":\"" + json_str + "\"}";
    ctrl_.send_to_head(payload.data(), payload.size());
  }
}

void DataPlane::send_image(const std::vector<uint8_t> & jpeg, uint64_t ts,
                           const nlohmann::json & meta)
{
  auto jmeta = meta;
  inject(jmeta);
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
  ctrl_.send_to_head(pkt.data(), pkt.size());
}

void DataPlane::send_heartbeat()
{
  nlohmann::json hb = {{"hb", 1}, {"_from", sender_}, {"ts", now_ns()}};
  auto payload = hb.dump();
  ctrl_.send_to_head(payload.data(), payload.size());
}

}  // namespace rdbg
}  // namespace tools
