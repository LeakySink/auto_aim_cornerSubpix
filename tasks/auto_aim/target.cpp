#include "target.hpp"

#include <cmath>

#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"

namespace auto_aim
{
Target::Target(
  const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
  Eigen::VectorXd P0_dig, EkfParams ekf_params)
: name(armor.name),
  armor_type(armor.type),
  jumped(false),
  last_id(0),
  update_count_(0),
  armor_num_(armor_num),
  t_(t),
  is_switch_(false),
  is_converged_(false),
  switch_count_(0),
  ekf_params_(ekf_params)
{
  auto r = radius;
  priority = armor.priority;
  const Eigen::VectorXd & xyz = armor.xyz_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;

  // 旋转中心的坐标
  auto center_x = xyz[0] + r * std::cos(ypr[0]);
  auto center_y = xyz[1] + r * std::sin(ypr[0]);
  auto center_z = xyz[2];

  // x vx y vy z vz a w r l h
  // a: angle
  // w: angular velocity
  // l: r2 - r1
  // h: z2 - z1
  Eigen::VectorXd x0{{center_x, 0, center_y, 0, center_z, 0, ypr[0], 0, r, 0, 0}};  //初始化预测量
  Eigen::MatrixXd P0 = P0_dig.asDiagonal();

  // 防止夹角求和出现异常值
  auto x_add = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a + b;
    c[6] = tools::limit_rad(c[6]);
    return c;
  };

  ekf_ = tools::ExtendedKalmanFilter(x0, P0, x_add);  //初始化滤波器（预测量、预测量协方差）
}

Target::Target(double x, double vyaw, double radius, double h) : armor_num_(4)
{
  Eigen::VectorXd x0{{x, 0, 0, 0, 0, 0, 0, vyaw, radius, 0, h}};
  Eigen::VectorXd P0_dig{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  Eigen::MatrixXd P0 = P0_dig.asDiagonal();

  // 防止夹角求和出现异常值
  auto x_add = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a + b;
    c[6] = tools::limit_rad(c[6]);
    return c;
  };

  ekf_ = tools::ExtendedKalmanFilter(x0, P0, x_add);  //初始化滤波器（预测量、预测量协方差）
}

void Target::predict(std::chrono::steady_clock::time_point t)
{
  auto dt = tools::delta_time(t, t_);
  predict(dt);
  t_ = t;
}

