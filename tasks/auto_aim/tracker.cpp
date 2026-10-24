#include "tracker.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"

namespace auto_aim
{
Tracker::Tracker(const std::string & config_path, Solver & solver)
: solver_{solver},
  detect_count_(0),
  temp_lost_count_(0),
  state_{"lost"},
  pre_state_{"lost"},
  last_timestamp_(std::chrono::steady_clock::now()),
  omni_target_priority_{ArmorPriority::fifth}
{
  auto yaml = YAML::LoadFile(config_path);
  enemy_color_ = (yaml["enemy_color"].as<std::string>() == "red") ? Color::red : Color::blue;
  min_detect_count_ = yaml["min_detect_count"].as<int>();
  max_temp_lost_count_ = yaml["max_temp_lost_count"].as<int>();
  outpost_max_temp_lost_count_ = yaml["outpost_max_temp_lost_count"].as<int>();
  normal_temp_lost_count_ = max_temp_lost_count_;

  auto ekf = yaml["ekf"];
  ekf_params_.q_xyz_acc_var = ekf["q_xyz_acc_var"].as<double>();
  ekf_params_.q_yaw_acc_var = ekf["q_yaw_acc_var"].as<double>();
  ekf_params_.r_yaw         = ekf["r_yaw"].as<double>();
  ekf_params_.r_pitch       = ekf["r_pitch"].as<double>();
  ekf_params_.r_dist_base   = ekf["r_dist_base"].as<double>();

  auto load_robot_cfg = [&](const std::string & key) -> RobotConfig {
    auto node = ekf[key];
    auto p0_vec = node["p0"].as<std::vector<double>>();
    return {
      node["radius"].as<double>(),
      node["armor_num"].as<int>(),
      Eigen::Map<Eigen::VectorXd>(p0_vec.data(), p0_vec.size())
    };
  };
  outpost_cfg_ = load_robot_cfg("outpost");
  base_cfg_    = load_robot_cfg("base");
  normal_cfg_  = load_robot_cfg("normal");
}

std::string Tracker::state() const { return state_; }

std::list<Target> Tracker::track(
  std::list<Armor> & armors, std::chrono::steady_clock::time_point t, bool use_enemy_color)
{
  auto dt = tools::delta_time(t, last_timestamp_);
  last_timestamp_ = t;

  // 时间间隔过长，说明可能发生了相机离线
  if (state_ != "lost" && dt > 0.1) {
    tools::RemoteLogger::instance().log("WARN", "[Tracker] Large dt: {:.3f}s", dt);
    state_ = "lost";
  }
  // 过滤掉非我方装甲板
  armors.remove_if([&](const auto_aim::Armor & a) { return a.color != enemy_color_; });

  // 过滤前哨站顶部装甲板
  // armors.remove_if([this](const auto_aim::Armor & a) {
  //   return a.name == ArmorName::outpost &&
  //          solver_.oupost_reprojection_error(a, 27.5 * CV_PI / 180.0) <
  //            solver_.oupost_reprojection_error(a, -15 * CV_PI / 180.0);
  // });

  // 优先选择靠近图像中心的装甲板。
  // 原先硬编码 (1440/2, 1080/2)（带 TODO）—— 换相机/换分辨率后这条判据就失效。
  // 改用检测阶段算好的归一化中心 center_norm（4 个 YOLO 与传统 Detector 都会填），
  // 与分辨率无关；未填时默认为 (0,0)，排序时自然落到最后。
  armors.sort([](const Armor & a, const Armor & b) {
    const cv::Point2f img_center(0.5f, 0.5f);
    const auto distance_1 = cv::norm(a.center_norm - img_center);
    const auto distance_2 = cv::norm(b.center_norm - img_center);
    return distance_1 < distance_2;
  });

  // 按优先级排序，优先级最高在首位(优先级越高数字越小，1的优先级最高)
  armors.sort(
    [](const auto_aim::Armor & a, const auto_aim::Armor & b) { return a.priority < b.priority; });

  bool found;
  if (state_ == "lost") {
    found = set_target(armors, t);
  }

  else {
    found = update_target(armors, t);
  }

  state_machine(found);

  // 发散检测
  if (state_ != "lost" && target_.diverged()) {
    tools::RemoteLogger::instance().log("DEBUG", "[Tracker] Target diverged!");
    state_ = "lost";
    return {};
  }

  // 收敛效果检测：窗口内 NIS 失败**比例** >= 40% 就丢目标。
  // 原实现写的是 `accumulate(...) >= 0.4 * window_size`，即"失败条数 >= 4000"，
  // 而窗口长度就等于更新次数 —— 前 4000 次更新（60fps 约 66s）内该判据数学上
  // 永远不成立，跑飞的目标根本不会被丢。这里改成按当前窗口的样本数求比例，
  // 并要求至少 30 个样本（避免刚锁定时的少量噪声直接判死）。
  {
    const auto & nis_fails = target_.ekf().recent_nis_failures;
    if (nis_fails.size() >= 30) {
      const int n_fail = std::accumulate(nis_fails.begin(), nis_fails.end(), 0);
      if (n_fail >= 0.4 * static_cast<double>(nis_fails.size())) {
        tools::RemoteLogger::instance().log(
          "DEBUG", "[Target] Bad Converge Found!（最近 {}/{} 次更新 NIS 失败）", n_fail,
          nis_fails.size());
        state_ = "lost";
        return {};
      }
    }
  }

  if (state_ == "lost") return {};

  std::list<Target> targets = {target_};
  return targets;
}

std::tuple<omniperception::DetectionResult, std::list<Target>> Tracker::track(
  const std::vector<omniperception::DetectionResult> & detection_queue, std::list<Armor> & armors,
  std::chrono::steady_clock::time_point t, bool use_enemy_color)
{
  omniperception::DetectionResult switch_target{std::list<Armor>(), t, 0, 0};
  omniperception::DetectionResult temp_target{std::list<Armor>(), t, 0, 0};
  if (!detection_queue.empty()) {
    temp_target = detection_queue.front();
  }

  auto dt = tools::delta_time(t, last_timestamp_);
  last_timestamp_ = t;

  // 时间间隔过长，说明可能发生了相机离线
  if (state_ != "lost" && dt > 0.1) {
    tools::RemoteLogger::instance().log("WARN", "[Tracker] Large dt: {:.3f}s", dt);
    state_ = "lost";
  }

  // 优先选择靠近图像中心的装甲板。
  // 原先硬编码 (1440/2, 1080/2)（带 TODO）—— 换相机/换分辨率后这条判据就失效。
  // 改用检测阶段算好的归一化中心 center_norm（4 个 YOLO 与传统 Detector 都会填），
  // 与分辨率无关；未填时默认为 (0,0)，排序时自然落到最后。
  armors.sort([](const Armor & a, const Armor & b) {
    const cv::Point2f img_center(0.5f, 0.5f);
    const auto distance_1 = cv::norm(a.center_norm - img_center);
    const auto distance_2 = cv::norm(b.center_norm - img_center);
    return distance_1 < distance_2;
  });

  // 按优先级排序，优先级最高在首位(优先级越高数字越小，1的优先级最高)
  armors.sort([](const Armor & a, const Armor & b) { return a.priority < b.priority; });

  bool found;
  if (state_ == "lost") {
    found = set_target(armors, t);
  }

  // 此时主相机画面中出现了优先级更高的装甲板，切换目标
  else if (state_ == "tracking" && !armors.empty() && armors.front().priority < target_.priority) {
    found = set_target(armors, t);
    tools::RemoteLogger::instance().log("DEBUG", "auto_aim switch target to {}", ARMOR_NAMES[armors.front().name]);
  }

  // 此时全向感知相机画面中出现了优先级更高的装甲板，切换目标
  else if (
    state_ == "tracking" && !temp_target.armors.empty() &&
    temp_target.armors.front().priority < target_.priority && target_.convergened()) {
    state_ = "switching";
    switch_target = omniperception::DetectionResult{
      temp_target.armors, t, temp_target.delta_yaw, temp_target.delta_pitch};
    omni_target_priority_ = temp_target.armors.front().priority;
    found = false;
    tools::RemoteLogger::instance().log("DEBUG", "omniperception find higher priority target");
  }

  else if (state_ == "switching") {
    found = !armors.empty() && armors.front().priority == omni_target_priority_;
  }

  else if (state_ == "detecting" && pre_state_ == "switching") {
    found = set_target(armors, t);
  }

  else {
    found = update_target(armors, t);
  }

  pre_state_ = state_;
  // 更新状态机
  state_machine(found);

  // 发散检测
  if (state_ != "lost" && target_.diverged()) {
    tools::RemoteLogger::instance().log("DEBUG", "[Tracker] Target diverged!");
    state_ = "lost";
    return {switch_target, {}};  // 返回switch_target和空的targets
  }

  if (state_ == "lost") return {switch_target, {}};  // 返回switch_target和空的targets

  std::list<Target> targets = {target_};

  return {switch_target, targets};
}

void Tracker::state_machine(bool found)
{
  if (state_ == "lost") {
    if (!found) return;

    state_ = "detecting";
    detect_count_ = 1;
  }

  else if (state_ == "detecting") {
    if (found) {
      detect_count_++;
      if (detect_count_ >= min_detect_count_) state_ = "tracking";
    } else {
      detect_count_ = 0;
      state_ = "lost";
    }
  }

  else if (state_ == "tracking") {
    if (found) return;

    temp_lost_count_ = 1;
    state_ = "temp_lost";
  }

  else if (state_ == "switching") {
    if (found) {
      state_ = "detecting";
    } else {
      temp_lost_count_++;
      if (temp_lost_count_ > 200) state_ = "lost";
    }
  }

  else if (state_ == "temp_lost") {
    if (found) {
      state_ = "tracking";
    } else {
      temp_lost_count_++;
      if (target_.name == ArmorName::outpost)
        //前哨站的temp_lost_count需要设置的大一些
        max_temp_lost_count_ = outpost_max_temp_lost_count_;
      else
        max_temp_lost_count_ = normal_temp_lost_count_;

      if (temp_lost_count_ > max_temp_lost_count_) state_ = "lost";
    }
  }
}

bool Tracker::set_target(std::list<Armor> & armors, std::chrono::steady_clock::time_point t)
{
  if (armors.empty()) return false;

  auto & armor = armors.front();
  // 位姿不可信（远距离低置信度框的典型表现）时不建立目标：让状态机继续停在
  // lost/detecting，而不是用脏观测去初始化 EKF（Target 的 P0 很小，初始值错了
  // 后面很难拉回来）。
  if (!solver_.solve(armor)) return false;

  const RobotConfig * cfg;
  if (armor.name == ArmorName::outpost)   cfg = &outpost_cfg_;
  else if (armor.name == ArmorName::base) cfg = &base_cfg_;
  else                                     cfg = &normal_cfg_;

  target_ = Target(armor, t, cfg->radius, cfg->armor_num, cfg->p0, ekf_params_);

  return true;
}

bool Tracker::update_target(std::list<Armor> & armors, std::chrono::steady_clock::time_point t)
{
  target_.predict(t);

  int found_count = 0;
  double min_x = 1e10;  // 画面最左侧
  for (const auto & armor : armors) {
    if (armor.name != target_.name || armor.type != target_.armor_type) continue;
    found_count++;
    min_x = armor.center.x < min_x ? armor.center.x : min_x;
  }

  if (found_count == 0) return false;

  // 空间门控的基准：本帧预测的 N 块装甲板位置（必须在任何 update 之前取）
  const auto predicted = target_.armor_xyza_list();
  const double pred_dist = predicted.empty() ? 0.0 : predicted.front().head<2>().norm();
  // 容差随距离放宽：3m 处 ≈1.6m，8m 处 ≈2.6m —— 既容得下旋转/预测误差，
  // 又能把"另一台同类号敌车"（通常相距数米）挡在外面。
  const double gate = 1.0 + 0.2 * pred_dist;

  int updated = 0;
  for (auto & armor : armors) {
    if (
      armor.name != target_.name || armor.type != target_.armor_type
      //  || armor.center.x != min_x
    )
      continue;

    // 位姿被 Solver 判定为不可信 -> 本帧不拿它更新 EKF（空转一帧优于注入脏观测）
    if (!solver_.solve(armor)) continue;

    // 空间门控（2026-10 加固）：只接受落在"预测装甲板附近"的观测。
    // 原来只按 (name, type) 匹配，视野里一旦出现同类号同类型的第二块板（第二台敌车、
    // 误框、远近两个目标），就会把它的观测喂进同一个 EKF —— 现场 obs_dist 在 3~9m
    // 之间跳就是这种"串目标"的指纹，结果是位置状态被拉跑，armor_xyza_list() 会预测
    // 出 0.3m 的幽灵板，Planner 取最近的板去解弹道 -> 对着地面打近场。
    double d_armor2pred = std::numeric_limits<double>::infinity();
    for (const auto & p : predicted) {
      d_armor2pred = std::min(d_armor2pred, (armor.xyz_in_world - p.head<3>()).norm());
    }
    if (!(d_armor2pred < gate)) {
      tools::RemoteLogger::instance().log(
        "DEBUG", "[Tracker] 丢弃远离预测的观测：观测距离={:.2f}m 与预测最近距离={:.2f}m（门限 {:.2f}m）",
        armor.xyz_in_world.norm(), d_armor2pred, gate);
      continue;
    }

    target_.update(armor);
    ++updated;
  }

  // 全部被拒时按"没找到"处理，交给状态机计数（否则会在 target_ 上假装找到了）
  return updated > 0;
}

}  // namespace auto_aim