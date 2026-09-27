#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
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

// ---- timing / preview ----
constexpr auto kAutoAddGap = std::chrono::milliseconds(250);
constexpr auto kUiGap = std::chrono::milliseconds(80);    // ~12fps 预览
constexpr auto kLoopGap = std::chrono::milliseconds(15);  // 主循环下限，检测可更慢
constexpr int kPreviewW = 400;
constexpr double kCalibFrameRate = 20.0;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---- camera params (yaml 里 exposure_ms 键名历史兼容，数值按微秒) ----
struct CamParams
{
  std::string camera_name = "hikrobot";
  double exposure_us = 10.0;
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
    if (y["exposure_us"])
      p.exposure_us = y["exposure_us"].as<double>();
    else if (y["exposure_ms"])
      p.exposure_us = y["exposure_ms"].as<double>();
    if (y["gain"]) p.gain = y["gain"].as<double>();
    if (y["gamma"]) p.gamma = y["gamma"].as<double>();
    if (y["vid_pid"]) p.vid_pid = y["vid_pid"].as<std::string>();
  } catch (const std::exception &) {
  }
  return p;
}

std::unique_ptr<io::CameraBase> open_camera(const CamParams & p)
{
  const double exposure_ms = p.exposure_us / 1e3;
  tools::RemoteLogger::instance().log(
    "INFO", "open camera '{}' exposure={:.1f}us ({:.4f}ms) gain={:.1f} auto_gain={}",
    p.camera_name, p.exposure_us, exposure_ms, p.gain, p.camera_name == "hikrobot");

  if (p.camera_name == "mindvision")
    return std::make_unique<io::MindVision>(exposure_ms, p.gamma, p.vid_pid);
  if (p.camera_name == "hikrobot")
    return std::make_unique<io::HikRobot>(
      exposure_ms, p.gain, p.vid_pid, /*auto_gain=*/true, kCalibFrameRate);
  throw std::runtime_error("unknown camera_name: " + p.camera_name);
}

void init_remote_logger(bool test_feed)
{
  tools::RemoteLogger::Config cfg;
  cfg.sender_name = test_feed ? "calibrate-test" : "calibrate";
  cfg.app = "calibrate";
  cfg.heartbeat_interval_ms = 500;
  cfg.enable_remote = true;
  cfg.enable_local = false;  // 预览优先，避免 img worker 被落盘拖住
  cfg.img_width = 0;
  cfg.img_quality = 45;
  tools::RemoteLogger::instance().init(cfg);
}

