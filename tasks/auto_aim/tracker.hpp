#ifndef AUTO_AIM__TRACKER_HPP
#define AUTO_AIM__TRACKER_HPP

#include <Eigen/Dense>
#include <chrono>
#include <list>
#include <map>
#include <set>
#include <array>
#include <string>

#include "armor.hpp"
#include "solver.hpp"
#include "target.hpp"
#include "tasks/omniperception/perceptron.hpp"
#include "tools/thread_safe_queue.hpp"

namespace auto_aim
{
class Tracker
{
public:
  Tracker(const std::string & config_path, Solver & solver);

  std::string state() const;

  std::list<Target> track(
    std::list<Armor> & armors, std::chrono::steady_clock::time_point t,
    bool use_enemy_color = true);

  std::tuple<omniperception::DetectionResult, std::list<Target>> track(
    const std::vector<omniperception::DetectionResult> & detection_queue, std::list<Armor> & armors,
    std::chrono::steady_clock::time_point t, bool use_enemy_color = true);

private:
  struct RobotConfig {
    double radius;
    int armor_num;
    Eigen::VectorXd p0;
  };

  Solver & solver_;
  Color enemy_color_;
  int min_detect_count_;
  int max_temp_lost_count_;
  int detect_count_;
  int temp_lost_count_;
  int outpost_max_temp_lost_count_;
  int normal_temp_lost_count_;
  std::string state_, pre_state_;
  Target target_;
  std::chrono::steady_clock::time_point last_timestamp_;
  ArmorPriority omni_target_priority_;

  EkfParams ekf_params_;
  RobotConfig outpost_cfg_;

  /// 几何量缓存：键是 `ArmorName` 的整数值。
  ///
  /// 动机：r/l/h 每帧只有一块板可见，可观测性很弱，**重新捕获时要花十几帧
  /// 重新收敛**。而同一个前哨站在一局里几何是固定的，没有理由每次重捕都从
  /// 配置默认值重新学。
  ///
  /// 写入条件：目标收敛（`convergened()`）时。
  /// 失效条件：`diverged()` 时清除；读取时再做一次范围校验
  /// （把脏缓存当先验比不缓存更糟）。
  std::map<int, GeoInit> geo_cache_;

  // ---- 前哨站首板歧义：3 假设并行 ----
  //
  // 前哨站只有一块板同时可见，`id` 又由首次观测决定 —— 第一帧根本不知道
  // 看到的是 id0/id1/id2。而模型里 `z_i = x[4] - i·h`，假设错一个 k，
  // 中心高度就差 k·h（最大 204 mm = 1.5 个板高差），EKF 只能用 h<0 吸收。
  //
  // 仿真（sim_run/tongji_port，S5/S5b）实测：首帧实际是 id1/id2 时，
  // h 收敛到 −43~−45 mm、z 误差 −148~−152 mm，**三块板预测高度全错、
  // 俯仰必然打空**，而位置误差只有 10 mm —— 日志上看不出异常，静默失效。
  //
  // 做法（参考 ckyf 的 3 个并行 UKF）：对 k=0/1/2 各建一个候选，
  // 每帧都 predict+update，累积 NIS 作为匹配度，选最小的作为输出；
  // 观察到 3 个不同板号（或跑够 outpost_lock_frames 帧）后锁定单跑。
  std::array<Target, 3> outpost_cand_;
  std::array<double, 3> outpost_err_ = {0.0, 0.0, 0.0};
  std::array<int, 3> outpost_reject_ = {0, 0, 0};
  std::array<std::set<int>, 3> outpost_seen_;
  int outpost_best_ = 0;
  int outpost_frames_ = 0;
  bool outpost_locked_ = false;

  /// 超时兜底帧数：跑这么久还没等到「区分度足够」就强制锁定并告警。
  /// 只在目标长期不转（永远看不到第 3 块板）时才会命中。
  /// 默认 300 帧 ≈ 5 s @60fps。
  int outpost_give_up_frames_ = 300;

  /// 区分度阈值：最优假设的累积误差必须小于次优者这个比例才算"分得开"。
  /// 实测：第 3 块板转到位后比值落在 0.29~0.39，
  /// 未到位时是 1.00（编号平移等价解，误差完全相同）。取 0.5 留余量。
  double outpost_discriminate_ratio_ = 0.5;

  /// 前哨站的 3 假设更新；成功返回 true。
  bool update_target_outpost(std::list<Armor> & armors, std::chrono::steady_clock::time_point t);

  /// 用当前最优候选覆盖 target_（对外只暴露一个 Target）。
  void sync_outpost_best();

  /// 取缓存的几何初值；无缓存或校验不过时返回默认构造（`valid()==false`）。
  GeoInit cached_geo(ArmorName name) const;

  /// 目标收敛时写缓存、发散时清除。只在 target_ 有效（state_ != "lost"）时调用。
  void update_geo_cache();
  RobotConfig base_cfg_;
  RobotConfig normal_cfg_;

  void state_machine(bool found);

  bool set_target(std::list<Armor> & armors, std::chrono::steady_clock::time_point t);

  bool update_target(std::list<Armor> & armors, std::chrono::steady_clock::time_point t);
};

}  // namespace auto_aim

#endif  // AUTO_AIM__TRACKER_HPP