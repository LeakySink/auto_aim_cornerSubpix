#include "mt_detector.hpp"

#include <cstring>  // std::memcpy

#include "tools/yaml.hpp"

namespace auto_aim
{
namespace multithread
{

MultiThreadDetector::MultiThreadDetector(const std::string & config_path, bool debug)
: yolo_(config_path, debug)
{
  auto yaml = tools::load(config_path);
  auto yolo_name = yaml["yolo_name"].as<std::string>();
  auto model_path = tools::project_path(yaml[yolo_name + "_model_path"].as<std::string>());
  device_ = yaml["device"].as<std::string>();

  auto model = core_.read_model(model_path);
  ov::preprocess::PrePostProcessor ppp(model);
  auto & input = ppp.input();

  input.tensor()
    .set_element_type(ov::element::u8)
    .set_shape({1, 640, 640, 3})  // TODO
    .set_layout("NHWC")
    .set_color_format(ov::preprocess::ColorFormat::BGR);

  input.model().set_layout("NCHW");

  input.preprocess()
    .convert_element_type(ov::element::f32)
    .convert_color(ov::preprocess::ColorFormat::RGB)
    // .resize(ov::preprocess::ResizeAlgorithm::RESIZE_LINEAR)
    .scale(255.0);

  model = ppp.build();
  compiled_model_ = core_.compile_model(
    model, device_, ov::hint::performance_mode(ov::hint::PerformanceMode::THROUGHPUT));

  tools::RemoteLogger::instance().log("INFO", "[MultiThreadDetector] initialized !");
}

void MultiThreadDetector::push(cv::Mat img, std::chrono::steady_clock::time_point t)
{
  auto x_scale = static_cast<double>(640) / img.rows;
  auto y_scale = static_cast<double>(640) / img.cols;
  auto scale = std::min(x_scale, y_scale);
  auto h = static_cast<int>(img.rows * scale);
  auto w = static_cast<int>(img.cols * scale);

  // preproces
  auto input = cv::Mat(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
  auto roi = cv::Rect(0, 0, w, h);
  cv::resize(img, input(roi), {w, h});

  auto input_port = compiled_model_.input();
  auto infer_request = compiled_model_.create_infer_request();

  // 不能再用 ov::Tensor(type, shape, void*) 把 input.data 直接包成输入张量：
  // 这个重载**不接管**缓冲区所有权，只是借用指针。push() 返回后局部 input 析构、
  // 缓冲区释放，而 start_async() 的推理通常还没读完输入 —— OpenVINO 的工作线程
  // 会继续读已释放内存，构成跨线程 use-after-free。
  // 崩溃点在 OV 内部线程，core 里看不到本项目栈帧，极难定位。
  //
  // 改为拷进 infer_request 自有的输入张量：该张量与 infer_request 同生命周期，
  // 会一直存活到 wait() 返回之后，异步读取安全。
  auto input_tensor = infer_request.get_input_tensor();
  if (input_tensor.get_element_type() != ov::element::u8 || input_tensor.get_shape().size() != 4) {
    tools::RemoteLogger::instance().log(
      "ERROR", "[MultiThreadDetector] unexpected input tensor type/rank");
    return;
  }
  const auto & in_shape = input_tensor.get_shape();  // NHWC = [1,640,640,3]
  const size_t in_bytes = in_shape[1] * in_shape[2] * in_shape[3];
  const size_t src_bytes = static_cast<size_t>(input.total()) * input.elemSize();
  if (src_bytes != in_bytes) {
    tools::RemoteLogger::instance().log(
      "ERROR", "[MultiThreadDetector] input size mismatch {} vs {}", src_bytes, in_bytes);
    return;
  }
  std::memcpy(input_tensor.data<uint8_t>(), input.data, in_bytes);

  infer_request.start_async();
  queue_.push({img.clone(), t, std::move(infer_request)});
}

std::tuple<std::list<Armor>, std::chrono::steady_clock::time_point> MultiThreadDetector::pop()
{
  auto [img, t, infer_request] = queue_.pop();
  infer_request.wait();

  // postprocess
  auto output_tensor = infer_request.get_output_tensor();
  auto output_shape = output_tensor.get_shape();
  cv::Mat output(output_shape[1], output_shape[2], CV_32F, output_tensor.data());
  auto x_scale = static_cast<double>(640) / img.rows;
  auto y_scale = static_cast<double>(640) / img.cols;
  auto scale = std::min(x_scale, y_scale);
  auto armors = yolo_.postprocess(scale, output, img, 0);  //暂不支持ROI

  return {std::move(armors), t};
}

std::tuple<cv::Mat, std::list<Armor>, std::chrono::steady_clock::time_point>
MultiThreadDetector::debug_pop()
{
  auto [img, t, infer_request] = queue_.pop();
  infer_request.wait();

  // postprocess
  auto output_tensor = infer_request.get_output_tensor();
  auto output_shape = output_tensor.get_shape();
  cv::Mat output(output_shape[1], output_shape[2], CV_32F, output_tensor.data());
  auto x_scale = static_cast<double>(640) / img.rows;
  auto y_scale = static_cast<double>(640) / img.cols;
  auto scale = std::min(x_scale, y_scale);
  auto armors = yolo_.postprocess(scale, output, img, 0);  //暂不支持ROI

  return {img, std::move(armors), t};
}

}  // namespace multithread

}  // namespace auto_aim
