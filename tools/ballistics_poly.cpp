#include "ballistics_poly.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace tools
{
namespace
{
std::shared_ptr<const BallisticsPoly> g_model;

std::mutex g_log_mutex;
std::function<void(const std::string &)> g_logger;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_time_by_category;

std::string num(double v)
{
  std::ostringstream oss;
  oss.precision(4);
  oss << v;
  return oss.str();
}

std::string trim(const std::string & s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool parse_double(const std::string & s, double & out)
{
  const auto t = trim(s);
  if (t.empty()) return false;
  try {
    std::size_t used = 0;
    out = std::stod(t, &used);
    if (used != t.size()) return false;  // 只接受整段都是数字（避免把表头当数据）
    return true;
  } catch (...) {
    return false;
  }
}

/// 按逗号切分 CSV 行；引号内的逗号不算分隔符（拟合表后两列是 "[...]"），引号本身被去掉。
std::vector<std::string> split_csv_line(const std::string & line)
{
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;
  for (const char ch : line) {
    if (ch == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (ch == ',' && !in_quotes) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(ch);
  }
  out.push_back(cur);
  return out;
}

/// "[a, b, c, ...]" -> 10 个系数
bool parse_coef_list(const std::string & field, std::array<double, BallisticsPoly::kCoefNum> & out,
                     std::string * err)
{
  std::string s = field;
  for (char & ch : s) {
    if (ch == '[' || ch == ']') ch = ' ';
  }
  std::vector<double> vals;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
    if (i >= s.size()) break;
    std::size_t j = i;
    while (j < s.size() && s[j] != ',') ++j;
    const auto token = trim(s.substr(i, j - i));
    i = j;
    if (token.empty()) continue;
    double v = 0.0;
    if (!parse_double(token, v)) {
      if (err) *err = "无法解析系数 '" + token + "'";
      return false;
    }
    vals.push_back(v);
  }
  if (vals.size() != out.size()) {
    if (err)
      *err = "系数个数不对：期望 " + std::to_string(out.size()) + "，实际 " + std::to_string(vals.size());
    return false;
  }
  std::copy(vals.begin(), vals.end(), out.begin());
  return true;
}
}  // namespace

bool BallisticsPoly::load_csv(const std::string & path, std::string * err)
{
  std::ifstream ifs(path);
  if (!ifs) {
    if (err) *err = "打不开文件 " + path;
    return false;
  }

  std::vector<BallisticsRow> rows;
  std::string line;
  int lineno = 0;
  int bad_lines = 0;
  std::string first_err;
  while (std::getline(ifs, line)) {
    ++lineno;
    const auto fields = split_csv_line(line);
    if (fields.size() < 3) continue;

    BallisticsRow row;
    if (!parse_double(fields[0], row.v)) continue;  // 表头 / 空行 / 注释行

    std::string e;
    if (!parse_coef_list(fields[1], row.pitch, &e)) {
      if (first_err.empty()) first_err = "第 " + std::to_string(lineno) + " 行 pitch: " + e;
      ++bad_lines;
      continue;
    }
    if (!parse_coef_list(fields[2], row.time, &e)) {
      if (first_err.empty()) first_err = "第 " + std::to_string(lineno) + " 行 time: " + e;
      ++bad_lines;
      continue;
    }
    rows.push_back(row);
  }

  if (rows.empty()) {
    if (err) *err = "没有解析到任何有效行" + (first_err.empty() ? std::string() : ("（" + first_err + "）"));
    return false;
  }
  return load_rows(std::move(rows), err);
}

bool BallisticsPoly::load_rows(std::vector<BallisticsRow> rows, std::string * err)
{
  if (rows.empty()) {
    if (err) *err = "行数为 0";
    return false;
  }
  std::sort(rows.begin(), rows.end(), [](const BallisticsRow & a, const BallisticsRow & b) {
    return a.v < b.v;
  });
  for (const auto & r : rows) {
    if (!std::isfinite(r.v) || r.v <= 0.0) {
      if (err) *err = "存在非法弹速 " + num(r.v);
      return false;
    }
  }
  if (err) *err = "ok";
  rows_ = std::move(rows);
  return true;
}

void BallisticsPoly::set_range(double x_min, double x_max, double y_min, double y_max)
{
  x_min_ = x_min;
  x_max_ = x_max;
  y_min_ = y_min;
  y_max_ = y_max;
}

double BallisticsPoly::eval(const std::array<double, kCoefNum> & c, double x, double y)
{
  // 与 ballistic-main/test.py 的 predict() 逐项一致
  return c[0] + c[1] * x + c[2] * y + c[3] * x * x + c[4] * x * y + c[5] * y * y +
         c[6] * x * x * x + c[7] * x * x * y + c[8] * x * y * y + c[9] * y * y * y;
}

bool BallisticsPoly::solve(
  double v0, double d, double h, double & pitch_rad, double & fly_time, std::string * note) const
{
  if (rows_.empty()) {
    if (note) *note = "拟合表为空";
    return false;
  }
  if (!std::isfinite(v0) || !std::isfinite(d) || !std::isfinite(h)) {
    if (note) *note = "入参含非有限值";
    return false;
  }
  if (d < x_min_ || d > x_max_ || h < y_min_ || h > y_max_) {
    if (note)
      *note = "超出拟合范围：d=" + num(d) + "∈[" + num(x_min_) + "," + num(x_max_) + "] h=" + num(h) +
              "∈[" + num(y_min_) + "," + num(y_max_) + "]";
    return false;
  }

  std::array<double, kCoefNum> pc{}, tc{};
  if (v0 <= rows_.front().v) {
    pc = rows_.front().pitch;
    tc = rows_.front().time;
    if (note) *note = "弹速低于表下限，按 V=" + num(rows_.front().v) + " 夹取";
  } else if (v0 >= rows_.back().v) {
    pc = rows_.back().pitch;
    tc = rows_.back().time;
    if (note) *note = "弹速高于表上限，按 V=" + num(rows_.back().v) + " 夹取";
  } else {
    std::size_t hi = 1;
    while (hi < rows_.size() && rows_[hi].v < v0) ++hi;
    if (hi >= rows_.size()) hi = rows_.size() - 1;
    const auto & lo = rows_[hi - 1];
    const auto & up = rows_[hi];
    const double w = (v0 - lo.v) / (up.v - lo.v);  // 线性插值权重
    for (std::size_t k = 0; k < kCoefNum; ++k) {
      pc[k] = lo.pitch[k] + w * (up.pitch[k] - lo.pitch[k]);
      tc[k] = lo.time[k] + w * (up.time[k] - lo.time[k]);
    }
    if (note) *note = "按弹速插值：V=" + num(v0) + " ∈ [" + num(lo.v) + "," + num(up.v) + "]";
  }

  const double pitch_deg = eval(pc, d, h);  // 单位：度（抬头为正）
  const double t = eval(tc, d, h);          // 单位：s
  if (!std::isfinite(pitch_deg) || !std::isfinite(t) || t <= 0.0) {
    if (note) *note = "多项式求值结果非法：pitch=" + num(pitch_deg) + "deg t=" + num(t) + "s";
    return false;
  }

  pitch_rad = pitch_deg * kDeg2Rad;
  fly_time = t;
  return true;
}

void install_ballistics_model(std::shared_ptr<const BallisticsPoly> model)
{
  g_model = std::move(model);
}

std::shared_ptr<const BallisticsPoly> ballistics_model() { return g_model; }

void set_ballistics_logger(std::function<void(const std::string &)> logger)
{
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_logger = std::move(logger);
}

void ballistics_log(const std::string & category, const std::string & msg)
{
  std::lock_guard<std::mutex> lock(g_log_mutex);
  const auto now = std::chrono::steady_clock::now();
  // 按"类别"限流：回退日志里的 d/h 数值每帧都在变，按整条消息去重等于没限流
  auto it = g_last_time_by_category.find(category);
  if (it != g_last_time_by_category.end() && now - it->second < std::chrono::seconds(2)) return;
  g_last_time_by_category[category] = now;
  if (g_logger) g_logger("[" + category + "] " + msg);
}

}  // namespace tools
