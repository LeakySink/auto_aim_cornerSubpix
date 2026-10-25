#ifndef AUTO_AIM__YOLOTUP_HPP
#define AUTO_AIM__YOLOTUP_HPP

#include <list>
#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <string>

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

  // 调试发布：把检测结果画到图上、经 RemoteLogger 发到 host（Watch 页按 name 选流）。
  // 由 yaml 的 publish_detections / publish_image_name 控制，缺省关闭。
  bool publish_ = false;
  std::string publish_name_ = "tup_detect";
  int save_frames_ = 0;   ///< >0 时把前 N 帧的全分辨率标注图存到 imgs/（调试用）
  int save_seq_ = 0;      ///< 存图序号（用自己的计数器：上层可能一直传 frame_count=-1）
  cv::Mat tmp_input_;  ///< 当帧 416×416 网络输入画布（<name>_input 话题用）

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
