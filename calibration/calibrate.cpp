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
constexpr auto kAutoAddGap = std::chrono::milliseconds(250);
constexpr auto kUiGap = std::chrono::milliseconds(66);  // ~15fps 推流
constexpr int kPreviewW = 480;

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
    return std::make_unique<io::HikRobot>(exposure_ms, gain, vid_pid, /*auto_gain=*/true);
  throw std::runtime_error("unknown camera_name: " + camera_name);
}

void init_remote_logger(bool test_feed)
{
  tools::RemoteLogger::Config cfg;
  cfg.sender_name = test_feed ? "calibrate-test" : "calibrate";
  cfg.heartbeat_interval_ms = 500;
  cfg.enable_remote = true;
  cfg.enable_local = true;
  cfg.img_width = 0;  // 预览已缩到 480，JPEG 不再二次缩放
  cfg.img_quality = 28;
  tools::RemoteLogger::instance().init(cfg);
}

cv::Mat annotate_preview(
  const cv::Mat & img, const std::vector<cv::Point2f> & live_corners, bool found,
  const calibration::Calibrator & calib, bool undistort, bool flash, int preview_w)
{
  cv::Mat small;
  const double scale =
    (preview_w > 0 && img.cols > preview_w) ? static_cast<double>(preview_w) / img.cols : 1.0;
  if (scale != 1.0)
    cv::resize(img, small, cv::Size(), scale, scale, cv::INTER_AREA);
  else
    small = img.clone();

  const auto pattern = calib.pattern_size();
  // 历史样本只画覆盖点，不画全分辨率外框（网页 map 已有覆盖）
  for (const auto & s : calib.sample_views()) {
    cv::circle(
      small,
      {static_cast<int>(s.params.x * small.cols), static_cast<int>(s.params.y * small.rows)}, 3,
      {40, 160, 220}, -1, cv::LINE_AA);
  }

  if (found && !live_corners.empty()) {
    std::vector<cv::Point2f> scaled = live_corners;
    if (scale != 1.0) {
      for (auto & c : scaled) {
        c.x = static_cast<float>(c.x * scale);
        c.y = static_cast<float>(c.y * scale);
      }
    }
    cv::drawChessboardCorners(small, pattern, scaled, true);
  }

  if (undistort && calib.has_camera()) {
    cv::Mat K = calib.camera().camera_matrix.clone();
    K.at<double>(0, 0) *= scale;
    K.at<double>(1, 1) *= scale;
    K.at<double>(0, 2) *= scale;
    K.at<double>(1, 2) *= scale;
    cv::Mat und;
    cv::undistort(small, und, K, calib.camera().distort_coeffs);
    small = und;
  }

  if (flash) cv::rectangle(small, cv::Rect(0, 0, small.cols, small.rows), {0, 220, 0}, 4);
  return small;
}

nlohmann::json status_json(
  const calibration::Progress & prog, const calibration::Calibrator & calib, bool board,
  bool undistort, const std::string & hint)
{
  nlohmann::json j;
  j["calib"] = true;
  j["board"] = board ? 1 : 0;
  j["n"] = prog.n;
  j["min_n"] = calibration::Calibrator::kMinSamples;
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
  auto last_ui = std::chrono::steady_clock::now() - kUiGap;

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

  auto do_calibrate = [&](const std::string & host_time) {
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }
    hint = "calibrating...";
    if (!calib.calibrate_camera()) {
      hint = "camera calib failed";
      tools::RemoteLogger::instance().log("INFO", "{}", hint);
      return;
    }
    if (!host_time.empty()) calib.set_calibrated_at(host_time);
    tools::RemoteLogger::instance().log(
      "INFO", "intrinsics ok, reproj {:.4f}px, at {}", calib.camera().reproj_error,
      calib.calibrated_at());
    fmt::print("\n{}\n", calib.yaml_snippet());
    if (calib.save_yaml()) {
      hint = fmt::format(
        "saved {} ({})", calibration::Calibrator::kResultPath, calib.calibrated_at());
      tools::RemoteLogger::instance().log("INFO", "{}", hint);
    } else {
      hint = fmt::format(
        "calib ok ({:.4f}px) but save failed", calib.camera().reproj_error);
      tools::RemoteLogger::instance().log("ERROR", "failed to write {}",
                                          calibration::Calibrator::kResultPath);
    }
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

  auto apply_remote = [&](const nlohmann::json & msg) {
    const auto cmd = msg.value("cmd", "");
    if (cmd.empty()) return;
    if (cmd == "add")
      want_add = true;
    else if (cmd == "calibrate")
      do_calibrate(msg.value("host_time", ""));
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

  auto apply_cmd = [&](const std::string & cmd) { apply_remote({{"cmd", cmd}}); };

  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  while (!exiter.exit() && !quit_cmd) {
    nlohmann::json remote;
    while (tools::RemoteLogger::instance().poll_json(remote)) apply_remote(remote);
    std::string legacy;
    while (tools::RemoteLogger::instance().poll_calib_cmd(legacy)) apply_cmd(legacy);

    camera->read(img, stamp);
    if (img.empty()) break;

    std::vector<cv::Point2f> corners;
    calibration::SampleParams params;
    const bool found = calib.detect(img, corners, params, /*refine=*/false);

    const auto now = std::chrono::steady_clock::now();
    const bool cooled = now - last_add >= kAutoAddGap;
    if (found && cooled && (want_add || calib.is_good_sample(params))) {
      auto refined = corners;
      calib.refine_corners(img, refined);
      const auto refined_params = calib.sample_params(refined, img.size());
      if (calib.add_sample(refined, refined_params, img.size(), nullptr)) {
        last_add = now;
        flash_until = now + std::chrono::milliseconds(180);
        hint = fmt::format("added #{}", calib.size());
        tools::RemoteLogger::instance().log("INFO", "sample {} added", calib.size());
      } else if (want_add) {
        hint = "sample too similar, move the board";
      }
    }
    want_add = false;

    if (now - last_ui >= kUiGap) {
      last_ui = now;
      const auto prog = calib.progress();
      const bool flash = now < flash_until;
      cv::Mat view =
        annotate_preview(img, corners, found, calib, undistort, flash, kPreviewW);
      tools::RemoteLogger::instance().plot(status_json(prog, calib, found, undistort, hint));
      tools::RemoteLogger::instance().plot_image(view, {{"name", "calibrate"}});
    }
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
