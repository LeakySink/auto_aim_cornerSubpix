#ifndef TOOLS__BALLISTICS_POLY_HPP
#define TOOLS__BALLISTICS_POLY_HPP

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tools
{

/// ballistic-main（离线弹道拟合）产物的运行时模型。
///
/// ballistic-main 用真实气动参数（Cd/S/m/ρ、动压 q）做数值积分，把
/// (水平距离 x [m], 高度差 y [m]) -> (pitch, fly_time) 拟合成 **三次多项式**，
/// 每个弹速 V 一组 10 个系数，存成 `V,pitch,time` 的 CSV（`data/small.csv` 等）。
/// 其 `test.py:predict()` 的基函数顺序固定为：
///
///   f(x, y) = c0 + c1·x + c2·y + c3·x² + c4·xy + c5·y²
///                + c6·x³ + c7·x²y + c8·xy² + c9·y³
///
/// 注意单位与符号（接线时最容易错的地方）：
///   - `pitch` 列的单位是 **度**，且 **抬头为正**；本类对外统一转成 **弧度**
///   - `time` 列的单位是 **秒**
///   - `x` 是水平距离；`y` 是目标相对**枪口**的高度差（与 trajectory.cpp 的 h 同义）
///
/// 设计约定：
///   - 只读、无第三方依赖（不 include OpenCV/Eigen/yaml-cpp），便于离线对拍验证
///   - 超出拟合表覆盖范围（x/y）时 `solve()` 返回 false，由调用方回退 RK4，
///     不做三阶多项式外推（外推误差不可控）
///   - 弹速超出表的上下限时**按端点夹取**并在 note 里说明（比整段回退 RK4 更安全）
struct BallisticsRow
{
  double v = 0.0;                     ///< 弹速 m/s
  std::array<double, 10> pitch{};     ///< 10 个系数，输出单位"度"
  std::array<double, 10> time{};      ///< 10 个系数，输出单位 s
};

class BallisticsPoly
{
public:
  static constexpr std::size_t kCoefNum = 10;
  static constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;

  /// 从 ballistic-main 的 CSV（表头 `V,pitch,time`，后两列是 `[...]` 形式的 10 系数）载入。
  /// 失败时返回 false，并把原因写进 err（若给出）。
  bool load_csv(const std::string & path, std::string * err = nullptr);

  /// 直接喂入若干行（供 yaml 内联系数使用）。会按 v 升序排序。
  bool load_rows(std::vector<BallisticsRow> rows, std::string * err = nullptr);

  /// 拟合表覆盖范围（默认取 ballistic-main 的网格：x∈[0.5,15] m，y∈[-2,2] m）
  void set_range(double x_min, double x_max, double y_min, double y_max);

  bool empty() const { return rows_.empty(); }
  std::size_t size() const { return rows_.size(); }
  double v_min() const { return rows_.empty() ? 0.0 : rows_.front().v; }
  double v_max() const { return rows_.empty() ? 0.0 : rows_.back().v; }
  double x_min() const { return x_min_; }
  double x_max() const { return x_max_; }
  double y_min() const { return y_min_; }
  double y_max() const { return y_max_; }

  /// 求解。成功时写 pitch_rad（抬头为正，rad）与 fly_time（s）。
  /// 失败（表为空 / 非有限入参 / d 或 h 超出表范围 / 结果非有限）返回 false。
  /// note 里给出可读的原因或提示（弹速夹取、范围外等）。
  bool solve(
    double v0, double d, double h, double & pitch_rad, double & fly_time,
    std::string * note = nullptr) const;

  /// 基函数求值（与 ballistic-main/test.py 的 predict 完全一致）
  static double eval(const std::array<double, kCoefNum> & c, double x, double y);

private:
  std::vector<BallisticsRow> rows_;  ///< 按 v 升序
  double x_min_ = 0.5, x_max_ = 15.0, y_min_ = -2.0, y_max_ = 2.0;
};

/// ---- 全局模型（进程内单次安装）----
/// Trajectory 构造时优先使用这里安装的模型，未安装或超出范围时回退内置 RK4。
void install_ballistics_model(std::shared_ptr<const BallisticsPoly> model);
std::shared_ptr<const BallisticsPoly> ballistics_model();

/// 日志出口（默认静默）。装到 RemoteLogger 后，车上能看到"多项式生效/回退 RK4"的原因。
void set_ballistics_logger(std::function<void(const std::string &)> logger);

/// 内部用：按**类别**限流的日志（同类 2s 内只打一次）。
/// 注意必须按类别而不是按整条消息去重：回退日志里的 d/h 每帧都在变，
/// 按整条消息去重等于没限流（现场实测每帧刷好几行）。
void ballistics_log(const std::string & category, const std::string & msg);

/// 从 yaml 读配置并安装模型（实现在 ballistics_config.cpp，依赖 yaml-cpp）。
/// 支持的键（全部可选，缺省 = 完全保持原行为）：
///   ballistics_model:        poly | rk4        （缺省 rk4）
///   ballistics_csv:          assets/ballistic/small.csv
///   ballistics_x_range:      [0.5, 15.0]
///   ballistics_y_range:      [-2.0, 2.0]
///   ballistics_pitch_coef:   [10 个数]         （与 time_coef 一起给出时优先于 csv）
///   ballistics_time_coef:    [10 个数]
///   ballistics_v:            22.0              （内联系数对应的弹速）
void install_ballistics_from_config(const std::string & config_path);

}  // namespace tools

#endif  // TOOLS__BALLISTICS_POLY_HPP
