#ifndef AUTO_AIM__YOLOTUP_HPP
#define AUTO_AIM__YOLOTUP_HPP

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
// TUP 2023 装甲板模型 (opt-1208-001)
// 输入: 1x3x416x416 BGR 0-255, 输出: 1x3549x21
// 每行: 8 角点偏移 + 1 置信度(已过 sigmoid) + 4 颜色 + 8 类别
class YOLOTUP : public YOLOBase
{
public:
  YOLOTUP(const std::string & config_path, bool debug);

  std::list<Armor> detect(const cv::Mat & bgr_img, int frame_count) override;

  std::list<Armor> postprocess(
    double scale, cv::Mat & output, const cv::Mat & bgr_img, int frame_count) override;

private:
  std::string device_, model_path_;
  bool debug_, use_roi_, use_traditional_;

  static constexpr int input_size_ = 416;
  const float nms_threshold_ = 0.35;
  const float score_threshold_ = 0.2;
  double min_confidence_;

  ov::Core core_;
  ov::CompiledModel compiled_model_;

  cv::Rect roi_;
  cv::Point2f offset_;

  Detector detector_;

  bool check_name(const Armor & armor) const;

  cv::Point2f get_center_norm(const cv::Mat & bgr_img, const cv::Point2f & center) const;

  std::list<Armor> parse(
    double scale, int left, int top, cv::Mat & output, const cv::Mat & bgr_img, int frame_count);

  void draw_detections(const cv::Mat & img, const std::list<Armor> & armors, int frame_count) const;
};

}  // namespace auto_aim

#endif  //AUTO_AIM__YOLOTUP_HPP
