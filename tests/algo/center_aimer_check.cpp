// 校验「瞄中心 + 等板转过来开火」这条策略（tasks/auto_aim/center_aimer.*）。
//
// 不需要硬件：Target(x, vyaw, radius, h) 会直接造一个"P0 = 0 的完美状态"目标
// （中心 (x,0)、基角 a=0、角速度 vyaw、半径 radius、板高差 h），predict(dt) 是精确外推，
// 可以拿来做解析对照。
//
// 断言：
//   1) 瞄准角指向**旋转中心**，不是某块板
//   2) fire 为真时，弹丸到达时刻该板必须在命中窗口内（自洽性）
//   3) 开火事件数 ≈ 穿越次数（4 板 = 每 90° 一次；只有 2 块板可用时 = 每 180° 一次）
//   4) 竖直方向不匹配的板（z 偏 0.3 m）不会被选中 —— 只瞄中心就意味着打不到它
//   5) 云台不到位时，即便预测命中也不开火
//
// 编译（只需要 Eigen；不需要 OpenCV/硬件）：
//   g++ -std=c++17 -O2 -I. -I<eigen> tests/algo/center_aimer_check.cpp
//       tasks/auto_aim/center_aimer.cpp tasks/auto_aim/target.cpp
//       tools/extended_kalman_filter.cpp tools/math_tools.cpp
//       tools/trajectory.cpp tools/ballistics_poly.cpp tools/ballistics_config.cpp
//       <fake_logger.cpp> -lyaml-cpp -lfmt -lopencv_core
//   ./center_aimer_check

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include <Eigen/Dense>

#include "tasks/auto_aim/center_aimer.hpp"
#include "tasks/auto_aim/target.hpp"

// 本测试链接的是 .deps/harness/fake_logger.cpp 那个 RemoteLogger 桩（真实实现要 socket），
// 桩只回调这个空实现，不改变被测行为。
void harness_on_log(const std::string &) {}

