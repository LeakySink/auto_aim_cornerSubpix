#ifndef AUTO_AIM__ARMOR_HPP
#define AUTO_AIM__ARMOR_HPP

#include <Eigen/Dense>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace auto_aim
{
enum Color
{
  red,
  blue,
  extinguish,
  purple
};
const std::vector<std::string> COLORS = {"red", "blue", "extinguish", "purple"};

enum ArmorType
{
  big,
  small
};
const std::vector<std::string> ARMOR_TYPES = {"big", "small"};

enum ArmorName
{
  one,
  two,
  three,
  four,
  five,
  sentry,
  outpost,
  base,
  not_armor
};
const std::vector<std::string> ARMOR_NAMES = {"one",    "two",     "three", "four",     "five",
                                              "sentry", "outpost", "base",  "not_armor"};

enum ArmorPriority
{
  first = 1,
  second,
  third,
  forth,
  fifth
};

// clang-format off
const std::vector<std::tuple<Color, ArmorName, ArmorType>> armor_properties = {
  {blue, sentry, small},     {red, sentry, small},     {extinguish, sentry, small},
  {blue, one, small},        {red, one, small},        {extinguish, one, small},
  {blue, two, small},        {red, two, small},        {extinguish, two, small},
  {blue, three, small},      {red, three, small},      {extinguish, three, small},
  {blue, four, small},       {red, four, small},       {extinguish, four, small},
  {blue, five, small},       {red, five, small},       {extinguish, five, small},
  {blue, outpost, small},    {red, outpost, small},    {extinguish, outpost, small},
  {blue, base, big},         {red, base, big},         {extinguish, base, big},      {purple, base, big},       
  {blue, base, small},       {red, base, small},       {extinguish, base, small},    {purple, base, small},    
  {blue, three, big},        {red, three, big},        {extinguish, three, big}, 
  {blue, four, big},         {red, four, big},         {extinguish, four, big},  
  {blue, five, big},         {red, five, big},         {extinguish, five, big}};
// clang-format on

struct Lightbar
{
  std::size_t id;
  Color color;
  cv::Point2f center, top, bottom, top2bottom;
  std::vector<cv::Point2f> points;
  double angle, angle_error, length, width, ratio;
  cv::RotatedRect rotated_rect;

  Lightbar(const cv::RotatedRect & rotated_rect, std::size_t id);
  Lightbar() {};
};

struct Armor
{
  Color color;
  Lightbar left, right;     //used to be const
  cv::Point2f center;       // 不是对角线交点，不能作为实际中心！
  // 归一化坐标（0~1）。Tracker 用它挑"最靠画面中心"的板，所以必须有确定的值：
  // cv::Point2f 的默认构造并不清零，未赋值时是栈上垃圾。给 (0,0) 表示"离中心最远"，
  // 排序时自然落到最后（正常情况下 4 个 YOLO 与传统 Detector 都会填它）。
  cv::Point2f center_norm{0.0f, 0.0f};
  std::vector<cv::Point2f> points;

  double ratio;              // 两灯条的中点连线与长灯条的长度之比
  double side_ratio = 1.0;   // 长灯条与短灯条的长度之比（只有传统方法会算，给默认值）
  double rectangular_error;  // 灯条和中点连线所成夹角与π/2的差值

  ArmorType type = ArmorType::small;
  ArmorName name = ArmorName::not_armor;
  // priority 必须有默认值：Tracker::track() 每次都会对整张 armors 列表按 priority 排序
  // （tracker.cpp 两处 std::list::sort），而 7 个自瞄入口（standard / uav / mt_standard /
  // mt_auto_aim_debug / standard_mpc / uav_debug / auto_aim_debug_mpc）从不给 priority
  // 赋值（只有走 Decider::set_priority 的 sentry 那几个入口会赋值）。读未初始化的枚举
  // 值是 UB，而且比较器会退化成不满足严格弱序 —— armors.front() 选中哪块板不确定。
  ArmorPriority priority = ArmorPriority::third;
  int class_id;
  cv::Rect box;
  cv::Mat pattern;
  double confidence;
  bool duplicated = false;

  Eigen::Vector3d xyz_in_gimbal;  // 单位：m
  Eigen::Vector3d xyz_in_world;   // 单位：m
  Eigen::Vector3d ypr_in_gimbal;  // 单位：rad
  Eigen::Vector3d ypr_in_world;   // 单位：rad
  Eigen::Vector3d ypd_in_world;   // 球坐标系

  double yaw_raw = 0.0;  // rad；只在 Solver::solve() 里赋值，调试输出可能早于它

  // ---- 平面靶 PnP 二义性的**备用解**（由 Solver::solve 填写）----
  //
  // 4 点平面靶用 IPPE 求解时天然有**两个**近似等价的位姿：二者关于视线方向
  // 镜像。单帧重投影误差几乎相同，OpenCV 只返回其中一个 —— 而这个"挑哪个"
  // 在实机上近似掷硬币。
  //
  // 实测（2026-11 实机 rlog，前哨站小装甲 @4.4 m，三份 log 一致）：
  //   · 两解相差约 72°（71.7° / 71.5° / 72.1°）
  //   · 约 27% 的帧在两解之间跳变
  //   · 两解的**距离只差 32~35 mm**（远小于距离噪声 55 mm）、
  //     `obs_pitch` 相同 —— 即两个解只差朝向、不差位置
  //   · 两簇精确对称分布在"圆心方位"两侧各 36°
  //
  // 因为只差朝向，`n̂·p > 0` 的可见性判据**无法**消歧（两解都朝向相机）。
  // 唯一可用的是**时序一致性**：所以这里把第二个解也存下来，交给
  // Target::update 在选定板号内择一（板号本身仍只由主解决定 —— 镜像解在
  // ±60° 附近会与相邻板重合，让它参与改板号会把关联带飞）。
  bool has_alt = false;
  Eigen::Vector3d xyz_in_world_alt = Eigen::Vector3d::Zero();
  Eigen::Vector3d ypr_in_world_alt = Eigen::Vector3d::Zero();

  Armor(const Lightbar & left, const Lightbar & right);
  Armor(
    int class_id, float confidence, const cv::Rect & box, std::vector<cv::Point2f> armor_keypoints);
  Armor(
    int class_id, float confidence, const cv::Rect & box, std::vector<cv::Point2f> armor_keypoints,
    cv::Point2f offset);
  Armor(
    int color_id, int num_id, float confidence, const cv::Rect & box,
    std::vector<cv::Point2f> armor_keypoints);
  Armor(
    int color_id, int num_id, float confidence, const cv::Rect & box,
    std::vector<cv::Point2f> armor_keypoints, cv::Point2f offset);
  // TUP模型构造函数: 直接给定颜色与类别, 类型默认为small
  Armor(
    Color color, ArmorName name, float confidence, const cv::Rect & box,
    std::vector<cv::Point2f> armor_keypoints);
  Armor(
    Color color, ArmorName name, float confidence, const cv::Rect & box,
    std::vector<cv::Point2f> armor_keypoints, cv::Point2f offset);
};

}  // namespace auto_aim

#endif  // AUTO_AIM__ARMOR_HPP