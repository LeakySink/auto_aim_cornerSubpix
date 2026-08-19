# RemoteLogger 远程调试日志库

## 概述

`RemoteLogger` 是单例模式的远程调试日志库，位于 `tools/remote_logger.hpp/.cpp`。库自动处理注册、重试、心跳、注销全流程，调用方只需 `init()` → `plot/log/plot_image` → `shutdown()`。

支持四种数据：
- **变量数据**：`plot(nlohmann::json)` — UDP 发送 + 本地 `.rlog` 持久化
- **文本日志**：`log(level, fmt, args...)` — 同时输出终端 stderr + 远程 UDP，支持 fmt 格式
- **图像数据**：`plot_image(cv::Mat, meta)` — worker 内 JPEG 压缩后 UDP 发送，并写入同一次运行的本地 `.rlog`（约 30fps）
- **心跳**：配置 `heartbeat_interval_ms` 后自动发送

## 快速开始

```cpp
#include "tools/remote_logger.hpp"

// 从 yaml 的 remote_logger 段读取配置（推荐）
tools::RemoteLogger::instance().init(config_path);

// 或手动构造 Config（测试程序）
tools::RemoteLogger::Config cfg;
cfg.remote_host = "192.168.1.100";
cfg.sender_name  = "my_robot";
cfg.heartbeat_interval_ms = 500;
tools::RemoteLogger::instance().init(cfg);

// 发送数据（库自动处理注册/重试）
tools::RemoteLogger::instance().plot({{"pitch", 0.15}, {"yaw", -0.3}});
tools::RemoteLogger::instance().log("INFO", "target locked");
tools::RemoteLogger::instance().log("ERROR", "motor {} fail, code={}", 3, 0x1F);
tools::RemoteLogger::instance().plot_image(frame, {{"cam", "front"}});

tools::RemoteLogger::instance().shutdown();  // 自动注销
```

## 配置项

从 yaml 的 `remote_logger` 段读取，缺键则退出：

```yaml
remote_logger:
  remote_host: "127.0.0.1"
  control_port: 15000
  enable_remote: true
  enable_local: true
  log_dir: "./logs"
  var_buffer_size: 1024
  img_buffer_size: 10
  img_width: 640
  img_quality: 50
  heartbeat_interval_ms: 500   # 0=关闭
  sender_name: "sentry"
  register_retry_ms: 3000
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `remote_host` | "127.0.0.1" | 远程主机 IP |
| `control_port` | 15000 | 控制端口（必须，用于注册） |
| `enable_remote` | true | 启用 UDP 发送 |
| `enable_local` | true | 启用本地日志 |
| `log_dir` | "./logs" | 本地日志目录 |
| `var_buffer_size` | 1024 | 变量缓冲条数 |
| `img_buffer_size` | 10 | 图像缓冲帧数 |
| `img_width` | 640 | 图像压缩宽度 |
| `img_quality` | 50 | JPEG 质量 |
| `heartbeat_interval_ms` | 0 | 心跳间隔，0=关闭 |
| `sender_name` | "" | 发送方名称（空=自动 `dev_xxxx`） |
| `register_retry_ms` | 3000 | 注册失败重试间隔 |

## 注册协议

远程发送必须先向控制口注册，数据端口由 host 分配。未注册前不发 UDP。

可视化 Host 端见 [`host/HOST.md`](host/HOST.md)，启动：`./host/watch.sh`。场控见 [`host/FIELD.md`](host/FIELD.md)。本地 `.rlog` 独立预览：[`replayer/README.md`](replayer/README.md)，`./replayer/replay.sh logs/run_xxx.rlog`。

```
Sender ──{"type":"register","name":"my_robot"}──→ Control :control_port
Sender ←──{"type":"register_ack","status":"ok","port":15001}── Control
Sender ──数据──→ port 15001
```

注册失败自动每 `register_retry_ms` 重试，支持 host 后启动。

`shutdown()` 时自动发送注销：
```
Sender ──{"type":"deregister","name":"my_robot"}──→ Control
```

## 发送方标识

所有外发数据自动注入 `_from` 字段：

```json
{"ts": 1234567890, "_from": "my_robot", "pitch": 0.15}
{"ts": 1234567890, "_from": "my_robot", "level": "INFO", "msg": "locked"}
{"hb": 1, "_from": "my_robot", "ts": 1234567890}
```

## 终端日志

`log(level, fmt, args...)` 支持 fmt 格式串，同时输出到终端 stderr 和远程 UDP：

```
14:30:05.123 [INFO] target locked
14:30:05.456 [ERROR] motor 3 fail, code=0x1f
```

## UDP 数据格式

### 变量 / 日志 / 心跳 — JSON

```json
{"ts": 1234567890123456789, "_from": "my_robot", "x": 1.0, "y": 2.0}
{"ts": 1234567890123456789, "_from": "my_robot", "level": "INFO", "msg": "hello"}
{"hb": 1, "_from": "my_robot", "ts": 1234567890123456789}
```

### 图像 — 二进制

```
[1B: 0xFF][8B: ts][4B: meta_len][meta][4B: jpg_len][jpeg]
```

## 本地日志 (`.rlog`)

单次 `init()` → `shutdown()` 写入同一个文件：`log_dir/run_<ts_ns>.rlog`。变量与图像交错追加。JPEG 编码和写盘只在 **worker** 完成；主线程 `plot_image` 仅入队（间隔小于约 33ms 的帧直接丢弃，约 30fps；缓冲满则丢最旧）。

格式 magic `RLG2`：

```
+------------------+
| magic (4B): RLG2 |
+------------------+
| type   (1B)      |  0x00 = json, 0x01 = image
| ts_ns  (8B)      |
| ... payload ...  |
+------------------+

type 0x00 json:
  len (4B) + json (len B)

type 0x01 image:
  meta_len (4B) + meta JSON + jpg_len (4B) + jpeg bytes
```

本地独立预览（预加载内存，左图右曲线 + 进度条）：

```bash
./replayer/replay.sh logs/run_<ts_ns>.rlog
```

## 线程模型

```
主线程                         worker 线程
──────                         ──────────
init() ──start──→              try_register() (重试)
plot() ──push──→ var_buf_      swap → 写本地 .rlog (json)
log()  ──push──→ var_buf_      swap → UDP 发送
         ──print──→ stderr     每轮 1 帧 → 压缩 → 写本地 / UDP
img()  ──push──→ img_buf_      heartbeat
shutdown() ──stop──→           刷剩余 + deregister
```
