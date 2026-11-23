// 基地（base）与前哨站共用 armor_num=3，但板高排布不同
//
// 前哨站：三块等间距递降，相邻差 h = 0.102 m（官方道具，几何固定）
// 基地  ：两层 3 块板，不是等间距三层递降
//
// 用户决定"基地先不改（只打其中 3 块）"，所以本轮只做**最小安全修法**：
//   - 前哨站 h 初值保持 0.102
//   - 基地 h 初值给 0（暂时按同高处理），由 EKF 从观测里学
//
// 若不加这个区分，基地会带着 102mm 的虚假递降初始化 —— 而竖直窗口只有
// ±19.6 mm，102mm 是它的 5.2 倍，另外两块板的高度会全错。
//
// 断言：
//   1) 前哨站初始化后 h == 0.102，三块板高度确实递降
//   2) 基地初始化后 h == 0，三块板高度相同（不会误用前哨站的递降）
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tasks/auto_aim/target.hpp"

void harness_on_log(const std::string &) {}

namespace
{
int fail = 0;
void check(bool ok, const char * what)
{
  if (!ok) {
    ++fail;
    std::printf("  [FAIL] %s\n", what);
  }
}

auto_aim::EkfParams default_params()
{
  auto_aim::EkfParams p;
  p.q_xyz_acc_var = 70.0;
  p.q_yaw_acc_var = 100.0;
  p.r_yaw = 5e-3;
  p.r_pitch = 2e-3;
  p.r_dist_base = 2e-1;
  p.q_geo = 0.0;
  p.omega_freeze_thresh = 0.5;
  return p;
}

auto_aim::Target make(auto_aim::ArmorName name, double radius)
{
  using namespace auto_aim;
  const double a = 0.0;
  const Eigen::Vector3d pos{4.5 - radius, 0.0, 1.5};

  std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
  Armor ar(Color::red, name, 0.9f, cv::Rect(100, 100, 40, 40), kp);
  ar.xyz_in_world = pos;
  ar.ypr_in_world = Eigen::Vector3d(a, 0.0, 0.0);
  ar.ypd_in_world =
    Eigen::Vector3d(std::atan2(pos.y(), pos.x()), std::atan2(pos.z(), std::hypot(pos.x(), pos.y())),
                    pos.norm());

  Eigen::VectorXd p0(11);
  p0 << 1, 64, 1, 64, 1, 64, 0.4, 100, 1.0e-4, 0, 0;
  return Target(ar, std::chrono::steady_clock::now(), radius, 3, p0, default_params(), {}, 0);
}
}  // namespace

int main()
{
  std::printf("=== 基地 / 前哨站 板高模型区分 ===\n\n");

  // ---------- 1) 前哨站：h=0.102，三块板递降 ----------
  {
    auto tgt = make(auto_aim::ArmorName::outpost, 0.2765);
    const auto plates = tgt.armor_xyza_list();
    const double h = tgt.ekf_x()[10];
    std::printf("1) 前哨站：h=%.4f m\n", h);
    for (std::size_t i = 0; i < plates.size(); ++i) {
      std::printf("     plate%zu z=%.4f\n", i, plates[i][2]);
    }
    check(std::abs(h - 0.102) < 1e-9, "前哨站 h 初值应为 0.102");
    check(plates[0][2] > plates[1][2] && plates[1][2] > plates[2][2], "前哨站三块板应递降");
    check(std::abs((plates[0][2] - plates[2][2]) - 0.204) < 1e-9, "前哨站首末板差应为 2h=0.204");
  }

  // ---------- 2) 基地：h=0，三块板同高（不误用前哨站模型）----------
  {
    auto tgt = make(auto_aim::ArmorName::base, 0.3205);
    const auto plates = tgt.armor_xyza_list();
    const double h = tgt.ekf_x()[10];
    std::printf("\n2) 基地：h=%.4f m\n", h);
    for (std::size_t i = 0; i < plates.size(); ++i) {
      std::printf("     plate%zu z=%.4f\n", i, plates[i][2]);
    }
    check(std::abs(h) < 1e-9, "基地 h 初值应为 0（不能套用前哨站的 0.102）");
    check(
      std::abs(plates[0][2] - plates[1][2]) < 1e-9 &&
        std::abs(plates[1][2] - plates[2][2]) < 1e-9,
      "基地三块板应同高（h=0 时退化为同高）");
  }

  // ---------- 3) GeoInit 显式指定时仍可覆盖（geo_cache 路径）----------
  {
    using namespace auto_aim;
    const Eigen::Vector3d pos{4.5 - 0.3205, 0.0, 1.5};
    std::vector<cv::Point2f> kp{{100.f, 100.f}, {140.f, 100.f}, {140.f, 140.f}, {100.f, 140.f}};
    Armor ar(Color::red, ArmorName::base, 0.9f, cv::Rect(100, 100, 40, 40), kp);
    ar.xyz_in_world = pos;
    ar.ypr_in_world = Eigen::Vector3d(0.0, 0.0, 0.0);
    ar.ypd_in_world =
      Eigen::Vector3d(0.0, std::atan2(1.5, 4.5 - 0.3205), std::hypot(4.5 - 0.3205, 1.5));

    Eigen::VectorXd p0(11);
    p0 << 1, 64, 1, 64, 1, 64, 0.4, 100, 1.0e-4, 0, 0;

    GeoInit geo;
    geo.r = 0.3205;
    geo.h = 0.05;   // 显式指定（模拟 geo_cache 给了值）
    Target tgt(ar, std::chrono::steady_clock::now(), 0.3205, 3, p0, default_params(), geo, 0);

    std::printf("\n3) 基地 + 显式 GeoInit(h=0.05)：h=%.4f\n", tgt.ekf_x()[10]);
    check(std::abs(tgt.ekf_x()[10] - 0.05) < 1e-9, "显式 GeoInit 应覆盖默认值");
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
