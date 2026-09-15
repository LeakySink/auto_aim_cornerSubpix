#include <fmt/core.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

#include "calibration/calibrator.hpp"
#include "io/camera.hpp"
#include "tools/exiter.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                          | 输出命令行参数说明}"
  "{@config-path   | configs/calibration.yaml | 相机 / remote_logger 配置}";

namespace
{
constexpr auto kAutoAddGap = std::chrono::milliseconds(400);

cv::Point2f outer_corner(
  const std::vector<cv::Point2f> & corners, cv::Size pattern, int idx)
{
  const int w = pattern.width;
  if (idx == 0) return corners.front();
  if (idx == 1) return corners[w - 1];
  if (idx == 2) return corners.back();
  return corners[corners.size() - w];
}

void draw_quad(
  cv::Mat & img, const std::vector<cv::Point2f> & corners, cv::Size pattern,
  const cv::Scalar & color)
{
  if (corners.size() < static_cast<size_t>(pattern.width * pattern.height)) return;
  std::vector<cv::Point> q = {
    outer_corner(corners, pattern, 0), outer_corner(corners, pattern, 1),
    outer_corner(corners, pattern, 2), outer_corner(corners, pattern, 3)};
  const std::vector<std::vector<cv::Point>> poly = {q};
  cv::polylines(img, poly, true, color, 2, cv::LINE_AA);
}

cv::Mat annotate(
  const cv::Mat & img, const std::vector<cv::Point2f> & live_corners, bool found,
  const calibration::Calibrator & calib, bool undistort, bool flash)
{
  cv::Mat view = img.clone();
  const auto pattern = calib.pattern_size();
  for (const auto & s : calib.sample_views())
    draw_quad(view, s.corners, pattern, cv::Scalar(40, 160, 220));

  if (found) {
    cv::drawChessboardCorners(view, pattern, live_corners, true);
    if (calib.has_camera() && live_corners.size() == calib.board_points().size()) {
      cv::Mat rvec, tvec;
      if (cv::solvePnP(
            calib.board_points(), live_corners, calib.camera().camera_matrix,
            calib.camera().distort_coeffs, rvec, tvec, false, cv::SOLVEPNP_IPPE)) {
        cv::drawFrameAxes(
          view, calib.camera().camera_matrix, calib.camera().distort_coeffs, rvec, tvec,
          static_cast<float>(calib.square_size_mm() * 3), 2);
      }
    }
  }

  if (undistort && calib.has_camera()) {
    cv::Mat und;
    cv::undistort(view, und, calib.camera().camera_matrix, calib.camera().distort_coeffs);
    view = und;
  }

  if (flash) cv::rectangle(view, cv::Rect(0, 0, view.cols, view.rows), {0, 220, 0}, 8);
  return view;
}

nlohmann::json status_json(
  const calibration::Progress & prog, const calibration::Calibrator & calib, bool board,
  bool undistort, const std::string & hint)
{
  nlohmann::json j;
  j["calib"] = true;
  j["board"] = board ? 1 : 0;
  j["n"] = prog.n;
  j["x"] = prog.x;
  j["y"] = prog.y;
  j["size"] = prog.size;
  j["skew"] = prog.skew;
  j["goodenough"] = prog.goodenough ? 1 : 0;
  j["hint"] = hint;
  j["undistort"] = undistort ? 1 : 0;
  j["has_cam"] = calib.has_camera() ? 1 : 0;
  j["reproj"] = calib.has_camera() ? calib.camera().reproj_error : -1.0;
  if (!calib.calibrated_at().empty()) j["calibrated_at"] = calib.calibrated_at();
  nlohmann::json samples = nlohmann::json::array();
  for (const auto & s : calib.sample_views()) {
    samples.push_back(
      {{"x", s.params.x},
       {"y", s.params.y},
       {"size", s.params.size},
       {"skew", s.params.skew}});
  }
  j["samples"] = samples;
  return j;
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

  auto yaml = tools::load(config_path);
  if (!yaml["remote_logger"]) {
    fmt::print(stderr, "calibration.yaml needs remote_logger for web UI\n");
    return 1;
  }
  tools::RemoteLogger::instance().init(config_path);

  tools::Exiter exiter;
  io::Camera camera(config_path);
  calibration::Calibrator calib;

  std::string hint = "open host calibrate page, wave the board";
  bool undistort = false;
  bool want_add = false;
  bool quit_cmd = false;
  auto last_add = std::chrono::steady_clock::now() - kAutoAddGap;
  auto flash_until = std::chrono::steady_clock::now();

  tools::RemoteLogger::instance().log(
    "INFO", "intrinsics-only calibrate, board {}x{}, save -> {}", calib.pattern_size().width,
    calib.pattern_size().height, calibration::Calibrator::kResultPath);
  tools::RemoteLogger::instance().log(
    "INFO", "run: ./host/calibrate.sh   then press buttons in the browser");

  auto do_calibrate = [&]() {
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }
    hint = "calibrating...";
    if (!calib.calibrate_camera())
      hint = "camera calib failed";
    else
      hint = fmt::format("intrinsics ok, reproj {:.4f}px  (SAVE)", calib.camera().reproj_error);
    tools::RemoteLogger::instance().log("INFO", "{}", hint);
    fmt::print("\n{}\n", calib.yaml_snippet());
  };

