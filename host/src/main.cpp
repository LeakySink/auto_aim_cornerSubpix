#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/udp_receiver.hpp"

namespace
{
volatile sig_atomic_t g_running = 1;
}

static void on_signal(int) { g_running = 0; }

int main(int argc, char * argv[])
{
  std::string host = "0.0.0.0";
  uint16_t port = 9871;
  int timeout_ms = 3000;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "--timeout" && i + 1 < argc) {
      timeout_ms = std::stoi(argv[++i]);
    } else if (arg == "-h" || arg == "--help") {
      std::printf("Usage: %s [--host HOST] [--port PORT] [--timeout MS]\n", argv[0]);
      std::printf("  --host     Listen address (default: 0.0.0.0)\n");
      std::printf("  --port     UDP listen port  (default: 9871)\n");
      std::printf("  --timeout  Heartbeat timeout in ms (default: 3000)\n");
      std::printf("\nOutputs JSON lines to stdout for the Python frontend.\n");
      return 0;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  backend::UDPReceiver receiver(host, port);
  receiver.start();

  std::vector<backend::PlotData> plot_buf;
  std::vector<backend::ImageData> img_buf;
  std::vector<backend::LogData> log_buf;

  bool was_connected = false;
  auto timeout = std::chrono::milliseconds(timeout_ms);

  while (g_running) {
    bool has_data = false;

    if (receiver.pop_all_plot(plot_buf)) {
      for (auto & p : plot_buf) {
        nlohmann::json out;
        out["type"] = "plot";
        out["ts"] = p.ts;
        try {
          out["data"] = nlohmann::json::parse(p.json_str);
        } catch (...) {
          out["data"] = p.json_str;
        }
        std::cout << out.dump() << std::endl;
      }
      plot_buf.clear();
      has_data = true;
    }

    if (receiver.pop_all_image(img_buf)) {
      for (auto & img : img_buf) {
        nlohmann::json out;
        out["type"] = "image";
        out["ts"] = img.ts;
        try {
          out["meta"] = nlohmann::json::parse(img.meta_json);
        } catch (...) {
          out["meta"] = img.meta_json;
        }
        out["jpg_b64"] =
          backend::base64_encode(img.jpeg.data(), img.jpeg.size());
        std::cout << out.dump() << std::endl;
      }
      img_buf.clear();
      has_data = true;
    }

    if (receiver.pop_all_log(log_buf)) {
      for (auto & l : log_buf) {
        nlohmann::json out;
        out["type"] = "log";
        out["ts"] = l.ts;
        out["level"] = l.level;
        out["msg"] = l.message;
        if (!l.sender.empty()) out["_from"] = l.sender;
        std::cout << out.dump() << std::endl;
      }
      log_buf.clear();
      has_data = true;
    }

    if (!has_data) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto now = std::chrono::steady_clock::now();
    bool connected = (now - receiver.last_packet_time()) < timeout;
    if (!was_connected && connected) {
      nlohmann::json out;
      out["type"] = "status";
      out["connected"] = true;
      out["sender"] = receiver.last_sender();
      std::cout << out.dump() << std::endl;
    } else if (was_connected && !connected) {
      nlohmann::json out;
      out["type"] = "status";
      out["connected"] = false;
      std::cout << out.dump() << std::endl;
    }
    was_connected = connected;
  }

  receiver.stop();
  return 0;
}
