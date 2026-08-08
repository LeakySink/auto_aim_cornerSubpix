#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend
{

struct PlotData
{
  uint64_t ts;
  std::string json_str;
};

struct ImageData
{
  uint64_t ts;
  std::string meta_json;
  std::vector<uint8_t> jpeg;
};

struct LogData
{
  uint64_t ts;
  std::string level;
  std::string message;
};

class UDPReceiver
{
public:
  UDPReceiver(const std::string & host, uint16_t port);
  ~UDPReceiver();

  void start();
  void stop();

  bool pop_all_plot(std::vector<PlotData> & out);
  bool pop_all_image(std::vector<ImageData> & out);
  bool pop_all_log(std::vector<LogData> & out);

  std::chrono::steady_clock::time_point last_packet_time() const;

private:
  void worker();

  std::string host_;
  uint16_t port_;
  int sock_{-1};
  std::atomic<bool> running_{false};
  std::thread thread_;

  std::vector<PlotData> plot_queue_;
  std::mutex plot_mtx_;

  std::vector<ImageData> img_queue_;
  std::mutex img_mtx_;

  std::vector<LogData> log_queue_;
  std::mutex log_mtx_;

  mutable std::mutex time_mtx_;
  std::chrono::steady_clock::time_point last_pkt_{};
};

std::string base64_encode(const uint8_t * data, size_t len);

}  // namespace backend
