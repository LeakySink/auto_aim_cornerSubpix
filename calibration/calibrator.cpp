#include "calibrator.hpp"

#include <fmt/core.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <opencv2/core/eigen.hpp>
#include <sstream>

#include "tools/math_tools.hpp"

namespace calibration
{
namespace
{
constexpr double kMinParamDist = 0.2;
constexpr double kNeedX = 0.7;
constexpr double kNeedY = 0.7;
constexpr double kNeedSize = 0.4;
constexpr double kNeedSkew = 0.5;

double pdist(const cv::Point2f & a, const cv::Point2f & b)
{
  return cv::norm(a - b);
}

void outside_corners(
  const std::vector<cv::Point2f> & corners, cv::Size pattern_size, cv::Point2f & up_left,
  cv::Point2f & up_right, cv::Point2f & down_right, cv::Point2f & down_left)
{
  const int w = pattern_size.width;
  up_left = corners.front();
  up_right = corners[w - 1];
  down_right = corners.back();
  down_left = corners[corners.size() - w];
}

double quad_area(
  const cv::Point2f & up_left, const cv::Point2f & up_right, const cv::Point2f & down_right,
  const cv::Point2f & down_left)
{
  // 两个三角形的海伦公式，与 ROS camera_calibration 一致
  auto heron = [](const cv::Point2f & a, const cv::Point2f & b, const cv::Point2f & c) {
    const double ab = pdist(a, b);
    const double bc = pdist(b, c);
    const double ca = pdist(c, a);
    const double s = 0.5 * (ab + bc + ca);
    const double v = s * (s - ab) * (s - bc) * (s - ca);
    return v > 0 ? std::sqrt(v) : 0.0;
  };
  return heron(up_left, up_right, down_right) + heron(up_left, down_left, down_right);
}

double corner_angle_from_right(
  const cv::Point2f & a, const cv::Point2f & b, const cv::Point2f & c)
{
  const cv::Point2f ab = a - b;
  const cv::Point2f cb = c - b;
  const double na = cv::norm(ab);
  const double nc = cv::norm(cb);
  if (na < 1e-6 || nc < 1e-6) return 0.0;
  const double cosv = std::clamp((ab.dot(cb)) / (na * nc), -1.0, 1.0);
  return std::abs(std::acos(cosv) - CV_PI / 2.0);
}

std::string format_flow(const cv::Mat & m)
{
  std::ostringstream ss;
  ss << "[";
  const int n = m.rows * m.cols;
  const double * p = m.ptr<double>();
  for (int i = 0; i < n; i++) {
    if (i) ss << ", ";
    ss << fmt::format("{:.16g}", p[i]);
  }
  ss << "]";
  return ss.str();
}

}  // namespace

std::string Calibrator::now_local_string()
{
  using clock = std::chrono::system_clock;
  const auto t = clock::to_time_t(clock::now());
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

Calibrator::Calibrator()
: pattern_size_(11, 8),
  square_size_mm_(40),
  R_gimbal2imubody_(Eigen::Matrix3d::Identity())
{
  try {
    const auto y = YAML::LoadFile(kResultPath);
    if (y["pattern_cols"] && y["pattern_rows"])
      pattern_size_ = {y["pattern_cols"].as<int>(), y["pattern_rows"].as<int>()};
    if (y["square_size_mm"]) square_size_mm_ = y["square_size_mm"].as<double>();
    if (y["R_gimbal2imubody"] && y["R_gimbal2imubody"].IsSequence() &&
        y["R_gimbal2imubody"].size() == 9) {
      auto v = y["R_gimbal2imubody"].as<std::vector<double>>();
      R_gimbal2imubody_ = Eigen::Map<Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(v.data());
    }
  } catch (const std::exception &) {
    // 缺文件或字段时用上面的默认值
  }
}

Calibrator::Calibrator(
  int pattern_cols, int pattern_rows, double square_size_mm,
  const Eigen::Matrix3d & R_gimbal2imubody)
: pattern_size_(pattern_cols, pattern_rows),
  square_size_mm_(square_size_mm),
  R_gimbal2imubody_(R_gimbal2imubody)
{
}

void Calibrator::invalidate_results()
{
  camera_ = {};
  handeye_ = {};
  calibrated_at_.clear();
}

bool Calibrator::detect(
  const cv::Mat & img, std::vector<cv::Point2f> & corners, SampleParams & params) const
{
  corners.clear();
  const int flags =
    cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_FAST_CHECK;
  if (!cv::findChessboardCorners(img, pattern_size_, corners, flags)) return false;

  cv::Mat gray;
  if (img.channels() == 3)
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
  else
    gray = img;

  cv::cornerSubPix(
    gray, corners, {11, 11}, {-1, -1},
    cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30, 0.1));

  params = compute_params(corners, img.size(), pattern_size_);
  return true;
}

bool Calibrator::is_good_sample(const SampleParams & params) const
{
  if (samples_.empty()) return true;
  double best = 1e9;
  for (const auto & s : samples_) best = std::min(best, param_l1(params, s.params));
  return best > kMinParamDist;
}

bool Calibrator::add_sample(
  const std::vector<cv::Point2f> & corners, const SampleParams & params, cv::Size img_size,
  const Eigen::Quaterniond * q)
{
  if (corners.size() != static_cast<size_t>(pattern_size_.width * pattern_size_.height))
    return false;
  if (!is_good_sample(params)) return false;

  img_size_ = img_size;
  Sample s;
  s.corners = corners;
  s.params = params;
  if (q) {
    s.q = *q;
    s.has_q = true;
  }
  samples_.push_back(std::move(s));
  invalidate_results();
  return true;
}

void Calibrator::drop_last()
{
  if (samples_.empty()) return;
  samples_.pop_back();
  invalidate_results();
}

void Calibrator::reset()
{
  samples_.clear();
  invalidate_results();
}

std::vector<Calibrator::SampleView> Calibrator::sample_views() const
{
  std::vector<SampleView> out;
  out.reserve(samples_.size());
  for (const auto & s : samples_) out.push_back({s.corners, s.params, s.has_q});
  return out;
}

Progress Calibrator::progress() const
{
  Progress p;
  p.n = static_cast<int>(samples_.size());
  for (const auto & s : samples_)
    if (s.has_q) p.n_with_q++;
  if (samples_.empty()) return p;

  double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
  double min_s = 1e9, max_s = -1e9, max_sk = 0;
  for (const auto & s : samples_) {
    min_x = std::min(min_x, s.params.x);
    max_x = std::max(max_x, s.params.x);
    min_y = std::min(min_y, s.params.y);
    max_y = std::max(max_y, s.params.y);
    min_s = std::min(min_s, s.params.size);
    max_s = std::max(max_s, s.params.size);
    max_sk = std::max(max_sk, s.params.skew);
  }
  p.x = std::min(1.0, (max_x - min_x) / kNeedX);
  p.y = std::min(1.0, (max_y - min_y) / kNeedY);
  p.size = std::min(1.0, (max_s - min_s) / kNeedSize);
  p.skew = std::min(1.0, max_sk / kNeedSkew);
  p.goodenough = p.x >= 1.0 && p.y >= 1.0 && p.size >= 1.0 && p.skew >= 1.0 && p.n >= kMinSamples;
  return p;
}

std::vector<cv::Point3f> Calibrator::object_points() const
{
  std::vector<cv::Point3f> pts;
  pts.reserve(pattern_size_.width * pattern_size_.height);
  for (int i = 0; i < pattern_size_.height; i++)
    for (int j = 0; j < pattern_size_.width; j++)
      pts.emplace_back(j * square_size_mm_, i * square_size_mm_, 0);
  return pts;
}

SampleParams Calibrator::compute_params(
  const std::vector<cv::Point2f> & corners, cv::Size img_size, cv::Size pattern_size)
{
  SampleParams p;
  cv::Point2f mean(0, 0);
  for (const auto & c : corners) mean += c;
  mean *= 1.f / static_cast<float>(corners.size());
  p.x = mean.x / img_size.width;
  p.y = mean.y / img_size.height;

  cv::Point2f ul, ur, dr, dl;
  outside_corners(corners, pattern_size, ul, ur, dr, dl);
  const double area = quad_area(ul, ur, dr, dl);
  p.size = std::sqrt(area / (img_size.width * img_size.height));
  p.skew = std::min(corner_angle_from_right(ul, ur, dr), corner_angle_from_right(ul, dl, dr));
  return p;
}

double Calibrator::param_l1(const SampleParams & a, const SampleParams & b)
{
  return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.size - b.size) +
         std::abs(a.skew - b.skew);
}

