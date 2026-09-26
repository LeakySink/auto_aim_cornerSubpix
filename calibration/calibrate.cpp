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
constexpr auto kUiGap = std::chrono::milliseconds(66);     // ~15fps 推流
constexpr auto kLoopGap = std::chrono::milliseconds(66);   // 主循环上限 ~15Hz，避免狂跑棋盘检测
constexpr int kPreviewW = 480;
constexpr double kCalibFrameRate = 15.0;  // 海康取流帧率（标定够用）

struct CamParams
{
  std::string camera_name = "hikrobot";
  double exposure_ms = 10.0;
  double gain = 16.0;
  double gamma = 12.0;
  std::string vid_pid = "2bdf:0001";
};

CamParams load_cam_params(const std::string & result_path)
{
  CamParams p;
  try {
    const auto y = YAML::LoadFile(result_path);
    if (y["camera_name"]) p.camera_name = y["camera_name"].as<std::string>();
    if (y["exposure_ms"]) p.exposure_ms = y["exposure_ms"].as<double>();
    if (y["gain"]) p.gain = y["gain"].as<double>();
    if (y["gamma"]) p.gamma = y["gamma"].as<double>();
    if (y["vid_pid"]) p.vid_pid = y["vid_pid"].as<std::string>();
  } catch (const std::exception &) {
  }
  return p;
}

std::unique_ptr<io::CameraBase> open_camera(const CamParams & p)
{
  tools::RemoteLogger::instance().log(
    "INFO", "open camera '{}' exposure_ms={:.1f} gain={:.1f} auto_gain={}", p.camera_name,
    p.exposure_ms, p.gain, p.camera_name == "hikrobot");

  if (p.camera_name == "mindvision")
    return std::make_unique<io::MindVision>(p.exposure_ms, p.gamma, p.vid_pid);
  if (p.camera_name == "hikrobot")
    return std::make_unique<io::HikRobot>(
      p.exposure_ms, p.gain, p.vid_pid, /*auto_gain=*/true, kCalibFrameRate);
  throw std::runtime_error("unknown camera_name: " + p.camera_name);
}

void init_remote_logger(bool test_feed)
{
  tools::RemoteLogger::Config cfg;
  cfg.sender_name = test_feed ? "calibrate-test" : "calibrate";
  cfg.app = "calibrate";
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
  bool undistort, const std::string & hint, double exposure_ms)
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
  j["exposure_ms"] = exposure_ms;
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

