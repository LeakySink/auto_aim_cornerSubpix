#include "yolotup.hpp"

#include <algorithm>
#include <filesystem>

#include <fmt/format.h>

#include "tasks/auto_aim/detector_publish.hpp"
#include "tools/img_tools.hpp"
#include "tools/ov_tensor_utils.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

namespace auto_aim
{
namespace
{
// TUP 模型颜色输出索引: 0=蓝 1=红 2=紫 3=无
const std::vector<Color> TUP_COLORS = {
  Color::blue, Color::red, Color::purple, Color::extinguish};

// TUP 模型类别输出索引
const std::vector<ArmorName> TUP_NAMES = {
  ArmorName::sentry,  ArmorName::one,  ArmorName::two,    ArmorName::three,
  ArmorName::four,    ArmorName::five, ArmorName::outpost, ArmorName::base};

// 各 stride 对应的 anchor 数量: 416/8=52, 416/16=26, 416/32=13
constexpr int GRID_SIZES[3] = {52, 26, 13};
constexpr int STRIDES[3] = {8, 16, 32};
constexpr int GRID_OFFSETS[3] = {0, 52 * 52, 52 * 52 + 26 * 26};
}  // namespace

YOLOTUP::YOLOTUP(const std::string & config_path, bool debug)
: debug_(debug), detector_(config_path, false)
{
  auto yaml = tools::load(config_path);

  model_path_ = tools::read_path(yaml, "tup_model_path");
  device_ = yaml["device"].as<std::string>();
  min_confidence_ = yaml["min_confidence"].as<double>();

  // 可选：把检测结果发布到图片（host Watch 页选流查看）。缺省关闭，不影响现有行为。
  if (yaml["publish_detections"] && yaml["publish_detections"].IsDefined()) {
    publish_ = yaml["publish_detections"].as<bool>();
  }
  if (yaml["publish_image_name"] && yaml["publish_image_name"].IsDefined()) {
    publish_name_ = yaml["publish_image_name"].as<std::string>();
  }
  if (yaml["save_detection_frames"] && yaml["save_detection_frames"].IsDefined()) {
    save_frames_ = yaml["save_detection_frames"].as<int>();
  }
  int x = 0, y = 0, width = 0, height = 0;
  x = yaml["roi"]["x"].as<int>();
  y = yaml["roi"]["y"].as<int>();
  width = yaml["roi"]["width"].as<int>();
  height = yaml["roi"]["height"].as<int>();
  use_roi_ = yaml["use_roi"].as<bool>();
  use_traditional_ = yaml["use_traditional"].as<bool>();
  roi_ = cv::Rect(x, y, width, height);
  offset_ = cv::Point2f(x, y);

  auto model = core_.read_model(model_path_);
  ov::preprocess::PrePostProcessor ppp(model);
  auto & input = ppp.input();

  input.tensor()
    .set_element_type(ov::element::u8)
    .set_shape({1, input_size_, input_size_, 3})
    .set_layout("NHWC")
    .set_color_format(ov::preprocess::ColorFormat::BGR);

  input.model().set_layout("NCHW");

  input.preprocess().convert_element_type(ov::element::f32);

  model = ppp.build();
  compiled_model_ = core_.compile_model(
    model, device_, ov::hint::performance_mode(ov::hint::PerformanceMode::LATENCY));
}

std::list<Armor> YOLOTUP::detect(const cv::Mat & raw_img, int frame_count)
{
  if (raw_img.empty()) {
    tools::RemoteLogger::instance().log("WARN", "Empty img!, camera drop!");
    return std::list<Armor>();
  }

  cv::Mat bgr_img;
  if (use_roi_) {
    if (roi_.width == -1) {  // -1 表示该维度不裁切
      roi_.width = raw_img.cols;
    }
    if (roi_.height == -1) {  // -1 表示该维度不裁切
      roi_.height = raw_img.rows;
    }
    bgr_img = raw_img(roi_);
  } else {
    bgr_img = raw_img;
  }

  auto scale = std::min(
    static_cast<double>(input_size_) / bgr_img.rows,
    static_cast<double>(input_size_) / bgr_img.cols);
  auto resize_h = static_cast<int>(bgr_img.rows * scale + 0.5);
  auto resize_w = static_cast<int>(bgr_img.cols * scale + 0.5);
  auto pad_h = input_size_ - resize_h;
  auto pad_w = input_size_ - resize_w;
  auto top = pad_h / 2;
  auto left = pad_w / 2;

  // preproces: 居中 letterbox, 灰色填充, 与训练时一致
  auto input = cv::Mat(input_size_, input_size_, CV_8UC3, cv::Scalar(114, 114, 114));
  auto roi = cv::Rect(left, top, resize_w, resize_h);
  cv::resize(bgr_img, input(roi), {resize_w, resize_h});
  tmp_input_ = input;  // 浅拷贝（cv::Mat 引用计数），供 <name>_input 话题画框图
  ov::Tensor input_tensor(ov::element::u8, {1, input_size_, input_size_, 3}, input.data);

  // infer
  auto infer_request = compiled_model_.create_infer_request();
  infer_request.set_input_tensor(input_tensor);
  infer_request.infer();

  // postprocess
  // 用 tools::to_mat_f32 包一层：它会先校验 rank==3 且元素类型为 f32 再构造
  // cv::Mat。原先直接取 output_shape[1]/[2] 并调用 data()，若模型输出维度/精度
  // 与假设不符（换模型、或走 GPU/NPU 插件），operator[] 越界、或按 float 访问
  // f16 缓冲区（只有一半大小）都会变成裸越界访问。校验失败返回空 Mat，
  // parse() 遇到 rows==0 直接返回空 list，等价于"本帧无检测结果"。
  auto output_tensor = infer_request.get_output_tensor();
  cv::Mat output = tools::to_mat_f32(output_tensor);
  if (output.empty()) return {};

  return parse(scale, left, top, output, bgr_img, frame_count);
}

std::list<Armor> YOLOTUP::parse(
  double scale, int left, int top, cv::Mat & output, const cv::Mat & bgr_img, int frame_count)
{
  // for each row: 4 角点偏移 + 置信度 + 4 颜色 + 8 类别
  std::vector<float> confidences;
  std::vector<cv::Rect> boxes;
  std::vector<Color> colors;
  std::vector<ArmorName> names;
  std::vector<std::vector<cv::Point2f>> armors_key_points;

  // 调试发布用：低于 score 阈值、但高于这个地板的候选也解码出来画框图，
  // 用来回答"是模型没检出，还是被 score/置信度门槛杀了"。
  constexpr float kRawCandFloor = 0.05f;
  std::vector<RawCand> raw_cands;

  auto to_img = [&](double net_x, double net_y) {
    return cv::Point2f((net_x - left) / scale, (net_y - top) / scale);
  };

  for (int r = 0; r < output.rows; r++) {
    const double score = output.at<float>(r, 8);  // 已过 sigmoid
    const bool above_threshold = (score >= score_threshold_);

    // 发布开启时，低于阈值但高于地板的候选也要走完解码（只用于画图）
    if (!above_threshold && !(publish_ && score >= kRawCandFloor)) continue;

    int idx = r, stride = STRIDES[0], grid_size = GRID_SIZES[0];
    if (r >= GRID_OFFSETS[1]) {
      idx = r - GRID_OFFSETS[1];
      stride = STRIDES[1];
      grid_size = GRID_SIZES[1];
    }
    if (r >= GRID_OFFSETS[2]) {
      idx = r - GRID_OFFSETS[2];
      stride = STRIDES[2];
      grid_size = GRID_SIZES[2];
    }
    auto gx = idx % grid_size;
    auto gy = idx / grid_size;

    // 颜色和类别独热向量
    cv::Mat color_scores = output.row(r).colRange(9, 13);     //color
    cv::Mat classes_scores = output.row(r).colRange(13, 21);  //num
    cv::Point class_id, color_id;
    double score_color, score_num;
    cv::minMaxLoc(classes_scores, NULL, &score_num, NULL, &class_id);
    cv::minMaxLoc(color_scores, NULL, &score_color, NULL, &color_id);

    // 角点解码: (offset + grid) * stride, 顺序 lt lb rb rt
    std::vector<cv::Point2f> armor_key_points;
    armor_key_points.push_back(
      to_img((output.at<float>(r, 0) + gx) * stride, (output.at<float>(r, 1) + gy) * stride));
    armor_key_points.push_back(
      to_img((output.at<float>(r, 6) + gx) * stride, (output.at<float>(r, 7) + gy) * stride));
    armor_key_points.push_back(
      to_img((output.at<float>(r, 4) + gx) * stride, (output.at<float>(r, 5) + gy) * stride));
    armor_key_points.push_back(
      to_img((output.at<float>(r, 2) + gx) * stride, (output.at<float>(r, 3) + gy) * stride));

    float min_x = armor_key_points[0].x;
    float max_x = armor_key_points[0].x;
    float min_y = armor_key_points[0].y;
    float max_y = armor_key_points[0].y;

    for (int i = 1; i < armor_key_points.size(); i++) {
      if (armor_key_points[i].x < min_x) min_x = armor_key_points[i].x;
      if (armor_key_points[i].x > max_x) max_x = armor_key_points[i].x;
      if (armor_key_points[i].y < min_y) min_y = armor_key_points[i].y;
      if (armor_key_points[i].y > max_y) max_y = armor_key_points[i].y;
    }

    cv::Rect rect(min_x, min_y, max_x - min_x, max_y - min_y);

    if (publish_ && score >= kRawCandFloor) {
      RawCand cand;
      cand.box = rect;
      cand.score = static_cast<float>(score);
      cand.cls = class_id.x;
      cand.col = color_id.x;
      raw_cands.push_back(cand);
    }
    if (!above_threshold) continue;  // 低于阈值的候选只用于画图，不进 NMS/装甲板列表

    confidences.emplace_back(score);
    boxes.emplace_back(rect);
    colors.emplace_back(TUP_COLORS[color_id.x]);
    names.emplace_back(TUP_NAMES[class_id.x]);
    armors_key_points.emplace_back(armor_key_points);
  }

  std::vector<int> indices;
  cv::dnn::NMSBoxes(boxes, confidences, score_threshold_, nms_threshold_, indices);

  std::list<Armor> armors;
  for (const auto & i : indices) {
    if (use_roi_) {
      armors.emplace_back(
        colors[i], names[i], confidences[i], boxes[i], armors_key_points[i], offset_);
    } else {
      armors.emplace_back(colors[i], names[i], confidences[i], boxes[i], armors_key_points[i]);
    }
  }

  for (auto it = armors.begin(); it != armors.end();) {
    if (!check_name(*it)) {
      it = armors.erase(it);
      continue;
    }

    // 使用传统方法二次矫正角点
    if (use_traditional_) detector_.detect(*it, bgr_img);

    it->center_norm = get_center_norm(bgr_img, it->center);
    ++it;
  }

  // ---- 调试发布：把全部候选 + 保留框画到图上（host Watch 页）----
  // 注意：即使一个候选都没有也要发（帧上只有头部文字），否则"没检出"和"发布坏了"
  // 在现场看起来是一样的 —— raw=0 本身就是关键信息。
  if (publish_) {
    // 标记哪些候选最终被保留（Armor::box 与候选框是同一个 rect，可直接比较）
    for (auto & c : raw_cands) {
      for (const auto & a : armors) {
        if (c.box == a.box) {
          c.kept = true;
          break;
        }
      }
    }
    // 只画分数最高的若干条，避免远距离小目标把画面刷满
    std::sort(raw_cands.begin(), raw_cands.end(), [](const RawCand & a, const RawCand & b) {
      return a.score > b.score;
    });
    if (raw_cands.size() > 16) raw_cands.resize(16);

    // 拒因统计 + 最大框像素尺寸：不用逐条读标签也能判断卡在哪一环；
    // 框像素尺寸还能反推距离（d ≈ fx·0.135/w，fx 见 yaml 的 camera_matrix）
    int n_drop_score = 0, n_drop_conf = 0;
    int max_w = 0, max_h = 0;
    for (const auto & c : raw_cands) {
      if (c.kept) continue;
      if (c.score < score_threshold_) {
        ++n_drop_score;
      } else if (c.score <= min_confidence_) {
        ++n_drop_conf;
      }
    }
    for (const auto & a : armors) {
      max_w = std::max(max_w, a.box.width);
      max_h = std::max(max_h, a.box.height);
    }
    for (const auto & c : raw_cands) {
      max_w = std::max(max_w, c.box.width);
      max_h = std::max(max_h, c.box.height);
    }

    const std::string header = fmt::format(
      "tup[{}] raw={} kept={} drop[s={} c={}] box={}x{}px score>={:.2f} conf>{:.2f} "
      "scale={:.3f} {}x{}",
      frame_count, raw_cands.size(), armors.size(), n_drop_score, n_drop_conf, max_w, max_h,
      score_threshold_, min_confidence_, scale, bgr_img.cols, bgr_img.rows);

    std::string save_main, save_input;
    if (save_frames_ > 0 && save_seq_ < save_frames_) {
      std::error_code ec;
      std::filesystem::create_directories("imgs", ec);
      const int seq = save_seq_++;  // 用自己的计数器（上层可能常量传 frame_count=-1）
      save_main = fmt::format("imgs/tup_{:04d}.png", seq);
      save_input = fmt::format("imgs/tup_{:04d}_input.png", seq);
    }
    publish_detections(
      bgr_img, raw_cands, armors, publish_name_, header, score_threshold_, min_confidence_,
      save_main);

    if (!tmp_input_.empty()) {
      publish_input_view(
        tmp_input_, raw_cands, armors, scale, left, top, publish_name_ + "_input",
        fmt::format(
          "net {}x{} scale={:.3f} left={} top={}", tmp_input_.cols, tmp_input_.rows, scale, left,
          top),
        score_threshold_, min_confidence_, save_input);
    }
  }

  if (debug_) draw_detections(bgr_img, armors, frame_count);

  return armors;
}

bool YOLOTUP::check_name(const Armor & armor) const
{
  return armor.confidence > min_confidence_;
}

cv::Point2f YOLOTUP::get_center_norm(const cv::Mat & bgr_img, const cv::Point2f & center) const
{
  auto h = bgr_img.rows;
  auto w = bgr_img.cols;
  return {center.x / w, center.y / h};
}

void YOLOTUP::draw_detections(
  const cv::Mat & img, const std::list<Armor> & armors, int frame_count) const
{
  auto detection = img.clone();
  tools::draw_text(detection, fmt::format("[{}]", frame_count), {10, 30}, {255, 255, 255});
  for (const auto & armor : armors) {
    auto info = fmt::format(
      "{:.2f} {} {}", armor.confidence, COLORS[armor.color], ARMOR_NAMES[armor.name]);
    tools::draw_points(detection, armor.points, {0, 255, 0});
    tools::draw_text(detection, info, armor.center, {0, 255, 0});
  }

  if (use_roi_) {
    cv::Scalar green(0, 255, 0);
    cv::rectangle(detection, roi_, green, 2);
  }
  cv::resize(detection, detection, {}, 0.5, 0.5);  // 显示时缩小图片尺寸
  cv::imshow("detection", detection);
}

std::list<Armor> YOLOTUP::postprocess(
  double scale, cv::Mat & output, const cv::Mat & bgr_img, int frame_count)
{
  // 单线程检测流程的 postprocess 入口(多线程检测暂不支持 tup)
  return parse(scale, 0, 0, output, bgr_img, frame_count);
}

}  // namespace auto_aim