bool Calibrator::calibrate_camera()
{
  if (static_cast<int>(samples_.size()) < kMinSamples) return false;

  const auto obj = object_points();
  std::vector<std::vector<cv::Point3f>> obj_points(samples_.size(), obj);
  std::vector<std::vector<cv::Point2f>> img_points;
  img_points.reserve(samples_.size());
  for (const auto & s : samples_) img_points.push_back(s.corners);

  cv::Mat camera_matrix, distort_coeffs;
  std::vector<cv::Mat> rvecs, tvecs;
  auto criteria =
    cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 100, DBL_EPSILON);
  cv::calibrateCamera(
    obj_points, img_points, img_size_, camera_matrix, distort_coeffs, rvecs, tvecs,
    cv::CALIB_FIX_K3, criteria);

  double error_sum = 0;
  size_t total = 0;
  for (size_t i = 0; i < obj_points.size(); i++) {
    std::vector<cv::Point2f> proj;
    cv::projectPoints(obj_points[i], rvecs[i], tvecs[i], camera_matrix, distort_coeffs, proj);
    total += proj.size();
    for (size_t j = 0; j < proj.size(); j++) error_sum += cv::norm(img_points[i][j] - proj[j]);
  }

  camera_.camera_matrix = camera_matrix;
  camera_.distort_coeffs = distort_coeffs;
  camera_.reproj_error = error_sum / static_cast<double>(total);
  handeye_ = {};
  calibrated_at_ = now_local_string();
  return true;
}

