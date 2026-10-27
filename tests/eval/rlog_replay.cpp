// 离线回放器：读 .rlog（RLG2）→ 逐帧重跑 检测(+PnP，可选 tracker) → 输出 CSV
//
// 用途：① 1 月换模型后，用**同一段录制数据**对比新旧模型的检出率/置信度/PnP 距离一致性；
//       ② 把现场问题（丢帧、抖动、关联锁死）离线复现，不用赌现场。
//
// 用法：
//   ./build/rlog_replay <config.yaml> <run.rlog> [options]
//     --out <csv>      输出 CSV（默认 replay.csv）
//     --topic <name>   只吃该图片话题（默认 raw，即未画标注的原始帧）
//     --limit <N>      只处理前 N 帧（快速迭代）
//     --tracker        同时跑 tracker（需要 rlog 里有 gimbal_qw/qx/qy/qz）
//     --min-conf <c>   只统计置信度 ≥ c 的框（默认 0）
//     --quiet          不打印逐帧进度
//
// 注意：
//   * 图片必须是**原始帧**（话题 raw）。reprojection/tup_detect 都画过框和关键点，
//     拿它们重跑检测会被自己画的标注污染 —— 工具会检测并告警。
//   * 本工具把 R_gimbal2world 设为 I（除非 --tracker 且 rlog 带姿态四元数）。
//     PnP 的**距离**与**板朝向**是旋转不变量，因此检出一致性评估不受影响；
//     只有 tracker 的世界系状态需要真实姿态。
//
// 数据来源：车端 auto_aim_debug_mpc 的 plot(data) 与 plot_image；
//           字段含义见 docs/architecture.md 附录 B。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include "tasks/auto_aim/armor.hpp"
#include "tasks/auto_aim/solver.hpp"
#include "tasks/auto_aim/tracker.hpp"
#include "tasks/auto_aim/yolo.hpp"

namespace
{
constexpr uint32_t kMagicV2 = 0x32474C52;  // "RLG2"
constexpr uint8_t kRecJson = 0x00;
constexpr uint8_t kRecImg = 0x01;

struct ImgRec
{
  uint64_t ts = 0;
  std::string topic;
  std::string jpeg;
};

/// 只保留回放需要的字段，避免把整份 plot JSON 常驻内存
struct JsonRow
{
  uint64_t ts = 0;
  bool has_gimbal = false;
  double yaw = 0.0, pitch = 0.0;
  bool has_q = false;
  double qw = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
  bool has_obs_dist = false;
  double obs_dist = 0.0;
};

bool read_exact(std::ifstream & fp, void * dst, std::size_t n)
{
  fp.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(fp.gcount()) == n;
}

bool read_u32(std::ifstream & fp, uint32_t & v) { return read_exact(fp, &v, sizeof(v)); }

/// 读 RLG2：头 4 字节 magic；随后 [type(1) ts(8)] + 分类型负载
bool load_rlog(
  const std::string & path, std::vector<ImgRec> & imgs, std::vector<JsonRow> & rows,
  std::string & err)
{
  std::ifstream fp(path, std::ios::binary);
  if (!fp) {
    err = "打不开文件 " + path;
    return false;
  }
  uint32_t magic = 0;
  if (!read_exact(fp, &magic, sizeof(magic))) {
    err = "文件过短";
    return false;
  }
  if (magic != kMagicV2) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "不是 RLG2（magic=0x%08x；v1 只有 JSON 无图片）", magic);
    err = buf;
    return false;
  }

  int skipped = 0;
  while (true) {
    uint8_t type = 0;
    uint64_t ts = 0;
    if (!read_exact(fp, &type, sizeof(type))) break;
    if (!read_exact(fp, &ts, sizeof(ts))) break;

    if (type == kRecJson) {
      uint32_t len = 0;
      if (!read_u32(fp, len)) break;
      std::string raw(len, '\0');
      if (len && !read_exact(fp, raw.data(), len)) break;
      auto j = nlohmann::json::parse(raw, nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        ++skipped;
        continue;
      }
      if (!j.contains("gimbal_yaw")) continue;  // 只留能配姿态的记录
      JsonRow r;
      r.ts = ts;
      r.has_gimbal = true;
      r.yaw = j.value("gimbal_yaw", 0.0);
      r.pitch = j.value("gimbal_pitch", 0.0);
      if (j.contains("gimbal_qw")) {
        r.has_q = true;
        r.qw = j.value("gimbal_qw", 1.0);
        r.qx = j.value("gimbal_qx", 0.0);
        r.qy = j.value("gimbal_qy", 0.0);
        r.qz = j.value("gimbal_qz", 0.0);
      }
      if (j.contains("obs_dist")) {
        r.has_obs_dist = true;
        r.obs_dist = j.value("obs_dist", 0.0);
      }
      rows.push_back(r);
    } else if (type == kRecImg) {
      uint32_t mlen = 0, jlen = 0;
      if (!read_u32(fp, mlen)) break;
      std::string meta(mlen, '\0');
      if (mlen && !read_exact(fp, meta.data(), mlen)) break;
      if (!read_u32(fp, jlen)) break;
      std::string jpg(jlen, '\0');
      if (jlen && !read_exact(fp, jpg.data(), jlen)) break;
      ImgRec im;
      im.ts = ts;
      im.jpeg = std::move(jpg);
      auto mj = nlohmann::json::parse(meta, nullptr, false);
      im.topic = (mj.is_object() && mj.contains("name")) ? mj.value("name", std::string()) : std::string();
      imgs.push_back(std::move(im));
    } else {
      err = "遇到未知记录类型，停止解析";
      break;
    }
  }
  std::sort(rows.begin(), rows.end(), [](const JsonRow & a, const JsonRow & b) { return a.ts < b.ts; });
  if (skipped) std::printf("[replay] 跳过 %d 条无法解析的 JSON 记录\n", skipped);
  return true;
}