nlohmann::json result_json(const calibration::Calibrator & calib, bool saved, const std::string & hint)
{
  nlohmann::json j;
  j["calib"] = true;
  j["calib_done"] = 1;
  j["saved"] = saved ? 1 : 0;
  j["hint"] = hint;
  j["has_cam"] = 1;
  j["n"] = calib.size();
  j["min_n"] = calibration::Calibrator::kMinSamples;
  j["reproj"] = calib.camera().reproj_error;
  j["calibrated_at"] = calib.calibrated_at();
  j["result_path"] = calib.result_path();
  j["camera_matrix"] = std::vector<double>(
    calib.camera().camera_matrix.begin<double>(), calib.camera().camera_matrix.end<double>());
  j["distort_coeffs"] = std::vector<double>(
    calib.camera().distort_coeffs.begin<double>(), calib.camera().distort_coeffs.end<double>());
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
  CamParams cam_params = load_cam_params(calib.result_path());
  std::unique_ptr<io::CameraBase> camera =
    test_feed ? calib_test_feed::make(calib.pattern_size()) : open_camera(cam_params);
  // CALIB_TEST_FEED ↑

  std::string hint = test_feed ? "TEST FEED on — no real camera"
                               : "open host calibrate page, wave the board";
  bool undistort = false;
  bool want_add = false;
  bool quit_cmd = false;
  bool finished = false;
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
    calib.pattern_size().height, calib.result_path());
  tools::RemoteLogger::instance().log(
    "INFO", "run: ./host/start.sh  then Calibrate in the portal");

  auto do_calibrate = [&](const std::string & host_time) {
    if (finished) return;
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }
    hint = "calibrating...";
    tools::RemoteLogger::instance().plot(
      status_json(prog, calib, false, false, hint, cam_params.exposure_ms));
    if (!calib.calibrate_camera()) {
      hint = "camera calib failed";
      tools::RemoteLogger::instance().log("ERROR", "{}", hint);
      tools::RemoteLogger::instance().plot(
        status_json(prog, calib, false, false, hint, cam_params.exposure_ms));
      return;
    }
    if (!host_time.empty()) calib.set_calibrated_at(host_time);

    const bool saved = calib.save_yaml();
    if (saved) {
      hint = fmt::format("saved {} ({})", calib.result_path(), calib.calibrated_at());
      tools::RemoteLogger::instance().log("INFO", "{}", hint);
    } else {
      hint = fmt::format(
        "calib ok ({:.4f}px) but save failed: {}", calib.camera().reproj_error, calib.result_path());
      tools::RemoteLogger::instance().log("ERROR", "{}", hint);
    }
    fmt::print("\n{}\n", calib.yaml_snippet());

    // 回传结果给 host；等待 host 下发 quit
    finished = true;
    auto payload = result_json(calib, saved, hint);
    tools::RemoteLogger::instance().plot(payload);
    // 多发几次，避免 UDP 丢包
    for (int i = 0; i < 5; i++) {
      tools::RemoteLogger::instance().plot(payload);
      std::this_thread::sleep_for(20ms);
    }
  };

  auto apply_remote = [&](const nlohmann::json & msg) {
    const auto cmd = msg.value("cmd", "");
    if (cmd.empty()) return;
    if (cmd == "add")
      want_add = true;
    else if (cmd == "calibrate")
      do_calibrate(msg.value("host_time", ""));
    else if (cmd == "drop") {
      if (!finished) {
        calib.drop_last();
        hint = "dropped last sample";
      }
    } else if (cmd == "reset") {
      if (!finished) {
        calib.reset();
        undistort = false;
        hint = "reset";
      }
    } else if (cmd == "set_exposure") {
      if (finished || test_feed) return;
      double ms = msg.value("exposure_ms", cam_params.exposure_ms);
      if (ms < 0.1) ms = 0.1;
      if (ms > 100.0) ms = 100.0;
      // 不改 io：关相机 → 用新曝光重开
      camera.reset();
      cam_params.exposure_ms = ms;
      try {
        camera = open_camera(cam_params);
        hint = fmt::format("exposure {:.1f} ms (reopened)", cam_params.exposure_ms);
        tools::RemoteLogger::instance().log("INFO", "{}", hint);
      } catch (const std::exception & e) {
        hint = fmt::format("reopen failed: {}", e.what());
        tools::RemoteLogger::instance().log("ERROR", "{}", hint);
      }
    } else if (cmd == "quit" || cmd == "done") {
      quit_cmd = true;
      hint = "host done, exiting";
      tools::RemoteLogger::instance().log("INFO", "quit by host");
    }
  };

  auto apply_cmd = [&](const std::string & cmd) { apply_remote({{"cmd", cmd}}); };

  cv::Mat img;
  std::chrono::steady_clock::time_point stamp;

  while (!exiter.exit() && !quit_cmd) {
    const auto loop_start = std::chrono::steady_clock::now();

    nlohmann::json remote;
    while (tools::RemoteLogger::instance().poll_json(remote)) apply_remote(remote);
    std::string legacy;
    while (tools::RemoteLogger::instance().poll_calib_cmd(legacy)) apply_cmd(legacy);

    if (finished) {
      std::this_thread::sleep_for(20ms);
      continue;
    }

    if (!camera) {
      std::this_thread::sleep_for(50ms);
      continue;
    }

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
      tools::RemoteLogger::instance().plot(
        status_json(prog, calib, found, undistort, hint, cam_params.exposure_ms));
      tools::RemoteLogger::instance().plot_image(view, {{"name", "calibrate"}});
    }

    // 限速：棋盘检测是 CPU 大户，不要跟相机硬件帧率硬扛
    const auto elapsed = std::chrono::steady_clock::now() - loop_start;
    if (elapsed < kLoopGap) std::this_thread::sleep_for(kLoopGap - elapsed);
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
