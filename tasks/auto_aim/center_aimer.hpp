#ifndef AUTO_AIM__CENTER_AIMER_HPP
#define AUTO_AIM__CENTER_AIMER_HPP

#include <Eigen/Dense>
#include <chrono>
#include <string>

#include "target.hpp"

namespace auto_aim
{

/// 重装自瞄策略：**瞄旋转中心，等板转过来再开火**（2026-11 组会定的目标）。
///
/// 为什么不是"瞄板"：
///   - 4 板目标在 45° 处两块板同时可见，逐板瞄准会让云台在两者间来回跳；
///   - 重装云台惯量大，跳变代价远大于把枪口稳定压在中心。
///
/// 为什么瞄中心也能打中：
///   板心到旋转中心的距离就是 r（0.2~0.3 m），子弹沿"云台→中心"这条线飞，
///   只要在**弹丸到达的那一刻**有板正好压在这条线上，就会命中板面。
///   所以开火判据不是"现在瞄得准不准"，而是"**到达时刻**板在不在线上"。
///
/// 时序（每个控制周期做一次）：
///   1. 迭代求飞行时间 fly_time（距离依赖 fly_time，fly_time 又依赖距离）；
///   2. 用 fly_time 把 EKF 状态外推到**弹丸到达时刻**，得到那时的中心与各板位置；
///   3. 瞄准点 = 外推后的中心（yaw/pitch 都按它算，含弹道抬高）；
///   4. 逐板检查"到达时刻这块板是否压在瞄准线上"，压住且云台已到位 → fire。
struct CenterAimConfig
{
  // 板面尺寸：默认取 armor_geometry 的历史硬编码值（2027 无大装甲，按小板算）
  double plate_width = 0.135;   ///< m，两灯条外缘间距
  double plate_height = 0.056;  ///< m，板高（= lightbar_length）

  /// 只把板面中间(1-margin)的范围当作"能打中"，用来吸收 PnP 与超前量误差。
  /// 0.3 表示只用板宽的 70% / 板高的 70% 做判据。
  double hit_margin = 0.30;

  /// 板面朝向门槛：板的外法向必须大致朝向枪口才可打。
  /// `cos_face` 定义为 n̂·p / |p_xy|（n̂ = 板→车体中心方向，见 h_armor_xyz），
  /// 正视 = +1，侧棱 = 0，背面 = −1。背面永远打不到（车身挡着），
  /// 侧棱投影宽度趋 0 也基本打不到，所以默认只接受 cos_face ≥ 0.15。
  double min_face_cos = 0.15;

  double yaw_offset = 0.0;    ///< rad，与 Aimer 同名参数同义
  double pitch_offset = 0.0;  ///< rad，pitch 指令在世界系向上为负
  double gimbal_tol = 0.5;    ///< deg，云台到位判据（yaw/pitch 各自）
  bool require_settled = true;  ///< 云台没到位就不开火（防止"预测对了但枪没到"）

  /// 从"判定开火"到"弹丸出膛"的总延迟 [s]：CAN 下发 + 下位机处理 + 摩擦轮/拨弹
  /// 建立时间。预测时刻 = launch_delay + fly_time，漏掉它会系统性打早。
  /// 默认 0（行为与未引入该参数时一致），实测后填。
  double launch_delay = 0.0;

  int max_iter = 6;            ///< 飞行时间迭代上限
  double fly_time_eps = 1e-3;  ///< s，相邻两次 fly_time 差小于它即认为收敛
};

struct CenterAimResult
{
  bool valid = false;  ///< false = 无目标/无解/状态非法，本帧不控制
  bool fire = false;

  /// 开火的两半条件，分开给出来：`fire` 一直是 0 时，先看这两个谁不满足。
  bool in_window = false;  ///< 到达时刻确实有板压在瞄准线上
  bool settled = false;    ///< 云台已到位（require_settled=false 时恒为 true）

  double yaw = 0.0;    ///< 云台指令角 [rad]
  double pitch = 0.0;  ///< 云台指令角 [rad]（向上为负，与 io::Command 一致）
  double aim_pitch_abs = 0.0;  ///< 瞄准点的绝对仰角 [rad]（向上为正，弹道解算结果）
  double fly_time = 0.0;       ///< 纯飞行时间 [s]
  double total_time = 0.0;     ///< launch_delay + fly_time [s]（判据实际用的外推时长）

  int plate_id = -1;            ///< 预计命中的装甲板编号
  double cos_face = 0.0;        ///< 该板朝向枪口的程度（1=正视 0=侧棱 <0=背面）
  double align_err_deg = 180.0; ///< 到达时刻该板相对瞄准线的 yaw 偏差
  double hit_window_deg = 0.0;  ///< 该距离下板面的 yaw 角宽（全宽）
  double pitch_err_deg = 0.0;   ///< 到达时刻该板相对瞄准线的 pitch 偏差
  double pitch_window_deg = 0.0;///< 该距离下板面的 pitch 角宽（全宽）
  double t_to_cross = 0.0;      ///< 距板心穿过瞄准线还有多久 [s]（调参观察用）

  Eigen::Vector3d aim_xyz = Eigen::Vector3d::Zero(); ///< 瞄准点（外推后的旋转中心）
};

class CenterAimer
{
public:
  /// 默认构造：不读 yaml，用结构体默认值（单元测试/离线仿真用）
  CenterAimer() = default;

  /// 显式给配置（单测里改 launch_delay / min_fire_gap 等，不需要写 yaml）
  explicit CenterAimer(const CenterAimConfig & cfg) : cfg_(cfg) {}

  explicit CenterAimer(const std::string & config_path);

  /// gimbal_yaw / gimbal_pitch：云台当前反馈角（与 io::Gimbal::state() 同约定）。
  CenterAimResult solve(
    const Target & target, std::chrono::steady_clock::time_point t, double gimbal_yaw,
    double gimbal_pitch, double bullet_speed) const;

  const CenterAimConfig & config() const { return cfg_; }

  /// 只算"瞄准中心的指令角"，不判开火。给调试/单测用。
  bool aim_center(
    const Target & target, std::chrono::steady_clock::time_point t, double bullet_speed,
    CenterAimResult * out) const;

private:
  CenterAimConfig cfg_;
};

}  // namespace auto_aim

#endif  // AUTO_AIM__CENTER_AIMER_HPP