struct Args
{
  std::string config;
  std::string rlog;
  std::string out = "replay.csv";
  std::string topic = "raw";
  int limit = 0;
  bool tracker = false;
  bool quiet = false;
  double min_conf = 0.0;
};

Args parse_args(int argc, char ** argv)
{
  Args a;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&](double & dst) { if (i + 1 < argc) dst = std::atof(argv[++i]); };
    if (s == "--out" && i + 1 < argc) a.out = argv[++i];
    else if (s == "--topic" && i + 1 < argc) a.topic = argv[++i];
    else if (s == "--limit" && i + 1 < argc) a.limit = std::atoi(argv[++i]);
    else if (s == "--min-conf") next(a.min_conf);
    else if (s == "--tracker") a.tracker = true;
    else if (s == "--quiet") a.quiet = true;
    else if (!s.empty() && s[0] != '-') pos.push_back(s);
  }
  if (pos.size() >= 1) a.config = pos[0];
  if (pos.size() >= 2) a.rlog = pos[1];
  return a;
}

struct FrameStat
{
  int frame = 0;
  uint64_t ts = 0;
  uint64_t json_ts = 0;
  bool has_json = false;
  double gimbal_yaw = 0.0, gimbal_pitch = 0.0;
  int det_n = 0;
  double max_conf = 0.0, mean_conf = 0.0;
  int best_name = -1, best_type = -1, best_color = -1;
  double best_conf = 0.0, best_dist = 0.0, best_armor_yaw = 0.0, best_cx = 0.0, best_cy = 0.0;
  bool has_obs_dist = false;
  double obs_dist = 0.0;
  // tracker（可选）
  bool has_ekf = false;
  double a = 0.0, w = 0.0, r = 0.0, l = 0.0, h = 0.0;
  int armor_id = -1;
  double nis = 0.0;
};

const char * name_of(int i)
{
  return (i >= 0 && i < static_cast<int>(auto_aim::ARMOR_NAMES.size()))
           ? auto_aim::ARMOR_NAMES[i].c_str()
           : "?";
}
const char * type_of(int i)
{
  return (i >= 0 && i < static_cast<int>(auto_aim::ARMOR_TYPES.size()))
           ? auto_aim::ARMOR_TYPES[i].c_str()
           : "?";
}
const char * color_of(int i)
{
  return (i >= 0 && i < static_cast<int>(auto_aim::COLORS.size())) ? auto_aim::COLORS[i].c_str() : "?";
}

}  // namespace

