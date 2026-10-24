#include "ballistics_poly.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

namespace tools
{
namespace
{
std::string fmt_num(double v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

void log(const char * level, const std::string & msg)
{
  RemoteLogger::instance().log(level, "{}", msg);
}

bool read_range(const YAML::Node & node, double & lo, double & hi)
{
  if (!node || !node.IsSequence() || node.size() != 2) return false;
  lo = node[0].as<double>();
  hi = node[1].as<double>();
  return hi > lo;
}
}  // namespace

void install_ballistics_from_config(const std::string & config_path)
{
  // 幂等：同一进程里同一个 config 只装一次（Planner / Aimer 构造函数都会调）
  static std::string installed_for;
  if (installed_for == config_path) return;
  installed_for = config_path;

  // 把模块内的日志接到 RemoteLogger（车上能看，host UI 也能看）
  set_ballistics_logger([](const std::string & msg) { log("DEBUG", msg); });

  auto yaml = tools::load(config_path);

  std::string model_name = "rk4";
  if (yaml["ballistics_model"] && yaml["ballistics_model"].IsDefined()) {
    model_name = yaml["ballistics_model"].as<std::string>();
  }

  if (model_name != "poly") {
    install_ballistics_model(nullptr);  // 明确走内置 RK4
    log("INFO", "[Ballistics] 弹道模型 = RK4(内置解析解+RK4)，未启用多项式表");
    return;
  }

  auto model = std::make_shared<BallisticsPoly>();

  double x_min = 0.5, x_max = 15.0, y_min = -2.0, y_max = 2.0;
  double tmp_lo = 0.0, tmp_hi = 0.0;
  if (read_range(yaml["ballistics_x_range"], tmp_lo, tmp_hi)) {
    x_min = tmp_lo;
    x_max = tmp_hi;
  }
  if (read_range(yaml["ballistics_y_range"], tmp_lo, tmp_hi)) {
    y_min = tmp_lo;
    y_max = tmp_hi;
  }
  model->set_range(x_min, x_max, y_min, y_max);

  bool ok = false;
  std::string err;

  const bool has_inline =
    (yaml["ballistics_pitch_coef"] && yaml["ballistics_pitch_coef"].IsSequence() &&
     yaml["ballistics_time_coef"] && yaml["ballistics_time_coef"].IsSequence());

  if (has_inline) {
    const auto p = yaml["ballistics_pitch_coef"].as<std::vector<double>>();
    const auto t = yaml["ballistics_time_coef"].as<std::vector<double>>();
    if (p.size() != BallisticsPoly::kCoefNum || t.size() != BallisticsPoly::kCoefNum) {
      err = "内联 10 系数个数不对：pitch=" + std::to_string(p.size()) +
            " time=" + std::to_string(t.size());
    } else {
      BallisticsRow row;
      row.v = (yaml["ballistics_v"] && yaml["ballistics_v"].IsDefined())
                ? yaml["ballistics_v"].as<double>()
                : 22.0;
      std::copy(p.begin(), p.end(), row.pitch.begin());
      std::copy(t.begin(), t.end(), row.time.begin());
      ok = model->load_rows({row}, &err);
    }
  } else if (yaml["ballistics_csv"] && yaml["ballistics_csv"].IsDefined()) {
    const auto path = tools::project_path(yaml["ballistics_csv"].as<std::string>());
    ok = model->load_csv(path, &err);
    err = err + "（文件 " + path + "）";
  } else {
    err = "ballistics_model=poly 但既没给 ballistics_csv，也没给内联的 ballistics_pitch_coef/time_coef";
  }

  if (!ok) {
    // 关键安全阀：配置/文件有问题时**不崩、不静默变形**，退回 RK4 并把原因写清楚
    install_ballistics_model(nullptr);
    log("ERROR", "[Ballistics] 多项式表加载失败，已回退 RK4：" + err);
    return;
  }

  install_ballistics_model(model);
  log(
    "INFO", "[Ballistics] 弹道模型 = poly（ballistics-main 离线拟合），行数=" +
              std::to_string(model->size()) + " V∈[" + fmt_num(model->v_min()) + "," +
              fmt_num(model->v_max()) + "] m/s，拟合范围 d∈[" + fmt_num(x_min) + "," +
              fmt_num(x_max) + "] m h∈[" + fmt_num(y_min) + "," + fmt_num(y_max) +
              "] m；超出该范围将回退 RK4");
}

}  // namespace tools
