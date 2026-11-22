#include "target.hpp"

#include <algorithm>
#include <cmath>

#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"

namespace auto_aim
{
namespace
{
/// 前哨站三块装甲板的相邻高度差 [m]。
///
/// 语义：h = 相邻板高差，三块板高度为 z, z-h, z-2h（id0 最高）。
///
/// 取值 0.102 来自参考实现
/// ckyf_vision/src/robot_auto_aim/src/outpost_target.cpp:37 的 `h_init = 0.102`，
/// 其 isDiverged() 给出的合理区间是 [0.05, 0.2]。
///
/// 注意：本文件早期版本曾写作 0.054，依据是「2027 规则手册 图 4-34 的三块板
/// 高度 1446/1496/1554 mm」。该依据**已被推翻** —— 那三条引线指向的是结构顶边、
/// 内部箱体顶边和绿灯中心，不是装甲板（见 docs/physical_assumptions.md A2-2b）。
constexpr double kOutpostPlateHeight = 0.102;
}  // namespace

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

  // ---- 3 板（前哨站）的初始高度语义 ----
  // h_armor_xyz 里 3 板是 `z_i = x[4] - i*h`，所以 x[4] 是 **id0 那块板**的高度，
  // 不是"三块板的共同高度"。而这里只知道看到了一块板、不知道它是 id 几，
  // 若直接把它的 z 当 x[4]、并把 h 置 0，就等于断言"看到的就是 id0 且三块同高"，
  // 一旦看到的是 id1/id2 就整体偏一个/两个 h。
  //
  // 最小改动的处理：h 取 kOutpostPlateHeight（3 板线性递降的相邻差），
  // 并假设当前观测是 **id0（最高板）**；若实际是别的板，Tracker 的多假设
  // 初始化会给出另外两个候选（见 Tracker::set_target）。
  // 这块板不是 id0 时，centre_z 会被高估 (id * h)，由 EKF 在前几帧修正
  // （h 有过程噪声，且观测的 elevation 直接约束它）。
  double h_init = 0.0;
  if (armor_num == 3) {
    h_init = kOutpostPlateHeight;
    // 假设观测到的是 id0：中心 z 就是这块板的高度
    center_z = xyz[2];
  }

  // x vx y vy z vz a w r l h
  // a: angle
  // w: angular velocity
  // l: r2 - r1        （仅 4 板使用）
  // h: 4 板 = z2 - z1；3 板 = 相邻板高度差（z_i = z - i*h）
  Eigen::VectorXd x0{{center_x, 0, center_y, 0, center_z, 0, ypr[0], 0, r, 0, h_init}};
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
  // 这个构造函数原先**没有给 ekf_params_ 赋值**，而 predict() 会读
  // ekf_params_.q_xyz_acc_var / q_yaw_acc_var / q_geo / omega_freeze_thresh，
  // 属于读未初始化内存（UB）。当前只有 3 个 planner 测试用它，
  // 一旦拿来跑完整流程就会出随机结果。这里显式给默认值。
  ekf_params_ = EkfParams{};

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

  // 几何状态（r / l / h）的随机游走方差。
  // 原来这三行恒为 0 —— 相当于断言"几何参数永不改变"，于是 P 只减不增，
  // 一旦估偏就再也纠正不回来（前哨站只有一块板同时可见，可观测性本来就弱）。
  // 这里按参考实现（ckyf_vision/outpost_target.cpp）的做法：用 |w| 自适应缩放，
  // 目标不转时 r/l/h 与中心位置在观测上共线，此时不该让它们随机游走。
  double omega_scale = 1.0;
  if (ekf_params_.omega_freeze_thresh > 1e-6) {
    omega_scale = std::min(1.0, std::abs(ekf_.x[7]) / ekf_params_.omega_freeze_thresh);
  }
  const double qg = ekf_params_.q_geo * omega_scale * dt;
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
    {     0,      0,      0,      0,      0,      0,      0,      0, qg, 0, 0},
    {     0,      0,      0,      0,      0,      0,      0,      0, 0, 0, 0},
    {     0,      0,      0,      0,      0,      0,      0,      0, 0, 0, qg}
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
  // ---- R 自适应（2026-10 新增）----
  // 动机：板面与视线的夹角越大（侧视），板在图像上被压扁，
  //   由单板 PnP 反解出的"距离"和"yaw"都迅速变病态（sigma_d ~ 1/cos）。
  //   原来 r_yaw 是常数、距离方差只随 |delta_angle| 对数微增，等于假设
  //   "侧视和正视一样可信"——这是抬枪误差(1.3 mrad)的主要来源之一。
  // 做法：用 view_penalty = 1/max(|cos(delta_angle)|, floor) 放大对应方差。
  //   delta_angle 是"板法向"与"从云台指向板的方向"之差：
  //   |cos|=1 正视（最可信，penalty=1）；|cos|->0 侧视（最多放大 1/floor 倍）。
  //   上限取 1/0.5=2 倍：再斜的板本来就该被几何门控/校验器丢掉，
  //   这里只做"降权"，不做"丢弃"（丢弃交给 tracker 的空间门控）。
  const double cos_view = std::abs(std::cos(delta_angle));
  // floor=0.5（最大降权 2 倍）而不是 0.25：
  //   仿真对照显示 floor=0.25 在"噪声其实不随侧视恶化"的工况下会让位置 RMSE
  //   从 67.0mm 劣化到 70.7mm（+5.5%），而 floor=0.5 只劣化 1.8% 却保留了
  //   大部分收益（115.5 vs 110.7mm）。取 0.5 是"收益/鲁棒"折中最优。
  const double view_penalty = 1.0 / std::max(cos_view, 0.5);