void Target::predict(double dt)
{
  // 状态转移矩阵
  // clang-format off
  Eigen::MatrixXd F{
    {1, dt,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {0,  0,  1, dt,  0,  0,  0,  0,  0,  0,  0},
    {0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0},
    {0,  0,  0,  0,  1, dt,  0,  0,  0,  0,  0},
    {0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0},
    {0,  0,  0,  0,  0,  0,  1, dt,  0,  0,  0},
    {0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0},
    {0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0},
    {0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0},
    {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1}
  };
  // clang-format on

  // Piecewise White Noise Model
  // https://github.com/rlabbe/Kalman-and-Bayesian-Filters-in-Python/blob/master/07-Kalman-Filter-Math.ipynb
  double v1, v2;
  //轮腿
  // v1 = 100;  // 加速度方差
  // v2 = 75;  // 角加速度方差
  //哨兵
  v1 = ekf_params_.q_xyz_acc_var;
  v2 = ekf_params_.q_yaw_acc_var;
  auto a = dt * dt * dt * dt / 4;
  auto b = dt * dt * dt / 2;
  auto c = dt * dt;
  // 预测过程噪声偏差的方差
  // clang-format off
  Eigen::MatrixXd Q{
    {a * v1, b * v1,      0,      0,      0,      0,      0,      0, 0, 0, 0},
    {b * v1, c * v1,      0,      0,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0, a * v1, b * v1,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0, b * v1, c * v1,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0, a * v1, b * v1,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0, b * v1, c * v1,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0, a * v2, b * v2, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0, b * v2, c * v2, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0,      0,      0, 0, 0, 0}
  };
  // clang-format on

  // 防止夹角求和出现异常值
  auto f = [&](const Eigen::VectorXd & x) -> Eigen::VectorXd {
    Eigen::VectorXd x_prior = F * x;
    x_prior[6] = tools::limit_rad(x_prior[6]);
    return x_prior;
  };

  // 前哨站转速特判
  if (this->convergened() && this->name == ArmorName::outpost && std::abs(this->ekf_.x[7]) > 2)
    this->ekf_.x[7] = this->ekf_.x[7] > 0 ? 2.51 : -2.51;

  ekf_.predict(F, Q, f);
}

void Target::update(const Armor & armor)
{
  // 观测有限性守卫（防御纵深）：即便上游漏检，NaN / inf 观测也不许进 EKF ——
  // 状态一旦被污染成 NaN，planner/aimer 会连带失效（弹道无解、云台不受控）。
  // 这里是所有更新路径（track 两个重载、全向感知、多线程）的共同收口。
  if (
    !armor.ypd_in_world.allFinite() || !armor.ypr_in_world.allFinite() ||
    !armor.xyz_in_world.allFinite()) {
    tools::RemoteLogger::instance().log("DEBUG", "[Target] 丢弃非有限观测，跳过本帧 EKF 更新");
    return;
  }

  const auto xyza_list = armor_xyza_list();

  int id = -1;
  double min_cost = 1e10;

  // PnP 给出的装甲板朝向有 ±π 二义性（平板正反面对称，IPPE 无法区分）。
  // 模型约定（见 h_armor_xyz：armor = center − r·n̂）：x[6]/armor_yaw 指的是
  // "从装甲板指向车体中心"的方向，因此对可见板有 n̂·p > 0（p 为板到云台原点的向量，
  // 车体中心在板的后方更远处）。用这条几何关系把二义性定死：
  // 不消歧的话，4 板目标里相差 π 的两块板在代价上完全等价（原来 min(yaw_err,
  // yaw_err±π) 让它们相等），关联会永远锁在 0 号板；旋转时相位 a 只能靠"跳
  // 90°/180°"去追，表现为状态抖动、w 学不出转速。
  const double obs_yaw_raw = armor.ypr_in_world[0];
  const double n_dot_p = std::cos(obs_yaw_raw) * armor.xyz_in_world[0] +
                         std::sin(obs_yaw_raw) * armor.xyz_in_world[1];
  const double obs_yaw = (n_dot_p < 0.0) ? tools::limit_rad(obs_yaw_raw + CV_PI) : obs_yaw_raw;

  const double obs_los  = armor.ypd_in_world[0];
  const double obs_dist = armor.ypd_in_world[2];

  for (int i = 0; i < armor_num_; ++i) {
    const auto & xyza = xyza_list[i];
    const Eigen::Vector3d ypd = tools::xyz2ypd(xyza.head<3>());

    // 观测 yaw 已按几何消歧成"绝对朝向"，直接与预测的绝对朝向比较；
    // 不再引入 ±π 分支（那正是 0↔2 / 1↔3 无法区分的根源）
    const double yaw_cost = std::abs(tools::limit_rad(obs_yaw - xyza[3]));

    const double los_cost  = std::abs(tools::limit_rad(obs_los - ypd[0]));
    const double dist_cost = std::abs(obs_dist - ypd[2]);

    double cost = yaw_cost + 0.5 * los_cost + 0.2 * dist_cost;

    // 低速时优先保持上一帧 id，抑制抖动
    // |w| 阈值可根据实际调，例如 2.0 ~ 3.0 rad/s
    if (update_count_ > 0 && i != last_id && std::abs(ekf_.x[7]) < 4.0) {
      cost += 0.35;  // 约 20° 的切换惩罚，需要根据数据调
    }

    if (cost < min_cost) {
      min_cost = cost;
      id = i;
    }
  }

  if (id < 0) id = 0;

  if (id != 0) jumped = true;

  is_switch_ = (update_count_ > 0 && id != last_id);
  if (is_switch_) switch_count_++;

  last_id = id;
  update_count_++;

  // 统一把"消歧后的绝对朝向"送进 EKF（以前是按预测值去翻转观测，等于让观测迁就
  // 已经跑偏的 a，只会把错误锁死）
  Armor corrected = armor;
  corrected.ypr_in_world[0] = obs_yaw;

  update_ypda(corrected, id);
}

void Target::update_ypda(const Armor & armor, int id)
{
  //观测jacobi
  Eigen::MatrixXd H = h_jacobian(ekf_.x, id);
  auto center_yaw = std::atan2(armor.xyz_in_world[1], armor.xyz_in_world[0]);
  auto delta_angle = tools::limit_rad(armor.ypr_in_world[0] - center_yaw);
  //串腿
  // Eigen::VectorXd R_dig{
  //   {4e-2, 4e-2, log(std::abs(delta_angle) + 1) + 1,
  //    log(std::abs(armor.ypd_in_world[2]) + 1) / 200 + 9e-2}};
  //哨兵
  Eigen::VectorXd R_dig{
      {ekf_params_.r_yaw,
       ekf_params_.r_pitch,
       log(std::abs(delta_angle) + 1) + 1,
       log(std::abs(armor.ypd_in_world[2]) + 1) / 200 + ekf_params_.r_dist_base}};

  //测量过程噪声偏差的方差
  Eigen::MatrixXd R = R_dig.asDiagonal();

  // 定义非线性转换函数h: x -> z
  auto h = [&](const Eigen::VectorXd & x) -> Eigen::Vector4d {
    Eigen::VectorXd xyz = h_armor_xyz(x, id);
    Eigen::VectorXd ypd = tools::xyz2ypd(xyz);
    auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
    return {ypd[0], ypd[1], ypd[2], angle};
  };

  // 防止夹角求差出现异常值
  auto z_subtract = [](const Eigen::VectorXd & a, const Eigen::VectorXd & b) -> Eigen::VectorXd {
    Eigen::VectorXd c = a - b;
    c[0] = tools::limit_rad(c[0]);
    c[1] = tools::limit_rad(c[1]);
    c[3] = tools::limit_rad(c[3]);
    return c;
  };

  const Eigen::VectorXd & ypd = armor.ypd_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;
  Eigen::VectorXd z{{ypd[0], ypd[1], ypd[2], ypr[0]}};  //获得观测量

  ekf_.update(z, H, R, h, z_subtract);
}

Eigen::VectorXd Target::ekf_x() const { return ekf_.x; }

const tools::ExtendedKalmanFilter & Target::ekf() const { return ekf_; }

std::vector<Eigen::Vector4d> Target::armor_xyza_list() const
{
  std::vector<Eigen::Vector4d> _armor_xyza_list;

  for (int i = 0; i < armor_num_; i++) {
    auto angle = tools::limit_rad(ekf_.x[6] + i * 2 * CV_PI / armor_num_);
    Eigen::Vector3d xyz = h_armor_xyz(ekf_.x, i);
    _armor_xyza_list.push_back({xyz[0], xyz[1], xyz[2], angle});
  }
  return _armor_xyza_list;
}

bool Target::diverged() const
{
  // ekf_.x 的维度恒为 11（x,y,z,vx,vy,vz,yaw,w,r,l,h）。默认构造的 Target
  // 其 ekf_.x 是空向量（data() == nullptr），此处直接取 [8]/[9] 等于解引用
  // 地址 64 -> SIGSEGV。
  // 当前靠状态机不变量保命（state_ 只在 set_target 之后才离开 "lost"，
  // 而 set_target 必然赋值 target_），但一改状态机就会崩。
  // kalman_markers.cpp:43 已写了 `if (x.size() < 11)`，说明 codebase 里
  // 确实预期过短 x 的存在。这里同样按"维度不足即视为发散"处理。
  if (ekf_.x.size() < 11) return true;

  // 旋转中心离云台太近同样是发散的表现：正常交战不会有 0.5 m 以内的目标，
  // 出现这种状态几乎都是脏框被 PnP 解成"贴脸"（现场日志里 d=0.30 m 那类）。
  // 不进这条判据的话，状态会带着一个假位置一直算下去（r/l 的口径拦不住它）。
  const double center_dist = std::hypot(ekf_.x[0], ekf_.x[2]);
  if (!std::isfinite(center_dist) || center_dist < 0.5) {
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Target] 旋转中心过近（d={:.3f} m），判为发散", center_dist);
    return true;
  }

  auto r_ok = ekf_.x[8] > 0.05 && ekf_.x[8] < 0.5;
  auto l_ok = ekf_.x[8] + ekf_.x[9] > 0.05 && ekf_.x[8] + ekf_.x[9] < 0.5;

  if (r_ok && l_ok) return false;

  tools::RemoteLogger::instance().log("DEBUG", "[Target] r={:.3f}, l={:.3f}", ekf_.x[8], ekf_.x[9]);
  return true;
}