bool Calibrator::calibrate_handeye()
{
  if (!has_camera()) return false;

  const auto obj = object_points();
  cv::Matx33d K(camera_.camera_matrix.ptr<double>());
  cv::Mat dist = camera_.distort_coeffs;

  std::vector<cv::Mat> R_gimbal2world_list, t_gimbal2world_list, rvecs, tvecs;
  for (const auto & s : samples_) {
    if (!s.has_q) continue;
    Eigen::Matrix3d R_imubody2imuabs = s.q.toRotationMatrix();
    Eigen::Matrix3d R_gimbal2world =
      R_gimbal2imubody_.transpose() * R_imubody2imuabs * R_gimbal2imubody_;

    cv::Mat t_gimbal2world = (cv::Mat_<double>(3, 1) << 0, 0, 0);
    cv::Mat R_gimbal2world_cv;
    cv::eigen2cv(R_gimbal2world, R_gimbal2world_cv);

    cv::Mat rvec, tvec;
    cv::solvePnP(obj, s.corners, K, dist, rvec, tvec, false, cv::SOLVEPNP_IPPE);

    R_gimbal2world_list.emplace_back(R_gimbal2world_cv);
    t_gimbal2world_list.emplace_back(t_gimbal2world);
    rvecs.emplace_back(rvec);
    tvecs.emplace_back(tvec);
  }

  if (static_cast<int>(rvecs.size()) < kMinHandeye) return false;

  cv::Mat R_camera2gimbal, t_camera2gimbal;
  cv::calibrateHandEye(
    R_gimbal2world_list, t_gimbal2world_list, rvecs, tvecs, R_camera2gimbal, t_camera2gimbal);
  t_camera2gimbal /= 1e3;

  Eigen::Matrix3d R_camera2gimbal_eigen;
  cv::cv2eigen(R_camera2gimbal, R_camera2gimbal_eigen);
  Eigen::Matrix3d R_gimbal2ideal{{0, -1, 0}, {0, 0, -1}, {1, 0, 0}};
  Eigen::Matrix3d R_camera2ideal = R_gimbal2ideal * R_camera2gimbal_eigen;

  handeye_.R_camera2gimbal = R_camera2gimbal;
  handeye_.t_camera2gimbal = t_camera2gimbal;
  handeye_.ypr_deg = tools::eulers(R_camera2ideal, 1, 0, 2) * 57.3;
  calibrated_at_ = now_local_string();
  return true;
}

