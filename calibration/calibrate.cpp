#include <fmt/core.h>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

#include "calibration/calibrator.hpp"
#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tools/exiter.hpp"
#include "tools/img_tools.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"
#include "tools/yaml.hpp"

using namespace std::chrono_literals;

const std::string keys =
  "{help h usage ? |                          | 输出命令行参数说明}"
  "{@config-path   | configs/calibration.yaml | yaml配置（棋盘格/相机/云台）}"
  "{output-path o  |                          | 结果写入路径，默认与配置相同}"
  "{camera-only    |                          | 只标内参，不打开云台}";

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
  const calibration::Calibrator & calib, bool undistort, bool flash,
  const Eigen::Vector3d * ypr)
{
  cv::Mat view = img.clone();
  const auto pattern = calib.pattern_size();
  for (const auto & s : calib.sample_views())
    draw_quad(view, s.corners, pattern, s.has_q ? cv::Scalar(80, 180, 80) : cv::Scalar(40, 160, 220));

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

  if (ypr) {
    tools::draw_text(view, fmt::format("yaw   {:.2f}", (*ypr)[0]), {16, 32}, {0, 0, 255}, 0.65, 2);
    tools::draw_text(view, fmt::format("pitch {:.2f}", (*ypr)[1]), {16, 60}, {0, 0, 255}, 0.65, 2);
    tools::draw_text(view, fmt::format("roll  {:.2f}", (*ypr)[2]), {16, 88}, {0, 0, 255}, 0.65, 2);
  }
  if (flash) cv::rectangle(view, cv::Rect(0, 0, view.cols, view.rows), {0, 220, 0}, 8);
  return view;
}

nlohmann::json status_json(
  const calibration::Progress & prog, const calibration::Calibrator & calib, bool board,
  bool undistort, bool camera_only, const std::string & hint, const Eigen::Vector3d * ypr)
{
  nlohmann::json j;
  j["calib"] = true;
  j["board"] = board ? 1 : 0;
  j["n"] = prog.n;
  j["n_q"] = prog.n_with_q;
  j["x"] = prog.x;
  j["y"] = prog.y;
  j["size"] = prog.size;
  j["skew"] = prog.skew;
  j["goodenough"] = prog.goodenough ? 1 : 0;
  j["hint"] = hint;
  j["undistort"] = undistort ? 1 : 0;
  j["camera_only"] = camera_only ? 1 : 0;
  j["has_cam"] = calib.has_camera() ? 1 : 0;
  j["has_hand"] = calib.has_handeye() ? 1 : 0;
  j["reproj"] = calib.has_camera() ? calib.camera().reproj_error : -1.0;
  if (calib.has_handeye()) {
    j["cam_yaw"] = calib.handeye().ypr_deg[0];
    j["cam_pitch"] = calib.handeye().ypr_deg[1];
    j["cam_roll"] = calib.handeye().ypr_deg[2];
  }
  if (ypr) {
    j["gimbal_yaw"] = (*ypr)[0];
    j["gimbal_pitch"] = (*ypr)[1];
    j["gimbal_roll"] = (*ypr)[2];
  }
  nlohmann::json samples = nlohmann::json::array();
  for (const auto & s : calib.sample_views()) {
    samples.push_back(
      {{"x", s.params.x},
       {"y", s.params.y},
       {"size", s.params.size},
       {"skew", s.params.skew},
       {"q", s.has_q ? 1 : 0}});
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
  auto output_path = cli.get<std::string>("output-path");
  if (output_path.empty()) output_path = config_path;
  const bool camera_only = cli.has("camera-only");

  auto yaml = tools::load(config_path);
  const int cols = tools::read<int>(yaml, "pattern_cols");
  const int rows = tools::read<int>(yaml, "pattern_rows");
  const double square_mm = tools::read<double>(yaml, "square_size_mm");
  auto R_data = tools::read<std::vector<double>>(yaml, "R_gimbal2imubody");
  Eigen::Matrix<double, 3, 3, Eigen::RowMajor> R_gimbal2imubody(R_data.data());

  if (!yaml["remote_logger"]) {
    fmt::print(stderr, "calibration.yaml needs remote_logger for web UI\n");
    return 1;
  }
  tools::RemoteLogger::instance().init(config_path);

  tools::Exiter exiter;
  io::Camera camera(config_path);
  std::unique_ptr<io::Gimbal> gimbal;
  if (!camera_only) gimbal = std::make_unique<io::Gimbal>(config_path);

  calibration::Calibrator calib(cols, rows, square_mm, R_gimbal2imubody);

  std::string hint = "open host calibrate page, wave the board";
  bool undistort = false;
  bool want_add = false;
  bool quit_cmd = false;
  auto last_add = std::chrono::steady_clock::now() - kAutoAddGap;
  auto flash_until = std::chrono::steady_clock::now();

  tools::RemoteLogger::instance().log(
    "INFO", "web calibrate ready, board {}x{}, save -> {}", cols, rows, output_path);
  tools::RemoteLogger::instance().log(
    "INFO", "run: ./host/calibrate.sh   then press buttons in the browser");

  auto do_calibrate = [&]() {
    const auto prog = calib.progress();
    if (prog.n < calibration::Calibrator::kMinSamples) {
      hint = fmt::format("need >= {} samples", calibration::Calibrator::kMinSamples);
      return;
    }
    hint = "calibrating...";
    const bool ok_cam = calib.calibrate_camera();
    bool ok_hand = false;
    if (ok_cam && !camera_only) ok_hand = calib.calibrate_handeye();
    if (!ok_cam)
      hint = "camera calib failed";
    else if (camera_only)
      hint = fmt::format("camera ok, reproj {:.4f}px  (SAVE)", calib.camera().reproj_error);
    else if (!ok_hand)
      hint = fmt::format(
        "camera ok ({:.4f}px), handeye needs more IMU poses", calib.camera().reproj_error);
    else
      hint = fmt::format("done, reproj {:.4f}px  (SAVE)", calib.camera().reproj_error);
    tools::RemoteLogger::instance().log("INFO", "{}", hint);
    fmt::print("\n{}\n", calib.yaml_snippet());
  };

  auto do_save = [&]() {
    if (!calib.has_camera()) {
      hint = "calibrate first";
      return;
    }
    if (calib.save_yaml(output_path)) {
      hint = fmt::format("saved {}", output_path);
      tools::RemoteLogger::instance().log("INFO", "wrote {}", output_path);
    } else {
      hint = "save failed";
      tools::RemoteLogger::instance().log("ERROR", "failed to write {}", output_path);
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
    std::string cmd;
    while (tools::RemoteLogger::instance().poll_calib_cmd(cmd)) apply_cmd(cmd);

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

    const auto prog = calib.progress();
    const bool flash = now < flash_until;
    cv::Mat view = annotate(img, corners, found, calib, undistort, flash, yprptr);

    tools::RemoteLogger::instance().plot(
      status_json(prog, calib, found, undistort, camera_only, hint, yprptr));
    tools::RemoteLogger::instance().plot_image(view, {{"name", "calibrate"}});

    std::this_thread::sleep_for(1ms);
  }

  tools::RemoteLogger::instance().shutdown();
  return 0;
}