int main(int argc, char ** argv)
{
  const Args args = parse_args(argc, argv);
  if (args.config.empty() || args.rlog.empty()) {
    std::printf(
      "用法：./rlog_replay <config.yaml> <run.rlog> [--out replay.csv] [--topic raw] [--limit N]\n"
      "               [--tracker] [--min-conf c] [--quiet]\n");
    return 2;
  }

  std::vector<ImgRec> imgs;
  std::vector<JsonRow> jsons;
  std::string err;
  if (!load_rlog(args.rlog, imgs, jsons, err)) {
    std::printf("[replay] 读取失败：%s\n", err.c_str());
    return 1;
  }
  std::printf("[replay] rlog: %zu 张图片, %zu 条含姿态的 JSON\n", imgs.size(), jsons.size());
  if (imgs.empty()) {
    std::printf("[replay] 没有图片记录：无法重跑检测（确认录制时 host 订阅过图片流）\n");
    return 1;
  }

  // 话题筛选：默认只吃 raw（原始帧）
  std::vector<const ImgRec *> use;
  for (const auto & im : imgs)
    if (im.topic == args.topic) use.push_back(&im);
  if (use.empty()) {
    std::printf(
      "[replay] 警告：没有 topic='%s' 的图片；回退到全部图片。\n"
      "         注意 reprojection/tup_detect 是**画过标注**的图，重跑检测会被污染；\n"
      "         请用带 raw 帧的新版本重录（auto_aim_debug_mpc 已发布 raw 话题）。\n",
      args.topic.c_str());
    for (const auto & im : imgs) use.push_back(&im);
  } else {
    std::printf("[replay] 使用 topic='%s' 的 %zu 帧\n", args.topic.c_str(), use.size());
  }

  auto_aim::YOLO yolo(args.config, false);
  auto_aim::Solver solver(args.config);
  std::unique_ptr<auto_aim::Tracker> tracker;
  if (args.tracker) {
    if (jsons.empty() || !jsons.front().has_q) {
      std::printf("[replay] --tracker 需要 rlog 里有 gimbal_qw/qx/qy/qz，本次无 → 跳过 tracker\n");
    } else {
      tracker = std::make_unique<auto_aim::Tracker>(args.config, solver);
    }
  }

  std::vector<FrameStat> stats;
  stats.reserve(use.size());
  int frame = 0;
  int decoded_fail = 0;
  for (const ImgRec * pim : use) {
    if (args.limit > 0 && frame >= args.limit) break;
    cv::Mat buf(1, static_cast<int>(pim->jpeg.size()), CV_8UC1,
                const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(pim->jpeg.data())));
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (img.empty()) {
      ++decoded_fail;
      ++frame;
      continue;
    }

    if (frame == 0) {
      std::printf("[replay] 图片分辨率 %dx%d
", img.cols, img.rows);
      if (img.cols < 1000)
        std::printf(
          "[replay] 警告：rlog 里的图被 remote_logger.img_width 压过（当前 %d 宽）。
"
          "         检测的等效尺度会与实车不同（416/%d vs 416/实车宽度），
"
          "         结论**不能**直接与实车对比。录制评估数据时请把 img_width 设为实车
"
          "         宽度（如 1440），或用 src/eval_record 采全分辨率帧。
",
          img.cols, img.cols);
    }

    FrameStat st;
    st.frame = frame++;
    st.ts = pim->ts;

    // 配最近的一条姿态 JSON（ts ≤ 图片 ts）
    if (!jsons.empty()) {
      auto it = std::upper_bound(
        jsons.begin(), jsons.end(), pim->ts,
        [](uint64_t ts, const JsonRow & r) { return ts < r.ts; });
      if (it != jsons.begin()) {
        const JsonRow & r = *(it - 1);
        st.has_json = true;
        st.json_ts = r.ts;
        st.gimbal_yaw = r.yaw;
        st.gimbal_pitch = r.pitch;
        st.has_obs_dist = r.has_obs_dist;
        st.obs_dist = r.obs_dist;
        if (tracker && r.has_q) {
          Eigen::Quaterniond q(r.qw, r.qx, r.qy, r.qz);
          solver.set_R_gimbal2world(q.normalized());
        }
      }
    }

    auto armors = yolo.detect(img, frame);
    double sum = 0.0;
    auto_aim::Armor best;
    bool has_best = false;
    for (auto & a : armors) {
      solver.solve(a);  // 填 xyz/ypr/ypd；距离与板朝向是旋转不变量
      if (a.confidence < args.min_conf) continue;
      ++st.det_n;
      sum += a.confidence;
      if (!has_best || a.confidence > best.confidence) {
        best = a;
        has_best = true;
      }
    }
    if (has_best) {
      st.best_conf = best.confidence;
      st.max_conf = best.confidence;
      st.mean_conf = st.det_n ? sum / st.det_n : 0.0;
      st.best_name = static_cast<int>(best.name);
      st.best_type = static_cast<int>(best.type);
      st.best_color = static_cast<int>(best.color);
      st.best_dist = best.xyz_in_world.norm();
      st.best_armor_yaw = best.ypr_in_world[0];
      st.best_cx = best.center.x;
      st.best_cy = best.center.y;
    }

    if (tracker) {
      const auto t = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(st.ts));
      auto targets = tracker->track(armors, t);
      if (!targets.empty()) {
        auto & tg = targets.front();
        auto x = tg.ekf_x();
        st.has_ekf = x.size() >= 11;
        if (st.has_ekf) {
          st.a = x[6];
          st.w = x[7];
          st.r = x[8];
          st.l = x[9];
          st.h = x[10];
          st.armor_id = tg.last_id;
          st.nis = tg.ekf().data.count("nis") ? tg.ekf().data.at("nis") : 0.0;
        }
      }
    }

    if (!args.quiet && (st.frame % 50 == 0))
      std::printf(
        "[replay] #%d det=%d maxconf=%.2f dist=%.2f\n", st.frame, st.det_n, st.max_conf, st.best_dist);
    stats.push_back(st);
  }

  std::ofstream out(args.out);
  if (!out) {
    std::printf("[replay] 无法写 %s\n", args.out.c_str());
    return 1;
  }
  out << "frame,ts,json_ts,has_json,gimbal_yaw,gimbal_pitch,det_n,max_conf,mean_conf,"
         "best_conf,best_name,best_type,best_color,best_dist,best_armor_yaw,best_cx,best_cy,"
         "obs_dist,dist_diff";
  if (tracker) out << ",a,w,r,l,h,armor_id,nis";
  out << "\n";
  out.setf(std::ios::fixed);
  int with_det = 0, with_json = 0, with_cmp = 0;
  double sum_conf = 0, sum_dist = 0, sum_diff = 0;
  for (const auto & s : stats) {
    if (s.det_n > 0) {
      ++with_det;
      sum_conf += s.max_conf;
      sum_dist += s.best_dist;
    }
    if (s.has_json) ++with_json;
    if (s.has_json && s.has_obs_dist && s.det_n > 0) {
      ++with_cmp;
      sum_diff += std::abs(s.best_dist - s.obs_dist);
    }
    out << s.frame << ',' << s.ts << ',' << s.json_ts << ',' << (s.has_json ? 1 : 0) << ','
        << std::setprecision(6) << s.gimbal_yaw << ',' << s.gimbal_pitch << ',' << s.det_n << ','
        << s.max_conf << ',' << s.mean_conf << ',' << s.best_conf << ',' << name_of(s.best_name)
        << ',' << type_of(s.best_type) << ',' << color_of(s.best_color) << ',' << s.best_dist << ','
        << s.best_armor_yaw
        << ',' << s.best_cx << ',' << s.best_cy << ',' << (s.has_obs_dist ? s.obs_dist : 0.0) << ','
        << ((s.has_obs_dist && s.det_n > 0) ? std::abs(s.best_dist - s.obs_dist) : 0.0);
    if (tracker)
      out << ',' << s.a << ',' << s.w << ',' << s.r << ',' << s.l << ',' << s.h << ',' << s.armor_id
          << ',' << s.nis;
    out << "\n";
  }
  out.close();

  const int n = static_cast<int>(stats.size());
  std::printf("\n===== 回放汇总 =====\n");
  std::printf("处理帧数          : %d（解码失败 %d）\n", n, decoded_fail);
  std::printf("有检出的帧        : %d (%.1f%%)\n", with_det, n ? 100.0 * with_det / n : 0.0);
  std::printf("配到姿态 JSON 的帧: %d (%.1f%%)\n", with_json, n ? 100.0 * with_json / n : 0.0);
  if (with_det) {
    std::printf("最高置信度均值    : %.3f\n", sum_conf / with_det);
    std::printf("PnP 距离均值      : %.3f m\n", sum_dist / with_det);
  }
  if (with_cmp)
    std::printf("与录制 obs_dist 的|差|均值: %.3f m（%d 帧可比）\n", sum_diff / with_cmp, with_cmp);
  std::printf("CSV               : %s\n", args.out.c_str());
  delete tracker;
  return 0;
}