  auto do_save = [&]() {
    if (!calib.has_camera()) {
      hint = "calibrate first";
      return;
    }
    if (calib.save_yaml()) {
      hint = fmt::format("saved {} ({})", calibration::Calibrator::kResultPath, calib.calibrated_at());
      tools::RemoteLogger::instance().log(
        "INFO", "wrote {} at {}", calibration::Calibrator::kResultPath, calib.calibrated_at());
    } else {
      hint = "save failed";
      tools::RemoteLogger::instance().log(
        "ERROR", "failed to write {}", calibration::Calibrator::kResultPath);
    }
  };

  auto apply_cmd = [&](const std::string & cmd) {
    if (cmd == "add")
      want_add = true;
    else if (cmd == "calibrate")
      do_calibrate();
    else if (cmd == "save")
      do_save();
    else if (cmd == "undistort") {
      if (!calib.has_camera())
        hint = "calibrate first";
      else {
        undistort = !undistort;
        hint = undistort ? "undistort ON" : "undistort OFF";
      }
    } else if (cmd == "drop") {
      calib.drop_last();
      hint = "dropped last sample";
    } else if (cmd == "reset") {
      calib.reset();
      undistort = false;
      hint = "reset";
    } else if (cmd == "quit") {
      quit_cmd = true;
    }
  };

  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  while (!exiter.exit() && !quit_cmd) {
    nlohmann::json remote;
    while (tools::RemoteLogger::instance().poll_json(remote)) {
      if (remote.contains("cmd") && remote["cmd"].is_string())
        apply_cmd(remote["cmd"].get<std::string>());
    }
    std::string legacy;
    while (tools::RemoteLogger::instance().poll_calib_cmd(legacy)) apply_cmd(legacy);

    camera.read(img, stamp);
    if (img.empty()) break;

    std::vector<cv::Point2f> corners;
    calibration::SampleParams params;
    const bool found = calib.detect(img, corners, params);

    const auto now = std::chrono::steady_clock::now();
    const bool cooled = now - last_add >= kAutoAddGap;
    if (found && cooled && (want_add || calib.is_good_sample(params))) {
      if (calib.add_sample(corners, params, img.size(), nullptr)) {
        last_add = now;
        flash_until = now + std::chrono::milliseconds(180);
        hint = fmt::format("added #{}", calib.size());
        tools::RemoteLogger::instance().log("INFO", "sample {} added", calib.size());
      } else if (want_add) {
        hint = "sample too similar, move the board";
      }
    }
    want_add = false;

    const auto prog = calib.progress();
    const bool flash = now < flash_until;
    cv::Mat view = annotate(img, corners, found, calib, undistort, flash);

    tools::RemoteLogger::instance().plot(status_json(prog, calib, found, undistort, hint));
    tools::RemoteLogger::instance().plot_image(view, {{"name", "calibrate"}});

    std::this_thread::sleep_for(1ms);
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