namespace
{
int g_fail = 0;
constexpr double kRad2Deg = 57.29577951308232;
constexpr double kDeg2Rad = 1.0 / 57.29577951308232;

void expect(bool ok, const std::string & what)
{
  if (!ok) {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
}

struct RunStats
{
  int frames = 0;
  int fire_frames = 0;
  int events = 0;           // 连续 fire 段数
  int inconsistent = 0;     // fire 但不在窗口内
  int bad_plate_id = 0;     // 选到了不该选的板
  int back_face_hits = 0;   // 选中了背面/侧棱板（cos_face < 0.15）
  std::set<int> plates_fired;  // 本段里选中过的板号（验证"4 块都能打"）
  double min_fire_cos_face = 1.0;
  double max_err_deg = 0.0;
  double max_dt_err = 0.0;  // |total_time - fly_time - launch_delay|
};

/// 用"理想跟随"跑一段：云台角直接取上一刻算出的指令角；时间戳按 dt 递增
/// 用"理想跟随"跑一段：**云台反馈取上一帧的指令角**（模拟真实云台跟指令）。
///
/// 之前这里传的是 `aim_center()` 算出的"瞄中心"角，而 `pitch_follow_plate=true` 时
/// 实际指令 pitch 是跟随板高的 —— 反馈喂错了对象，等于让云台永远瞄中心，
/// 于是"跟随板高"这条路在测试里走不通。改成跟上一帧指令才是真实语义。
RunStats run(auto_aim::CenterAimer & aimer, auto_aim::Target target, double speed, int steps,
             double dt, bool require_even_plate, double launch_delay = 0.0)
{
  RunStats st;
  auto tp = std::chrono::steady_clock::now();
  bool prev = false;
  // 初始云台角：先用一次 aim_center 对齐，避免第一帧"云台凭空在原点"导致 settled 判失败
  double gs_yaw = 0.0, gs_pitch = 0.0;
  if (auto_aim::CenterAimResult init; aimer.aim_center(target, tp, speed, &init)) {
    gs_yaw = init.yaw;
    gs_pitch = -init.pitch;  // 反馈抬头为正
  }
  for (int i = 0; i < steps; ++i) {
    const auto res = aimer.solve(target, tp, gs_yaw, gs_pitch, speed);
    ++st.frames;
    st.max_dt_err = std::max(st.max_dt_err, std::abs(res.total_time - res.fly_time - launch_delay));
    // 可见性：任何时候都不该选中"背面/侧棱"的板（不论是否开火）
    if (res.valid) {
      if (res.cos_face < 0.15) ++st.back_face_hits;
      st.min_fire_cos_face = std::min(st.min_fire_cos_face, res.cos_face);
      if (res.plate_id >= 0) {
        st.plates_fired.insert(res.plate_id);
      }
    }
    if (res.fire) {
      ++st.fire_frames;
      if (!prev) ++st.events;
      if (res.align_err_deg > res.hit_window_deg / 2.0 + 1e-6) ++st.inconsistent;
      if (res.pitch_err_deg > res.pitch_window_deg / 2.0 + 1e-6) ++st.inconsistent;
      if (require_even_plate && (res.plate_id % 2 != 0)) ++st.bad_plate_id;
      st.max_err_deg = std::max(st.max_err_deg, res.align_err_deg);
    }
    // 云台跟上一帧的指令（理想跟随，零滞后）
    if (res.valid) {
      gs_yaw = res.yaw;
      gs_pitch = -res.pitch;
    }
    prev = res.fire;
    target.predict(dt);
    tp += std::chrono::microseconds(static_cast<int>(dt * 1e6));
  }
  return st;
}

}  // namespace

int main()
{
  using auto_aim::CenterAimResult;
  using auto_aim::CenterAimer;
  using auto_aim::Target;

  const double kDist = 5.0;
  const double kRadius = 0.25;
  const double kSpeed = 16.5;
  const double dt = 0.01;

  CenterAimer aimer;  // 默认配置：板 0.135×0.056、hit_margin 0.3、require_settled true

  // ---------- 1) 瞄准点必须是旋转中心 ----------
  std::printf("== 1) 瞄准点 = 旋转中心 ==\n");
  {
    Target target(kDist, 1.0, kRadius, 0.0);
    std::chrono::steady_clock::time_point tp{};
    CenterAimResult r;
    const bool ok = aimer.aim_center(target, tp, kSpeed, &r);
    expect(ok, "aim_center 应成功");
    if (ok) {
      const double yaw_center = std::atan2(r.aim_xyz.y(), r.aim_xyz.x());
      const double err = std::abs(r.yaw - yaw_center) * kRad2Deg;
      std::printf("  yaw=%.3f° 中心方位=%.3f° 误差=%.4f°  fly=%.3fs\n", r.yaw * kRad2Deg,
                  yaw_center * kRad2Deg, err, r.fly_time);
      expect(err < 1e-3, "yaw 应严格指向旋转中心");
      expect(r.fly_time > 0.1 && r.fly_time < 0.6, "5 m / 16.5 m/s 的飞行时间应 ≈0.3 s");
    }
  }

  // ---------- 2) 4 块板都能打：每 90° 一次 ----------
  std::printf("\n== 2) h=0（四块板等高）：w=1 rad/s, T=3 s ==\n");
  {
    const double w = 1.0;
    const int steps = 300;
    Target target(kDist, w, kRadius, 0.0);
    const auto st = run(aimer, target, kSpeed, steps, dt, false);
    const double expect_events = w * (steps * dt) / (M_PI / 2.0);
    std::printf("  帧=%d fire 帧=%d 事件=%d（期望 ≈%.2f）最大对齐误差=%.3f°\n", st.frames,
                st.fire_frames, st.events, expect_events, st.max_err_deg);
    std::printf("  背面/侧棱被选中=%d 次  最小 cos_face=%.2f\n", st.back_face_hits,
                st.min_fire_cos_face);
    expect(st.frames > 0, "应有有效帧");
    expect(st.events >= 1, "3 秒内至少一次开火窗口");
    expect(std::abs(st.events - expect_events) <= 1.5, "事件数应与 90° 穿越次数一致");
    expect(st.inconsistent == 0, "fire 时必须落在命中窗口内（自洽性）");
    expect(st.fire_frames > 0 && st.fire_frames < st.frames, "fire 应只占一部分帧");
    // 关键：一整圈里背面/侧棱的板压在瞄准线上的次数和正面一样多，
    // 不判 cos_face 就会有一半的"命中"打在背面上。
    expect(st.back_face_hits == 0, "任何时刻都不该选中背面/侧棱板");
    expect(st.min_fire_cos_face >= 0.15, "被选中的板必须朝向我们");
  }

  // ---------- 3) 高度不一致：pitch 跟随板高 -> 4 块板都能打 ----------
  // 规范只限制"任意两块板下边缘高度差 ≤100mm"，不规定排布，所以
  // "一低一高两中"是合规工况。竖直窗口只有 板高×35% ≈ ±19.6mm，
  // 因此若 pitch 固定瞄中心，高度差 >39mm 的板**永远**打不到。
  std::printf("\n== 3) h=0.3（1/3 号板高 0.3 m）：pitch 跟随板高 -> 4 块都能打 ==\n");
  {
    // 默认配置 pitch_follow_plate = true
    // 跑满一整圈：w=1 rad/s、dt=0.01 -> 一圈 2π ≈ 6.28s ≈ 628 帧，取 720 帧留余量。
    // 4 块板压线的相位是 0°/90°/180°/270°，3 秒(300 帧)只转到 172°，会漏掉 270° 那块。
    const int steps = 720;
    Target target(kDist, 1.0, kRadius, 0.3);
    const auto st = run(aimer, target, kSpeed, steps, dt, /*require_even_plate=*/false);
    const double expect_events = 1.0 * (steps * dt) / (M_PI / 2.0);  // 每 90° 一次
    std::printf("  帧=%d fire 帧=%d 事件=%d（期望 ≈%.2f）\n", st.frames, st.fire_frames, st.events,
                expect_events);
    std::printf("  选中过的板号: ");
    for (int id : st.plates_fired) std::printf("%d ", id);
    std::printf("（共 %zu 块）\n", st.plates_fired.size());
    expect(st.inconsistent == 0, "fire 时必须落在命中窗口内");
    expect(st.events >= 1, "应有开火窗口");
    // 跟随板高后，4 块板都应该被选中过（而不是只有与中心同高的 0/2）
    expect(st.plates_fired.size() == 4, "pitch 跟随板高时 4 块板都该有开火机会");
    expect(st.events >= static_cast<int>(expect_events) - 1, "窗口数应接近 h=0 时的水平");
  }

  // ---------- 3b) 关掉跟随：退回"只有与中心同高的板能打" ----------
  std::printf("\n== 3b) h=0.3 且 pitch_follow_plate=false：只能打 0/2 号板 ==\n");
  {
    auto_aim::CenterAimConfig cfg;
    cfg.pitch_follow_plate = false;
    auto_aim::CenterAimer fixed(cfg);

    Target target(kDist, 1.0, kRadius, 0.3);
    const auto st = run(fixed, target, kSpeed, 720, dt, /*require_even_plate=*/true);
    const double expect_events = 1.0 * 7.2 / M_PI;  // 只剩两块 -> 每 180° 一次
    std::printf("  帧=%d fire 帧=%d 事件=%d（期望 ≈%.2f）选错板=%d\n", st.frames, st.fire_frames,
                st.events, expect_events, st.bad_plate_id);
    expect(st.inconsistent == 0, "fire 时必须落在命中窗口内");
    expect(st.bad_plate_id == 0, "不许开火打在高度不匹配的板上");
    expect(st.events >= 1, "与中心同高的两块板仍应能打");
    expect(st.events <= static_cast<int>(expect_events) + 1, "窗口数应约为 h=0 时的一半");
  }

  // ---------- 4) 反例：云台不到位不开火 ----------
  std::printf("\n== 4) 云台偏差 5°（> 默认 0.5° 容差）==\n");
  {
    Target target(kDist, 1.0, kRadius, 0.0);
    std::chrono::steady_clock::time_point tp{};
    int fire_when_offset = 0, checked = 0;
    for (int i = 0; i < 300; ++i) {
      CenterAimResult ref;
      if (aimer.aim_center(target, tp, kSpeed, &ref)) {
        const auto res = aimer.solve(target, tp, ref.yaw + 5.0 * kDeg2Rad, -ref.pitch, kSpeed);
        ++checked;
        if (res.fire) ++fire_when_offset;
      }
      target.predict(dt);
    }
    std::printf("  检查 %d 帧，偏 5° 时 fire 帧=%d\n", checked, fire_when_offset);
    expect(checked > 0, "应有有效帧");
    expect(fire_when_offset == 0, "云台未到位时不应开火");
  }

  // ---------- 5) 静态板：w=0 且板正对 -> 应持续可打 ----------
  std::printf("\n== 5) w=0、板正对瞄准线 ==\n");
  {
    Target target(kDist, 0.0, kRadius, 0.0);
    std::chrono::steady_clock::time_point tp = std::chrono::steady_clock::now();
    CenterAimResult ref;
    expect(aimer.aim_center(target, tp, kSpeed, &ref), "w=0 时应可解算");
    const auto res = aimer.solve(target, tp, ref.yaw, -ref.pitch, kSpeed);  // 反馈抬头为正
    std::printf("  fire=%d plate_id=%d 对齐误差=%.3f°\n", res.fire ? 1 : 0, res.plate_id,
                res.align_err_deg);
    expect(res.valid, "结果应有效");
    expect(res.in_window, "板正对且不转 -> 到达时刻应压在线上");
    expect(res.settled, "按正确符号给反馈 -> 应判为到位");
    expect(res.fire, "板正对且不转 -> 应可开火");
  }

  // ---------- 5b) pitch 符号回归：把指令符号当反馈喂回去 -> 必须判为未到位 ----------
  // 反馈抬头为正、指令抬头为负，误差是 |反馈 + 指令|。若写成相减，
  // 稳态误差恒为 2×pitch（5 m 处约 3°），fire 会永远是 0（2026-11 真实踩过）。
  std::printf("\n== 5b) 反馈符号搞反时不应开火 ==\n");
  {
    Target target(kDist, 0.0, kRadius, 0.0);
    std::chrono::steady_clock::time_point tp = std::chrono::steady_clock::now();
    CenterAimResult ref;
    expect(aimer.aim_center(target, tp, kSpeed, &ref), "w=0 时应可解算");
    const auto res = aimer.solve(target, tp, ref.yaw, ref.pitch, kSpeed);  // 错误符号
    std::printf("  fire=%d in_window=%d settled=%d |指令 pitch|=%.2f°\n", res.fire ? 1 : 0,
                res.in_window ? 1 : 0, res.settled ? 1 : 0, -res.pitch * kRad2Deg);
    expect(res.in_window, "几何上仍在窗口内（错的只是云台反馈）");
    expect(!res.settled, "符号搞反 -> 应判为未到位");
    expect(!res.fire, "未到位 -> 不应开火");
  }

  // ---------- 6) 出膛延迟补偿：预测时刻必须 = launch_delay + fly_time ----------
  std::printf("\n== 6) launch_delay=0.15 s ==\n");
  {
    auto_aim::CenterAimConfig cfg;
    cfg.launch_delay = 0.15;
    auto_aim::CenterAimer delayed(cfg);
    Target target(kDist, 1.0, kRadius, 0.0);
    const auto st = run(delayed, target, kSpeed, 300, dt, false, cfg.launch_delay);
    std::printf("  帧=%d fire 帧=%d 事件=%d  max|total-fly-delay|=%.2e s\n", st.frames,
                st.fire_frames, st.events, st.max_dt_err);
    expect(st.frames > 0, "应有有效帧");
    expect(st.max_dt_err < 1e-6, "total_time 必须等于 launch_delay + fly_time");
    expect(st.inconsistent == 0, "补偿延迟后判据仍须自洽");
    expect(st.events >= 1, "补偿延迟后仍应能在窗口内开火");
  }

  // ---------- 7) 热量闸：按 2027 规则 5.1.3 的 42mm 参数 ----------
  // Q0=300、每发 +100、冷却 100/s、10Hz 结算、Q1>Q0 锁发射机构直到归零。
  // 期望：连续打 3 发（0→100→200→300）后必须停，等冷却够了才能再打。
  std::printf("\n== 7) 热量闸（Q0=300, 每发+100, 冷却100/s）==\n");
  {
    auto_aim::CenterAimConfig cfg;
    cfg.heat.enabled = true;
    cfg.heat.limit = 300.0;
    cfg.heat.shot_cost = 100.0;
    cfg.heat.cool_rate = 100.0;
    cfg.heat.settle_hz = 10.0;
    cfg.heat.reserve = 0.0;
    auto_aim::CenterAimer heated(cfg);

    // 静止靶、云台理想跟随 -> 除了热量以外全满足，fire 完全由热量决定
    Target target(kDist, 0.0, kRadius, 0.0);
    auto tp = std::chrono::steady_clock::now();
    double gs_yaw = 0.0, gs_pitch = 0.0;
    if (auto_aim::CenterAimResult init; heated.aim_center(target, tp, kSpeed, &init)) {
      gs_yaw = init.yaw;
      gs_pitch = -init.pitch;
    }

    int fires = 0;
    int blocked_by_heat = 0;
    double max_q1 = 0.0;
    const int steps = 500;  // 5.0 s
    for (int i = 0; i < steps; ++i) {
      const auto res = heated.solve(target, tp, gs_yaw, gs_pitch, kSpeed);
      max_q1 = std::max(max_q1, res.heat_q1);
      if (res.in_window && res.settled && !res.heat_ok) ++blocked_by_heat;
      if (res.fire) {
        ++fires;
        gs_yaw = res.yaw;
        gs_pitch = -res.pitch;
      }
      target.predict(dt);
      tp += std::chrono::microseconds(static_cast<int>(dt * 1e6));
    }

    // 5 秒内：前 3 发瞬间打满（0/2 帧内），之后每 1 秒（100 冷却）才够 1 发
    // -> 3 + 约 4 = 约 7 发；上限取决于 5s 内能冷却出多少发
    const int max_possible = 3 + static_cast<int>((steps * dt - 0.0) * cfg.heat.cool_rate /
                                                  cfg.heat.shot_cost);
    std::printf("  fire 次数=%d  被热量挡下=%d 帧  Q1 峰值=%.0f\n", fires, blocked_by_heat, max_q1);
    std::printf("  理论上限约 %d 发（3 发打满 + 冷却补足）\n", max_possible);
    expect(fires >= 3, "热量允许时至少要能连打 3 发");
    expect(fires <= max_possible + 1, "不能超过热量允许的发射数");
    expect(max_q1 <= cfg.heat.limit + 1e-6, "Q1 绝不能超过 Q0（超过就要锁机构）");
    expect(blocked_by_heat > 0, "应该有'该打但热量不够'的帧");
  }

  // ---------- 7b) 热量关掉时应恢复原行为（不做限流） ----------
  std::printf("\n== 7b) heat.enabled=false 时不限流 ==\n");
  {
    auto_aim::CenterAimConfig cfg;
    cfg.heat.enabled = false;
    auto_aim::CenterAimer no_heat(cfg);
    Target target(kDist, 0.0, kRadius, 0.0);
    const auto st = run(no_heat, target, kSpeed, 300, dt, false);
    std::printf("  帧=%d fire 帧=%d\n", st.frames, st.fire_frames);
    expect(st.fire_frames > 100, "关掉热量后 fire 应持续为真（不做限流）");
  }

  std::printf("\n%s（失败项 %d）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return g_fail == 0 ? 0 : 1;
}
