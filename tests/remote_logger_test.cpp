#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>

#include "tools/exiter.hpp"
#include "tools/remote_logger.hpp"

int main(int argc, char * argv[])
{
  std::string host = "127.0.0.1";
  uint16_t port = 9871;
  uint16_t ctrl_port = 15000;
  std::string name;
  uint32_t hb = 0;
  int rate = 50;
  std::string video_path;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::printf("Usage: %s [options]\n", argv[0]);
      std::printf("  --host=IP       Remote host IP (default: 127.0.0.1)\n");
      std::printf("  --port=PORT     Data port      (default: 9871)\n");
      std::printf("  --ctrl-port=P   Control port   (default: 15000)\n");
      std::printf("  --name=NAME     Sender name    (default: auto)\n");
      std::printf("  --hb=MS         Heartbeat ms   (default: 0=off)\n");
      std::printf("  --rate=HZ       Send rate      (default: 50)\n");
      std::printf("  --video=PATH    Video file     (optional)\n");
      return 0;
    }
    auto eq = arg.find('=');
    std::string k = (eq != std::string::npos) ? arg.substr(0, eq) : arg;
    std::string v = (eq != std::string::npos) ? arg.substr(eq + 1) : (i + 1 < argc ? argv[++i] : "");
    if (k == "--host") host = v;
    else if (k == "--port") port = static_cast<uint16_t>(std::stoi(v));
    else if (k == "--ctrl-port") ctrl_port = static_cast<uint16_t>(std::stoi(v));
    else if (k == "--name") name = v;
    else if (k == "--hb") hb = static_cast<uint32_t>(std::stoul(v));
    else if (k == "--rate") rate = std::stoi(v);
    else if (k == "--video") video_path = v;
  }

  cv::VideoCapture cap;
  bool use_video = !video_path.empty();
  if (use_video) {
    cap.open(video_path);
    if (!cap.isOpened()) {
      std::cerr << "Failed to open video: " << video_path << std::endl;
      return 1;
    }
  }

  tools::Exiter exiter;
  tools::RemoteLogger::Config cfg;
  cfg.remote_host = host;
  cfg.remote_port = port;
  cfg.control_port = ctrl_port;
  cfg.sender_name = name;
  cfg.heartbeat_interval_ms = hb;
  cfg.log_dir = "./logs";
  cfg.img_width = 320;
  cfg.img_quality = 40;

  tools::RemoteLogger::instance().init(cfg);
  tools::RemoteLogger::instance().log("INFO", "test started, host=" + host +
                                             " port=" + std::to_string(port));

  auto interval = std::chrono::microseconds(1000000 / rate);
  auto t0 = std::chrono::steady_clock::now();
  uint64_t iter = 0;
  double ball_x = 160, ball_y = 120, ball_dx = 3.0, ball_dy = 2.0;

  while (!exiter.exit()) {
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    double sin1 = std::sin(elapsed * 2.0 * M_PI * 1.0);
    double sin2 = std::sin(elapsed * 2.0 * M_PI * 3.0);
    double saw = 2.0 * std::fmod(elapsed * 0.5, 1.0) - 1.0;
    double tri = 2.0 * std::abs(2.0 * std::fmod(elapsed * 0.5 + 0.25, 1.0) - 1.0) - 1.0;
    double sqr = (std::sin(elapsed * 2.0 * M_PI * 0.5) > 0) ? 1.0 : -1.0;

    tools::RemoteLogger::instance().plot({
      {"elapsed", elapsed},
      {"sin_1Hz", sin1},
      {"sin_3Hz", sin2},
      {"sawtooth", saw},
      {"triangle", tri},
      {"square", sqr},
    });

    if (iter % 200 == 0) {
      tools::RemoteLogger::instance().log(
        "ERROR", "high error rate detected at iter=" + std::to_string(iter));
    } else if (iter % 100 == 0) {
      tools::RemoteLogger::instance().log(
        "WARN", "gimbal approaching limit, iter=" + std::to_string(iter));
    } else if (iter % 50 == 0) {
      tools::RemoteLogger::instance().log(
        "INFO", "target locked, distance=3." + std::to_string(iter % 10) + "m");
    } else if (iter % 25 == 0) {
      tools::RemoteLogger::instance().log(
        "DEBUG", "frame " + std::to_string(iter) + " processed in " +
                   std::to_string(1.0 / rate * 1000).substr(0, 4) + "ms");
    }

    cv::Mat frame;
    if (use_video) {
      cap >> frame;
      if (frame.empty()) {
        cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        cap >> frame;
      }
    } else {
      frame = cv::Mat(240, 320, CV_8UC3);
      for (int y = 0; y < frame.rows; y++) {
        for (int x = 0; x < frame.cols; x++) {
          frame.at<cv::Vec3b>(y, x) = cv::Vec3b(
            static_cast<uchar>((std::sin(x * 0.05 + elapsed) * 0.3 + 0.5) * 200),
            static_cast<uchar>((std::cos(y * 0.05 + elapsed) * 0.3 + 0.5) * 180),
            static_cast<uchar>(120));
        }
      }

      ball_x += ball_dx;
      ball_y += ball_dy;
      if (ball_x < 20 || ball_x > 300) ball_dx = -ball_dx;
      if (ball_y < 20 || ball_y > 220) ball_dy = -ball_dy;
      cv::circle(frame, cv::Point(static_cast<int>(ball_x), static_cast<int>(ball_y)),
                 15, cv::Scalar(0, 200, 255), -1);

      cv::line(frame, cv::Point(160, 110), cv::Point(160, 130), cv::Scalar(0, 255, 0), 1);
      cv::line(frame, cv::Point(150, 120), cv::Point(170, 120), cv::Scalar(0, 255, 0), 1);

      cv::putText(frame, "iter:" + std::to_string(iter), cv::Point(8, 20),
                  cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
      cv::putText(frame, "t:" + std::to_string(elapsed).substr(0, 5) + "s",
                  cv::Point(8, 40), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
      cv::putText(frame, "sin1:" + std::to_string(sin1).substr(0, 5),
                  cv::Point(8, 60), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 200, 255), 1);
      cv::putText(frame, "sin2:" + std::to_string(sin2).substr(0, 5),
                  cv::Point(8, 80), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 255, 200), 1);
    }

    tools::RemoteLogger::instance().plot_image(frame,
      {{"name", use_video ? "video" : "simulation"}});

    if (iter % 3 == 0 && !use_video) {
      cv::Mat dbg(120, 320, CV_8UC3, cv::Scalar(20, 20, 30));
      for (int x = 1; x < dbg.cols; x++) {
        double tv = elapsed - (dbg.cols - x) * 0.02;
        int y1 = 60 - static_cast<int>(std::sin(tv * 2 * M_PI) * 50);
        int y2 = 60 - static_cast<int>(std::sin(tv * 6 * M_PI) * 50);
        cv::line(dbg, cv::Point(x - 1, std::clamp(y1, 0, 119)),
                 cv::Point(x, std::clamp(static_cast<int>(60 - std::sin((tv + 0.02) * 2 * M_PI) * 50), 0, 119)),
                 cv::Scalar(100, 200, 255), 2);
        cv::line(dbg, cv::Point(x - 1, std::clamp(y2, 0, 119)),
                 cv::Point(x, std::clamp(static_cast<int>(60 - std::sin((tv + 0.02) * 6 * M_PI) * 50), 0, 119)),
                 cv::Scalar(100, 255, 200), 2);
      }
      tools::RemoteLogger::instance().plot_image(dbg, {{"name", "waveform"}});
    }

    iter++;
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t1);
    if (elapsed_us < interval) {
      std::this_thread::sleep_for(interval - elapsed_us);
    }
  }

  tools::RemoteLogger::instance().log("INFO", "test stopped, total iter=" +
                                             std::to_string(iter));
  tools::RemoteLogger::instance().shutdown();

  return 0;
}
