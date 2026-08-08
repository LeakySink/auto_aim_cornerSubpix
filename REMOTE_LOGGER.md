# RemoteLogger 远程调试日志库

## 概述

`RemoteLogger` 是一个单例模式的远程调试日志库，位于 `tools/remote_logger.hpp/.cpp`。

支持三种数据：
- **变量数据**（JSON）：`plot()` — 本地二进制持久化 + UDP 实时发送
- **文本日志**：`log()` — 以 JSON 形式通过同一管道发送和存储
- **图像数据**（cv::Mat）：`plot_image()` — 自动压缩后 UDP 发送

所有 I/O 操作（写文件、网络发送）均在独立后台线程中完成。

## 快速开始

```cpp
#include "tools/remote_logger.hpp"

tools::RemoteLogger::Config cfg;
cfg.remote_host = "192.168.1.100";     // 远程调试主机 IP
cfg.remote_port = 9871;                 // 数据端口（注册后自动更新）
cfg.control_port = 15000;               // 控制端口，0=跳过注册
cfg.sender_name = "robot_1";            // 发送方名称（空=自动生成 dev_xxxx）
cfg.heartbeat_interval_ms = 500;        // 心跳间隔，0=关闭
cfg.log_dir = "./logs";
cfg.img_width = 640;
cfg.img_quality = 50;

tools::RemoteLogger::instance().init(cfg);

// 记录变量
tools::RemoteLogger::instance().plot({{"pitch", 0.15}, {"yaw", -0.3}});

// 文本日志
tools::RemoteLogger::instance().log("INFO", "gimbal initialized");

// 记录图像
tools::RemoteLogger::instance().plot_image(frame, {{"cam", "front"}});

// 程序退出前关闭（自动执行注销）
tools::RemoteLogger::instance().shutdown();
```

## 配置项说明

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `remote_host` | "127.0.0.1" | 远程接收端 IP |
| `remote_port` | 9871 | 数据端口（注册后由 host 分配覆盖） |
| `control_port` | 0 | 控制端口，0=不使用注册协议 |
| `sender_name` | "" | 发送方名称，空则自动生成 `dev_xxxx` |
| `heartbeat_interval_ms` | 0 | 心跳间隔，0=关闭 |
| `log_dir` | "./logs" | 本地日志目录 |
| `var_buffer_size` | 1024 | 变量缓冲区大小 |
| `img_buffer_size` | 10 | 图像缓冲区最大数量 |
| `img_width` | 640 | 压缩图像目标宽度 |
| `img_quality` | 50 | JPEG 压缩质量 |
| `enable_remote` | true | 是否启用 UDP 远程发送 |
| `enable_local` | true | 是否启用本地二进制日志 |

## 注册协议

当 `control_port > 0` 时，`init()` 启动前自动执行注册流程：

```
发送端                               控制服务器 (port 15000)
  │                                       │
  │  {"type":"register","name":"robot"} ──→│
  │                                       │ 分配数据端口
  │  ←── {"type":"register_ack",          │
  │        "status":"ok","port":15001}     │
  │                                       │
  │  向 port 15001 发送数据 ──────────────→│
  │                                       │
  ⋮  (运行中)                              ⋮
  │                                       │
  │  {"type":"deregister","name":"robot"} →│  (shutdown 时)
  │                                       │ 释放端口
```

## 心跳

当 `heartbeat_interval_ms > 0` 时，工作线程按周期发送：

```json
{"hb": 1, "_from": "robot_1"}
```

主机端在 `timeout` 时间无数据（含心跳）则判定断连。

## 发送方标识

所有外发数据自动注入 `_from` 字段：

```json
{"_from": "robot_1", "ts": 1234567890, "pitch": 0.15, "yaw": -0.3}
```

图像包的 meta 中也包含 `_from`。

## 时间戳处理

- 若 JSON 中包含 `"ts"` 字段（number 类型），使用用户提供的时间戳
- 否则自动赋值为当前系统时间（`system_clock`，纳秒级 epoch）

```cpp
logger.plot({{"x", 1.0}});                    // 自动时间戳
logger.plot({{"ts", 1234567890123456789ULL}, {"x", 1.0}});  // 手动时间戳
```

## 本地日志格式

变量数据存入 `{log_dir}/var_{session_ns}.rlog`：

```
+------------------+
| magic (4B): RLOG |  0x524C4F47
+------------------+
| entry 1          |
|   ts_ns  (8B)    |
|   len    (4B)    |
|   json   (len B) |
+------------------+
| entry 2 ...      |
+------------------+
```

## UDP 数据协议

### 变量 / 日志 / 心跳

带 `ts` 和 `_from` 的 JSON 字符串：

```json
{"ts": 1234567890123456789, "_from": "robot_1", "x": 1.0, "y": 2.0}
```

日志：
```json
{"ts": 1234567890123456789, "_from": "robot_1", "level": "INFO", "msg": "target locked"}
```

心跳：
```json
{"hb": 1, "_from": "robot_1"}
```

### 图像数据包

```
[ 1B: 0xFF ]            // 图像包标识
[ 8B: ts_ns ]           // 时间戳
[ 4B: meta_len ]        // meta JSON 长度
[ meta_len B: meta ]    // meta JSON（含 _from、name 等）
[ 4B: jpg_len ]         // JPEG 数据长度
[ jpg_len B: jpeg ]     // JPEG 压缩数据
```

## 线程模型

```
主线程                     后台工作线程
──────                     ────────────
plot() ──push──> var_buf_    worker():
                   │          ├─ wait(200ms) / notify
plot_image()       │          ├─ swap var_buf_ → 写本地文件
   ──push──> img_buf_         ├─ swap var_buf_ → UDP 发送
                  │          ├─ swap img_buf_ → 压缩 → UDP 发送
                  └──notify──> ├─ 心跳检测 → send_heartbeat()
                               └─ loop
```

## 注意事项

1. `shutdown()` 务必在主线程结束后调用，确保缓冲区数据刷盘和注销
2. 单帧 JPEG 超过 60KB 时跳过发送
3. Socket 创建失败时自动禁用远程发送，不影响本地日志
4. 图像零拷贝：`cv::Mat` 通过内部引用计数传递
5. `control_port > 0` 时，`remote_port` 会被 host 分配的值覆盖
