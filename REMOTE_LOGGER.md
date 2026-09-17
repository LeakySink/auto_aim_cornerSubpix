# RemoteLogger 远程调试日志库

## 概述

`RemoteLogger` 是单例模式的远程调试日志库。调用方只 include [`tools/remote_logger.hpp`](tools/remote_logger.hpp)：`init()` → `plot/log/plot_image` → `shutdown()`。实现在 [`tools/rdbg/`](tools/rdbg/)，按层拆开：

```
plot / log / plot_image     对外 API（tools/remote_logger.hpp）
        │
        ▼
   rdbg/engine          入队 + 三条 worker
        ├─ rdbg/session     本地 .rlog（RLG2）
        ├─ rdbg/data        数据 UDP → 仅队首
        ├─ rdbg/control     beacon + host 队列
        └─ rdbg/transport   UDP bind / sendto / recv
```

host 来排队；只向队首发 UDP。控制协议见 [`host/PROTOCOL.md`](host/PROTOCOL.md)。

支持四种数据：
- **变量数据**：`plot(nlohmann::json)` — UDP 发送 + 本地 `.rlog` 持久化
- **文本日志**：`log(level, fmt, args...)` — 同时输出终端 stderr + 远程 UDP，支持 fmt 格式
- **图像数据**：`plot_image(cv::Mat, meta)` — 主线程按采集时间相位锁定选 ~30fps（未入选直接 return）；worker resize 后 JPEG；`.rlog` 照写；**UDP 仅在 host `img_subscribe` 了该 `meta.name` 时**，以 ≤1200B 的 `0xFE` 分片发给队首
- **心跳**：配置 `heartbeat_interval_ms` 后自动发送

## 快速开始

```cpp
#include "tools/remote_logger.hpp"

// 从 yaml 的 remote_logger 段读取配置（推荐）
tools::RemoteLogger::instance().init(config_path);

// 或手动构造 Config（测试程序）
tools::RemoteLogger::Config cfg;
cfg.sender_name  = "my_robot";
cfg.heartbeat_interval_ms = 500;
tools::RemoteLogger::instance().init(cfg);

// 发送数据（host 向车注册后才会出现在队首的 UDP 里）
tools::RemoteLogger::instance().plot({{"pitch", 0.15}, {"yaw", -0.3}});
tools::RemoteLogger::instance().log("INFO", "target locked");
tools::RemoteLogger::instance().log("ERROR", "motor {} fail, code={}", 3, 0x1F);
tools::RemoteLogger::instance().plot_image(frame, {{"name", "front"}});

tools::RemoteLogger::instance().shutdown();  // 自动注销
```

## 配置项

从 yaml 的 `remote_logger` 段读取，缺键则退出：

```yaml
remote_logger:
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
  beacon_interval_ms: 1000     # 缺省 1000；连上后也一直发
  head_timeout_ms: 2000        # 缺省 2000；队首失联出队
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `control_port` | 15000 | 车 bind 的控制口，host 来 register |
| `enable_remote` | true | 启用 UDP / beacon |
| `enable_local` | true | 启用本地日志 |
| `log_dir` | "./logs" | 本地日志目录 |
| `var_buffer_size` | 1024 | 变量缓冲条数 |
| `img_buffer_size` | 10 | yaml 兼容保留，图像侧不再使用深队列 |
| `img_width` | 640 | 图像压缩宽度 |
| `img_quality` | 50 | JPEG 质量 |
| `heartbeat_interval_ms` | 0 | 向队首发 `hb` 的间隔，0=关闭 |
| `sender_name` | "" | 发送方名称（空=自动 `dev_xxxx`），多车必须唯一 |
| `beacon_interval_ms` | 1000 | LAN 发现广播间隔，已连接也不停 |
| `head_timeout_ms` | 2000 | 队首无 `head_alive` 则出队 |

旧键 `remote_host` / `register_retry_ms` 若仍写在 yaml 里会被忽略。

## 注册协议

host 向车注册，不是车向 host。详细报文见 [`host/PROTOCOL.md`](host/PROTOCOL.md)。

```
车 --beacon--> 255.255.255.255:15999     （enable_remote 期间一直发）
Host --register--> 车:15000
车 --register_ack role=head|follower--> Host:15100
车 --数据/hb--> 仅队首 :15001
队首 --原样 UDP--> 后入 Host :15001
```

未入队（队列空）时不发数据 UDP。`.rlog` 照写。可视化：`./host/watch.sh` 实时，`./host/replay.sh` 回放。

`shutdown()` 停 beacon；host 约 3s 听不到该 `name` 即标离线。车上不向 host 发 `deregister`。

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

### 图像 — UDP 分片（`0xFE`）

```
公共头: [1B 0xFE][8B ts][2B seq][2B idx][2B cnt][1B from_len][from]
frag0:  [2B meta_len][meta][4B jpg_total][chunk]
frag n: [chunk]
```

每片总长 ≤1200B，避免 WiFi IP 分片。未订阅的流不发 UDP。另有约 1s 一次的目录 JSON：`{"img_streams":[...],"_from":...}`。

## 本地日志 (`.rlog`)

单次 `init()` → `shutdown()` 写入同一个文件：`log_dir/run_<ts_ns>.rlog`。变量与图像交错追加。

`plot_image` 按 `meta.name` 分路，每路独立对齐到 30Hz 网格。未入选的帧只比较时间戳后返回。入选帧 **clone 像素** 后按 name 放入深度 1 邮箱（每路在飞 1 张 + 等待最新 1 张），调用方可立即复用/改写原 `Mat`。`img_worker` 轮询各路，resize 到 `img_width` 后立刻 `release` 全分辨率，再 JPEG；`enable_local` 时写盘；仅当该流被 host 订阅时再分片 UDP。未订阅且关闭本地时，主线程不 clone。JPEG 跟不上时只覆盖该路等待槽。

回放：`./host/replay.sh logs/run_<ts_ns>.rlog`（与 `./host/watch.sh` 独立，不占用控制口）。

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

## 线程模型

三条后台路径；本地会话与线上控制/数据解耦。

```
主线程                         img_worker             var_worker        ctrl_worker
──────                         ──────────             ──────────        ───────────
plot_image
  按 name 未到 30Hz → return
  入选 → clone → mailbox(1) ──► JPEG
                               session + data
plot/log → var_buf  ──────────────────────────────► session + data（仅队首）
shutdown → join                                     JSON              transport+control
```

| 线程 | 职责 | 唤醒 |
|------|------|------|
| `var_worker` | JSON → `session` + `data` | `plot()` notify；空闲 poll 50ms |
| `img_worker` | 邮箱 → JPEG → `session` + `data` | 入选帧 publish；空闲 poll 50ms |
| `ctrl_worker` | `control`：beacon、队列、队首超时 | 独立轮询 200ms（`enable_remote` 时启动） |

150fps+ 热路径：未入选帧无拷贝。入选帧 clone 后入邮箱，避免异步 JPEG 读到被覆盖的像素。每路保存间隔为 `33.3ms ± 一帧相机周期`。不同 `meta.name` 互不影响。
