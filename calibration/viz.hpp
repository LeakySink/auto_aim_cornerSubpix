#ifndef CALIBRATION__VIZ_HPP
#define CALIBRATION__VIZ_HPP

#include <Eigen/Dense>
#include <opencv2/opencv.hpp>
#include <string>

#include "calibration/calibrator.hpp"

namespace calibration
{

enum class UiAction { None, Add, Calibrate, Save, Undistort, Drop, Reset };

struct UiLayout
{
  cv::Rect image;
  cv::Rect calibrate;
  cv::Rect save;
  cv::Rect undistort;
  cv::Rect drop;
  cv::Rect reset;
};

struct VizFlags
{
  bool undistort = false;
  bool camera_only = false;
  bool flash = false;
  bool board_found = false;
  std::string hint;
  const Eigen::Vector3d * ypr_deg = nullptr;
};

cv::Mat render_viz(
  const cv::Mat & img, const std::vector<cv::Point2f> & live_corners, const Calibrator & calib,
  const Progress & prog, const VizFlags & flags, const cv::Point & mouse, UiLayout & layout);

UiAction hit_test(const UiLayout & layout, int x, int y);

}  // namespace calibration

#endif  // CALIBRATION__VIZ_HPP
