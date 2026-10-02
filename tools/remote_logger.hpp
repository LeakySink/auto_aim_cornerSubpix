#ifndef TOOLS__REMOTE_LOGGER_HPP
#define TOOLS__REMOTE_LOGGER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace tools
{

/// 车上远程调试日志（单例）。调用方只 include 本头文件。
///
/// 生命周期：`init` → `plot` / `log` / `plot_image` /（可选）`set_json_callback` → `shutdown`。
/// 实现分层在 `tools/rdbg/`：engine（入队+worker）/ session（.rlog）/
/// data（UDP→队首）/ control（beacon+host 队列）/ transport。
///
/// 行为要点：
/// - Host 向车 `register`；**仅队首**收 plot/log/image/hb
/// - `plot` JSON 可嵌套 `markers`（Watch 3D marker_v1）、`tf` / `frames`
/// - `plot_image` 按 `meta.name` ~30fps 选帧；UDP 仅当 Host `img_subscribe` 该流
/// - 落盘优先级：image(0) < normal(1) < WARN(2) < ERROR(3)；队列满丢低优先级
/// - Host 下行优先 `set_json_callback`；`poll_json` / `poll_calib_cmd` 仍可用
/// - 配置见 yaml `remote_logger`；文档 `REMOTE_LOGGER.md` / 门户 Help `#vehicle`
class RemoteLogger
{
public:
  struct Config
  {
    uint16_t control_port = 15000;   ///< 车控制口，Host register 目标
    uint16_t beacon_port = 15999;    ///< LAN 发现（与 Host 发现口一致）
    bool enable_remote = true;       ///< beacon + 数据 UDP
    bool enable_local = true;        ///< 写 `log_dir/run_*.rlog`
    std::string log_dir = "./logs";
    size_t var_buffer_size = 1024;   ///< plot/log 环形缓冲条数
    size_t img_buffer_size = 10;     ///< yaml 兼容保留；图像侧用深 1 邮箱
    int img_width = 640;             ///< JPEG 前缩放宽度
    int img_quality = 50;            ///< JPEG 质量
    uint32_t heartbeat_interval_ms = 0;  ///< 0=关闭向队首 hb
    std::string sender_name;         ///< 多车唯一；空则自动 `dev_xxxx`
    /// beacon 身份：normal=调试，calibrate=标定，tfviz=TF Viz（门户按此开页）
    std::string app = "normal";
    uint32_t beacon_interval_ms = 1000;  ///< 连上后也持续广播
    uint32_t head_timeout_ms = 2000;     ///< 队首无 head_alive 则出队
  };

  static RemoteLogger & instance();

  /// 从内存配置启动 worker（测试程序常用）。
  void init(const Config & cfg);
  /// 从 yaml 的 `remote_logger` 段启动；缺键则报错退出。
  void init(const std::string & config_path);

  /// 变量/嵌套 JSON → 队首 UDP + 可选 .rlog。自动注入 `ts`、`_from`。
  void plot(const nlohmann::json & data);

  /// 文本日志：stderr + 远程（转成带 level/msg 的 plot）。
  void log(const std::string & level, const std::string & msg);

  template <typename... Args>
  void log(const std::string & level, const std::string & fmt_str, Args &&... args)
  {
    log(level, fmt::format(fmt::runtime(fmt_str), std::forward<Args>(args)...));
  }

  /// 图像：按 meta["name"] 选帧；UDP 0xFE 分片仅订阅流；本地可写 .rlog。
  void plot_image(const cv::Mat & img, const nlohmann::json & meta);

  /// Host 网页下发的标定指令（旧协议，保留兼容）。
  bool poll_calib_cmd(std::string & cmd);
  /// Host 下发的通用 JSON（控制口 `type=json` 的 `data`）；仍可轮询。
  bool poll_json(nlohmann::json & data);
  /// Host `type=json` 到达时回调（控制线程、锁外）。空 function 取消。优先于每帧 poll。
  void set_json_callback(std::function<void(const nlohmann::json &)> cb);

  /// 远程 UDP JPEG 上限（与 img_subscribe / set_img_tx 同语义；max_level≥0 表示至少降到该档）。
  /// 只影响 UDP 编码与 img_tx_level 自适应；本地 .rlog 仍用 yaml img_width/img_quality。
  void apply_tx_cap(int max_width, int max_quality, int max_fps, int max_level = -1);

  /// 停 beacon、冲刷落盘、释放资源。
  void shutdown();

private:
  RemoteLogger();
  ~RemoteLogger();
  RemoteLogger(const RemoteLogger &) = delete;
  RemoteLogger & operator=(const RemoteLogger &) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tools

#endif  // TOOLS__REMOTE_LOGGER_HPP
