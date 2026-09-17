#include "data.hpp"

#include "clock.hpp"
#include "control.hpp"
#include "proto.hpp"

#include <cstdio>
#include <cstddef>

namespace tools
{
namespace rdbg
{

namespace
{

void append_le16(std::vector<uint8_t> & out, uint16_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void append_le32(std::vector<uint8_t> & out, uint32_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void append_le64(std::vector<uint8_t> & out, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

}  // namespace

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
  if (meta_str.size() > 0xffff) {
    std::fprintf(stderr, "[RemoteLogger] image meta too large, skipped\n");
    return;
  }
  if (sender_.size() > 255) {
    std::fprintf(stderr, "[RemoteLogger] sender name too long, skipped\n");
    return;
  }

  const uint16_t frame_seq = frame_seq_.fetch_add(1);
  const size_t from_len = sender_.size();
  // Common: marker + ts + seq + idx + cnt + from_len + from
  const size_t common = 1 + 8 + 2 + 2 + 2 + 1 + from_len;
  // Frag0 extra: meta_len + meta + jpg_total
  const size_t frag0_hdr = common + 2 + meta_str.size() + 4;
  if (frag0_hdr >= kUdpSafePayload) {
    std::fprintf(stderr, "[RemoteLogger] image header too large, skipped\n");
    return;
  }

  const size_t frag0_cap = kUdpSafePayload - frag0_hdr;
  const size_t frag_n_cap = kUdpSafePayload - common;
  if (frag_n_cap == 0) return;

  const size_t jpg_size = jpeg.size();
  const size_t first_chunk = jpg_size < frag0_cap ? jpg_size : frag0_cap;
  const size_t left_after0 = jpg_size - first_chunk;
  const size_t extra =
    left_after0 == 0 ? 0 : (left_after0 + frag_n_cap - 1) / frag_n_cap;
  if (1 + extra > 0xffff) {
    std::fprintf(stderr, "[RemoteLogger] image too many fragments, skipped\n");
    return;
  }
  const uint16_t frag_cnt = static_cast<uint16_t>(1 + extra);

  size_t offset = 0;
  for (uint16_t idx = 0; idx < frag_cnt; ++idx) {
    std::vector<uint8_t> pkt;
    pkt.reserve(kUdpSafePayload);
    pkt.push_back(kImgFragMarker);
    append_le64(pkt, ts);
    append_le16(pkt, frame_seq);
    append_le16(pkt, idx);
    append_le16(pkt, frag_cnt);
    pkt.push_back(static_cast<uint8_t>(from_len));
    pkt.insert(pkt.end(), sender_.begin(), sender_.end());

    size_t cap = (idx == 0) ? frag0_cap : frag_n_cap;
    if (idx == 0) {
      append_le16(pkt, static_cast<uint16_t>(meta_str.size()));
      pkt.insert(pkt.end(), meta_str.begin(), meta_str.end());
      append_le32(pkt, static_cast<uint32_t>(jpg_size));
    }
    size_t remain = jpg_size - offset;
    size_t n = remain < cap ? remain : cap;
    if (n > 0) {
      pkt.insert(pkt.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(offset),
                 jpeg.begin() + static_cast<std::ptrdiff_t>(offset + n));
      offset += n;
    }
    if (!ctrl_.send_to_head(pkt.data(), pkt.size())) return;
  }
}

void DataPlane::send_heartbeat()
{
  nlohmann::json hb = {{"hb", 1}, {"_from", sender_}, {"ts", now_ns()}};
  auto payload = hb.dump();
  ctrl_.send_to_head(payload.data(), payload.size());
}

void DataPlane::send_img_catalog(const std::vector<std::string> & streams)
{
  nlohmann::json j;
  j["img_streams"] = streams;
  j["_from"] = sender_;
  j["ts"] = now_ns();
  auto payload = j.dump();
  ctrl_.send_to_head(payload.data(), payload.size());
}

}  // namespace rdbg
}  // namespace tools
