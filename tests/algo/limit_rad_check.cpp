// 校验 tools::limit_rad 的改写与原实现语义等价，且不再出现 ±inf 死循环。
//
// 背景：原实现
//   while (angle >  CV_PI) angle -= 2 * CV_PI;
//   while (angle <= -CV_PI) angle += 2 * CV_PI;
// 在 angle = ±inf 时循环条件恒真，调用线程 100% CPU 空转。
// 新实现先用 isfinite 挡掉非有限值（降级为 0），再用 fmod 做 O(1) 折算，
// 取值区间应保持与原实现一致的 (-PI, PI]。
//
// 编译（本仓库根目录；行尾不要留反斜杠，否则触发 -Wcomment）：
//   g++ -std=c++17 -O2 -I. tests/algo/limit_rad_check.cpp tools/math_tools.cpp
//       -o /tmp/limit_rad_check $(pkg-config --cflags --libs opencv4)
//   /tmp/limit_rad_check

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <opencv2/core.hpp>  // CV_PI

#include "tools/math_tools.hpp"

namespace
{

double old_limit_rad(double angle)
{
  while (angle > CV_PI) angle -= 2 * CV_PI;
  while (angle <= -CV_PI) angle += 2 * CV_PI;
  return angle;
}

int failures = 0;

void expect(bool ok, const std::string & what)
{
  if (!ok) {
    failures++;
    std::printf("  [FAIL] %s\n", what.c_str());
  } else {
    std::printf("  [ ok ] %s\n", what.c_str());
  }
}

void expect_near(double a, double b, double tol, const std::string & what)
{
  bool ok = std::fabs(a - b) <= tol;
  if (!ok) {
    failures++;
    std::printf(
      "  [FAIL] %s (old=%.17g new=%.17g diff=%.3g)\n", what.c_str(), a, b, std::fabs(a - b));
  } else {
    std::printf("  [ ok ] %s (old=%.17g new=%.17g)\n", what.c_str(), a, b);
  }
}

// 用 long double 计算"折算到 (-PI, PI]"的真值，作为判定基准。
// 不拿旧实现当基准：它在 |angle| 很大时要循环减 2π 十几万次，
// 每次减法都带舍入，累积误差可达 1e-6 量级（旧实现本身不准）。
long double ref_limit_rad(long double x)
{
  const long double PI_L = 3.1415926535897932384626433832795028841971L;
  const long double TWO_PI_L = 6.2831853071795864769252867665590057683943L;
  long double r = x - TWO_PI_L * std::floor((x + PI_L) / TWO_PI_L);  // [-PI, PI)
  if (r <= -PI_L) r += TWO_PI_L;  // 语义是 (-PI, PI]，-PI 归到 +PI
  return r;
}

void check_against_reference(double x, const std::string & label)
{
  const long double TWO_PI_L = 6.2831853071795864769252867665590057683943L;
  long double ref = ref_limit_rad(static_cast<long double>(x));
  double got = tools::limit_rad(x);
  long double g = static_cast<long double>(got);
  // ±PI 处真值可能以 2PI 环绕的等价形式出现，两个方向都要接受
  double d0 = static_cast<double>(std::fabs(g - ref));
  double d1 = static_cast<double>(std::fabs(g - (ref + TWO_PI_L)));
  double d2 = static_cast<double>(std::fabs(g - (ref - TWO_PI_L)));
  double diff = d0 < d1 ? d0 : d1;
  diff = diff < d2 ? diff : d2;
  // 容差按量级缩放：|x|=1e6 时 double 的 ulp 已达 ~1.16e-10，
  // 任何 double 实现都不可能比这更准，故按 ~10 ulp 放宽
  double tol = 1e-12 * (1.0 + std::fabs(x) * 1e-3);
  char buf[200];
  std::snprintf(buf, sizeof(buf), "%s: got=%.17g ref=%.17Lf diff=%.3g tol=%.3g", label.c_str(),
    got, ref, diff, tol);
  expect(diff <= tol, buf);
}

}  // namespace