// ---- host JSON / preview ----
cv::Mat make_preview(
  const cv::Mat & img, const std::vector<cv::Point2f> & corners, bool found,
  const calibration::Calibrator & calib, bool undistort, bool flash)
{
  cv::Mat small;
  const double scale =
    (kPreviewW > 0 && img.cols > kPreviewW) ? static_cast<double>(kPreviewW) / img.cols : 1.0;
  if (scale != 1.0)
    cv::resize(img, small, cv::Size(), scale, scale, cv::INTER_AREA);
  else
    small = img.clone();

  for (const auto & s : calib.sample_views()) {
    cv::circle(
      small,
      {static_cast<int>(s.params.x * small.cols), static_cast<int>(s.params.y * small.rows)}, 3,
      {40, 160, 220}, -1, cv::LINE_AA);
  }

  if (found && !corners.empty()) {
    auto scaled = corners;
    if (scale != 1.0) {
      for (auto & c : scaled) {
        c.x = static_cast<float>(c.x * scale);
        c.y = static_cast<float>(c.y * scale);
      }
    }
    cv::drawChessboardCorners(small, calib.pattern_size(), scaled, true);
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

nlohmann::json make_status(
  const calibration::Progress & prog, const calibration::Calibrator & calib, bool board,
  bool undistort, const std::string & hint, double exposure_us)
{
  nlohmann::json j{
    {"calib", true},
    {"board", board ? 1 : 0},
    {"n", prog.n},
    {"min_n", calibration::Calibrator::kMinSamples},
    {"x", prog.x},
    {"y", prog.y},
    {"size", prog.size},
    {"skew", prog.skew},
    {"goodenough", prog.goodenough ? 1 : 0},
    {"hint", hint},
    {"undistort", undistort ? 1 : 0},
    {"has_cam", calib.has_camera() ? 1 : 0},
    {"reproj", calib.has_camera() ? calib.camera().reproj_error : -1.0},
    {"exposure_us", exposure_us},
  };
  if (!calib.calibrated_at().empty()) j["calibrated_at"] = calib.calibrated_at();

  nlohmann::json samples = nlohmann::json::array();
  for (const auto & s : calib.sample_views()) {
    samples.push_back(
      {{"x", s.params.x}, {"y", s.params.y}, {"size", s.params.size}, {"skew", s.params.skew}});
  }
  j["samples"] = std::move(samples);
  return j;
}

nlohmann::json make_result(
  const calibration::Calibrator & calib, bool saved, const std::string & hint)
{
  return {
    {"calib", true},
    {"calib_done", 1},
    {"saved", saved ? 1 : 0},
    {"hint", hint},
    {"has_cam", 1},
    {"n", calib.size()},
    {"min_n", calibration::Calibrator::kMinSamples},
    {"reproj", calib.camera().reproj_error},
    {"calibrated_at", calib.calibrated_at()},
    {"result_path", calib.result_path()},
    {"camera_matrix",
     std::vector<double>(
       calib.camera().camera_matrix.begin<double>(), calib.camera().camera_matrix.end<double>())},
    {"distort_coeffs",
     std::vector<double>(
       calib.camera().distort_coeffs.begin<double>(), calib.camera().distort_coeffs.end<double>())},
  };
}

void publish(const nlohmann::json & j) { tools::RemoteLogger::instance().plot(j); }

void publish_image(const cv::Mat & view)
{
  tools::RemoteLogger::instance().plot_image(view, {{"name", "calibrate"}});
}

// ---- session: 状态 + 命令 + 主循环步骤 ----
struct Session
{
  calibration::Calibrator calib;
  CamParams cam;
  std::unique_ptr<io::CameraBase> camera;
  bool test_feed = false;

  std::string hint;
  bool undistort = false;
  bool want_add = false;
  bool quit = false;
  bool finished = false;

  TimePoint last_add = Clock::now() - kAutoAddGap;
  TimePoint flash_until{};
  TimePoint last_ui = Clock::now() - kUiGap;

  // 上一帧检测结果，供预览叠加（预览先于本次 detect）
  std::vector<cv::Point2f> overlay_corners;
  bool overlay_found = false;

  void log_banner() const
  {
    if (test_feed) {
      tools::RemoteLogger::instance().log(
        "WARN", "CALIB_TEST_FEED active (--test); sender=calibrate-test; remove after debug");
    }
    tools::RemoteLogger::instance().log(
      "INFO", "intrinsics-only calibrate, board {}x{}, save -> {}", calib.pattern_size().width,
      calib.pattern_size().height, calib.result_path());
    tools::RemoteLogger::instance().log(
      "INFO", "run: ./host/start.sh  then Calibrate in the portal");
  }

  void poll_host()
  {
    nlohmann::json msg;
    while (tools::RemoteLogger::instance().poll_json(msg)) handle_msg(msg);
    std::string legacy;
    while (tools::RemoteLogger::instance().poll_calib_cmd(legacy)) handle_msg({{"cmd", legacy}});
  }

  void handle_msg(const nlohmann::json & msg)
  {
    const auto cmd = msg.value("cmd", "");
    if (cmd.empty()) return;

    if (cmd == "add") {
      want_add = true;
    } else if (cmd == "calibrate") {
      run_calibrate(msg.value("host_time", ""));
    } else if (cmd == "drop") {
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
      set_exposure(msg);
    } else if (cmd == "quit" || cmd == "done") {
      quit = true;
      hint = "host done, exiting";
      tools::RemoteLogger::instance().log("INFO", "quit by host");
    }
  }

  void set_exposure(const nlohmann::json & msg)
  {
    if (finished || test_feed) return;

    double us = cam.exposure_us;
    if (msg.contains("exposure_us"))
      us = msg.value("exposure_us", us);
    else if (msg.contains("exposure_ms"))
      us = msg.value("exposure_ms", us);
    us = std::clamp(us, 1.0, 1e6);

    camera.reset();
    cam.exposure_us = us;
    try {
      camera = open_camera(cam);
      hint = fmt::format("exposure {:.0f} us (reopened)", cam.exposure_us);
      tools::RemoteLogger::instance().log("INFO", "{}", hint);
    } catch (const std::exception & e) {
      hint = fmt::format("reopen failed: {}", e.what());
      tools::RemoteLogger::instance().log("ERROR", "{}", hint);
    }
  }

  void run_calibrate(const std::string & host_time)
  {
    if (finished) return;
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }

    hint = "calibrating...";
    publish(make_status(prog, calib, false, false, hint, cam.exposure_us));

    if (!calib.calibrate_camera()) {
      hint = "camera calib failed";
      tools::RemoteLogger::instance().log("ERROR", "{}", hint);
      publish(make_status(prog, calib, false, false, hint, cam.exposure_us));
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

    finished = true;
    const auto payload = make_result(calib, saved, hint);
    for (int i = 0; i < 5; ++i) {  // UDP 多发几次防丢包
      publish(payload);
      std::this_thread::sleep_for(20ms);
    }
  }

  /// 用上一帧角点叠加当前图，先推流再 detect，避免检测阻塞造成画面发旧
  void maybe_push_preview(const cv::Mat & img)
  {
    const auto now = Clock::now();
    if (now - last_ui < kUiGap) return;
    last_ui = now;

    const bool flash = now < flash_until;
    publish(make_status(
      calib.progress(), calib, overlay_found, undistort, hint, cam.exposure_us));
    publish_image(make_preview(img, overlay_corners, overlay_found, calib, undistort, flash));
  }

  void detect_and_maybe_add(const cv::Mat & img)
  {
    std::vector<cv::Point2f> corners;
    calibration::SampleParams params;
    const bool found = calib.detect(img, corners, params, /*refine=*/false);

    overlay_found = found;
    overlay_corners = found ? corners : std::vector<cv::Point2f>{};

    const auto now = Clock::now();
    const bool cooled = now - last_add >= kAutoAddGap;
    if (!found || !cooled || !(want_add || calib.is_good_sample(params))) {
      want_add = false;
      return;
    }

    auto refined = corners;
    calib.refine_corners(img, refined);
    const auto refined_params = calib.sample_params(refined, img.size());
    if (calib.add_sample(refined, refined_params, img.size(), nullptr)) {
      last_add = now;
      flash_until = now + 180ms;
      hint = fmt::format("added #{}", calib.size());
      tools::RemoteLogger::instance().log("INFO", "sample {} added", calib.size());
    } else if (want_add) {
      hint = "sample too similar, move the board";
    }
    want_add = false;
  }

  /// 一轮：收命令 → 取流 → 预览 → 检测/采样
  /// @return false 表示应退出主循环（空帧）
  bool step()
  {
    poll_host();

    if (finished) {
      std::this_thread::sleep_for(20ms);
      return true;
    }
    if (!camera) {
      std::this_thread::sleep_for(50ms);
      return true;
    }

    cv::Mat img;
    TimePoint stamp;
    camera->read(img, stamp);
    if (img.empty()) return false;

    maybe_push_preview(img);
    detect_and_maybe_add(img);
    return true;
  }
};

}  // namespace

int main(int argc, char * argv[])
{
  cv::CommandLineParser cli(argc, argv, keys);
  if (cli.has("help")) {
    cli.printMessage();
    return 0;
  }

  // CALIB_TEST_FEED ↓ 调试结束后删除本块
  const bool test_feed = cli.has("test");
  init_remote_logger(test_feed);

  tools::Exiter exiter;
  Session app;
  app.test_feed = test_feed;
  app.cam = load_cam_params(app.calib.result_path());
  app.camera =
    test_feed ? calib_test_feed::make(app.calib.pattern_size()) : open_camera(app.cam);
  app.hint = test_feed ? "TEST FEED on — no real camera"
                       : "open host calibrate page, wave the board";
  // CALIB_TEST_FEED ↑

  app.log_banner();

  while (!exiter.exit() && !app.quit) {
    const auto t0 = Clock::now();
    if (!app.step()) break;
    const auto dt = Clock::now() - t0;
    if (dt < kLoopGap) std::this_thread::sleep_for(kLoopGap - dt);
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
