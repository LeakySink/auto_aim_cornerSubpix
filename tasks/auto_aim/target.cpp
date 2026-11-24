#include "target.hpp"

#include <algorithm>
#include <cmath>

#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"

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

/// 把 11 维旧版 P0 扩成 14 维：在每轴的速度后插入一个加速度方差。
/// 保留此函数是为了让旧的 yaml（11 个元素）仍可加载，不必逐个改配置。
Eigen::VectorXd expand_p0_11_to_14(const Eigen::VectorXd & p11)
{
  constexpr double kDefaultAccVar = 1.0;   // σ_a = 1 m/s²
  const auto & p = p11;
  Eigen::VectorXd p14(idx::DIM);
  p14 << p[0], p[1], kDefaultAccVar,   // x vx ax
    p[2], p[3], kDefaultAccVar,        // y vy ay
    p[4], p[5], kDefaultAccVar,        // z vz az
    p[6], p[7], p[8], p[9], p[10];     // a w r l h
  return p14;
}

// 变量 + 变量，其中 yaw 分量做回绕
Eigen::VectorXd x_add_impl(const Eigen::VectorXd & a, const Eigen::VectorXd & b)
{
  Eigen::VectorXd c = a + b;
  c[idx::A] = tools::limit_rad(c[idx::A]);
  return c;
}
}  // namespace

Target::Target(
  const Armor & armor, std::chrono::steady_clock::time_point t, double radius, int armor_num,
  Eigen::VectorXd P0_dig, EkfParams ekf_params, GeoInit geo, int id_hypothesis)
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
  // 有缓存几何量时优先用缓存（重捕省掉重新估 r/h 的十几帧）
  auto r = (geo.r >= 0.0) ? geo.r : radius;
  priority = armor.priority;
  const Eigen::VectorXd & xyz = armor.xyz_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;

  // 旋转中心的坐标
  auto center_x = xyz[0] + r * std::cos(ypr[0]);
  auto center_y = xyz[1] + r * std::sin(ypr[0]);
  auto center_z = xyz[2];

  // ---- 3 板（前哨站 / 基地）的初始高度语义 ----
  // h_armor_xyz 里 3 板是 `z_i = x[Z] - i*h`，所以 x[Z] 是 **id0 那块板**的高度，
  // 不是"三块板的共同高度"。而这里只知道看到了一块板、不知道它是 id 几。
  double h_init = 0.0;
  double a_init = ypr[0];
  // 3 板的分支同时被 **前哨站** 和 **基地** 命中（两者 armor_num 都是 3），
  // 但它们的板高排布完全不同：
  //   前哨站：三块等间距递降，相邻差 h = 0.102 m（官方场地道具，几何固定）
  //   基地  ：**两层** 3 块板，不是等间距三层递降
  //
  // 原来这里不看 name、一律给 kOutpostPlateHeight，等于让基地套用前哨站的
  // 高度模型。而竖直命中窗口只有 ±19.6 mm，h=102mm 已是窗口的 5.2 倍 ——
  // 另外两块板的高度会全错。
  //
  // 用户决定"基地先不改"（只打其中 3 块），所以这里的最小安全修法是：
  //   - 前哨站：保持 h = 0.102（有实测/参考依据）
  //   - 基地  ：h 初值给 0（"暂时按同高处理"），让 EKF 自己从观测里学。
  //   将来若要精确建模基地的两层结构，改这里即可。
  const bool is_outpost = (armor.name == ArmorName::outpost);
  if (armor_num == 3) {
    if (geo.h >= 0.0) {
      h_init = geo.h;  // 缓存/显式指定优先
    } else {
      h_init = is_outpost ? kOutpostPlateHeight : 0.0;
    }
    // 假设首帧看到的是 id = k 的板，反解状态：
    //   z_obs   = x[Z] - k·h     ->  x[Z] = z_obs + k·h
    //   obs_yaw = x[A] + k·2π/3  ->  x[A] = obs_yaw - k·2π/3
    // 假设错一个 k，x[Z] 就差 k·h（最大 204 mm），EKF 只能用 h<0 去吸收
    // —— 这正是仿真 S5 里 h 收敛到 -45 mm、z 误差 -150 mm 的根因。
    const int k = (id_hypothesis >= 0 && id_hypothesis < 3) ? id_hypothesis : 0;
    center_z = xyz[2] + k * h_init;
    a_init = tools::limit_rad(ypr[0] - k * 2.0 * CV_PI / 3.0);
  }

  // 4 板的 h 是"奇偶面高度差"，同样支持缓存
  double h_final = h_init;
  double l_final = 0.0;
  if (armor_num != 3) {
    if (geo.h >= 0.0) h_final = geo.h;
    if (geo.l >= 0.0) l_final = geo.l;
  }

  // ---- 状态向量（14 维 CA 模型）----
  // x vx ax | y vy ay | z vz az | a w r l h
  // 加速度初值取 0（对匀速/静止目标是最小惊讶假设；
  // 圆周运动时由前几帧观测把向心加速度估出来）
  Eigen::VectorXd x0 = Eigen::VectorXd::Zero(idx::DIM);
  x0[idx::X] = center_x;
  x0[idx::Y] = center_y;
  x0[idx::Z] = center_z;
  x0[idx::A] = a_init;
  x0[idx::R] = r;
  x0[idx::L] = l_final;
  x0[idx::H] = h_final;

  // 兼容旧的 11 维 p0（各 yaml 里普遍还是 11 个元素）
  Eigen::VectorXd p0_dig = (P0_dig.size() == idx::DIM) ? P0_dig : expand_p0_11_to_14(P0_dig);
  Eigen::MatrixXd P0 = p0_dig.asDiagonal();

  ekf_ = tools::ExtendedKalmanFilter(x0, P0, x_add_impl);
}