std::string Calibrator::yaml_snippet() const
{
  if (!has_camera()) return {};

  YAML::Emitter out;
  out << YAML::BeginMap;
  if (!calibrated_at_.empty()) {
    out << YAML::Key << "calibrated_at" << YAML::Value << calibrated_at_;
  }
  out << YAML::Comment(fmt::format("重投影误差: {:.4f}px", camera_.reproj_error));
  out << YAML::Key << "camera_matrix";
  out << YAML::Value << YAML::Flow
      << std::vector<double>(
           camera_.camera_matrix.begin<double>(), camera_.camera_matrix.end<double>());
  out << YAML::Key << "distort_coeffs";
  out << YAML::Value << YAML::Flow
      << std::vector<double>(
           camera_.distort_coeffs.begin<double>(), camera_.distort_coeffs.end<double>());
  if (has_handeye()) {
    out << YAML::Newline;
    out << YAML::Comment(fmt::format(
      "相机同理想情况的偏角: yaw{:.2f} pitch{:.2f} roll{:.2f} degree", handeye_.ypr_deg[0],
      handeye_.ypr_deg[1], handeye_.ypr_deg[2]));
    out << YAML::Key << "R_camera2gimbal";
    out << YAML::Value << YAML::Flow
        << std::vector<double>(
             handeye_.R_camera2gimbal.begin<double>(), handeye_.R_camera2gimbal.end<double>());
    out << YAML::Key << "t_camera2gimbal";
    out << YAML::Value << YAML::Flow
        << std::vector<double>(
             handeye_.t_camera2gimbal.begin<double>(), handeye_.t_camera2gimbal.end<double>());
  }
  out << YAML::Newline << YAML::EndMap;
  return out.c_str();
}

bool Calibrator::save_yaml() const { return write_yaml(kResultPath); }

bool Calibrator::write_yaml(const std::string & path) const
{
  if (!has_camera()) return false;

  std::ostringstream out;
  out << "# 标定结果（SAVE 写回；棋盘格参数也在此修改）\n";
  out << "# 仅内参：camera_matrix / distort_coeffs\n";
  out << "# 用法见 calibration.md\n\n";
  out << "pattern_cols: " << pattern_size_.width << "\n";
  out << "pattern_rows: " << pattern_size_.height << "\n";
  out << "square_size_mm: " << square_size_mm_ << "\n\n";
  if (!calibrated_at_.empty()) out << "calibrated_at: \"" << calibrated_at_ << "\"\n";
  out << "# 重投影误差: " << fmt::format("{:.4f}px", camera_.reproj_error) << "\n";
  out << "camera_matrix: " << format_flow(camera_.camera_matrix) << "\n";
  out << "distort_coeffs: " << format_flow(camera_.distort_coeffs) << "\n";
  if (has_handeye()) {
    out << "# 相机同理想情况的偏角: yaw" << fmt::format("{:.2f}", handeye_.ypr_deg[0])
        << " pitch" << fmt::format("{:.2f}", handeye_.ypr_deg[1]) << " roll"
        << fmt::format("{:.2f}", handeye_.ypr_deg[2]) << " degree\n";
    out << "R_camera2gimbal: " << format_flow(handeye_.R_camera2gimbal) << "\n";
    out << "t_camera2gimbal: " << format_flow(handeye_.t_camera2gimbal) << "\n";
  }

  std::ofstream file(path);
  if (!file) return false;
  file << out.str();
  return static_cast<bool>(file);
}

}  // namespace calibration
