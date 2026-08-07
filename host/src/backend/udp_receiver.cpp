#include "backend/udp_receiver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <nlohmann/json.hpp>

namespace backend
{

static const char kBase64Table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t * data, size_t len)
{
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
    out.push_back(kBase64Table[(n >> 18) & 0x3F]);
    out.push_back(kBase64Table[(n >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? kBase64Table[(n >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? kBase64Table[n & 0x3F] : '=');
  }
  return out;
}

UDPReceiver::UDPReceiver(const std::string & host, uint16_t port)
: host_(host), port_(port)
{
}

UDPReceiver::~UDPReceiver() { stop(); }

void UDPReceiver::start()
{
  sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_ < 0) return;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = ::htons(port_);
  if (host_ == "0.0.0.0")
    addr.sin_addr.s_addr = INADDR_ANY;
  else
    addr.sin_addr.s_addr = ::inet_addr(host_.c_str());

  if (::bind(sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(sock_);
    sock_ = -1;
    return;
  }

  running_ = true;
  thread_ = std::thread(&UDPReceiver::worker, this);
}

void UDPReceiver::stop()
{
  if (!running_) return;
  running_ = false;
  if (sock_ >= 0) {
    ::shutdown(sock_, SHUT_RDWR);
    ::close(sock_);
    sock_ = -1;
  }
  if (thread_.joinable()) thread_.join();
}

bool UDPReceiver::pop_all_plot(std::vector<PlotData> & out)
{
  std::lock_guard<std::mutex> lock(plot_mtx_);
  if (plot_queue_.empty()) return false;
  out.swap(plot_queue_);
  return true;
}

bool UDPReceiver::pop_all_image(std::vector<ImageData> & out)
{
  std::lock_guard<std::mutex> lock(img_mtx_);
  if (img_queue_.empty()) return false;
  out.swap(img_queue_);
  return true;
}

bool UDPReceiver::pop_all_log(std::vector<LogData> & out)
{
  std::lock_guard<std::mutex> lock(log_mtx_);
  if (log_queue_.empty()) return false;
  out.swap(log_queue_);
  return true;
}

void UDPReceiver::worker()
{
  uint8_t buf[65536];

  while (running_) {
    ssize_t n = ::recvfrom(sock_, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n <= 0) continue;
    if (static_cast<size_t>(n) < 1) continue;

    if (buf[0] == 0xFF) {
      if (static_cast<size_t>(n) < 13) continue;
      uint64_t ts;
      std::memcpy(&ts, buf + 1, 8);
      uint32_t jpg_len;
      std::memcpy(&jpg_len, buf + 9, 4);
      if (static_cast<size_t>(n) < 13 + jpg_len) continue;

      ImageData img;
      img.ts = ts;
      img.jpeg.assign(buf + 13, buf + 13 + jpg_len);

      std::lock_guard<std::mutex> lock(img_mtx_);
      img_queue_.push_back(std::move(img));
    } else {
      std::string json_str(reinterpret_cast<char *>(buf),
                           static_cast<size_t>(n));
      try {
        auto j = nlohmann::json::parse(json_str);
        if (j.contains("level") && j.contains("msg") &&
            j["level"].is_string() && j["msg"].is_string()) {
          LogData log;
          log.ts = j.value("ts", uint64_t(0));
          log.level = j["level"].get<std::string>();
          log.message = j["msg"].get<std::string>();

          std::lock_guard<std::mutex> lock(log_mtx_);
          log_queue_.push_back(std::move(log));
        } else {
          PlotData plot;
          plot.ts = j.value("ts", uint64_t(0));
          plot.json_str = std::move(json_str);

          std::lock_guard<std::mutex> lock(plot_mtx_);
          plot_queue_.push_back(std::move(plot));
        }
      } catch (...) {
      }
    }
  }
}

}  // namespace backend
