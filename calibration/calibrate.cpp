#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>

#include "calibration/calibrator.hpp"
#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

const std::string keys =
  "{help h usage ? |                          | 输出命令行参数说明}"
  "{@config-path   | configs/calibration.yaml | yaml配置（棋盘格/相机/云台）}"
  "{output-path o  |                          | 结果写入路径，默认与配置相同}"
  "{camera-only    |                          | 只标内参，不打开云台}";

namespace
{
constexpr int kPanelW = 340;
constexpr auto kAutoAddGap = std::chrono::milliseconds(400);

cv::Scalar bar_color(double v)
{
  return v >= 1.0 ? cv::Scalar(40, 180, 70) : cv::Scalar(40, 160, 220);
}

void draw_bar(cv::Mat & panel, int y, const std::string & name, double v)
{
  tools::draw_text(panel, name, {16, y}, {230, 230, 230}, 0.55, 1);
  const cv::Rect box(90, y - 16, 220, 18);
  cv::rectangle(panel, box, {90, 90, 90}, 1);
  cv::Rect fill = box;
  fill.width = std::max(1, static_cast<int>(box.width * std::clamp(v, 0.0, 1.0)));
  cv::rectangle(panel, fill, bar_color(v), cv::FILLED);
  cv::rectangle(panel, box, {180, 180, 180}, 1);
  tools::draw_text(
    panel, fmt::format("{:3.0f}%", 100.0 * std::clamp(v, 0.0, 1.0)), {92, y - 2}, {15, 15, 15},
    0.45, 1);
}

cv::Mat render(
  const cv::Mat & img, const std::vector<cv::Point2f> & corners, bool found, cv::Size pattern,
  const calibration::Progress & prog, const calibration::Calibrator & calib, bool undistort,
  bool camera_only, bool flash, const std::string & hint, const Eigen::Vector3d * ypr_deg)
{
  cv::Mat view = img.clone();
  if (found) cv::drawChessboardCorners(view, pattern, corners, true);

  if (undistort && calib.has_camera()) {
    cv::Mat undist;
    cv::undistort(view, undist, calib.camera().camera_matrix, calib.camera().distort_coeffs);
    view = undist;
  }

  if (ypr_deg) {
    tools::draw_text(view, fmt::format("yaw   {:.2f}", (*ypr_deg)[0]), {20, 36}, {0, 0, 255}, 0.7, 2);
    tools::draw_text(view, fmt::format("pitch {:.2f}", (*ypr_deg)[1]), {20, 68}, {0, 0, 255}, 0.7, 2);
    tools::draw_text(view, fmt::format("roll  {:.2f}", (*ypr_deg)[2]), {20, 100}, {0, 0, 255}, 0.7, 2);
  }

  const double scale = view.cols > 960 ? 960.0 / view.cols : 1.0;
  if (scale < 1.0) cv::resize(view, view, {}, scale, scale);

  cv::Mat canvas(view.rows, view.cols + kPanelW, view.type(), cv::Scalar(36, 36, 36));
  view.copyTo(canvas(cv::Rect(0, 0, view.cols, view.rows)));
  if (flash) cv::rectangle(canvas, cv::Rect(0, 0, view.cols, view.rows), {0, 220, 0}, 6);

  cv::Mat panel = canvas(cv::Rect(view.cols, 0, kPanelW, canvas.rows));
  tools::draw_text(panel, "Camera Calibrator", {16, 36}, {80, 200, 255}, 0.7, 2);

  const char * status = found ? "BOARD OK" : "NO BOARD";
  tools::draw_text(panel, status, {16, 68}, found ? cv::Scalar(40, 220, 80) : cv::Scalar(80, 80, 220), 0.6, 2);

  tools::draw_text(
    panel, fmt::format("samples  {}  (q:{})", prog.n, prog.n_with_q), {16, 100}, {230, 230, 230},
    0.55, 1);

  draw_bar(panel, 140, "X", prog.x);
  draw_bar(panel, 172, "Y", prog.y);
  draw_bar(panel, 204, "Size", prog.size);
  draw_bar(panel, 236, "Skew", prog.skew);

  const bool can_calib = prog.n >= calibration::Calibrator::kMinSamples;
  tools::draw_text(
    panel, prog.goodenough ? "coverage READY" : (can_calib ? "coverage low, C still ok" : "need more poses"),
    {16, 272},
    prog.goodenough ? cv::Scalar(40, 220, 80)
                    : (can_calib ? cv::Scalar(40, 180, 220) : cv::Scalar(160, 160, 160)),
    0.45, 1);

  int y = 310;
  if (calib.has_camera()) {
    tools::draw_text(
      panel, fmt::format("reproj  {:.4f} px", calib.camera().reproj_error), {16, y}, {40, 220, 80},
      0.55, 1);
    y += 28;
  }
  if (calib.has_handeye()) {
    const auto & r = calib.handeye();
    tools::draw_text(
      panel,
      fmt::format("cam yaw/pitch/roll {:.1f}/{:.1f}/{:.1f}", r.ypr_deg[0], r.ypr_deg[1], r.ypr_deg[2]),
      {16, y}, {40, 220, 80}, 0.45, 1);
    y += 24;
  } else if (camera_only && calib.has_camera()) {
    tools::draw_text(panel, "handeye skipped (--camera-only)", {16, y}, {160, 160, 160}, 0.45, 1);
    y += 24;
  }

  if (undistort) {
    tools::draw_text(panel, "undistort ON", {16, y}, {80, 200, 255}, 0.5, 1);
    y += 24;
  }

  tools::draw_text(panel, hint, {16, y + 8}, {200, 220, 255}, 0.45, 1);

  const int help_y = std::max(canvas.rows - 130, y + 40);
  tools::draw_text(panel, "SPACE add    C calibrate", {16, help_y}, {190, 190, 190}, 0.45, 1);
  tools::draw_text(panel, "S save       U undistort", {16, help_y + 22}, {190, 190, 190}, 0.45, 1);
  tools::draw_text(panel, "D drop last  R reset", {16, help_y + 44}, {190, 190, 190}, 0.45, 1);
  tools::draw_text(panel, "Q quit   (wave the board)", {16, help_y + 66}, {190, 190, 190}, 0.45, 1);
  return canvas;
}

}  // namespace

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }

  const auto config_path = cli.get<std::string>(0);
  auto output_path = cli.get<std::string>("output-path");
  if (output_path.empty()) output_path = config_path;
  const bool camera_only = cli.has("camera-only");

  auto yaml = tools::load(config_path);
  const int cols = tools::read<int>(yaml, "pattern_cols");
  const int rows = tools::read<int>(yaml, "pattern_rows");
  const double square_mm = tools::read<double>(yaml, "square_size_mm");
  auto R_data = tools::read<std::vector<double>>(yaml, "R_gimbal2imubody");
  Eigen::Matrix<double, 3, 3, Eigen::RowMajor> R_gimbal2imubody(R_data.data());

  if (yaml["remote_logger"]) tools::RemoteLogger::instance().init(config_path);

  tools::Exiter exiter;
  io::Camera camera(config_path);
  std::unique_ptr<io::Gimbal> gimbal;
  if (!camera_only) gimbal = std::make_unique<io::Gimbal>(config_path);

  calibration::Calibrator calib(cols, rows, square_mm, R_gimbal2imubody);

  std::string hint = "wave chessboard through the view";
  bool undistort = false;
  bool flash = false;
  bool want_add = false;
  auto last_add = std::chrono::steady_clock::now() - kAutoAddGap;
  auto flash_until = std::chrono::steady_clock::now();

  cv::namedWindow("calibrate", cv::WINDOW_NORMAL);
  cv::setMouseCallback(
    "calibrate",
    [](int event, int, int, int, void * userdata) {
      if (event == cv::EVENT_LBUTTONDOWN) *static_cast<bool *>(userdata) = true;
    },
    &want_add);

  tools::RemoteLogger::instance().log(
    "INFO", "calibrate started, board {}x{}, square {} mm, save -> {}", cols, rows, square_mm,
    output_path);

  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  while (!exiter.exit()) {
    camera.read(img, stamp);
    if (img.empty()) break;

    const Eigen::Quaterniond * qptr = nullptr;
    Eigen::Quaterniond q;
    Eigen::Vector3d ypr;
    const Eigen::Vector3d * yprptr = nullptr;
    if (gimbal) {
      q = gimbal->q(stamp);
      qptr = &q;
      ypr = tools::eulers(q, 2, 1, 0) * 57.3;
      yprptr = &ypr;
    }

    std::vector<cv::Point2f> corners;
    calibration::SampleParams params;
    const bool found = calib.detect(img, corners, params);

    const auto now = std::chrono::steady_clock::now();
    const bool cooled = now - last_add >= kAutoAddGap;
    if (found && cooled && (want_add || calib.is_good_sample(params))) {
      if (calib.add_sample(corners, params, img.size(), qptr)) {
        last_add = now;
        flash_until = now + std::chrono::milliseconds(180);
        hint = fmt::format("added #{}", calib.size());
        tools::RemoteLogger::instance().log("INFO", "sample {} added", calib.size());
      } else if (want_add) {
        hint = "sample too similar, move the board";
      }
    }
    want_add = false;
    flash = now < flash_until;

    const auto prog = calib.progress();
    cv::Mat canvas = render(
      img, corners, found, cv::Size(cols, rows), prog, calib, undistort, camera_only, flash, hint,
      yprptr);
    cv::imshow("calibrate", canvas);
    tools::RemoteLogger::instance().plot_image(canvas, {{"name", "calibrate"}});

    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q' || key == 27) break;
    if (key == ' ' || key == 'a') want_add = true;
    if (key == 'd') {
      calib.drop_last();
      hint = "dropped last sample";
    }
    if (key == 'r') {
      calib.reset();
      undistort = false;
      hint = "reset";
    }
    if (key == 'u') {
      if (!calib.has_camera())
        hint = "calibrate first";
      else {
        undistort = !undistort;
        hint = undistort ? "undistort ON" : "undistort OFF";
      }
    }
    if (key == 'c') {
      if (prog.n < calibration::Calibrator::kMinSamples) {
        hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      } else {
        hint = "calibrating...";
        cv::imshow("calibrate", canvas);
        cv::waitKey(1);
        const bool ok_cam = calib.calibrate_camera();
        bool ok_hand = false;
        if (ok_cam && !camera_only) ok_hand = calib.calibrate_handeye();
        if (!ok_cam)
          hint = "camera calib failed";
        else if (camera_only)
          hint = fmt::format("camera ok, reproj {:.4f}px  (S to save)", calib.camera().reproj_error);
        else if (!ok_hand)
          hint = fmt::format(
            "camera ok ({:.4f}px), handeye needs more IMU poses", calib.camera().reproj_error);
        else
          hint = fmt::format("done, reproj {:.4f}px  (S to save)", calib.camera().reproj_error);
        tools::RemoteLogger::instance().log("INFO", "{}", hint);
        fmt::print("\n{}\n", calib.yaml_snippet());
      }
    }
    if (key == 's' || key == 'w') {
      if (!calib.has_camera()) {
        hint = "calibrate first (C)";
      } else if (calib.save_yaml(output_path)) {
        hint = fmt::format("saved {}", output_path);
        tools::RemoteLogger::instance().log("INFO", "wrote {}", output_path);
      } else {
        hint = "save failed";
        tools::RemoteLogger::instance().log("ERROR", "failed to write {}", output_path);
      }
    }
  }

  cv::destroyAllWindows();
  tools::RemoteLogger::instance().shutdown();
  return 0;
}
