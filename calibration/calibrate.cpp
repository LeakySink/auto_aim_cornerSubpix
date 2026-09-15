#include <fmt/core.h>

#include <chrono>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>

#include "calibration/calibrator.hpp"
#include "calibration/viz.hpp"
#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
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
constexpr auto kAutoAddGap = std::chrono::milliseconds(400);

struct MouseState
{
  int x = -1;
  int y = -1;
  bool click = false;
};

void on_mouse(int event, int x, int y, int, void * userdata)
{
  auto * m = static_cast<MouseState *>(userdata);
  m->x = x;
  m->y = y;
  if (event == cv::EVENT_LBUTTONDOWN) m->click = true;
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

  calibration::VizFlags flags;
  flags.camera_only = camera_only;
  flags.hint = "wave chessboard through the view";

  bool want_add = false;
  auto last_add = std::chrono::steady_clock::now() - kAutoAddGap;
  auto flash_until = std::chrono::steady_clock::now();
  MouseState mouse;
  calibration::UiLayout layout;

  cv::namedWindow("calibrate", cv::WINDOW_NORMAL);
  cv::setMouseCallback("calibrate", on_mouse, &mouse);

  tools::RemoteLogger::instance().log(
    "INFO", "calibrate started, board {}x{}, square {} mm, save -> {}", cols, rows, square_mm,
    output_path);

  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  auto do_calibrate = [&]() {
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      flags.hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }
    flags.hint = "calibrating...";
    const bool ok_cam = calib.calibrate_camera();
    bool ok_hand = false;
    if (ok_cam && !camera_only) ok_hand = calib.calibrate_handeye();
    if (!ok_cam)
      flags.hint = "camera calib failed";
    else if (camera_only)
      flags.hint = fmt::format("camera ok, reproj {:.4f}px  (SAVE)", calib.camera().reproj_error);
    else if (!ok_hand)
      flags.hint = fmt::format(
        "camera ok ({:.4f}px), handeye needs more IMU poses", calib.camera().reproj_error);
    else
      flags.hint = fmt::format("done, reproj {:.4f}px  (SAVE)", calib.camera().reproj_error);
    tools::RemoteLogger::instance().log("INFO", "{}", flags.hint);
    fmt::print("\n{}\n", calib.yaml_snippet());
  };

  auto do_save = [&]() {
    if (!calib.has_camera()) {
      flags.hint = "calibrate first";
      return;
    }
    if (calib.save_yaml(output_path)) {
      flags.hint = fmt::format("saved {}", output_path);
      tools::RemoteLogger::instance().log("INFO", "wrote {}", output_path);
    } else {
      flags.hint = "save failed";
      tools::RemoteLogger::instance().log("ERROR", "failed to write {}", output_path);
    }
  };

  auto do_undistort = [&]() {
    if (!calib.has_camera())
      flags.hint = "calibrate first";
    else {
      flags.undistort = !flags.undistort;
      flags.hint = flags.undistort ? "undistort ON" : "undistort OFF";
    }
  };

  while (!exiter.exit()) {
    camera.read(img, stamp);
    if (img.empty()) break;

    const Eigen::Quaterniond * qptr = nullptr;
    Eigen::Quaterniond q;
    Eigen::Vector3d ypr;
    flags.ypr_deg = nullptr;
    if (gimbal) {
      q = gimbal->q(stamp);
      qptr = &q;
      ypr = tools::eulers(q, 2, 1, 0) * 57.3;
      flags.ypr_deg = &ypr;
    }

    std::vector<cv::Point2f> corners;
    calibration::SampleParams params;
    flags.board_found = calib.detect(img, corners, params);

    const auto now = std::chrono::steady_clock::now();
    const bool cooled = now - last_add >= kAutoAddGap;
    if (flags.board_found && cooled && (want_add || calib.is_good_sample(params))) {
      if (calib.add_sample(corners, params, img.size(), qptr)) {
        last_add = now;
        flash_until = now + std::chrono::milliseconds(180);
        flags.hint = fmt::format("added #{}", calib.size());
        tools::RemoteLogger::instance().log("INFO", "sample {} added", calib.size());
      } else if (want_add) {
        flags.hint = "sample too similar, move the board";
      }
    }
    want_add = false;
    flags.flash = now < flash_until;

    const auto prog = calib.progress();
    cv::Mat canvas = calibration::render_viz(
      img, corners, calib, prog, flags, {mouse.x, mouse.y}, layout);

    if (mouse.click) {
      mouse.click = false;
      switch (calibration::hit_test(layout, mouse.x, mouse.y)) {
        case calibration::UiAction::Add:
          want_add = true;
          break;
        case calibration::UiAction::Calibrate:
          do_calibrate();
          break;
        case calibration::UiAction::Save:
          do_save();
          break;
        case calibration::UiAction::Undistort:
          do_undistort();
          break;
        case calibration::UiAction::Drop:
          calib.drop_last();
          flags.hint = "dropped last sample";
          break;
        case calibration::UiAction::Reset:
          calib.reset();
          flags.undistort = false;
          flags.hint = "reset";
          break;
        default:
          break;
      }
    }

    cv::imshow("calibrate", canvas);
    tools::RemoteLogger::instance().plot_image(canvas, {{"name", "calibrate"}});

    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q' || key == 27) break;
    if (key == ' ' || key == 'a') want_add = true;
    if (key == 'd') {
      calib.drop_last();
      flags.hint = "dropped last sample";
    }
    if (key == 'r') {
      calib.reset();
      flags.undistort = false;
      flags.hint = "reset";
    }
    if (key == 'u') do_undistort();
    if (key == 'c') do_calibrate();
    if (key == 's' || key == 'w') do_save();
  }

  cv::destroyAllWindows();
  tools::RemoteLogger::instance().shutdown();
  return 0;
}
