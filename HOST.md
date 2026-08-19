# Remote Debugger — Host 端文档

## 概述

Host 端是远程调试系统的接收与可视化端，运行在调试 PC 上（Linux / macOS / Windows，只需 Python 3.8+，无第三方包）。

| 模块 | 文件 | 职责 |
|------|------|------|
| **控制协议** | `host/control.py` | UDP 注册/注销协议，发件方注册表，端口分配 |
| **UDP 接收** | `host/udp_rx.py` | 进程内解析 plot / log / image，输出 JSON 行 |
| **静态资源** | `host/assets.py` | Chart.js 等本地 JS，缺失时 302 到 CDN |
| **HTTP/SSE 服务器** | `host/server.py` | 前端页面 + SSE 推送 + 状态轮询 |

## 架构

```
                    UDP (注册)           UDP (数据)
发送端 ──────────────────→ control.py ───→ udp_rx.py (线程)
  │                            │                    │
  │                            │ poller 轮询        │ on_output
  │                            ↓                    ↓
  │                        server.py ←──────── sse_queue
  │                            │
  │                     SSE (state + data)
  │                            ↓
  │                         浏览器
  └── RemoteLogger API
```

**关键设计原则：**
- 纯 Python 标准库，Mac 上直接 `python3 host/server.py` 即可
- 前端 JS 不包含任何业务决策，只做渲染
- 状态统一通过 `{"type":"state",...}` SSE 消息推送
- poller 线程轮询 `control.version()`，检测发件方变化后自动切换监听端口

## 快速开始

```bash
./host/watch.sh
# 或
python3 host/server.py --port 8080 --control-port 15000
```

浏览器打开 `http://localhost:8080`。

离线使用（把 Hammer / zoom 插件也下载到本地）：

```bash
python3 host/server.py --download-assets
```

## 模块详解

### control.py — 控制协议

UDP 端口 15000，JSON 协议。

**注册：**
```
Sender → Control:  {"type":"register","name":"robot_alpha"}
Control → Sender: {"type":"register_ack","status":"ok","port":15001}
```

**注销：**
```
Sender → Control:  {"type":"deregister","name":"robot_alpha"}
Control → Sender: {"type":"deregister_ack","status":"ok"}
```

**Host 关闭：**
```
Control → All Senders: {"type":"host_shutdown"}
```

接口：
- `version()` — 每次注册/注销递增，用于 poller 检测变化
- `get_senders()` — 当前已注册发件方列表
- `get_sender_info(name)` — 发件方的 `{addr, data_port}`

### udp_rx.py — UDP 接收

进程内绑定数据端口，解析与车上 `RemoteLogger` 一致的 UDP 包，回调 JSON 行（格式与原先 C++ `udp_backend` stdout 相同）。

```python
rx = UdpBackend()
rx.on_output = lambda line: sse_queue.put(line)
rx.on_state = lambda: push_state()

rx.start(15001, "robot_alpha")   # 在端口 15001 监听
rx.switch("robot_beta", 15002)   # 停当前，换端口
rx.stop()
```

属性：`active_sender`, `active_port`, `running`

### server.py — 编排

```
poller 线程 (0.5s 循环):
  if control.version() changed:
      if new senders and not udp_backend.running:
          udp_backend.switch(first_sender, port)
      push_state() → SSE

HTTP API:
  GET /select?sender=name  →  do_select(name) → udp_backend.switch()
  GET /events               →  SSE 流
  GET /                     →  HTML 页面
  GET /chart.js 等          →  本地 JS 或 302 CDN
```

### 前端 JavaScript

**不包含业务逻辑。** 前端职责仅限于：
- 接收 SSE `state` 消息 → 更新下拉框选项和选中值
- 接收 SSE `data` (plot/image/log) → 渲染图表/图像/日志
- 用户选择下拉框 → HTTP `GET /select?sender=X` → 服务端处理

**SSE 消息类型：**

| type | 说明 |
|------|------|
| `state` | 状态快照 `{active_sender, senders: [...]}` |
| `plot` | 变量数据 |
| `image` | 图像数据 (base64 JPEG) |
| `log` | 日志消息 |
| `status` | 连接状态 `{connected, sender}` |

## server.py 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 8080 | HTTP 端口 |
| `--control-port` | 15000 | 控制端口 |
| `--download-assets` | off | 下载前端 JS 到 `host/` 供离线使用 |

## 使用示例

```bash
# 启动 host（Mac / Linux / Windows）
python3 host/server.py

# 单发送端测试（车上或同机）
./build/remote_logger_test --host=127.0.0.1 --ctrl-port=15000 --name=mybot

# 多发送端测试
./build/multi_sender_test --ctrl-port=15000
```
