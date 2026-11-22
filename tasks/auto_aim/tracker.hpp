#ifndef AUTO_AIM__TRACKER_HPP
#define AUTO_AIM__TRACKER_HPP

#include <Eigen/Dense>
#include <chrono>
#include <list>
#include <map>
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