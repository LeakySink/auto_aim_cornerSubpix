// 全分辨率评估数据采集器
//
// 为什么需要它：.rlog 里的图片会被 remote_logger.img_width 压到 640 宽，无法用来
// 重跑检测（等效尺度会变），也不适合 1 月重训 YOLO。本工具**直接写标准 RLG2 .rlog**，
// 但图片按**全分辨率**编码，因此：
//   * ./build/rlog_replay <config.yaml> <file.rlog>   离线重跑检测做模型对比
//   * python -m rdbg metrics <file.rlog>              逐帧指标（同一套字段名）
//   * python -m rdbg replay  <file.rlog>              host 端可视化回放
//
// 用法：
//   ./build/eval_record <config.yaml> [--out <file.rlog>] [--every K] [--max-frames N]
//                        [--quality Q] [--no-detect] [--quiet]
//   默认：out=./eval_data/run_<时间戳>.rlog, every=2（每 2 帧存 1 张）, max-frames=3000,
//         quality=90（JPEG）, 开检测（用于附带每帧检出信息）
//
// 写盘量估算：1440x1080 JPEG q90 ≈ 250 KB/张；every=2 @60fps ≈ 7.5 MB/s。
// 采 3000 帧（约 100 s）≈ 750 MB，请确认磁盘空间。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "io/camera.hpp"
#include "io/gimbal/gimbal.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/yolo.hpp"
#include "tools/exiter.hpp"
#include "tools/math_tools.hpp"
#include "tools/remote_logger.hpp"

namespace
{
constexpr uint32_t kMagicV2 = 0x32474C52;  // "RLG2"
constexpr uint8_t kRecJson = 0x00;
constexpr uint8_t kRecImg = 0x01;

void write_u32(std::ofstream & fp, uint32_t v) { fp.write(reinterpret_cast<const char *>(&v), 4); }
void write_u64(std::ofstream & fp, uint64_t v) { fp.write(reinterpret_cast<const char *>(&v), 8); }
void write_u8(std::ofstream & fp, uint8_t v) { fp.write(reinterpret_cast<const char *>(&v), 1); }

/// 写一条 JSON 记录：type + ts + len + payload
void write_json(std::ofstream & fp, uint64_t ts, const nlohmann::json & j)
{
  const std::string raw = j.dump();
  write_u8(fp, kRecJson);
  write_u64(fp, ts);
  write_u32(fp, static_cast<uint32_t>(raw.size()));
  fp.write(raw.data(), static_cast<std::streamsize>(raw.size()));
}

/// 写一条图片记录：type + ts + meta_len + meta + jpg_len + jpeg
void write_img(std::ofstream & fp, uint64_t ts, const std::string & topic, const std::vector<unsigned char> & jpg)
{
  const std::string meta = nlohmann::json{{"name", topic}}.dump();
  write_u8(fp, kRecImg);
  write_u64(fp, ts);
  write_u32(fp, static_cast<uint32_t>(meta.size()));
  fp.write(meta.data(), static_cast<std::streamsize>(meta.size()));
  write_u32(fp, static_cast<uint32_t>(jpg.size()));
  fp.write(reinterpret_cast<const char *>(jpg.data()), static_cast<std::streamsize>(jpg.size()));
}
}  // namespace

