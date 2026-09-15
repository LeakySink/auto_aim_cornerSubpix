#ifndef CALIBRATION__CALIBRATOR_HPP
#define CALIBRATION__CALIBRATOR_HPP

#include <Eigen/Dense>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace calibration
{

// 与 ROS camera_calibration 相同的四维采样描述：X / Y / Size / Skew
struct SampleParams
{
  double x = 0;     // 角点中心 x / 图像宽，约 0~1
  double y = 0;     // 角点中心 y / 图像高，约 0~1
  double size = 0;  // sqrt(棋盘格面积 / 图像面积)
  double skew = 0;  // 相对正面的偏斜（弧度）
};

struct Progress
{
  double x = 0;  // 0~1，覆盖度
  double y = 0;
  double size = 0;
  double skew = 0;
  int n = 0;
  int n_with_q = 0;
  bool goodenough = false;
};

struct CameraResult
{
  cv::Mat camera_matrix;
  cv::Mat distort_coeffs;
  double reproj_error = -1;
};

struct HandeyeResult
{
  cv::Mat R_camera2gimbal;
  cv::Mat t_camera2gimbal;
  Eigen::Vector3d ypr_deg = Eigen::Vector3d::Zero();
};

class Calibrator
{
public:
  /// 从 calibration/result.yaml 读棋盘格（缺省 11×8 / 40mm）
  Calibrator();

  /// 测试或显式覆盖用；生产路径请用无参构造
  Calibrator(
    int pattern_cols, int pattern_rows, double square_size_mm,
    const Eigen::Matrix3d & R_gimbal2imubody);

  /// refine=false：缩小快速检测（预览）；true：再 cornerSubPix（入库）
  bool detect(
    const cv::Mat & img, std::vector<cv::Point2f> & corners, SampleParams & params,
    bool refine = true) const;

  void refine_corners(const cv::Mat & img, std::vector<cv::Point2f> & corners) const;
  SampleParams sample_params(const std::vector<cv::Point2f> & corners, cv::Size img_size) const;

  bool is_good_sample(const SampleParams & params) const;

  // q 为空则只用于内参；返回 false 表示与已有样本太接近
  bool add_sample(
    const std::vector<cv::Point2f> & corners, const SampleParams & params, cv::Size img_size,
    const Eigen::Quaterniond * q);

  void drop_last();
  void reset();

  Progress progress() const;
  int size() const { return static_cast<int>(samples_.size()); }

  bool calibrate_camera();
  bool calibrate_handeye();

  bool has_camera() const { return camera_.reproj_error >= 0; }
  bool has_handeye() const { return handeye_.t_camera2gimbal.rows == 3; }

  const CameraResult & camera() const { return camera_; }
  const HandeyeResult & handeye() const { return handeye_; }
  const std::string & calibrated_at() const { return calibrated_at_; }
  void set_calibrated_at(std::string t) { calibrated_at_ = std::move(t); }

  std::string yaml_snippet() const;
  /// 写回 calibration/result.yaml（含 calibrated_at）
  bool save_yaml() const;

  struct SampleView
  {
    std::vector<cv::Point2f> corners;
    SampleParams params;
    bool has_q = false;
  };

  cv::Size pattern_size() const { return pattern_size_; }
  double square_size_mm() const { return square_size_mm_; }
  std::vector<cv::Point3f> board_points() const { return object_points(); }
  std::vector<SampleView> sample_views() const;

  static constexpr const char * kResultPath = "calibration/result.yaml";
  static constexpr int kMinSamples = 20;
  static constexpr int kMinHandeye = 5;

private:
  struct Sample
  {
    std::vector<cv::Point2f> corners;
    SampleParams params;
    Eigen::Quaterniond q = Eigen::Quaterniond::Identity();
    bool has_q = false;
  };

  cv::Size pattern_size_;
  double square_size_mm_;
  Eigen::Matrix3d R_gimbal2imubody_;
  cv::Size img_size_;

  std::vector<Sample> samples_;
  CameraResult camera_;
  HandeyeResult handeye_;
  std::string calibrated_at_;

  void invalidate_results();
  bool write_yaml(const std::string & path) const;

  std::vector<cv::Point3f> object_points() const;
  static SampleParams compute_params(
    const std::vector<cv::Point2f> & corners, cv::Size img_size, cv::Size pattern_size);
  static double param_l1(const SampleParams & a, const SampleParams & b);
  static std::string now_local_string();
};

}  // namespace calibration

#endif  // CALIBRATION__CALIBRATOR_HPP
