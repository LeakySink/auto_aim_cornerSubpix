// 验证 tools::Trajectory 对非有限输入不再"放行"，且正常输入的弹道结果不变。
//
// 背景：Trajectory 构造函数原本只写
//     if (v0 <= 0.0 || d <= 0.0) { unsolvable = true; return; }
// 这两个比较对 NaN **都是 false**，于是 NaN 一路穿过 solve_no_drag
// （那里的 `delta < 0.0` 对 NaN 同样是 false），最终得到 NaN 的
// pitch / fly_time 且 unsolvable 仍为 false。下游
//     aimer.cpp: static_cast<int>(prev_fly_time * 1e6)
// 对 NaN 是 UB（x86 上得到 INT_MIN），时间戳被回拨约 35 分钟 ->
// 下一帧 dt 变成巨大值 -> EKF 的 dt^4 项爆炸 -> 状态推成 inf ->
// tools::limit_rad(inf) 死循环。
//
// 修复后：构造函数用 !(x > 0) 拦 NaN，并额外要求 h 与 k 有限；
// 收尾再统一校验 pitch / fly_time 是否有限。
//
// 编译（本仓库根目录）：
//   g++ -std=c++17 -O2 -I. -Itools tests/algo/trajectory_nan_check.cpp \
//       tools/trajectory.cpp -o /tmp/trajectory_nan_check
//   /tmp/trajectory_nan_check

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "trajectory.hpp"

namespace
{

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

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

// 任何非有限输入都必须判为无解，且输出量不得是 NaN/inf
void expect_unsolvable(double v0, double d, double h, double k, const std::string & label)
{
  tools::Trajectory t(v0, d, h, k);
  char buf[256];
  std::snprintf(
    buf, sizeof(buf), "%s -> unsolvable (got unsolvable=%d, t=%.6g, pitch=%.6g)", label.c_str(),
    static_cast<int>(t.unsolvable), t.fly_time, t.pitch);
  bool ok = t.unsolvable && std::isfinite(t.fly_time) && std::isfinite(t.pitch);
  expect(ok, buf);
}

}  // namespace

int main()
{
  std::printf("== 1. NaN 必须在入口被拦截（本次修复的核心）==\n");
  expect_unsolvable(kNaN, 5.0, 0.0, 0.0, "v0=NaN");
  expect_unsolvable(23.0, kNaN, 0.0, 0.0, "d=NaN");
  expect_unsolvable(23.0, 5.0, kNaN, 0.0, "h=NaN");
  expect_unsolvable(23.0, 5.0, 0.0, kNaN, "k=NaN (drag branch)");

  std::printf("\n== 2. inf 也必须被拦截 ==\n");
  expect_unsolvable(kInf, 5.0, 0.0, 0.0, "v0=+inf");
  expect_unsolvable(-kInf, 5.0, 0.0, 0.0, "v0=-inf");
  expect_unsolvable(23.0, kInf, 0.0, 0.0, "d=+inf");
  expect_unsolvable(23.0, 5.0, kInf, 0.0, "h=+inf");
  expect_unsolvable(23.0, 5.0, 0.0, kInf, "k=+inf");

  std::printf("\n== 3. 原有非法输入仍被拒（回归）==\n");
  expect_unsolvable(0.0, 5.0, 0.0, 0.0, "v0=0");
  expect_unsolvable(-1.0, 5.0, 0.0, 0.0, "v0=-1");
  expect_unsolvable(23.0, 0.0, 0.0, 0.0, "d=0");
  expect_unsolvable(23.0, -3.0, 0.0, 0.0, "d=-3");

  std::printf("\n== 4. 正常输入必须仍可解，且结果物理合理 ==\n");
  {
    // 23 m/s 平射 5 m，h=0。无阻力下应是小角度、短飞行时间
    tools::Trajectory t(23.0, 5.0, 0.0, 0.0);
    std::printf("  v0=23 d=5 h=0 k=0 -> unsolvable=%d pitch=%.4f rad t=%.5f s\n",
      static_cast<int>(t.unsolvable), t.pitch, t.fly_time);
    expect(!t.unsolvable, "5m 平射有解");
    expect(std::isfinite(t.pitch) && std::isfinite(t.fly_time), "结果有限");
    expect(t.pitch > 0.0 && t.pitch < 0.2, "小角度低伸弹道（0 < pitch < 0.2 rad）");
    expect(t.fly_time > 0.0 && t.fly_time < 0.5, "飞行时间合理（0 < t < 0.5 s）");
  }
  {
    // 23 m/s 打 10 m 远、2 m 高，需要抬枪
    tools::Trajectory t(23.0, 10.0, 2.0, 0.0);
    std::printf("  v0=23 d=10 h=2 k=0 -> unsolvable=%d pitch=%.4f rad t=%.5f s\n",
      static_cast<int>(t.unsolvable), t.pitch, t.fly_time);
    expect(!t.unsolvable, "10m/2m 高差有解");
    expect(t.pitch > 0.0, "打高处需要正仰角");
  }
  {
    // 加入阻力后仍应可解。k 取 0.02 /m（特征长度 50m，8m 处约衰减 15%），
    // 接近真实 17mm 弹丸的量级；k=0.5 那种量级在 8m 内会衰减 98%，属无解场景。
    tools::Trajectory t(23.0, 8.0, 0.5, 0.02);
    std::printf("  v0=23 d=8 h=0.5 k=0.02 -> unsolvable=%d pitch=%.4f rad t=%.5f s\n",
      static_cast<int>(t.unsolvable), t.pitch, t.fly_time);
    expect(!t.unsolvable, "带阻力有解");
    expect(std::isfinite(t.pitch) && std::isfinite(t.fly_time), "带阻力结果有限");
  }

  std::printf("\n== 5. 明确无解的场景（超出射程）==\n");
  {
    // 23 m/s 打 60 m，无阻力也够不到
    tools::Trajectory t(23.0, 60.0, 0.0, 0.0);
    std::printf("  v0=23 d=60 k=0 -> unsolvable=%d\n", static_cast<int>(t.unsolvable));
    expect(t.unsolvable, "60m 超出射程判无解");
    expect(std::isfinite(t.fly_time) && std::isfinite(t.pitch), "无解时输出仍有限");
  }

  std::printf("\n%s (failures=%d)\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
    failures);
  return failures == 0 ? 0 : 1;
}
