#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <thread>

#include "tools/exiter.hpp"
#include "tools/remote_logger.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                     | 输出命令行参数说明}"
  "{host           |      127.0.0.1      | 远程接收端 IP}"
  "{port p         |         9871        | 远程接收端端口}"
  "{rate r         |         50          | 发送频率 Hz}"
  "{video v        |                     | 视频文件路径（可选，不填则生成测试画面）}";

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }

  auto host = cli.get<std::string>("host");
  auto port = static_cast<uint16_t>(cli.get<int>("port"));
  auto rate = cli.get<int>("rate");
  auto video_path = cli.get<std::string>("video");

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

    // ── two sine waves ──
    double sin1 = std::sin(elapsed * 2.0 * M_PI * 1.0);   // 1 Hz
    double sin2 = std::sin(elapsed * 2.0 * M_PI * 3.0);   // 3 Hz

    tools::RemoteLogger::instance().plot({
      {"elapsed", elapsed},
      {"sin_1Hz", sin1},
      {"sin_3Hz", sin2},
    });

    // ── periodic log messages ──
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

    // ── image ──
    cv::Mat frame;
    if (use_video) {
      cap >> frame;
      if (frame.empty()) {
        cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        cap >> frame;
      }
    } else {
      frame = cv::Mat(240, 320, CV_8UC3);
      // animated background
      for (int y = 0; y < frame.rows; y++) {
        for (int x = 0; x < frame.cols; x++) {
          frame.at<cv::Vec3b>(y, x) = cv::Vec3b(
            static_cast<uchar>((std::sin(x * 0.05 + elapsed) * 0.3 + 0.5) * 200),
            static_cast<uchar>((std::cos(y * 0.05 + elapsed) * 0.3 + 0.5) * 180),
            static_cast<uchar>(120));
        }
      }

      // moving ball
      ball_x += ball_dx;
      ball_y += ball_dy;
      if (ball_x < 20 || ball_x > 300) ball_dx = -ball_dx;
      if (ball_y < 20 || ball_y > 220) ball_dy = -ball_dy;
      cv::circle(frame, cv::Point(static_cast<int>(ball_x), static_cast<int>(ball_y)),
                 15, cv::Scalar(0, 200, 255), -1);

      // crosshair
      cv::line(frame, cv::Point(160, 110), cv::Point(160, 130),
               cv::Scalar(0, 255, 0), 1);
      cv::line(frame, cv::Point(150, 120), cv::Point(170, 120),
               cv::Scalar(0, 255, 0), 1);

      // HUD text
      cv::putText(frame, "iter:" + std::to_string(iter), cv::Point(8, 20),
                  cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
      cv::putText(frame, "t:" + std::to_string(elapsed).substr(0, 5) + "s",
                  cv::Point(8, 40), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                  cv::Scalar(255, 255, 255), 1);
      cv::putText(frame, "sin1:" + std::to_string(sin1).substr(0, 5),
                  cv::Point(8, 60), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                  cv::Scalar(100, 200, 255), 1);
      cv::putText(frame, "sin2:" + std::to_string(sin2).substr(0, 5),
                  cv::Point(8, 80), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                  cv::Scalar(100, 255, 200), 1);
    }

    tools::RemoteLogger::instance().plot_image(frame);

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
