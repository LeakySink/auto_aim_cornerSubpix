#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <fmt/core.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/eigen.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "calibration/calibrator.hpp"

namespace
{
int g_failed = 0;
int g_passed = 0;

void expect(bool ok, const std::string & name, const std::string & detail = {})
{
  if (ok) {
    g_passed++;
    fmt::print("[PASS] {}\n", name);
  } else {
    g_failed++;
    if (detail.empty())
      fmt::print("[FAIL] {}\n", name);
    else
      fmt::print("[FAIL] {}  ({})\n", name, detail);
  }
}

std::vector<cv::Point3f> object_points(cv::Size pattern, float square_mm)
{
  std::vector<cv::Point3f> pts;
  for (int i = 0; i < pattern.height; i++)
    for (int j = 0; j < pattern.width; j++)
      pts.emplace_back(j * square_mm, i * square_mm, 0);
  return pts;
}

cv::Mat make_chessboard(cv::Size pattern, int square_px, int margin)
{
  const int cols = pattern.width + 1;
  const int rows = pattern.height + 1;
  cv::Mat board(rows * square_px + 2 * margin, cols * square_px + 2 * margin, CV_8UC3,
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

calibration::SampleParams spaced_params(int i, const std::vector<cv::Point2f> & corners, cv::Size img)
{
  cv::Point2f mean(0, 0);
  for (const auto & c : corners) mean += c;
  mean *= 1.f / static_cast<float>(corners.size());
  calibration::SampleParams p;
  p.x = mean.x / img.width + 0.05 * (i % 6);
  p.y = mean.y / img.height + 0.05 * ((i / 3) % 5);
  p.size = 0.10 + 0.03 * (i % 8);
  p.skew = 0.03 * (i % 10);
  return p;
}

double rotation_deg_error(const cv::Mat & R_est, const Eigen::Matrix3d & R_true)
{
  Eigen::Matrix3d Re;
  cv::cv2eigen(R_est, Re);
  Eigen::Matrix3d dR = Re.transpose() * R_true;
  const double c = std::clamp((dR.trace() - 1.0) * 0.5, -1.0, 1.0);
  return std::acos(c) * 180.0 / CV_PI;
}

}  // namespace

int main()
{
  const cv::Size pattern(11, 8);
  const float square_mm = 40.f;
  const cv::Size img_size(1280, 720);
  const Eigen::Matrix3d R_gimbal2imubody = Eigen::Matrix3d::Identity();
  calibration::Calibrator calib(pattern.width, pattern.height, square_mm, R_gimbal2imubody);

  // 1. 合成棋盘格图，走 detect()
  auto board_img = make_chessboard(pattern, 48, 48);
  std::vector<cv::Point2f> det_corners;
  calibration::SampleParams det_params;
  const bool detected = calib.detect(board_img, det_corners, det_params);
  expect(
    detected && det_corners.size() == static_cast<size_t>(pattern.width * pattern.height),
    "detect synthetic chessboard",
    detected ? fmt::format("corners={}", det_corners.size()) : "findChessboardCorners failed");

  if (detected) {
    expect(calib.is_good_sample(det_params), "first sample is accepted");
    expect(
      calib.add_sample(det_corners, det_params, board_img.size(), nullptr), "add first detect sample");
    expect(!calib.is_good_sample(det_params), "duplicate pose is rejected");
    expect(
      !calib.add_sample(det_corners, det_params, board_img.size(), nullptr),
      "add_sample rejects duplicate");
    calib.drop_last();
    expect(calib.size() == 0, "drop_last clears the only sample");
  }

  // 2. 已知内参投影多姿态角点，测内参标定
  cv::Mat K_true = (cv::Mat_<double>(3, 3) << 800, 0, 640, 0, 800, 360, 0, 0, 1);
  cv::Mat dist_true = (cv::Mat_<double>(5, 1) << -0.08, 0.12, 0.001, -0.001, 0);
  const auto obj = object_points(pattern, square_mm);

  std::vector<cv::Vec3d> tvecs = {
    {0, 0, 700},    {-80, 40, 650}, {90, -50, 800}, {-120, -30, 900}, {60, 70, 750},
    {-40, 90, 820}, {110, 20, 680}, {-90, -80, 860}, {30, -100, 720}, {70, 50, 950},
    {-60, 10, 780}, {20, -40, 840}, {-110, 60, 710}, {50, -70, 880}, {-20, 30, 760},
    {100, -20, 830}, {-70, -50, 690}, {40, 80, 910}, {-130, 20, 740}, {15, -90, 800},
    {85, 35, 870}, {-45, -15, 730}, {55, -55, 920}, {-95, 75, 770}};
  std::vector<cv::Vec3d> rvecs = {
    {0.05, 0.08, 0.02},  {0.25, -0.15, 0.05}, {-0.2, 0.3, -0.08}, {0.15, 0.22, 0.12},
    {-0.28, -0.1, 0.18}, {0.08, -0.32, -0.1}, {0.35, 0.05, 0.2},  {-0.12, 0.28, -0.22},
    {0.18, -0.25, 0.15}, {-0.3, 0.12, 0.08},  {0.22, 0.18, -0.16}, {-0.08, -0.2, 0.25},
    {0.12, -0.08, 0.1},  {-0.18, 0.14, -0.12}, {0.28, -0.22, 0.06}, {-0.06, 0.26, 0.14},
    {0.32, 0.1, -0.18},  {-0.24, -0.16, 0.2}, {0.04, 0.3, -0.05}, {-0.14, -0.28, 0.09},
    {0.2, 0.02, -0.24},  {-0.34, 0.08, 0.11}, {0.1, -0.3, 0.16}, {-0.02, 0.16, -0.2}};

  calibration::Calibrator cam_calib(pattern.width, pattern.height, square_mm, R_gimbal2imubody);
  int added = 0;
  for (size_t i = 0; i < tvecs.size(); i++) {
    std::vector<cv::Point2f> corners;
    cv::projectPoints(obj, rvecs[i], tvecs[i], K_true, dist_true, corners);
    auto params = spaced_params(static_cast<int>(i), corners, img_size);
    if (cam_calib.add_sample(corners, params, img_size, nullptr)) added++;
  }
  expect(added >= calibration::Calibrator::kMinSamples, "enough projected samples",
         fmt::format("added={}", added));
  expect(cam_calib.calibrate_camera(), "calibrate_camera");
  expect(cam_calib.has_camera(), "has_camera after calib");

  if (cam_calib.has_camera()) {
    expect(cam_calib.camera().reproj_error < 0.1, "reprojection error < 0.1px",
           fmt::format("{:.4f}", cam_calib.camera().reproj_error));
    const double fx = cam_calib.camera().camera_matrix.at<double>(0, 0);
    const double cx = cam_calib.camera().camera_matrix.at<double>(0, 2);
    expect(std::abs(fx - 800) < 15, "fx recovered", fmt::format("fx={:.2f}", fx));
    expect(std::abs(cx - 640) < 15, "cx recovered", fmt::format("cx={:.2f}", cx));
  }

  cam_calib.drop_last();
  expect(!cam_calib.has_camera(), "changing samples invalidates camera result");
  cam_calib.reset();
  expect(cam_calib.size() == 0, "reset empties samples");

  // 3. 合成眼在手上运动，测手眼
  Eigen::Matrix3d R_cg_true;
  R_cg_true << 0, 0, 1, -1, 0, 0, 0, -1, 0;
  const Eigen::Vector3d t_cg_true_mm(180.0, 4.0, 94.0);

  Eigen::Matrix3d R_bc0;
  R_bc0 << 1, 0, 0, 0, -1, 0, 0, 0, -1;
  const Eigen::Vector3d t_bc0_mm(0.0, 0.0, 1200.0);
  const Eigen::Matrix3d R_bw = R_cg_true * R_bc0;
  const Eigen::Vector3d t_bw = R_cg_true * t_bc0_mm + t_cg_true_mm;

  calibration::Calibrator he_calib(pattern.width, pattern.height, square_mm, R_gimbal2imubody);
  const std::vector<double> yaws = {
    -0.4, -0.2, 0.0, 0.15, 0.35, -0.3, 0.25, 0.05, -0.12, 0.4, -0.08, 0.22,
    0.3, -0.35, 0.1, -0.25, 0.18, -0.05, 0.42, -0.15, 0.08, -0.28, 0.33, -0.18};
  const std::vector<double> pitches = {
    0.1, -0.15, 0.0, 0.2, -0.08, 0.12, -0.18, 0.05, 0.16, -0.1, 0.08, -0.2,
    0.14, -0.12, 0.06, -0.22, 0.18, -0.04, 0.11, -0.16, 0.02, 0.2, -0.09, 0.13};

  added = 0;
  for (size_t i = 0; i < yaws.size(); i++) {
    Eigen::Matrix3d R_gw =
      (Eigen::AngleAxisd(yaws[i], Eigen::Vector3d::UnitZ()) *
       Eigen::AngleAxisd(pitches[i], Eigen::Vector3d::UnitY()))
        .toRotationMatrix();
    Eigen::Quaterniond q(R_gw);

    const Eigen::Matrix3d R_bg = R_gw.transpose() * R_bw;
    const Eigen::Vector3d t_bg = R_gw.transpose() * t_bw;
    const Eigen::Matrix3d R_bc = R_cg_true.transpose() * R_bg;
    const Eigen::Vector3d t_bc = R_cg_true.transpose() * (t_bg - t_cg_true_mm);

    cv::Mat R_bc_cv, rvec, tvec;
    cv::eigen2cv(R_bc, R_bc_cv);
    cv::Rodrigues(R_bc_cv, rvec);
    tvec = (cv::Mat_<double>(3, 1) << t_bc.x(), t_bc.y(), t_bc.z());

    std::vector<cv::Point2f> corners;
    cv::projectPoints(obj, rvec, tvec, K_true, dist_true, corners);

    auto params = spaced_params(static_cast<int>(i), corners, img_size);
    if (he_calib.add_sample(corners, params, img_size, &q)) added++;
  }

  expect(added >= calibration::Calibrator::kMinSamples, "enough handeye samples",
         fmt::format("added={}", added));
  expect(he_calib.calibrate_camera(), "handeye path: camera calib");
  expect(he_calib.calibrate_handeye(), "calibrate_handeye");

  if (he_calib.has_handeye()) {
    const double rot_err = rotation_deg_error(he_calib.handeye().R_camera2gimbal, R_cg_true);
    const Eigen::Vector3d t_est(
      he_calib.handeye().t_camera2gimbal.at<double>(0),
      he_calib.handeye().t_camera2gimbal.at<double>(1),
      he_calib.handeye().t_camera2gimbal.at<double>(2));
    const double t_err = (t_est - t_cg_true_mm / 1000.0).norm();
    expect(rot_err < 3.0, "handeye rotation error < 3deg", fmt::format("{:.3f} deg", rot_err));
    expect(t_err < 0.03, "handeye translation error < 3cm", fmt::format("{:.4f} m", t_err));
  }

  // 4. yaml 写回（落到 resolve 后的 result_path）
  expect(he_calib.save_yaml(), "save_yaml");
  auto node = YAML::LoadFile(he_calib.result_path());
  expect(node["calibrated_at"] && !node["calibrated_at"].as<std::string>().empty(),
         "save_yaml writes calibrated_at");
  expect(node["camera_matrix"] && node["camera_matrix"].size() == 9, "save_yaml writes camera_matrix");
  expect(node["R_camera2gimbal"] && node["R_camera2gimbal"].size() == 9, "save_yaml writes R_camera2gimbal");
  expect(node["pattern_cols"] && node["pattern_cols"].as<int>() == pattern.width,
         "save_yaml keeps pattern_cols");

  expect(!calib.yaml_snippet().empty() || !he_calib.yaml_snippet().empty(), "yaml_snippet nonempty");

  fmt::print("\n{} passed, {} failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
