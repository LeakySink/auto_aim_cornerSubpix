#include "viz.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <vector>

#include "tools/img_tools.hpp"

namespace calibration
{
namespace
{
constexpr int kPanelW = 320;
constexpr int kMinH = 640;

cv::Point2f outer(const std::vector<cv::Point2f> & corners, cv::Size pattern, int idx)
{
  const int w = pattern.width;
  if (idx == 0) return corners.front();
  if (idx == 1) return corners[w - 1];
  if (idx == 2) return corners.back();
  return corners[corners.size() - w];
}

void draw_quad(
  cv::Mat & img, const std::vector<cv::Point2f> & corners, cv::Size pattern, const cv::Scalar & color,
  int thickness)
{
  if (corners.size() < static_cast<size_t>(pattern.width * pattern.height)) return;
  std::vector<cv::Point> q = {
    outer(corners, pattern, 0), outer(corners, pattern, 1), outer(corners, pattern, 2),
    outer(corners, pattern, 3)};
  const std::vector<std::vector<cv::Point>> poly = {q};
  cv::polylines(img, poly, true, color, thickness, cv::LINE_AA);
}

void draw_bar(cv::Mat & panel, int x, int y, int w, const std::string & name, double v)
{
  tools::draw_text(panel, name, {x, y}, {220, 220, 220}, 0.5, 1);
  const cv::Rect box(x + 52, y - 14, w, 16);
  cv::Rect fill = box;
  fill.width = std::max(1, static_cast<int>(box.width * std::clamp(v, 0.0, 1.0)));
  const cv::Scalar c = v >= 1.0 ? cv::Scalar(50, 190, 80) : cv::Scalar(50, 165, 220);
  cv::rectangle(panel, fill, c, cv::FILLED);
  cv::rectangle(panel, box, {160, 160, 160}, 1);
  tools::draw_text(
    panel, fmt::format("{:3.0f}%", 100.0 * std::clamp(v, 0.0, 1.0)), {x + 56, y - 2}, {20, 20, 20},
    0.4, 1);
}

void draw_coverage_map(cv::Mat & panel, const cv::Rect & box, const std::vector<Calibrator::SampleView> & samples)
{
  cv::rectangle(panel, box, {28, 28, 32}, cv::FILLED);
  cv::rectangle(panel, box, {90, 90, 100}, 1);
  tools::draw_text(panel, "coverage XY", {box.x + 6, box.y + 16}, {160, 160, 170}, 0.4, 1);
  for (const auto & s : samples) {
    const int px = box.x + static_cast<int>(std::clamp(s.params.x, 0.0, 1.0) * (box.width - 8)) + 4;
    const int py = box.y + static_cast<int>(std::clamp(s.params.y, 0.0, 1.0) * (box.height - 8)) + 4;
    const int r = std::max(3, static_cast<int>(s.params.size * 28));
    const cv::Scalar color = s.has_q ? cv::Scalar(80, 210, 120) : cv::Scalar(80, 180, 230);
    cv::circle(panel, {px, py}, r, color, 1, cv::LINE_AA);
    cv::circle(panel, {px, py}, 2, color, cv::FILLED, cv::LINE_AA);
  }
}

void put_centered(
  cv::Mat & img, const cv::Rect & r, const std::string & text, const cv::Scalar & color, double scale)
{
  int baseline = 0;
  const cv::Size sz = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline);
  const cv::Point org(r.x + (r.width - sz.width) / 2, r.y + (r.height + sz.height) / 2 - 2);
  cv::putText(img, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

cv::Rect draw_button(
  cv::Mat & panel, const cv::Rect & r, const std::string & label, bool enabled, bool hover)
{
  cv::Scalar bg(48, 48, 56);
  cv::Scalar fg(170, 170, 180);
  cv::Scalar bd(80, 80, 90);
  if (enabled) {
    bg = hover ? cv::Scalar(70, 150, 80) : cv::Scalar(52, 92, 64);
    fg = {240, 240, 240};
    bd = hover ? cv::Scalar(140, 230, 150) : cv::Scalar(90, 170, 110);
  }
  cv::rectangle(panel, r, bg, cv::FILLED);
  cv::rectangle(panel, r, bd, 1);
  put_centered(panel, r, label, fg, 0.48);
  return r;
}

void draw_axes_if_possible(
  cv::Mat & view, const std::vector<cv::Point2f> & corners, const Calibrator & calib)
{
  if (!calib.has_camera() || corners.size() != calib.board_points().size()) return;
  cv::Mat rvec, tvec;
  if (!cv::solvePnP(
        calib.board_points(), corners, calib.camera().camera_matrix, calib.camera().distort_coeffs,
        rvec, tvec, false, cv::SOLVEPNP_IPPE))
    return;
  cv::drawFrameAxes(
    view, calib.camera().camera_matrix, calib.camera().distort_coeffs, rvec, tvec,
    static_cast<float>(calib.square_size_mm() * 3), 2);
}

}  // namespace

cv::Mat render_viz(
  const cv::Mat & img, const std::vector<cv::Point2f> & live_corners, const Calibrator & calib,
  const Progress & prog, const VizFlags & flags, const cv::Point & mouse, UiLayout & layout)
{
  cv::Mat view = img.clone();
  const cv::Size pattern = calib.pattern_size();
  const auto samples = calib.sample_views();

  for (const auto & s : samples)
    draw_quad(view, s.corners, pattern, s.has_q ? cv::Scalar(80, 180, 80) : cv::Scalar(180, 140, 40), 1);

  if (flags.board_found) {
    cv::drawChessboardCorners(view, pattern, live_corners, true);
    draw_axes_if_possible(view, live_corners, calib);
  }

  cv::Mat shown = view;
  if (flags.undistort && calib.has_camera()) {
    cv::undistort(view, shown, calib.camera().camera_matrix, calib.camera().distort_coeffs);
  }

  if (flags.ypr_deg) {
    tools::draw_text(shown, fmt::format("yaw   {:.2f}", (*flags.ypr_deg)[0]), {16, 32}, {0, 0, 255}, 0.65, 2);
    tools::draw_text(shown, fmt::format("pitch {:.2f}", (*flags.ypr_deg)[1]), {16, 60}, {0, 0, 255}, 0.65, 2);
    tools::draw_text(shown, fmt::format("roll  {:.2f}", (*flags.ypr_deg)[2]), {16, 88}, {0, 0, 255}, 0.65, 2);
  }

  const double scale = shown.cols > 960 ? 960.0 / shown.cols : 1.0;
  if (scale < 1.0) cv::resize(shown, shown, {}, scale, scale);

  const int h = std::max(shown.rows, kMinH);
  cv::Mat canvas(h, shown.cols + kPanelW, CV_8UC3, cv::Scalar(30, 30, 34));
  const int y0 = (h - shown.rows) / 2;
  shown.copyTo(canvas(cv::Rect(0, y0, shown.cols, shown.rows)));
  layout.image = cv::Rect(0, y0, shown.cols, shown.rows);
  if (flags.flash) cv::rectangle(canvas, layout.image, {0, 220, 0}, 5);

  cv::Mat panel = canvas(cv::Rect(shown.cols, 0, kPanelW, h));
  tools::draw_text(panel, "Calibrator", {16, 34}, {90, 200, 255}, 0.72, 2);

  const bool found = flags.board_found;
  tools::draw_text(
    panel, found ? "BOARD  OK" : "BOARD  --", {16, 64},
    found ? cv::Scalar(60, 220, 90) : cv::Scalar(90, 90, 210), 0.55, 2);
  tools::draw_text(
    panel, fmt::format("samples  {}    imu  {}", prog.n, prog.n_with_q), {16, 92}, {220, 220, 220},
    0.5, 1);

  draw_bar(panel, 16, 128, 232, "X", prog.x);
  draw_bar(panel, 16, 154, 232, "Y", prog.y);
  draw_bar(panel, 16, 180, 232, "Size", prog.size);
  draw_bar(panel, 16, 206, 232, "Skew", prog.skew);

  const char * cov = prog.goodenough ? "coverage READY"
                    : prog.n >= Calibrator::kMinSamples ? "coverage low, still ok"
                                                        : "need more poses";
  tools::draw_text(
    panel, cov, {16, 232},
    prog.goodenough ? cv::Scalar(60, 220, 90)
                    : (prog.n >= Calibrator::kMinSamples ? cv::Scalar(50, 180, 220)
                                                         : cv::Scalar(150, 150, 160)),
    0.42, 1);

  draw_coverage_map(panel, {16, 248, 288, 110}, samples);

  int y = 378;
  if (calib.has_camera()) {
    tools::draw_text(
      panel, fmt::format("reproj  {:.4f} px", calib.camera().reproj_error), {16, y}, {70, 220, 100},
      0.5, 1);
    y += 22;
    const cv::Rect prev(16, y, 288, 92);
    cv::Mat und;
    cv::undistort(img, und, calib.camera().camera_matrix, calib.camera().distort_coeffs);
    cv::Mat thumb;
    cv::resize(und, thumb, {prev.width, prev.height});
    thumb.copyTo(panel(prev));
    cv::rectangle(panel, prev, {100, 100, 110}, 1);
    tools::draw_text(panel, "undistort", {prev.x + 8, prev.y + 18}, {240, 240, 240}, 0.4, 1);
    y += 104;
  }
  if (calib.has_handeye()) {
    const auto & r = calib.handeye();
    tools::draw_text(
      panel,
      fmt::format("cam  yaw {:.1f}  pitch {:.1f}  roll {:.1f}", r.ypr_deg[0], r.ypr_deg[1], r.ypr_deg[2]),
      {16, y}, {70, 220, 100}, 0.4, 1);
    y += 20;
  } else if (flags.camera_only && calib.has_camera()) {
    tools::draw_text(panel, "handeye skipped", {16, y}, {150, 150, 160}, 0.42, 1);
    y += 20;
  }

  tools::draw_text(panel, flags.hint, {16, y + 6}, {190, 220, 255}, 0.42, 1);

  const int bw = 136, bh = 34, gap = 10;
  const int by = h - 16 - bh * 3 - gap * 2;
  const int bx = 16;
  const cv::Point m = mouse.x >= shown.cols ? cv::Point(mouse.x - shown.cols, mouse.y) : cv::Point(-1, -1);
  auto hover = [&](const cv::Rect & r) { return r.contains(m); };

  const bool can_c = prog.n >= Calibrator::kMinSamples;
  const bool can_s = calib.has_camera();
  const bool can_u = calib.has_camera();
  const bool can_d = prog.n > 0;
  const bool can_r = prog.n > 0 || calib.has_camera();

  layout.calibrate = draw_button(panel, {bx, by, bw, bh}, "CALIBRATE", can_c, hover({bx, by, bw, bh}));
  layout.save = draw_button(panel, {bx + bw + gap, by, bw, bh}, "SAVE", can_s, hover({bx + bw + gap, by, bw, bh}));
  layout.undistort =
    draw_button(panel, {bx, by + bh + gap, bw, bh}, flags.undistort ? "UNDIST ON" : "UNDISTORT", can_u,
                hover({bx, by + bh + gap, bw, bh}));
  layout.drop =
    draw_button(panel, {bx + bw + gap, by + bh + gap, bw, bh}, "DROP LAST", can_d,
                hover({bx + bw + gap, by + bh + gap, bw, bh}));
  layout.reset =
    draw_button(panel, {bx, by + 2 * (bh + gap), bw * 2 + gap, bh}, "RESET", can_r,
                hover({bx, by + 2 * (bh + gap), bw * 2 + gap, bh}));

  // 按钮坐标转到整张 canvas
  const int ox = shown.cols;
  layout.calibrate.x += ox;
  layout.save.x += ox;
  layout.undistort.x += ox;
  layout.drop.x += ox;
  layout.reset.x += ox;
  return canvas;
}

UiAction hit_test(const UiLayout & layout, int x, int y)
{
  const cv::Point p(x, y);
  if (layout.calibrate.contains(p)) return UiAction::Calibrate;
  if (layout.save.contains(p)) return UiAction::Save;
  if (layout.undistort.contains(p)) return UiAction::Undistort;
  if (layout.drop.contains(p)) return UiAction::Drop;
  if (layout.reset.contains(p)) return UiAction::Reset;
  if (layout.image.contains(p)) return UiAction::Add;
  return UiAction::None;
}

}  // namespace calibration
