#ifndef AUTO_AIM__DETECTOR_PUBLISH_HPP
#define AUTO_AIM__DETECTOR_PUBLISH_HPP

// 把检测结果"发布到图片"，供 host 端（host/start.sh + Watch 页）远程查看。
//
// 设计要点：
//   - header-only / inline，不引入新依赖（OpenCV + fmt + RemoteLogger）
//   - 每个检测器发布两路图（话题名由 yaml 的 publish_image_name 决定）：
//       <name>        原图：全部候选框(黄，带拒因) + 保留框(绿) + 4 个关键点 + 头部阈值信息
//       <name>_input  网络输入画布(如 416×416)：映射后的同一批框
//                     —— 一眼看出"模型看到的装甲板只有几个像素"
//   - 候选框带拒因（drop:score / drop:conf / KEEP），直接回答"是模型没检出还是被门槛杀了"
//   - 话题名首次出现时会被 RemoteLogger 登记，host 会周期性收到 img_streams 目录，
//     因此新增话题无需改 host 侧代码，Watch 页里直接选即可

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <opencv2/opencv.hpp>

#include "armor.hpp"
#include "tools/img_tools.hpp"
#include "tools/remote_logger.hpp"

namespace auto_aim
{

/// 网络输出的原始候选（未经阈值过滤），仅用于画图/诊断
struct RawCand
{
  cv::Rect box;          ///< 与 Armor::box 同一坐标系（原图整像素），便于按框匹配
  float score = 0.f;     ///< 置信度（已过 sigmoid）
  int cls = -1;          ///< 类别下标（TUP_NAMES / armor_properties）
  int col = -1;          ///< 颜色下标
  bool kept = false;     ///< 是否最终被保留（通过了全部过滤）
};

namespace detail
{
inline std::string name_str(ArmorName n)
{
  const int i = static_cast<int>(n);
  return (i >= 0 && i < static_cast<int>(ARMOR_NAMES.size())) ? ARMOR_NAMES[i] : "?";
}

inline std::string color_str(Color c)
{
  const int i = static_cast<int>(c);
  return (i >= 0 && i < static_cast<int>(COLORS.size())) ? COLORS[i] : "?";
}

inline std::string type_str(ArmorType t)
{
  const int i = static_cast<int>(t);
  return (i >= 0 && i < static_cast<int>(ARMOR_TYPES.size())) ? ARMOR_TYPES[i] : "?";
}

/// 候选被拒的原因（TUP 只有 score 与 min_confidence 两道 NN 侧门槛）
inline const char * reject_reason(const RawCand & c, float score_threshold, double min_confidence)
{
  if (c.kept) return "KEEP";
  if (c.score < score_threshold) return "drop:score";
  if (c.score <= min_confidence) return "drop:conf";
  return "drop:other";
}

inline void put_label(cv::Mat & img, const std::string & text, const cv::Rect & box,
                      const cv::Scalar & color, double font_scale, int thickness)
{
  const int y = std::max(12, box.y - 4);
  tools::draw_text(img, text, cv::Point(box.x, y), color, font_scale, thickness);
}
}  // namespace detail

/// 原图上的检测结果图
inline void publish_detections(
  const cv::Mat & img, const std::vector<RawCand> & raws, const std::list<Armor> & armors,
  const std::string & topic, const std::string & header, float score_threshold,
  double min_confidence, const std::string & save_path = {})
{
  if (img.empty()) return;

  cv::Mat canvas = img.clone();

  // 被拒候选：黄色细框 + "分数 类别 拒因"
  for (const auto & c : raws) {
    if (c.kept) continue;  // 保留的用绿框单独画，避免同一框两种颜色
    const cv::Scalar yellow(0, 200, 255);
    cv::rectangle(canvas, c.box, yellow, 1);
    detail::put_label(
      canvas, fmt::format("{:.2f} c{} {}", c.score, c.cls, detail::reject_reason(c, score_threshold, min_confidence)),
      c.box, yellow, 0.5, 1);
  }

  // 保留的装甲板：绿框 + 4 个关键点 + "置信度 颜色 名称 类型"
  for (const auto & a : armors) {
    const cv::Scalar green(0, 255, 0);
    cv::rectangle(canvas, a.box, green, 2);
    tools::draw_points(canvas, a.points, green, 2);
    detail::put_label(
      canvas,
      fmt::format("{:.2f} {} {} {}", a.confidence, detail::color_str(a.color), detail::name_str(a.name),
                  detail::type_str(a.type)),
      a.box, green, 0.6, 2);
  }

  tools::draw_text(canvas, header, cv::Point(10, 30), cv::Scalar(255, 255, 255), 0.8, 2);
  // 需要时存一份**全分辨率** PNG：UDP 发布的图会被压到 img_width(640)+JPEG，
  // 标签和角点细节看不清；本地存档用于事后逐像素分析
  if (!save_path.empty()) cv::imwrite(save_path, canvas);
  tools::RemoteLogger::instance().plot_image(canvas, {{"name", topic}});
}

/// 网络输入画布上的同一批框（left/top 是 letterbox 的填充偏移，scale 是缩放系数）
inline void publish_input_view(
  const cv::Mat & input, const std::vector<RawCand> & raws, const std::list<Armor> & armors,
  double scale, double left, double top, const std::string & topic, const std::string & header,
  float score_threshold, double min_confidence, const std::string & save_path = {})
{
  if (input.empty() || !(scale > 0.0)) return;

  cv::Mat canvas = input.clone();
  const auto to_net = [&](const cv::Rect & b) {
    return cv::Rect(
      cvRound(b.x * scale + left), cvRound(b.y * scale + top), std::max(1, cvRound(b.width * scale)),
      std::max(1, cvRound(b.height * scale)));
  };

  for (const auto & c : raws) {
    if (c.kept) continue;
    const cv::Scalar yellow(0, 200, 255);
    const auto nb = to_net(c.box);
    cv::rectangle(canvas, nb, yellow, 1);
    detail::put_label(canvas, fmt::format("{:.2f}", c.score), nb, yellow, 0.4, 1);
  }

  for (const auto & a : armors) {
    const cv::Scalar green(0, 255, 0);
    const auto nb = to_net(a.box);
    cv::rectangle(canvas, nb, green, 1);
    tools::draw_points(canvas, a.points, green, 1);
  }

  tools::draw_text(canvas, header, cv::Point(6, 20), cv::Scalar(255, 255, 255), 0.6, 2);
  if (!save_path.empty()) cv::imwrite(save_path, canvas);
  tools::RemoteLogger::instance().plot_image(canvas, {{"name", topic}});
}

}  // namespace auto_aim

#endif  // AUTO_AIM__DETECTOR_PUBLISH_HPP