int main()
{
  const double PI = CV_PI;
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  std::printf("== 1. 与 long double 参考值的正确性（有限值）==\n");
  std::vector<double> cases = {
    0.0,
    0.5,
    -0.5,
    PI,
    -PI,
    PI - 1e-12,
    -PI + 1e-12,
    2 * PI,
    -2 * PI,
    3 * PI,
    -3 * PI,
    100 * PI,
    -100 * PI,
    1e6,
    -1e6,
    7.0,
    -7.0,
    0.1,
    -0.1,
  };
  for (double c : cases) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "limit_rad(%.10g)", c);
    check_against_reference(c, buf);
  }

  std::printf("\n== 1b. 与旧实现的等价性（旧实现在无累积误差的区间内应一致）==\n");
  bool same_small = true;
  for (int i = -100000; i <= 100000; ++i) {
    double v = i * 1e-3;  // -100 .. 100 rad，覆盖正常业务量程
    if (std::fabs(old_limit_rad(v) - tools::limit_rad(v)) > 1e-12) {
      same_small = false;
      std::printf("  differ at %.4f: old=%.17g new=%.17g\n", v, old_limit_rad(v),
        tools::limit_rad(v));
      break;
    }
  }
  expect(same_small, "[-100,100] rad 内新旧实现完全一致");

  std::printf("\n== 2. 输出区间必须落在 (-PI, PI] ==\n");
  bool in_range = true;
  for (int i = -2000; i <= 2000; ++i) {
    double v = i * 0.01 - 10.0;  // -30 .. 10 扫描
    double r = tools::limit_rad(v);
    if (!(r > -PI - 1e-12 && r <= PI + 1e-12)) {
      in_range = false;
      std::printf("  out of range: limit_rad(%.3f) = %.17g\n", v, r);
      break;
    }
  }
  expect(in_range, "所有采样点均落在 (-PI, PI]");

  std::printf("\n== 3. 非有限值不再死循环（这是本次修复的核心）==\n");
  // 若此处死循环，测试进程会挂住，本身就是失败信号
  double r_inf = tools::limit_rad(inf);
  double r_ninf = tools::limit_rad(-inf);
  double r_nan = tools::limit_rad(nan);
  expect(std::isfinite(r_inf) && r_inf == 0.0, "limit_rad(+inf) 有限且为 0");
  expect(std::isfinite(r_ninf) && r_ninf == 0.0, "limit_rad(-inf) 有限且为 0");
  expect(std::isfinite(r_nan) && r_nan == 0.0, "limit_rad(NaN) 有限且为 0");

  std::printf("\n== 4. 关键边界点取值 ==\n");
  expect_near(tools::limit_rad(PI), PI, 0.0, "limit_rad(+PI) 应为 +PI（保持 -PI 开区间）");
  expect_near(tools::limit_rad(-PI), PI, 1e-12, "limit_rad(-PI) 应为 +PI");
  expect_near(tools::limit_rad(2 * PI), 0.0, 1e-12, "limit_rad(2PI) 应为 0");
  expect_near(tools::limit_rad(3 * PI), PI, 1e-12, "limit_rad(3PI) 应为 +PI");
  expect_near(tools::limit_rad(0.0), 0.0, 0.0, "limit_rad(0) 应为 0");

  std::printf("\n== 5. 大角度精度：新实现应比旧实现更接近真值 ==\n");
  for (double big : {12345.6789, 1e6, -98765.4321}) {
    long double ref = ref_limit_rad(static_cast<long double>(big));
    double o = old_limit_rad(big), n = tools::limit_rad(big);
    double eo = static_cast<double>(std::fabs(static_cast<long double>(o) - ref));
    double en = static_cast<double>(std::fabs(static_cast<long double>(n) - ref));
    std::printf("  x=%-12g old_err=%.3g  new_err=%.3g\n", big, eo, en);
    expect(en <= eo + 1e-15, "新实现误差不大于旧实现");
    expect(n > -PI - 1e-9 && n <= PI + 1e-9, "新实现结果仍在 (-PI,PI] 内");
  }

  std::printf("\n%s (failures=%d)\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
    failures);
  return failures == 0 ? 0 : 1;
}
