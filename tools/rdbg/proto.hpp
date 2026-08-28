#ifndef TOOLS_RDBG_PROTO_HPP
#define TOOLS_RDBG_PROTO_HPP

#include <cstdint>
#include <cstddef>

namespace tools
{
namespace rdbg
{

constexpr uint32_t kFileMagicV2 = 0x32474C52;  // "RLG2"
constexpr uint8_t kRecJson = 0x00;
constexpr uint8_t kRecImg = 0x01;
constexpr uint8_t kImgMarker = 0xFF;
constexpr size_t kMaxUdpPayload = 60000;
constexpr size_t kSessionIoBuf = 256 * 1024;
constexpr int kImgSaveFps = 30;
constexpr uint64_t kImgSavePeriodNs = 1000000000ULL / kImgSaveFps;
constexpr uint32_t kSessionFlushMs = 200;
constexpr int kVarWorkerPollMs = 50;
constexpr int kImgWorkerPollMs = 50;
constexpr int kCtrlWorkerPollMs = 200;
constexpr size_t kMaxHosts = 32;

}  // namespace rdbg
}  // namespace tools

#endif
