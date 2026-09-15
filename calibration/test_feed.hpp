#pragma once
// =============================================================================
// CALIB_TEST_FEED — --test 后门假相机。调试结束后整文件删除，并去掉
// calibrate.cpp 里所有带 CALIB_TEST_FEED 标记的代码。
// =============================================================================

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <string>

#include "io/camera.hpp"

namespace calib_test_feed
{

inline cv::Mat make_chessboard(cv::Size pattern, int square_px, int margin)
{
  const int cols = pattern.width + 1;
  const int rows = pattern.height + 1;
  cv::Mat board(
    rows * square_px + 2 * margin, cols * square_px + 2 * margin, CV_8UC3,
    cv::Scalar(255, 255, 255));
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if ((r + c) % 2 == 0) continue;
      cv::rectangle(
        board, {margin + c * square_px, margin + r * square_px},
        {margin + (c + 1) * square_px - 1, margin + (r + 1) * square_px - 1}, {0, 0, 0},
        cv::FILLED);
    }
  }
  return board;
}

// 无真机：周期性平移/缩放/轻微透视，便于网页看到推流与角点采样。
class FakeCamera : public io::CameraBase
{
public:
  explicit FakeCamera(cv::Size pattern, cv::Size frame = {640, 480})
  : pattern_(pattern), frame_(frame), t0_(std::chrono::steady_clock::now())
  {
    board_ = make_chessboard(pattern_, 36, 24);
  }

  void read(cv::Mat & img, std::chrono::steady_clock::time_point & timestamp) override
  {
    timestamp = std::chrono::steady_clock::now();
    const double t =
      std::chrono::duration<double>(timestamp - t0_).count();

    img = cv::Mat(frame_, CV_8UC3, cv::Scalar(32, 32, 40));

    // 慢扫 X/Y/Size，覆盖度条能慢慢涨
    const double cx = frame_.width * (0.5 + 0.28 * std::sin(t * 0.35));
    const double cy = frame_.height * (0.5 + 0.22 * std::cos(t * 0.27));
    const double scale = 0.55 + 0.25 * (0.5 + 0.5 * std::sin(t * 0.19));
    const double skew = 0.12 * std::sin(t * 0.41);

    const double bw = board_.cols * scale;
    const double bh = board_.rows * scale;
    std::vector<cv::Point2f> src = {
      {0, 0},
      {static_cast<float>(board_.cols - 1), 0},
      {static_cast<float>(board_.cols - 1), static_cast<float>(board_.rows - 1)},
      {0, static_cast<float>(board_.rows - 1)},
    };
    std::vector<cv::Point2f> dst = {
      {static_cast<float>(cx - bw * 0.5 + skew * bh), static_cast<float>(cy - bh * 0.5)},
      {static_cast<float>(cx + bw * 0.5 + skew * bh), static_cast<float>(cy - bh * 0.5)},
      {static_cast<float>(cx + bw * 0.5 - skew * bh), static_cast<float>(cy + bh * 0.5)},
      {static_cast<float>(cx - bw * 0.5 - skew * bh), static_cast<float>(cy + bh * 0.5)},
    };
    cv::Mat H = cv::getPerspectiveTransform(src, dst);
    cv::Mat warped = cv::Mat::zeros(frame_, CV_8UC3);
    cv::warpPerspective(board_, warped, H, frame_);
    cv::Mat mask;
    cv::cvtColor(warped, mask, cv::COLOR_BGR2GRAY);
    warped.copyTo(img, mask);

    cv::putText(
      img, "TEST FEED --test (remove after debug)", {12, 28}, cv::FONT_HERSHEY_SIMPLEX, 0.55,
      {0, 200, 255}, 2, cv::LINE_AA);
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

private:
  cv::Size pattern_;
  cv::Size frame_;
  cv::Mat board_;
  std::chrono::steady_clock::time_point t0_;
};

inline std::unique_ptr<io::CameraBase> make(cv::Size pattern)
{
  return std::make_unique<FakeCamera>(pattern);
}

}  // namespace calib_test_feed
