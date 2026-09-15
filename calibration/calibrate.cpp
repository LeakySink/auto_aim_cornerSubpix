#include <fmt/core.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "calibration/calibrator.hpp"
#include "io/hikrobot/hikrobot.hpp"
#include "io/mindvision/mindvision.hpp"
#include "tools/exiter.hpp"
#include "tools/remote_logger.hpp"
// CALIB_TEST_FEED ↓ 调试结束后删除下一行
#include "calibration/test_feed.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? | | 输出命令行参数说明}"
  // CALIB_TEST_FEED ↓ 调试结束后删除本行
  "{test           | | 无相机假数据源（调试后门，结束后删除）}";

namespace
{
constexpr auto kAutoAddGap = std::chrono::milliseconds(400);

std::unique_ptr<io::CameraBase> open_camera()
{
  // 默认海康；可在 calibration/result.yaml 覆盖 camera_name / exposure_ms / gain / gamma / vid_pid
  std::string camera_name = "hikrobot";
  double exposure_ms = 5.0;
  double gain = 16.0;
  double gamma = 1.0;
  std::string vid_pid = "2bdf:0001";
  try {
    const auto y = YAML::LoadFile(calibration::Calibrator::kResultPath);
    if (y["camera_name"]) camera_name = y["camera_name"].as<std::string>();
    if (y["exposure_ms"]) exposure_ms = y["exposure_ms"].as<double>();
    if (y["gain"]) gain = y["gain"].as<double>();
    if (y["gamma"]) gamma = y["gamma"].as<double>();
    if (y["vid_pid"]) vid_pid = y["vid_pid"].as<std::string>();
  } catch (const std::exception &) {
  }

  if (camera_name == "mindvision")
    return std::make_unique<io::MindVision>(exposure_ms, gamma, vid_pid);
  if (camera_name == "hikrobot")
    return std::make_unique<io::HikRobot>(exposure_ms, gain, vid_pid);
  throw std::runtime_error("unknown camera_name: " + camera_name);
}

void init_remote_logger(bool test_feed)
{
  tools::RemoteLogger::Config cfg;
  cfg.sender_name = test_feed ? "calibrate-test" : "calibrate";
  cfg.heartbeat_interval_ms = 500;
  cfg.enable_remote = true;
  cfg.enable_local = true;
  cfg.img_width = 640;
  cfg.img_quality = 50;
  tools::RemoteLogger::instance().init(cfg);
}

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

  // CALIB_TEST_FEED ↓ 调试结束后删除本块，并改回 init_remote_logger() / open_camera()
  const bool test_feed = cli.has("test");
  init_remote_logger(test_feed);
  tools::Exiter exiter;
  calibration::Calibrator calib;
  std::unique_ptr<io::CameraBase> camera =
    test_feed ? calib_test_feed::make(calib.pattern_size()) : open_camera();
  // CALIB_TEST_FEED ↑

  std::string hint = test_feed ? "TEST FEED on — no real camera"
                               : "open host calibrate page, wave the board";
  bool undistort = false;
  bool want_add = false;
  bool quit_cmd = false;
  auto last_add = std::chrono::steady_clock::now() - kAutoAddGap;
  auto flash_until = std::chrono::steady_clock::now();

  if (test_feed) {
    // CALIB_TEST_FEED
    tools::RemoteLogger::instance().log(
      "WARN", "CALIB_TEST_FEED active (--test); sender=calibrate-test; remove after debug");
  }
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

    camera->read(img, stamp);
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
