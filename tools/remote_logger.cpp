#include "remote_logger.hpp"

#include "rdbg/engine.hpp"
#include "rdbg/tf_auto_pub.hpp"

#include "tools/yaml.hpp"

namespace tools
{

struct RemoteLogger::Impl : rdbg::Engine
{
};

RemoteLogger::RemoteLogger() : impl_(std::make_unique<Impl>()) {}

RemoteLogger::~RemoteLogger() { shutdown(); }

RemoteLogger & RemoteLogger::instance()
{
  static RemoteLogger inst;
  return inst;
}

void RemoteLogger::init(const Config & cfg) { impl_->init(cfg); }

void RemoteLogger::init(const std::string & config_path)
{
  auto yaml = tools::load(config_path);
  auto node = yaml["remote_logger"];
  if (!node) {
    log("ERROR", "[YAML] remote_logger not found!");
    exit(1);
  }

  Config cfg;
  cfg.control_port = tools::read<uint16_t>(node, "control_port");
  cfg.enable_remote = tools::read<bool>(node, "enable_remote");
  cfg.enable_local = tools::read<bool>(node, "enable_local");
  cfg.log_dir = tools::read<std::string>(node, "log_dir");
  cfg.var_buffer_size = tools::read<size_t>(node, "var_buffer_size");
  cfg.img_buffer_size = tools::read<size_t>(node, "img_buffer_size");
  cfg.img_width = tools::read<int>(node, "img_width");
  cfg.img_quality = tools::read<int>(node, "img_quality");
  cfg.heartbeat_interval_ms = tools::read<uint32_t>(node, "heartbeat_interval_ms");
  cfg.sender_name = tools::read<std::string>(node, "sender_name");
  if (node["app"]) cfg.app = node["app"].as<std::string>();
  if (node["beacon_interval_ms"])
    cfg.beacon_interval_ms = node["beacon_interval_ms"].as<uint32_t>();
  if (node["head_timeout_ms"])
    cfg.head_timeout_ms = node["head_timeout_ms"].as<uint32_t>();
  if (node["beacon_port"])
    cfg.beacon_port = node["beacon_port"].as<uint16_t>();

  init(cfg);

  // 外参只发一次；姿态由 io::Gimbal::q 持续发布
  const std::string resolved = tools::project_path(config_path);
  tools::rdbg::publish_tf_extrinsics_once(yaml, resolved);
}

void RemoteLogger::plot(const nlohmann::json & data) { impl_->plot(data); }

void RemoteLogger::log(const std::string & level, const std::string & msg)
{
  impl_->log(level, msg);
}

void RemoteLogger::plot_image(const cv::Mat & img, const nlohmann::json & meta)
{
  impl_->plot_image(img, meta);
}

bool RemoteLogger::poll_calib_cmd(std::string & cmd) { return impl_->poll_calib_cmd(cmd); }

bool RemoteLogger::poll_json(nlohmann::json & data) { return impl_->poll_json(data); }

void RemoteLogger::apply_tx_cap(int max_width, int max_quality, int max_fps,
                                int max_level)
{
  rdbg::TxCap cap;
  cap.max_width = max_width;
  cap.max_quality = max_quality;
  cap.max_fps = max_fps;
  cap.max_level = max_level;
  impl_->apply_tx_cap(cap);
}

void RemoteLogger::shutdown() { impl_->shutdown(); }

}  // namespace tools