  Eigen::VectorXd R_dig{
      {ekf_params_.r_yaw * view_penalty,             // yaw 方差：侧视时压缩，噪声大
       ekf_params_.r_pitch,                          // pitch 方差：与视角弱相关，保持
       (log(std::abs(delta_angle) + 1) + 1) * view_penalty,   // 距离方差：侧视最病态，重点降权
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

  // ---- 新息门控（2026-10 新增）----
  // 目的：把"明显不可能"的观测挡在 EKF 之外，而不是让它以很大权重拉跑状态。
  //   r/l/h 三个参数在 Q 里没有过程噪声，一旦被脏观测污染就不可逆
  //   （这正是历史上"幽灵板 d=0.3m"的成因）。
  // 判据：d^2 = nu^T S^{-1} nu，卡方分布自由度=观测维数 4。
  //   取 99.9% 分位数 18.47（而不是 95% 的 9.488）：这里只拦"粗差"，
  //   正常机动/换板造成的中等残差应当交给 R 自适应平滑吸收，
  //   否则会误杀真实的大机动观测、拖慢重捕。
  // 注意：本门控与 NIS 看门狗（tracker 的 Bad Converge）用的是同一族统计量，
  //   改这里会影响看门狗的触发率，故阈值取宽（只拦粗差）。
  {
    constexpr double kInnovationGateChi2 = 18.47;  // chi2(4, 0.999)
    const Eigen::VectorXd z_now{{armor.ypd_in_world[0], armor.ypd_in_world[1],
                                 armor.ypd_in_world[2], armor.ypr_in_world[0]}};
    Eigen::VectorXd nu = z_subtract(z_now, h(ekf_.x));
    Eigen::MatrixXd S = H * ekf_.P * H.transpose() + R;
    // 显式取 (0,0)：表达式是 1x1，直接赋给 double 依赖 Eigen 版本的隐式转换，不可靠
    const double d2 = (nu.transpose() * S.inverse() * nu)(0, 0);
    if (std::isfinite(d2) && d2 > kInnovationGateChi2) {
      tools::RemoteLogger::instance().log(
        "DEBUG", "[Target] 新息门控丢弃观测：d2={:.2f} > {:.2f}（dist={:.2f}m conf={:.2f}）", d2,
        kInnovationGateChi2, armor.ypd_in_world[2], armor.confidence);
      return;
    }
  }

  const Eigen::VectorXd & ypd = armor.ypd_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;
  Eigen::VectorXd z{{ypd[0], ypd[1], ypd[2], ypr[0]}};  //获得观测量

  ekf_.update(z, H, R, h, z_subtract);
}

Eigen::VectorXd Target::ekf_x() const { return ekf_.x; }

const tools::ExtendedKalmanFilter & Target::ekf() const { return ekf_; }

void Target::set_w(double w) { ekf_.x[7] = w; }

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

  // 高度：
  //   4 板 —— 奇偶两档（x[4] 与 x[4]+h）
  //   3 板 —— 前哨站三块板**线性递降**，相邻差 h，id 0 最高
  //            依据 2027 规则手册 图 4-34：三块板高度 1446/1496/1554 mm，
  //            相邻差约 54 mm；参考实现 ckyf_vision/outpost_target.cpp 的
  //            `az = cz - id*h` 与此一致。
  //   其它 —— 同高
  double armor_z;
  if (armor_num_ == 3) {
    armor_z = x[4] - id * x[10];
  } else {
    armor_z = (use_l_h) ? x[4] + x[10] : x[4];
  }

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

  // 3 板时 z = x[4] - id*x[10] -> dz/dh = -id；4 板奇偶组 dz/dh = 1
  auto dz_dh = (armor_num_ == 3) ? -static_cast<double>(id) : ((use_l_h) ? 1.0 : 0.0);

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