Target::Target(double x, double vyaw, double radius, double h) : armor_num_(4)
{
  // 这个构造函数原先**没有给 ekf_params_ 赋值**，而 predict() 会读
  // ekf_params_.q_xyz_acc_var / q_yaw_acc_var / q_geo / omega_freeze_thresh，
  // 属于读未初始化内存（UB）。这里显式给默认值。
  ekf_params_ = EkfParams{};

  Eigen::VectorXd x0 = Eigen::VectorXd::Zero(idx::DIM);
  x0[idx::X] = x;
  x0[idx::W] = vyaw;
  x0[idx::R] = radius;
  x0[idx::H] = h;

  Eigen::VectorXd P0_dig = Eigen::VectorXd::Zero(idx::DIM);

  ekf_ = tools::ExtendedKalmanFilter(x0, P0_dig.asDiagonal(), x_add_impl);
}

void Target::predict(std::chrono::steady_clock::time_point t)
{
  auto dt = tools::delta_time(t, t_);
  predict(dt);
  t_ = t;
}

void Target::predict(double dt)
{
  // ---- 状态转移：常加速（CA）模型 ----
  //
  // 每轴：p += v·dt + ½a·dt²，v += a·dt，a 保持不变
  // yaw：a += w·dt
  //
  // 为什么用 CA 而不是 CV：目标绕竖直轴匀速自转时，板做圆周运动有向心
  // 加速度 a = ω²r。CV 模型假设 a=0，预测误差 = ½·a·t²：
  //     前哨站(ω=2.5, r=0.28)     @0.25s -> 55 mm
  //     小陀螺(ω=10,  r=0.20)     @0.25s -> 625 mm
  // 而横向命中窗口只有约 47 mm，所以 CV 对转靶存在**系统性**超前误差。
  Eigen::MatrixXd F = Eigen::MatrixXd::Identity(idx::DIM, idx::DIM);
  for (int axis = 0; axis < 3; ++axis) {
    const int p = 3 * axis;      // X/Y/Z
    const int v = p + 1;         // VX/VY/VZ
    const int a = p + 2;         // AX/AY/AZ
    F(p, v) = dt;
    F(p, a) = 0.5 * dt * dt;
    F(v, a) = dt;
  }
  F(idx::A, idx::W) = dt;

  // ---- 过程噪声 ----
  // 平移用 CA 的 jerk 白噪声模型（离散化后 P 阵），
  // yaw 保持原来的 CWNA 模型。
  const double v1 = ekf_params_.q_xyz_acc_var;  // jerk 方差 [m²/s⁶]
  const double v2 = ekf_params_.q_yaw_acc_var;  // 角加速度方差

  Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(idx::DIM, idx::DIM);

  // CA 平移噪声：对 (p,v,a) 三状态，由 jerk 白噪声积分得到
  const double T2 = dt * dt;
  const double T3 = T2 * dt;
  const double T4 = T3 * dt;
  const double T5 = T4 * dt;
  const double q11 = v1 * T5 / 20.0;  // (p,p)
  const double q12 = v1 * T4 / 8.0;   // (p,v)
  const double q13 = v1 * T3 / 6.0;   // (p,a)
  const double q22 = v1 * T3 / 3.0;   // (v,v)
  const double q23 = v1 * T2 / 2.0;   // (v,a)
  const double q33 = v1 * dt;         // (a,a)

  for (int axis = 0; axis < 3; ++axis) {
    const int p = 3 * axis;
    const int v = p + 1;
    const int a = p + 2;
    Q(p, p) = q11;
    Q(p, v) = q12;
    Q(p, a) = q13;
    Q(v, p) = q12;
    Q(v, v) = q22;
    Q(v, a) = q23;
    Q(a, p) = q13;
    Q(a, v) = q23;
    Q(a, a) = q33;
  }

  // yaw 的 CWNA（与旧模型一致）
  const double ya = dt * dt * dt * dt / 4.0;
  const double yb = dt * dt * dt / 2.0;
  const double yc = dt * dt;
  Q(idx::A, idx::A) = ya * v2;
  Q(idx::A, idx::W) = yb * v2;
  Q(idx::W, idx::A) = yb * v2;
  Q(idx::W, idx::W) = yc * v2;

  // ---- 几何状态（r / l / h）的随机游走 ----
  // 默认 q_geo=0（冻结）。理由见 target.hpp：r/h 是固定物理常数，
  // 常参数不需要过程噪声；加噪声会让 h 出现 10 s 级慢漂（实测峰峰值 8.2 mm）。
  // 保留开关是为了 4 板/特殊工况可单独启用。
  double omega_scale = 1.0;
  if (ekf_params_.omega_freeze_thresh > 1e-6) {
    omega_scale = std::min(1.0, std::abs(ekf_.x[idx::W]) / ekf_params_.omega_freeze_thresh);
  }
  const double qg = ekf_params_.q_geo * omega_scale * dt;
  Q(idx::R, idx::R) = qg;
  Q(idx::H, idx::H) = qg;

  // 防止夹角求和出现异常值
  auto f = [&](const Eigen::VectorXd & x) -> Eigen::VectorXd {
    Eigen::VectorXd x_prior = F * x;
    x_prior[idx::A] = tools::limit_rad(x_prior[idx::A]);
    return x_prior;
  };

  // ---- 前哨站转速约束：只拦"物理上不可能"的值，不做硬钳位 ----
  //
  // 原实现（同济祖先继承，无注释说明动机）：
  //     if (convergened() && outpost && |w| > 2)  w = ±2.51;
  // 这是**无条件钳死**，后果是目标停转后 w 被永久钉在 2.51。
  //
  // 实测（.deps/diag_noclamp.cpp，10s 停转）：
  //     有钳位：停转后 2s w=2.4260，跑到 120s 仍是 2.4260（永不归零）
  //     无钳位：停转后 2s w=0.0036，结束时 w=0.0000（2 秒内归零）
  //   -> 钳位是唯一根因。EKF 自己完全能收敛，不需要任何"停转检测"。
  //
  // 但也不能完全不管：规则给出前哨站稳态转速 0.8π = 2.5133 rad/s（5 秒内
  // 加速达到），所以 |w| 超过它一大截就是脏观测/关联错误。这里放宽到
  // 0.8π 的 1.5 倍（3.77 rad/s）。
  constexpr double kOutpostSpinNominal = 0.8 * CV_PI;              // 2.5133
  constexpr double kOutpostSpinLimit = 1.5 * kOutpostSpinNominal;  // 3.77
  if (this->name == ArmorName::outpost && std::abs(this->ekf_.x[idx::W]) > kOutpostSpinLimit) {
    const double clamped =
      this->ekf_.x[idx::W] > 0 ? kOutpostSpinNominal : -kOutpostSpinNominal;
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Target] 前哨站转速异常 w={:.3f} 超出 ±{:.2f}，拉回 ±{:.3f}",
      this->ekf_.x[idx::W], kOutpostSpinLimit, kOutpostSpinNominal);
    this->ekf_.x[idx::W] = clamped;
  }

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
  // 模型约定（见 h_armor_xyz：armor = center − r·n̂）：x[A]/armor_yaw 指的是
  // "从装甲板指向车体中心"的方向，因此对可见板有 n̂·p > 0。
  // 不消歧的话，4 板目标里相差 π 的两块板在代价上完全等价，关联会永远
  // 锁在 0 号板；旋转时相位 a 只能靠"跳 90°/180°"去追，表现为状态抖动。
  const double obs_yaw_raw = armor.ypr_in_world[0];
  const double n_dot_p = std::cos(obs_yaw_raw) * armor.xyz_in_world[0] +
                         std::sin(obs_yaw_raw) * armor.xyz_in_world[1];
  const double obs_yaw = (n_dot_p < 0.0) ? tools::limit_rad(obs_yaw_raw + CV_PI) : obs_yaw_raw;

  const double obs_los = armor.ypd_in_world[0];
  const double obs_dist = armor.ypd_in_world[2];

  for (int i = 0; i < armor_num_; ++i) {
    const auto & xyza = xyza_list[i];
    const Eigen::Vector3d ypd = tools::xyz2ypd(xyza.head<3>());

    // 观测 yaw 已按几何消歧成"绝对朝向"，直接与预测的绝对朝向比较
    const double yaw_cost = std::abs(tools::limit_rad(obs_yaw - xyza[3]));

    const double los_cost = std::abs(tools::limit_rad(obs_los - ypd[0]));
    const double dist_cost = std::abs(obs_dist - ypd[2]);

    double cost = yaw_cost + 0.5 * los_cost + 0.2 * dist_cost;

    // 低速时优先保持上一帧 id，抑制抖动
    if (update_count_ > 0 && i != last_id && std::abs(ekf_.x[idx::W]) < 4.0) {
      cost += 0.35;  // 约 20° 的切换惩罚
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

  // 统一把"消歧后的绝对朝向"送进 EKF
  Armor corrected = armor;
  corrected.ypr_in_world[0] = obs_yaw;

  update_ypda(corrected, id);
}

void Target::update_ypda(const Armor & armor, int id)
{
  Eigen::MatrixXd H = h_jacobian(ekf_.x, id);
  auto center_yaw = std::atan2(armor.xyz_in_world[1], armor.xyz_in_world[0]);
  auto delta_angle = tools::limit_rad(armor.ypr_in_world[0] - center_yaw);

  // ---- R 按视角自适应 ----
  // 动机：板面与视线的夹角越大（侧视），板在图像上被压扁，
  //   由单板 PnP 反解出的"距离"和"yaw"都迅速变病态（sigma_d ~ 1/cos）。
  // 做法：用 view_penalty = 1/max(|cos(delta_angle)|, floor) 放大对应方差。
  //   |cos|=1 正视（最可信，penalty=1）；|cos|->0 侧视（最多放大 1/floor 倍）。
  //   floor 取 0.5（最大降权 2 倍）：仿真对照显示 0.25 在"噪声不随侧视恶化"
  //   的工况下会让位置 RMSE 从 67.0mm 劣化到 70.7mm，而 0.5 只劣化 1.8%
  //   却保留大部分收益。取 0.5 是"收益/鲁棒"折中最优。
  const double cos_view = std::abs(std::cos(delta_angle));
  const double view_penalty = 1.0 / std::max(cos_view, 0.5);

  // 切板帧：板朝向通道的观测噪声放大。
  // 换板时 `angle = x[A] + id·2π/armor_num` 整跳一个板间距，
  // 若仍按普通帧采信，新息会以很大权重把 a/w 拽一下，并连带把 h 拉偏。
  // 必须用 is_switch_：update_ypda 是在 `last_id = id` 之后调用的，
  // 此时比较 `id != last_id` 恒为假。
  const double yaw_switch_penalty = is_switch_ ? ekf_params_.r_yaw_adaptive_factor : 1.0;

  Eigen::VectorXd R_dig{
    {ekf_params_.r_yaw * view_penalty,           // yaw 方差：侧视时压缩，噪声大
     ekf_params_.r_pitch,                        // pitch 方差：与视角弱相关，保持
     (log(std::abs(delta_angle) + 1) + 1) * view_penalty,  // 距离方差：侧视最病态
     (log(std::abs(armor.ypd_in_world[2]) + 1) / 200 + ekf_params_.r_dist_base) *
       yaw_switch_penalty}};  // 板朝向：切板时放大，避免拽动 a/w/h

  Eigen::MatrixXd R = R_dig.asDiagonal();

  // 定义非线性转换函数 h: x -> z
  auto h = [&](const Eigen::VectorXd & x) -> Eigen::Vector4d {
    Eigen::VectorXd xyz = h_armor_xyz(x, id);
    Eigen::VectorXd ypd = tools::xyz2ypd(xyz);
    auto angle = tools::limit_rad(x[idx::A] + id * 2 * CV_PI / armor_num_);
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

  // ---- 新息门控 ----
  // 目的：把"明显不可能"的观测挡在 EKF 之外，而不是让它以很大权重拉跑状态。
  // 判据：d^2 = nu^T S^{-1} nu，卡方分布自由度=观测维数 4。
  //   取 99.9% 分位数 18.47（而不是 95% 的 9.488）：这里只拦"粗差"，
  //   正常机动/换板造成的中等残差应当交给 R 自适应平滑吸收。
  {
    constexpr double kInnovationGateChi2 = 18.47;  // chi2(4, 0.999)
    const Eigen::VectorXd z_now{{armor.ypd_in_world[0], armor.ypd_in_world[1],
                                 armor.ypd_in_world[2], armor.ypr_in_world[0]}};
    Eigen::VectorXd nu = z_subtract(z_now, h(ekf_.x));
    Eigen::MatrixXd S = H * ekf_.P * H.transpose() + R;
    // 显式取 (0,0)：表达式是 1x1，直接赋给 double 依赖 Eigen 版本的隐式转换
    const double d2 = (nu.transpose() * S.inverse() * nu)(0, 0);
    if (std::isfinite(d2) && d2 > kInnovationGateChi2) {
      tools::RemoteLogger::instance().log(
        "DEBUG", "[Target] 新息门控丢弃观测：d2={:.2f} > {:.2f}（dist={:.2f}m conf={:.2f}）",
        d2, kInnovationGateChi2, armor.ypd_in_world[2], armor.confidence);
      return;
    }
  }

  const Eigen::VectorXd & ypd = armor.ypd_in_world;
  const Eigen::VectorXd & ypr = armor.ypr_in_world;
  Eigen::VectorXd z{{ypd[0], ypd[1], ypd[2], ypr[0]}};  // 获得观测量

  ekf_.update(z, H, R, h, z_subtract);
}

Eigen::VectorXd Target::ekf_x() const { return ekf_.x; }

const tools::ExtendedKalmanFilter & Target::ekf() const { return ekf_; }

GeoInit Target::geometric_params() const
{
  if (ekf_.x.size() < idx::DIM) return {};
  GeoInit g;
  g.r = ekf_.x[idx::R];
  g.l = ekf_.x[idx::L];
  g.h = ekf_.x[idx::H];
  return g;
}

void Target::set_w(double w) { ekf_.x[idx::W] = w; }

std::vector<Eigen::Vector4d> Target::armor_xyza_list() const
{
  std::vector<Eigen::Vector4d> _armor_xyza_list;

  for (int i = 0; i < armor_num_; i++) {
    auto angle = tools::limit_rad(ekf_.x[idx::A] + i * 2 * CV_PI / armor_num_);
    Eigen::Vector3d xyz = h_armor_xyz(ekf_.x, i);
    _armor_xyza_list.push_back({xyz[0], xyz[1], xyz[2], angle});
  }
  return _armor_xyza_list;
}

bool Target::diverged() const
{
  // ekf_.x 的维度恒为 idx::DIM。默认构造的 Target 其 ekf_.x 是空向量
  // （data() == nullptr），直接取下标等于解引用空指针 -> SIGSEGV。
  // 当前靠状态机不变量保命（state_ 只在 set_target 之后才离开 "lost"），
  // 但一改状态机就会崩。这里按"维度不足即视为发散"处理。
  if (ekf_.x.size() < idx::DIM) return true;

  // 旋转中心离云台太近同样是发散的表现：正常交战不会有 0.5 m 以内的目标，
  // 出现这种状态几乎都是脏框被 PnP 解成"贴脸"。
  const double center_dist = std::hypot(ekf_.x[idx::X], ekf_.x[idx::Y]);
  if (!std::isfinite(center_dist) || center_dist < 0.5) {
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Target] 旋转中心过近（d={:.3f} m），判为发散", center_dist);
    return true;
  }

  // 几何协方差发散（低可观测下的慢漂移）。
  // 参考 ckyf 的 robot_target_cv.cpp:360：`cov(R) > 1.0 || ...`。
  // 为什么要看协方差而不是只看状态值：r/l/h 每帧只有一块板可见，
  // 可观测性天然很弱。状态值可能还在合理区间（r=0.3），但协方差已经
  // 涨到 σ=1 m 量级——这时候的"合理值"只是噪声。
  const double cov_r = ekf_.P(idx::R, idx::R);
  const double cov_l = ekf_.P(idx::L, idx::L);
  const double cov_h = ekf_.P(idx::H, idx::H);
  if (cov_r > ekf_params_.max_geo_cov || cov_l > ekf_params_.max_geo_cov ||
      cov_h > ekf_params_.max_geo_cov) {
    tools::RemoteLogger::instance().log(
      "DEBUG", "[Target] 几何协方差发散：P(r)={:.3f} P(l)={:.3f} P(h)={:.3f}", cov_r, cov_l,
      cov_h);
    return true;
  }

  auto r_ok = ekf_.x[idx::R] > 0.05 && ekf_.x[idx::R] < 0.5;
  auto l_ok = ekf_.x[idx::R] + ekf_.x[idx::L] > 0.05 && ekf_.x[idx::R] + ekf_.x[idx::L] < 0.5;

  if (r_ok && l_ok) return false;

  tools::RemoteLogger::instance().log(
    "DEBUG", "[Target] r={:.3f}, l={:.3f}", ekf_.x[idx::R], ekf_.x[idx::L]);
  return true;
}

bool Target::convergened()
{
  if (this->name != ArmorName::outpost && update_count_ > 3 && !this->diverged()) {
    is_converged_ = true;
  }

  // 前哨站特殊判断
  if (this->name == ArmorName::outpost && update_count_ > 10 && !this->diverged()) {
    is_converged_ = true;
  }

  return is_converged_;
}

// 计算出装甲板中心的坐标（考虑长短轴）
Eigen::Vector3d Target::h_armor_xyz(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[idx::A] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[idx::R] + x[idx::L] : x[idx::R];
  auto armor_x = x[idx::X] - r * std::cos(angle);
  auto armor_y = x[idx::Y] - r * std::sin(angle);

  // 高度：
  //   4 板 —— 奇偶两档（x[Z] 与 x[Z]+h）
  //   3 板 —— 前哨站三块板**等间距递降**，相邻差 h，id 0 最高
  //   其它 —— 同高
  double armor_z;
  if (armor_num_ == 3) {
    armor_z = x[idx::Z] - id * x[idx::H];
  } else {
    armor_z = (use_l_h) ? x[idx::Z] + x[idx::H] : x[idx::Z];
  }

  return {armor_x, armor_y, armor_z};
}

Eigen::MatrixXd Target::h_jacobian(const Eigen::VectorXd & x, int id) const
{
  auto angle = tools::limit_rad(x[idx::A] + id * 2 * CV_PI / armor_num_);
  auto use_l_h = (armor_num_ == 4) && (id == 1 || id == 3);

  auto r = (use_l_h) ? x[idx::R] + x[idx::L] : x[idx::R];
  auto dx_da = r * std::sin(angle);
  auto dy_da = -r * std::cos(angle);

  auto dx_dr = -std::cos(angle);
  auto dy_dr = -std::sin(angle);
  auto dx_dl = (use_l_h) ? -std::cos(angle) : 0.0;
  auto dy_dl = (use_l_h) ? -std::sin(angle) : 0.0;

  // 3 板时 z = x[Z] - id*x[H] -> dz/dh = -id；4 板奇偶组 dz/dh = 1
  auto dz_dh = (armor_num_ == 3) ? -static_cast<double>(id) : ((use_l_h) ? 1.0 : 0.0);

  // 平移/加速度状态对装甲板位置的影响：只有位置分量进 H。
  // 加速度列全 0（观测不直接约束加速度，靠过程模型传播）。
  Eigen::MatrixXd H_armor_xyza = Eigen::MatrixXd::Zero(4, idx::DIM);
  H_armor_xyza(0, idx::X) = 1;
  H_armor_xyza(0, idx::A) = dx_da;
  H_armor_xyza(0, idx::R) = dx_dr;
  H_armor_xyza(0, idx::L) = dx_dl;
  H_armor_xyza(1, idx::Y) = 1;
  H_armor_xyza(1, idx::A) = dy_da;
  H_armor_xyza(1, idx::R) = dy_dr;
  H_armor_xyza(1, idx::L) = dy_dl;
  H_armor_xyza(2, idx::Z) = 1;
  H_armor_xyza(2, idx::H) = dz_dh;
  H_armor_xyza(3, idx::A) = 1;

  Eigen::VectorXd armor_xyz = h_armor_xyz(x, id);
  Eigen::MatrixXd H_armor_ypd = tools::xyz2ypd_jacobian(armor_xyz);
  Eigen::MatrixXd H_armor_ypda = Eigen::MatrixXd::Zero(4, 4);
  H_armor_ypda.block<3, 3>(0, 0) = H_armor_ypd;
  H_armor_ypda(3, 3) = 1;

  return H_armor_ypda * H_armor_xyza;
}

bool Target::checkinit() { return isinit; }

}  // namespace auto_aim
