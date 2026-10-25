// 校验"多项式弹道"分支（ballistics-main 离线拟合产物接入）：
//   1) CSV 解析与基函数求值 是否与 ballistic-main/test.py:predict() 完全一致
//   2) 系数单位自检（pitch 列是"度/米"量级，不是弧度）
//   3) 多项式 vs 内置 RK4 的 pitch / fly_time 对照（含 sentry.yaml 里 0.2415 的失效边界）
//   4) 超出拟合范围时必须回退 RK4（used_poly == false），不能静默外推
//   5) 弹速超出表上下限时按端点夹取（solve 返回 true 并给出 note）
//
// 编译（本机无 OpenCV/Eigen 也能跑，这三个文件都不依赖它们）：
//   g++ -std=c++17 -O2 -I. -Itools tests/algo/ballistics_poly_check.cpp
//       tools/ballistics_poly.cpp tools/trajectory.cpp -o ballistics_poly_check
//   ./ballistics_poly_check assets/ballistic/small.csv

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/ballistics_poly.hpp"
#include "tools/trajectory.hpp"

namespace
{
int g_fail = 0;

void expect(bool ok, const std::string & what)
{
  if (!ok) {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
}

// 独立复刻 ballistic-main/test.py:predict()，用于交叉验证 BallisticsPoly::eval
double ref_predict(const std::array<double, 10> & p, double x, double y)
{
  return p[0] + p[1] * x + p[2] * y + p[3] * x * x + p[4] * x * y + p[5] * y * y +
         p[6] * x * x * x + p[7] * x * x * y + p[8] * x * y * y + p[9] * y * y * y;
}

struct R
{
  bool ok = false;
  bool poly = false;
  double pitch_deg = 0.0;
  double t = 0.0;
};

R solve_traj(double v0, double d, double h, double k)
{
  const tools::Trajectory tr(v0, d, h, k);
  R r;
  r.ok = !tr.unsolvable;
  r.poly = tr.used_poly;
  r.pitch_deg = tr.pitch * 180.0 / 3.14159265358979323846;
  r.t = tr.fly_time;
  return r;
}
}  // namespace

int main(int argc, char ** argv)
{
  const std::string csv = (argc > 1) ? argv[1] : "assets/ballistic/small.csv";

  // ---------- 1) 解析 ----------
  auto model = std::make_shared<tools::BallisticsPoly>();
  std::string err;
  if (!model->load_csv(csv, &err)) {
    std::printf("加载 %s 失败：%s\n", csv.c_str(), err.c_str());
    return 2;
  }
  std::printf("== 1) 拟合表解析 ==\n  文件=%s 行数=%zu  V∈[%.2f, %.2f] m/s\n", csv.c_str(),
              model->size(), model->v_min(), model->v_max());
  expect(model->size() > 100, "行数应 >100");
  expect(model->v_min() > 12.0 && model->v_min() < 13.0, "V 下限应 ≈12.54");
  expect(model->v_max() > 29.0 && model->v_max() < 30.5, "V 上限应 ≈29.91");

  // ---------- 2) 基函数与单位自检 ----------
  std::printf("\n== 2) 基函数一致性 + 单位自检 ==\n");
  // 重新读第一行与最后一行系数，用独立实现对比 eval
  {
    std::vector<tools::BallisticsRow> rows;
    {
      // 借 load_rows 不暴露行数据，这里直接从 CSV 里手工取两行做对比
      FILE * fp = std::fopen(csv.c_str(), "rb");
      expect(fp != nullptr, "能打开 CSV");
      char line[4096];
      int taken = 0;
      while (fp && std::fgets(line, sizeof(line), fp)) {
        std::string s(line);
        if (s.find('[') == std::string::npos) continue;
        // 提取方括号里的两组系数
        std::vector<double> nums;
        for (std::size_t i = 0; i < s.size(); ++i) {
          if (s[i] == '-' || s[i] == '+' || (s[i] >= '0' && s[i] <= '9') || s[i] == '.') {
            std::size_t j = i;
            while (j < s.size() && (s[j] == '-' || s[j] == '+' || (s[j] >= '0' && s[j] <= '9') ||
                                    s[j] == '.' || s[j] == 'e' || s[j] == 'E'))
              ++j;
            try {
              nums.push_back(std::stod(s.substr(i, j - i)));
            } catch (...) {
            }
            i = j;
          }
        }
        // 第 1 个数是 V，后 20 个是 pitch/time
        if (nums.size() >= 21) {
          tools::BallisticsRow row;
          row.v = nums[0];
          for (std::size_t k = 0; k < 10; ++k) {
            row.pitch[k] = nums[1 + k];
            row.time[k] = nums[11 + k];
          }
          rows.push_back(row);
          if (++taken >= 2) break;
        }
      }
      if (fp) std::fclose(fp);
    }
    expect(rows.size() == 2, "手工取到 2 行");
    if (rows.size() == 2) {
      for (const auto & row : rows) {
        for (double x : {2.0, 5.0, 9.5}) {
          for (double y : {-1.0, 0.0, 1.5}) {
            const double mine = tools::BallisticsPoly::eval(row.pitch, x, y);
            const double ref = ref_predict(row.pitch, x, y);
            expect(std::abs(mine - ref) < 1e-9, "eval 与 test.py:predict 一致");
          }
        }
        // 单位自检：c1 应 ≈ 无阻力解析斜率 g/(2V²)（单位 度/米），拖阻力使其略大
        const double slope_deg_per_m = row.pitch[1];
        const double no_drag = 9.8 / (2.0 * row.v * row.v) * 180.0 / 3.14159265358979323846;
        const double ratio = slope_deg_per_m / no_drag;
        std::printf("  V=%6.2f m/s : c1=%.4f deg/m，无阻力解析=%.4f deg/m，比值=%.3f\n", row.v,
                    slope_deg_per_m, no_drag, ratio);
        expect(ratio > 0.9 && ratio < 1.6, "c1 量级应接近 no-drag 斜率（说明单位是度、x 是米）");
      }
    }
  }

  // ---------- 3) 多项式 vs RK4 ----------
  std::printf("\n== 3) 多项式 vs 内置 RK4（h=0）==\n");
  const double k_phys = 0.01875;  // = ρ·Cd·S/(2M)，17mm 物理值（ballistic-main 参数）
  const double k_bad = 0.2415;    // sentry.yaml 原值
  for (double v0 : {22.0, 28.0}) {
    std::printf("  v0=%.1f m/s\n   %-6s %-18s %-18s %-18s\n", v0, "d(m)", "poly(本模型)", "RK4 k=0.01875",
                "RK4 k=0.2415");
    for (double d : {5.0, 8.0, 10.0, 12.0, 15.0}) {
      std::string note;
      double pr = 0.0, pt = 0.0;
      const bool pok = model->solve(v0, d, 0.0, pr, pt, &note);
      tools::install_ballistics_model(nullptr);
      const auto r1 = solve_traj(v0, d, 0.0, k_phys);
      const auto r2 = solve_traj(v0, d, 0.0, k_bad);
      tools::install_ballistics_model(model);
      char c1[64], c2[64], c3[64];
      std::snprintf(
        c1, sizeof(c1), "%s %.3fdeg t=%.3f", pok ? "ok  " : "FAIL", pr * 180 / 3.14159265358979323846, pt);
      if (r1.ok)
        std::snprintf(c2, sizeof(c2), "ok   %.3fdeg t=%.3f", r1.pitch_deg, r1.t);
      else
        std::snprintf(c2, sizeof(c2), "无解");
      if (r2.ok)
        std::snprintf(c3, sizeof(c3), "ok   %.3fdeg t=%.3f", r2.pitch_deg, r2.t);
      else
        std::snprintf(c3, sizeof(c3), "无解（>7.2m 即此路）");
      std::printf("   %-6.1f %-18s %-18s %-18s\n", d, c1, c2, c3);
      expect(pok, "多项式应能求解该点");
    }
  }

  // ---------- 4) 范围外回退 RK4 ----------
  std::printf("\n== 4) 超出拟合表范围 → 回退 RK4 ==\n");
  tools::install_ballistics_model(model);
  {
    std::string note;
    double pr = 0.0, pt = 0.0;
    const bool ok = model->solve(22.0, 20.0, 0.0, pr, pt, &note);
    std::printf("  model.solve(d=20m) -> %s（note: %s）\n", ok ? "true" : "false", note.c_str());
    expect(!ok, "d=20m 超出表范围应返回 false");
    const auto r = solve_traj(22.0, 20.0, 0.0, k_phys);
    std::printf("  Trajectory(20m) -> ok=%d used_poly=%d pitch=%.3fdeg\n", r.ok ? 1 : 0,
                r.poly ? 1 : 0, r.pitch_deg);
    expect(r.ok && !r.poly, "应回退 RK4 且可解");
  }

  // ---------- 5) 弹速夹取 ----------
  std::printf("\n== 5) 弹速超出表范围 → 端点夹取 ==\n");
  {
    std::string note;
    double pr = 0.0, pt = 0.0;
    const bool ok = model->solve(10.0, 8.0, 0.0, pr, pt, &note);
    std::printf("  solve(v0=10) -> %s pitch=%.3fdeg（note: %s）\n", ok ? "true" : "false",
                pr * 180 / 3.14159265358979323846, note.c_str());
    expect(ok, "低于表下限时应按端点夹取而不是失败");
  }

  // ---------- 6) 回归：未装模型时必须与原 RK4 逐值一致 ----------
  // 期望值取自 docs/history/segfault_fix_handover.md §2.8 的实测（那次改动已证明 RK4 数值未变），
  // 这里用来保证"新增多项式分支"没有顺手改掉默认路径。
  std::printf("\n== 6) 回归：未装多项式模型时的 RK4 与改动前一致 ==\n");
  tools::install_ballistics_model(nullptr);
  {
    constexpr double rad = 3.14159265358979323846 / 180.0;
    const auto a = solve_traj(23.0, 5.0, 0.0, 0.0);
    std::printf("  23m/s 5m 平射 : pitch=%.4f rad t=%.4f s（文档 0.0463 / 0.2176）\n",
                a.pitch_deg * rad, a.t);
    expect(std::abs(a.pitch_deg * rad - 0.0463) < 1e-3, "平射 pitch 与文档一致");
    expect(std::abs(a.t - 0.2176) < 1e-3, "平射 fly_time 与文档一致");

    const auto b = solve_traj(23.0, 10.0, 2.0, 0.0);
    std::printf("  23m/s 10m 抬2m: pitch=%.4f rad t=%.4f s（文档 0.2922 / 0.4540）\n",
                b.pitch_deg * rad, b.t);
    expect(std::abs(b.pitch_deg * rad - 0.2922) < 2e-3, "抬射 pitch 与文档一致");
    expect(std::abs(b.t - 0.4540) < 2e-3, "抬射 fly_time 与文档一致");
  }

  std::printf("\n%s（失败项 %d）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return g_fail == 0 ? 0 : 1;
}
