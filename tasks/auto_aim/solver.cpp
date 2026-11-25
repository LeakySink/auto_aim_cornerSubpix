#include "solver.hpp"

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <vector>

#include "armor_geometry.hpp"
#include "keypoint_check.hpp"
#include "tools/remote_logger.hpp"
#include "tools/math_tools.hpp"

namespace auto_aim
{

Solver::Solver(const std::string & config_path) : R_gimbal2world_(Eigen::Matrix3d::Identity())
{
  auto yaml = YAML::LoadFile(config_path);

  auto R_gimbal2imubody_data = yaml["R_gimbal2imubody"].as<std::vector<double>>();
  auto R_camera2gimbal_data = yaml["R_camera2gimbal"].as<std::vector<double>>();
  auto t_camera2gimbal_data = yaml["t_camera2gimbal"].as<std::vector<double>>();
  R_gimbal2imubody_ = Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(R_gimbal2imubody_data.data());
  R_camera2gimbal_ = Eigen::Matrix<double, 3, 3, Eigen::RowMajor>(R_camera2gimbal_data.data());
  t_camera2gimbal_ = Eigen::Matrix<double, 3, 1>(t_camera2gimbal_data.data());

  auto camera_matrix_data = yaml["camera_matrix"].as<std::vector<double>>();
  auto distort_coeffs_data = yaml["distort_coeffs"].as<std::vector<double>>();
  Eigen::Matrix<double, 3, 3, Eigen::RowMajor> camera_matrix(camera_matrix_data.data());
  Eigen::Matrix<double, 1, 5> distort_coeffs(distort_coeffs_data.data());
  cv::eigen2cv(camera_matrix, camera_matrix_);
  cv::eigen2cv(distort_coeffs, distort_coeffs_);

  // 可选门限：位姿解算结果的合理距离范围（缺省 0.2 ~ 50 m）。
  // 注意用 IsDefined() 判断，不能用 tools::read —— 它对缺键会直接 exit(1)。
  if (yaml["min_valid_dist"] && yaml["min_valid_dist"].IsDefined()) {
    min_valid_dist_ = yaml["min_valid_dist"].as<double>();
  }
  if (yaml["max_valid_dist"] && yaml["max_valid_dist"].IsDefined()) {
    max_valid_dist_ = yaml["max_valid_dist"].as<double>();
  }

  // 装甲板几何（板宽/灯条长/倾角/厚度）：1 月换样式只改 yaml（见 armor_geometry.hpp）
  load_armor_geometry(yaml);

  // 关键点语义校验默认只记 WARN（不改变现有行为）；设为 true 才真的拒收
  if (yaml["reject_bad_keypoints"] && yaml["reject_bad_keypoints"].IsDefined()) {
    reject_bad_keypoints_ = yaml["reject_bad_keypoints"].as<bool>();
  }
}

Eigen::Matrix3d Solver::R_gimbal2world() const { return R_gimbal2world_; }

void Solver::set_R_gimbal2world(const Eigen::Quaterniond & q)
{
  Eigen::Matrix3d R_imubody2imuabs = q.toRotationMatrix();
  R_gimbal2world_ = R_gimbal2imubody_.transpose() * R_imubody2imuabs * R_gimbal2imubody_;
}

//solvePnP（获得姿态）
bool Solver::solve(Armor & armor) const
{
  // 清零而不是留 NaN：万一调用方忘了检查返回值，也不会把 NaN 带进滤波/可视化
  auto zero_pose = [&armor]() {
    armor.xyz_in_gimbal = Eigen::Vector3d::Zero();
    armor.xyz_in_world = Eigen::Vector3d::Zero();
    armor.ypr_in_gimbal = Eigen::Vector3d::Zero();
    armor.ypr_in_world = Eigen::Vector3d::Zero();
    armor.ypd_in_world = Eigen::Vector3d::Zero();
    armor.has_alt = false;
    armor.xyz_in_world_alt = Eigen::Vector3d::Zero();
    armor.ypr_in_world_alt = Eigen::Vector3d::Zero();
  };

  // 关键点语义校验：换模型时第一时间发现"顺序/语义接错"（默认只记日志、不拒收）
  // 注意必须放在 reject/clear 定义之前能用的形态（这里用 zero_pose 直接清零返回）
  const auto kp = check_keypoints(armor.points, armor_geometry(), armor.type == ArmorType::big);
  if (!kp.ok) {
    static std::atomic<int> bad_kp_count{0};
    const int n = ++bad_kp_count;
    if (n <= 5 || n % 200 == 0) {
      tools::RemoteLogger::instance().log(
        "WARN",
        "[Solver] 关键点校验失败：{}（aspect={:.2f} 上/下={:.2f} 左/右={:.2f}，累计 {} 次）",
        kp.reason, kp.aspect, kp.top_bottom_ratio, kp.left_right_ratio, n);
    }
    if (reject_bad_keypoints_) {
      zero_pose();
      return false;
    }
  }

  const auto object_points = armor_geometry().points(armor.type);

  // ---- 取 IPPE 的**全部**解（通常是 2 个），不只 OpenCV 挑的那个 ----
  //
  // 平面靶天然二义：两个位姿关于视线镜像，单帧重投影误差几乎相同，
  // 噪声一大"挑哪个"就近似掷硬币。旧实现用 cv::solvePnP（只回一个），
  // 实机实测 27% 的帧在两解间跳变，是 w 估计被污染的主因。
  //
  // 这里两个都留下：主解走原有逻辑；备用解经同样的坐标变换与合理性校验后
  // 存进 armor 的 `_alt` 字段，由 Target::update 在选定板号内按"与预测更贴合"
  // 逐帧择一（板号本身仍只由主解决定，理由见 target.cpp 的说明）。
  std::vector<cv::Mat> rvecs, tvecs;
  cv::solvePnPGeneric(
    object_points, armor.points, camera_matrix_, distort_coeffs_, rvecs, tvecs, false,
    cv::SOLVEPNP_IPPE);
  if (rvecs.empty() || tvecs.empty()) {
    tools::RemoteLogger::instance().log("WARN", "[Solver] IPPE 无解，丢弃该装甲板");
    zero_pose();
    return false;
  }

  // rvec/tvec 是 3x1（或 1x3）的 CV_64F，按连续内存取前 3 个 double
  auto mat2vec = [](const cv::Mat & m) {
    const double * p = m.ptr<double>(0);
    return cv::Vec3d(p[0], p[1], p[2]);
  };
  const cv::Vec3d rvec = mat2vec(rvecs[0]);
  const cv::Vec3d tvec = mat2vec(tvecs[0]);

  Eigen::Vector3d xyz_in_camera;
  cv::cv2eigen(tvec, xyz_in_camera);

  // 位姿可信度校验（2026-10 加固）。动机：远距离低置信度框只有几个像素，
  // 4 个角点的噪声会让 IPPE 解出几十米甚至 NaN 的位姿；这种观测一旦进 EKF，
  // 11 维状态会被污染（r/l/h 又没有过程噪声，很难自己恢复），下游 planner
  // 便报"弹道无解"、云台不受控。这里统一在"进 EKF 之前"拦掉。
  auto reject = [&](const char * why) {
    tools::RemoteLogger::instance().log(
      "WARN", "[Solver] 丢弃不可信位姿（{}）：cam_z={:.3f} conf={:.2f}", why, xyz_in_camera[2],
      armor.confidence);
    zero_pose();
    return false;
  };

  // 相机系 z 必须为正（目标在相机前方），且位姿必须有限
  if (!xyz_in_camera.allFinite() || !(xyz_in_camera[2] > 0.0)) {
    return reject("相机系深度非法/非有限");
  }

  armor.xyz_in_gimbal = R_camera2gimbal_ * xyz_in_camera + t_camera2gimbal_;
  armor.xyz_in_world = R_gimbal2world_ * armor.xyz_in_gimbal;

  cv::Mat rmat;
  cv::Rodrigues(rvec, rmat);
  Eigen::Matrix3d R_armor2camera;
  cv::cv2eigen(rmat, R_armor2camera);
  Eigen::Matrix3d R_armor2gimbal = R_camera2gimbal_ * R_armor2camera;
  Eigen::Matrix3d R_armor2world = R_gimbal2world_ * R_armor2gimbal;
  armor.ypr_in_gimbal = tools::eulers(R_armor2gimbal, 2, 1, 0);
  armor.ypr_in_world = tools::eulers(R_armor2world, 2, 1, 0);

  armor.ypd_in_world = tools::xyz2ypd(armor.xyz_in_world);

  // 世界系结果必须有限，且距离落在合理窗口内（缺省 0.2 ~ 50 m，可用 yaml 覆盖）
  if (
    !armor.xyz_in_world.allFinite() || !armor.ypr_in_world.allFinite() ||
    !armor.ypd_in_world.allFinite()) {
    return reject("世界系结果非有限");
  }
  const double dist = armor.xyz_in_world.norm();
  if (dist < min_valid_dist_ || dist > max_valid_dist_) {
    tools::RemoteLogger::instance().log(
      "WARN", "[Solver] 丢弃距离越界位姿：dist={:.2f} m（允许 {:.2f}~{:.2f}） conf={:.2f}", dist,
      min_valid_dist_, max_valid_dist_, armor.confidence);
    zero_pose();
    return false;
  }

  // ---- 备用解：与主解**同样的**坐标变换与合理性校验 ----
  // 校验不过就干脆不给（has_alt=false），宁可退回旧行为也不要塞脏数据。
  armor.has_alt = false;
  if (rvecs.size() >= 2) {
    const cv::Vec3d rvec_a = mat2vec(rvecs[1]);
    const cv::Vec3d tvec_a = mat2vec(tvecs[1]);
    Eigen::Vector3d xyz_c_a;
    cv::cv2eigen(tvec_a, xyz_c_a);
    if (xyz_c_a.allFinite() && xyz_c_a[2] > 0.0) {
      cv::Mat rmat_a;
      cv::Rodrigues(rvec_a, rmat_a);
      Eigen::Matrix3d R_a2c_a;
      cv::cv2eigen(rmat_a, R_a2c_a);
      const Eigen::Matrix3d R_a2w_a = R_gimbal2world_ * (R_camera2gimbal_ * R_a2c_a);
      const Eigen::Vector3d xyz_w_a =
        R_gimbal2world_ * (R_camera2gimbal_ * xyz_c_a + t_camera2gimbal_);
      const Eigen::Vector3d ypr_w_a = tools::eulers(R_a2w_a, 2, 1, 0);
      const double d_a = xyz_w_a.norm();
      if (
        xyz_w_a.allFinite() && ypr_w_a.allFinite() && d_a >= min_valid_dist_ &&
        d_a <= max_valid_dist_) {
        armor.xyz_in_world_alt = xyz_w_a;
        armor.ypr_in_world_alt = ypr_w_a;
        armor.has_alt = true;
      }
    }
  }

  // 平衡不做yaw优化，因为pitch假设不成立
  auto is_balance = (armor.type == ArmorType::big) &&
                    (armor.name == ArmorName::three || armor.name == ArmorName::four ||
                     armor.name == ArmorName::five);
  if (is_balance) return true;

  // optimize_yaw(armor);
  return true;
}

std::vector<cv::Point2f> Solver::reproject_armor(
  const Eigen::Vector3d & xyz_in_world, double yaw, ArmorType type, ArmorName name) const
{
  auto sin_yaw = std::sin(yaw);
  auto cos_yaw = std::cos(yaw);

  auto pitch = armor_geometry().tilt_rad(name);
  auto sin_pitch = std::sin(pitch);
  auto cos_pitch = std::cos(pitch);

  // clang-format off
  const Eigen::Matrix3d R_armor2world {
    {cos_yaw * cos_pitch, -sin_yaw, cos_yaw * sin_pitch},
    {sin_yaw * cos_pitch,  cos_yaw, sin_yaw * sin_pitch},
    {         -sin_pitch,        0,           cos_pitch}
  };
  // clang-format on

  // get R_armor2camera t_armor2camera
  const Eigen::Vector3d & t_armor2world = xyz_in_world;
  Eigen::Matrix3d R_armor2camera =
    R_camera2gimbal_.transpose() * R_gimbal2world_.transpose() * R_armor2world;
  Eigen::Vector3d t_armor2camera =
    R_camera2gimbal_.transpose() * (R_gimbal2world_.transpose() * t_armor2world - t_camera2gimbal_);

  // get rvec tvec
  cv::Vec3d rvec;
  cv::Mat R_armor2camera_cv;
  cv::eigen2cv(R_armor2camera, R_armor2camera_cv);
  cv::Rodrigues(R_armor2camera_cv, rvec);
  cv::Vec3d tvec(t_armor2camera[0], t_armor2camera[1], t_armor2camera[2]);

  // reproject
  std::vector<cv::Point2f> image_points;
  const auto object_points = armor_geometry().points(type);
  cv::projectPoints(object_points, rvec, tvec, camera_matrix_, distort_coeffs_, image_points);
  return image_points;
}

double Solver::oupost_reprojection_error(Armor armor, const double & pitch)
{
  // solve
  const auto object_points = armor_geometry().points(armor.type);

  cv::Vec3d rvec, tvec;
  cv::solvePnP(
    object_points, armor.points, camera_matrix_, distort_coeffs_, rvec, tvec, false,
    cv::SOLVEPNP_IPPE);

  Eigen::Vector3d xyz_in_camera;
  cv::cv2eigen(tvec, xyz_in_camera);
  armor.xyz_in_gimbal = R_camera2gimbal_ * xyz_in_camera + t_camera2gimbal_;
  armor.xyz_in_world = R_gimbal2world_ * armor.xyz_in_gimbal;

  cv::Mat rmat;
  cv::Rodrigues(rvec, rmat);
  Eigen::Matrix3d R_armor2camera;
  cv::cv2eigen(rmat, R_armor2camera);
  Eigen::Matrix3d R_armor2gimbal = R_camera2gimbal_ * R_armor2camera;
  Eigen::Matrix3d R_armor2world = R_gimbal2world_ * R_armor2gimbal;
  armor.ypr_in_gimbal = tools::eulers(R_armor2gimbal, 2, 1, 0);
  armor.ypr_in_world = tools::eulers(R_armor2world, 2, 1, 0);

  armor.ypd_in_world = tools::xyz2ypd(armor.xyz_in_world);

  auto yaw = armor.ypr_in_world[0];
  auto xyz_in_world = armor.xyz_in_world;

  auto sin_yaw = std::sin(yaw);
  auto cos_yaw = std::cos(yaw);

  auto sin_pitch = std::sin(pitch);
  auto cos_pitch = std::cos(pitch);

  // clang-format off
  const Eigen::Matrix3d _R_armor2world {
    {cos_yaw * cos_pitch, -sin_yaw, cos_yaw * sin_pitch},
    {sin_yaw * cos_pitch,  cos_yaw, sin_yaw * sin_pitch},
    {         -sin_pitch,        0,           cos_pitch}
  };
  // clang-format on

  // get R_armor2camera t_armor2camera
  const Eigen::Vector3d & t_armor2world = xyz_in_world;
  Eigen::Matrix3d _R_armor2camera =
    R_camera2gimbal_.transpose() * R_gimbal2world_.transpose() * _R_armor2world;
  Eigen::Vector3d t_armor2camera =
    R_camera2gimbal_.transpose() * (R_gimbal2world_.transpose() * t_armor2world - t_camera2gimbal_);

  // get rvec tvec
  cv::Vec3d _rvec;
  cv::Mat R_armor2camera_cv;
  cv::eigen2cv(_R_armor2camera, R_armor2camera_cv);
  cv::Rodrigues(R_armor2camera_cv, _rvec);
  cv::Vec3d _tvec(t_armor2camera[0], t_armor2camera[1], t_armor2camera[2]);

  // reproject
  std::vector<cv::Point2f> image_points;
  cv::projectPoints(object_points, _rvec, _tvec, camera_matrix_, distort_coeffs_, image_points);

  auto error = 0.0;
  for (int i = 0; i < 4; i++) error += cv::norm(armor.points[i] - image_points[i]);
  return error;
}

void Solver::optimize_yaw(Armor & armor) const
{
  Eigen::Vector3d gimbal_ypr = tools::eulers(R_gimbal2world_, 2, 1, 0);

  constexpr double SEARCH_RANGE = 140;  // degree
  auto yaw0 = tools::limit_rad(gimbal_ypr[0] - SEARCH_RANGE / 2 * CV_PI / 180.0);

  auto min_error = 1e10;
  auto best_yaw = armor.ypr_in_world[0];

  for (int i = 0; i < SEARCH_RANGE; i++) {
    double yaw = tools::limit_rad(yaw0 + i * CV_PI / 180.0);
    auto error = armor_reprojection_error(armor, yaw, (i - SEARCH_RANGE / 2) * CV_PI / 180.0);

    if (error < min_error) {
      min_error = error;
      best_yaw = yaw;
    }
  }

  armor.yaw_raw = armor.ypr_in_world[0];
  armor.ypr_in_world[0] = best_yaw;
}

double Solver::SJTU_cost(
  const std::vector<cv::Point2f> & cv_refs, const std::vector<cv::Point2f> & cv_pts,
  const double & inclined) const
{
  std::size_t size = cv_refs.size();
  std::vector<Eigen::Vector2d> refs;
  std::vector<Eigen::Vector2d> pts;
  for (std::size_t i = 0u; i < size; ++i) {
    refs.emplace_back(cv_refs[i].x, cv_refs[i].y);
    pts.emplace_back(cv_pts[i].x, cv_pts[i].y);
  }
  double cost = 0.;
  for (std::size_t i = 0u; i < size; ++i) {
    std::size_t p = (i + 1u) % size;
    // i - p 构成线段。过程：先移动起点，再补长度，再旋转
    Eigen::Vector2d ref_d = refs[p] - refs[i];  // 标准
    Eigen::Vector2d pt_d = pts[p] - pts[i];
    // 长度差代价 + 起点差代价(1 / 2)（0 度左右应该抛弃)
    double pixel_dis =  // dis 是指方差平面内到原点的距离
      (0.5 * ((refs[i] - pts[i]).norm() + (refs[p] - pts[p]).norm()) +
       std::fabs(ref_d.norm() - pt_d.norm())) /
      ref_d.norm();
    double angular_dis = ref_d.norm() * tools::get_abs_angle(ref_d, pt_d) / ref_d.norm();
    // 平方可能是为了配合 sin 和 cos
    // 弧度差代价（0 度左右占比应该大）
    double cost_i =
      tools::square(pixel_dis * std::sin(inclined)) +
      tools::square(angular_dis * std::cos(inclined)) * 2.0;  // DETECTOR_ERROR_PIXEL_BY_SLOPE
    // 重投影像素误差越大，越相信斜率
    cost += std::sqrt(cost_i);
  }
  return cost;
}

double Solver::armor_reprojection_error(
  const Armor & armor, double yaw, const double & inclined) const
{
  auto image_points = reproject_armor(armor.xyz_in_world, yaw, armor.type, armor.name);
  auto error = 0.0;
  for (int i = 0; i < 4; i++) error += cv::norm(armor.points[i] - image_points[i]);

  return error;
}

// 世界坐标到像素坐标的转换
std::vector<cv::Point2f> Solver::world2pixel(const std::vector<cv::Point3f> & worldPoints)
{
  Eigen::Matrix3d R_world2camera = R_camera2gimbal_.transpose() * R_gimbal2world_.transpose();
  Eigen::Vector3d t_world2camera = -R_camera2gimbal_.transpose() * t_camera2gimbal_;

  cv::Mat rvec;
  cv::Mat tvec;
  cv::eigen2cv(R_world2camera, rvec);
  cv::eigen2cv(t_world2camera, tvec);

  std::vector<cv::Point3f> valid_world_points;
  for (const auto & world_point : worldPoints) {
    Eigen::Vector3d world_point_eigen(world_point.x, world_point.y, world_point.z);
    Eigen::Vector3d camera_point = R_world2camera * world_point_eigen + t_world2camera;

    if (camera_point.z() > 0) {
      valid_world_points.push_back(world_point);
    }
  }
  // 如果没有有效点，返回空vector
  if (valid_world_points.empty()) {
    return std::vector<cv::Point2f>();
  }
  std::vector<cv::Point2f> pixelPoints;
  cv::projectPoints(valid_world_points, rvec, tvec, camera_matrix_, distort_coeffs_, pixelPoints);
  return pixelPoints;
}
}  // namespace auto_aim