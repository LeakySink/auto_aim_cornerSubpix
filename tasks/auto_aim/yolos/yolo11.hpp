#ifndef AUTO_AIM__YOLO11_HPP
#define AUTO_AIM__YOLO11_HPP

#include <list>
#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <string>
#include <vector>

#include "tasks/auto_aim/armor.hpp"
#include "tasks/auto_aim/detector.hpp"
#include "tasks/auto_aim/yolo.hpp"

namespace auto_aim
{
class YOLO11 : public YOLOBase
{
public:
  YOLO11(const std::string & config_path, bool debug);

  std::list<Armor> detect(const cv::Mat & bgr_img, int frame_count) override;

  std::list<Armor> postprocess(
    double scale, cv::Mat & output, const cv::Mat & bgr_img, int frame_count) override;

private:
  std::string device_, model_path_;
  std::string save_path_, debug_path_;
  bool debug_, use_roi_;

  const int class_num_ = 38;
  const float nms_threshold_ = 0.3;
  const float score_threshold_ = 0.7;
  double min_confidence_, binary_threshold_;
  // 分类 top-1 / top-2 的间隔门槛（yaml 可选键 min_class_margin）。
  // 默认 0 = 不启用，保持原有行为；需要压"网络在两类之间犹豫"的误判时再开。
  double min_class_margin_ = 0.0;

  ov::Core core_;
  ov::CompiledModel compiled_model_;

  cv::Rect roi_;
  cv::Point2f offset_;
  cv::Mat tmp_img_;

  Detector detector_;

  bool check_name(const Armor & armor) const;
  bool check_type(const Armor & armor) const;

  cv::Point2f get_center_norm(const cv::Mat & bgr_img, const cv::Point2f & center) const;

  std::list<Armor> parse(double scale, cv::Mat & output, const cv::Mat & bgr_img, int frame_count);

  void save(const Armor & armor) const;
  void draw_detections(const cv::Mat & img, const std::list<Armor> & armors, int frame_count) const;
  void sort_keypoints(std::vector<cv::Point2f> & keypoints);
};

}  // namespace auto_aim

#endif  //AUTO_AIM__YOLO11_HPP