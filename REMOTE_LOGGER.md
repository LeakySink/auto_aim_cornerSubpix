# RemoteLogger 远程调试日志库

## 概述

`RemoteLogger` 是单例模式的远程调试日志库，位于 `tools/remote_logger.hpp/.cpp`。库自动处理注册、重试、心跳、注销全流程，调用方只需 `init()` → `plot/log/plot_image` → `shutdown()`。

支持四种数据：
- **变量数据**：`plot(nlohmann::json)` — UDP 发送 + 本地 `.rlog` 持久化
- **文本日志**：`log(level, fmt, args...)` — 同时输出终端 stderr + 远程 UDP，支持 fmt 格式
- **图像数据**：`plot_image(cv::Mat, meta)` — JPEG 压缩后 UDP 发送
- **心跳**：配置 `heartbeat_interval_ms` 后自动发送

## 快速开始

```cpp
#include "tools/remote_logger.hpp"

tools::RemoteLogger::Config cfg;
cfg.remote_host = "192.168.1.100";  // 远程主机 IP
cfg.sender_name  = "my_robot";      // 发送方名称（空=自动）
cfg.heartbeat_interval_ms = 500;           // 心跳间隔，0=关闭

tools::RemoteLogger::instance().init(cfg);

// 发送数据（库自动处理注册/重试）
tools::RemoteLogger::instance().plot({{"pitch", 0.15}, {"yaw", -0.3}});
tools::RemoteLogger::instance().log("INFO", "target locked");
tools::RemoteLogger::instance().log("ERROR", "motor {} fail, code={}", 3, 0x1F);
tools::RemoteLogger::instance().plot_image(frame, {{"cam", "front"}});

tools::RemoteLogger::instance().shutdown();  // 自动注销
```

## 配置项

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `remote_host` | "127.0.0.1" | 远程主机 IP |
| `remote_port` | 9871 | 数据端口（注册后被 host 分配覆盖） |
| `control_port` | 15000 | 控制端口 |
| `register_retry_ms` | 3000 | 注册失败重试间隔 |
| `sender_name` | "" | 发送方名称（空=自动 `dev_xxxx`） |
| `heartbeat_interval_ms` | 0 | 心跳间隔，0=关闭 |
| `log_dir` | "./logs" | 本地日志目录 |
| `img_width` | 640 | 图像压缩宽度 |
| `img_quality` | 50 | JPEG 质量 |
| `enable_remote` | true | 启用 UDP 发送 |
| `enable_local` | true | 启用本地日志 |

## 注册协议

设置 `control_port > 0` 后，库自动在 worker 线程中执行注册：

```
Sender ──{"type":"register","name":"my_robot"}──→ Control (upd_port)
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

```
+------------------+
| magic (4B): RLOG |
+------------------+
| ts_ns  (8B)      |
| len    (4B)      |
| json   (len B)   |
+------------------+
```

## 线程模型

```
主线程                         worker 线程
──────                         ──────────
init() ──start──→              try_register() (重试)
plot() ──push──→ var_buf_      swap → 写本地 .rlog
log()  ──push──→ var_buf_      swap → UDP 发送
         ──print──→ stderr     swap img → 压缩 → UDP
img()  ──push──→ img_buf_      heartbeat
shutdown() ──stop──→           deregister
```
