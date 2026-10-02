#include "data.hpp"

#include "clock.hpp"
#include "control.hpp"
#include "proto.hpp"

#include <cstdio>
#include <cstring>
#include <sys/uio.h>

namespace tools
{
namespace rdbg
{

namespace
{

void put_le16(uint8_t * p, uint16_t v)
{
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void put_le32(uint8_t * p, uint32_t v)
{
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void put_le64(uint8_t * p, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
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

void DataPlane::send_raw_json(const std::string & json_str)
{
  if (json_str.empty()) return;
  ctrl_.send_to_head(json_str.data(), json_str.size());
}

void DataPlane::send_image(const std::vector<uint8_t> & jpeg, uint64_t ts,
                           const std::string & meta_str)
{
  (void)try_send_image(jpeg, ts, meta_str);
}

bool DataPlane::try_send_image(const std::vector<uint8_t> & jpeg, uint64_t ts,
                               const std::string & meta_str)
{
  if (meta_str.size() > 0xffff) {
    std::fprintf(stderr, "[RemoteLogger] image meta too large, skipped\n");
    return false;
  }
  if (sender_.size() > 255) {
    std::fprintf(stderr, "[RemoteLogger] sender name too long, skipped\n");
    return false;
  }

  const uint16_t frame_seq = frame_seq_.fetch_add(1);
  const size_t from_len = sender_.size();
  const size_t common = 1 + 8 + 2 + 2 + 2 + 1 + from_len;
  const size_t frag0_hdr = common + 2 + meta_str.size() + 4;
  if (frag0_hdr >= kUdpSafePayload) {
    std::fprintf(stderr, "[RemoteLogger] image header too large, skipped\n");
    return false;
  }

  const size_t frag0_cap = kUdpSafePayload - frag0_hdr;
  const size_t frag_n_cap = kUdpSafePayload - common;
  if (frag_n_cap == 0) return false;

  const size_t jpg_size = jpeg.size();
  const size_t first_chunk = jpg_size < frag0_cap ? jpg_size : frag0_cap;
  const size_t left_after0 = jpg_size - first_chunk;
  const size_t extra =
    left_after0 == 0 ? 0 : (left_after0 + frag_n_cap - 1) / frag_n_cap;
  if (1 + extra > 0xffff) {
    std::fprintf(stderr, "[RemoteLogger] image too many fragments, skipped\n");
    return false;
  }
  const uint16_t frag_cnt = static_cast<uint16_t>(1 + extra);

  alignas(8) uint8_t hdr[kUdpSafePayload];
  size_t offset = 0;
  for (uint16_t idx = 0; idx < frag_cnt; ++idx) {
    size_t h = 0;
    hdr[h++] = kImgFragMarker;
    put_le64(hdr + h, ts);
    h += 8;
    put_le16(hdr + h, frame_seq);
    h += 2;
    put_le16(hdr + h, idx);
    h += 2;
    put_le16(hdr + h, frag_cnt);
    h += 2;
    hdr[h++] = static_cast<uint8_t>(from_len);
    if (from_len) {
      std::memcpy(hdr + h, sender_.data(), from_len);
      h += from_len;
    }

    size_t cap = frag_n_cap;
    if (idx == 0) {
      put_le16(hdr + h, static_cast<uint16_t>(meta_str.size()));
      h += 2;
      if (!meta_str.empty()) {
        std::memcpy(hdr + h, meta_str.data(), meta_str.size());
        h += meta_str.size();
      }
      put_le32(hdr + h, static_cast<uint32_t>(jpg_size));
      h += 4;
      cap = frag0_cap;
    }

    size_t remain = jpg_size - offset;
    size_t n = remain < cap ? remain : cap;
    ::iovec iov[2];
    int iovcnt = 1;
    iov[0].iov_base = hdr;
    iov[0].iov_len = h;
    if (n > 0) {
      iov[1].iov_base =
        const_cast<uint8_t *>(jpeg.data() + offset);
      iov[1].iov_len = n;
      iovcnt = 2;
      offset += n;
    }
    if (!ctrl_.send_to_head(iov, iovcnt)) return false;
  }
  return true;
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