bool Target::convergened()
{
  if (this->name != ArmorName::outpost && update_count_ > 3 && !this->diverged()) {
    is_converged_ = true;
  }

  //前哨站特殊判断
  if (this->name == ArmorName::outpost && update_count_ > 10 && !this->diverged()) {
    is_converged_ = true;
  }

  return is_converged_;
}

// 计算出装甲板中心的坐标（考虑长短轴）
Eigen::Vector3d Target::h_armor_xyz(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[8] + x[9] : x[8];
  auto armor_x = x[0] - r * std::cos(angle);
  auto armor_y = x[2] - r * std::sin(angle);
  auto armor_z = (use_l_h) ? x[4] + x[10] : x[4];

  return {armor_x, armor_y, armor_z};
}

Eigen::MatrixXd Target::h_jacobian(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[6] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[8] + x[9] : x[8];
  auto dx_da = r * std::sin(angle);
  auto dy_da = -r * std::cos(angle);

  auto dx_dr = -std::cos(angle);
  auto dy_dr = -std::sin(angle);
  auto dx_dl = (use_l_h) ? -std::cos(angle) : 0.0;
  auto dy_dl = (use_l_h) ? -std::sin(angle) : 0.0;

  auto dz_dh = (use_l_h) ? 1.0 : 0.0;

  // clang-format off
  Eigen::MatrixXd H_armor_xyza{
    {1, 0, 0, 0, 0, 0, dx_da, 0, dx_dr, dx_dl,     0},
    {0, 0, 1, 0, 0, 0, dy_da, 0, dy_dr, dy_dl,     0},
    {0, 0, 0, 0, 1, 0,     0, 0,     0,     0, dz_dh},
    {0, 0, 0, 0, 0, 0,     1, 0,     0,     0,     0}
  };
  // clang-format on

  Eigen::VectorXd armor_xyz = h_armor_xyz(x, id);
  Eigen::MatrixXd H_armor_ypd = tools::xyz2ypd_jacobian(armor_xyz);
  // clang-format off
  Eigen::MatrixXd H_armor_ypda{
    {H_armor_ypd(0, 0), H_armor_ypd(0, 1), H_armor_ypd(0, 2), 0},
    {H_armor_ypd(1, 0), H_armor_ypd(1, 1), H_armor_ypd(1, 2), 0},
    {H_armor_ypd(2, 0), H_armor_ypd(2, 1), H_armor_ypd(2, 2), 0},
    {                0,                 0,                 0, 1}
  };
  // clang-format on

  return H_armor_ypda * H_armor_xyza;
}

bool Target::checkinit() { return isinit; }

}  // namespace auto_aim
