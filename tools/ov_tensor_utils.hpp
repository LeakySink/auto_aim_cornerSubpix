#ifndef TOOLS__OV_TENSOR_UTILS_HPP
#define TOOLS__OV_TENSOR_UTILS_HPP

#include <openvino/openvino.hpp>
#include <opencv2/core.hpp>

namespace tools
{

/// 把 OpenVINO 输出张量安全地包成 CV_32F 的 cv::Mat。
///
/// 各 yolo 后处理原先统一写成：
///
///     auto output_tensor = infer_request.get_output_tensor();
///     auto output_shape  = output_tensor.get_shape();
///     cv::Mat output(output_shape[1], output_shape[2], CV_32F, output_tensor.data());
///
/// 这条语句隐含两个**从未被校验**的前提：
///
/// 1. rank == 3。ov::Shape 继承 std::vector<size_t>，operator[] 在 Release
///    （-DNDEBUG）下不做边界检查。若模型实际输出 rank < 3，读到的是垃圾 size_t，
///    交给 cv::Mat(rows, cols, ...) 后：
///      - 被当成负数 -> CV_Assert 抛 cv::Exception，主循环无 try/catch ->
///        std::terminate -> SIGABRT；
///      - 巨大正数 -> 断言通过，声明出一个尺寸巨大的 Mat，随后 at<float>()
///        是裸越界读 -> SIGSEGV。
///
/// 2. 元素类型是 f32。ov::Tensor::data() 的**非模板**重载返回裸 void* 且不做
///    类型检查。若实际是 f16/bf16，缓冲区只有一半大小，按 float 访问尾部必然
///    越界。configs/ascento.yaml 用 device: GPU、classifier 用 "AUTO" 编译，
///    这两种情况最容易踩到。
///
/// 现状（已核对 assets/*.xml 的真实输出维度）：
///     yolov5 = [1,25200,22]、yolov8 = [1,14,3549]、
///     yolo11 = [1,50,8400]、tup = [1,3549,21]、yolo11_buff = [1,17,8400]
/// 全部 rank=3 且 FP32，因此现在**不会触发**。但换模型或换推理设备即崩，
/// 属于典型的"改配置才炸"的潜伏缺陷。
///
/// 校验失败时返回空 Mat（cv::Mat()）。各 parse() 都以 `for (r = 0; r <
/// output.rows; ++r)` 开始，rows == 0 时循环不执行、直接返回空 list，
/// 等价于"本帧无检测结果"，不会崩。
inline cv::Mat to_mat_f32(const ov::Tensor & tensor)
{
  const auto & shape = tensor.get_shape();

  if (shape.size() != 3) return {};
  if (tensor.get_element_type() != ov::element::f32) return {};

  // data<const float>() 返回 const 指针，而 cv::Mat 的构造函数需要 void*，
  // 故去掉 const。这里只读取，不写入。
  return cv::Mat(
    static_cast<int>(shape[1]), static_cast<int>(shape[2]), CV_32F,
    const_cast<float *>(tensor.data<const float>()));
}

}  // namespace tools

#endif  // TOOLS__OV_TENSOR_UTILS_HPP