int main(int argc, char ** argv)
{
  std::string config_path;
  std::string out_path;
  int every = 2;
  int max_frames = 3000;
  int quality = 90;
  bool detect = true;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto next = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (s == "--out") { if (const char * v = next()) out_path = v; }
    else if (s == "--every") { if (const char * v = next()) every = std::atoi(v); }
    else if (s == "--max-frames") { if (const char * v = next()) max_frames = std::atoi(v); }
    else if (s == "--quality") { if (const char * v = next()) quality = std::atoi(v); }
    else if (s == "--no-detect") detect = false;
    else if (s == "--quiet") quiet = true;
    else if (!s.empty() && s[0] != '-') config_path = s;
  }
  if (config_path.empty()) {
    std::printf(
      "用法：./build/eval_record <config.yaml> [--out <file.rlog>] [--every K] [--max-frames N]\n"
      "                      [--quality Q] [--no-detect] [--quiet]\n");
    return 2;
  }
  if (out_path.empty()) {
    std::filesystem::create_directories("eval_data");
    out_path = "eval_data/run_" + std::to_string(
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count()) + ".rlog";
  } else {
    std::filesystem::path p(out_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
  }

  std::ofstream fp(out_path, std::ios::binary);
  if (!fp) {
    std::printf("[eval_record] 无法写 %s\n", out_path.c_str());
    return 1;
  }
  write_u32(fp, kMagicV2);

  tools::RemoteLogger::instance().init(config_path);
  io::Camera camera(config_path);
  io::Gimbal gimbal(config_path);

  // 检测只用于附带每帧检出信息（评估/预标注）；--no-detect 时纯采图
  std::unique_ptr<auto_aim::YOLO> yolo;
  std::unique_ptr<auto_aim::Solver> solver;
  if (detect) {
    yolo = std::make_unique<auto_aim::YOLO>(config_path, false);
    solver = std::make_unique<auto_aim::Solver>(config_path);
  }

  std::printf("[eval_record] 输出 %s（every=%d, quality=%d, detect=%d, max_frames=%d）\n",
              out_path.c_str(), every, quality, detect ? 1 : 0, max_frames);

  tools::Exiter exiter;
  cv::Mat img;
  std::chrono::steady_clock::time_point t;
  const auto t0 = std::chrono::steady_clock::now();
  int frame = 0, saved = 0, with_det = 0;
  std::vector<unsigned char> jpg;
  const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, quality};

  while (!exiter.exit() && (max_frames <= 0 || saved < max_frames)) {
    camera.read(img, t);
    const auto q = gimbal.q(t);
    if (solver) solver->set_R_gimbal2world(q);
    const auto gs = gimbal.state();
    const uint64_t ts = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());

    // 每帧都写 JSON（指标可看连续帧），按 every 抽帧存图
    nlohmann::json row;
    row["t"] = tools::delta_time(t, t0);
    row["gimbal_yaw"] = gs.yaw;      // rad
    row["gimbal_pitch"] = gs.pitch;
    row["gimbal_yaw_vel"] = gs.yaw_vel;
    row["gimbal_pitch_vel"] = gs.pitch_vel;
    row["gimbal_qw"] = q.w();
    row["gimbal_qx"] = q.x();
    row["gimbal_qy"] = q.y();
    row["gimbal_qz"] = q.z();
    row["bullet_speed"] = gs.bullet_speed;

    int det_n = 0;
    double max_conf = 0.0, best_dist = 0.0;
    if (yolo && solver) {
      auto armors = yolo->detect(img, frame);
      for (auto & a : armors) {
        solver->solve(a);
        if (a.confidence <= max_conf) continue;
        max_conf = a.confidence;
        best_dist = a.xyz_in_world.norm();
      }
      det_n = static_cast<int>(armors.size());
      if (det_n > 0) ++with_det;
    }
    row["det_n"] = det_n;
    row["max_conf"] = max_conf;
    row["obs_dist"] = best_dist;

    if (frame % every == 0) {
      if (cv::imencode(".jpg", img, jpg, params)) {
        write_img(fp, ts, "raw", jpg);  // 全分辨率原始帧
        ++saved;
      }
    }
    write_json(fp, ts, row);
    if (!quiet && frame % 60 == 0)
      std::printf("[eval_record] 帧 %d 已存图 %d det=%d maxconf=%.2f\n", frame, saved, det_n, max_conf);
    ++frame;
  }

  fp.flush();
  fp.close();
  std::printf("\n[eval_record] 完成：处理 %d 帧，存图 %d 张（有检出 %d 帧）-> %s\n", frame, saved,
              with_det, out_path.c_str());
  std::printf("[eval_record] 后续：\n");
  std::printf("  ./build/replay %s %s            # 重跑检测\n", config_path.c_str(), out_path.c_str());
  std::printf("  python -m rdbg replay %s   # host 可视化回放\n", out_path.c_str());
  return 0;
}
