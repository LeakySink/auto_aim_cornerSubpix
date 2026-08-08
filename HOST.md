# Remote Debugger — Host 端文档

## 概述

Host 端是远程调试系统的接收与可视化端，运行在调试 PC 上。

| 模块 | 文件 | 职责 |
|------|------|------|
| **控制协议** | `host/control.py` | UDP 注册/注销协议，发件方注册表，端口分配 |
| **后端管理器** | `host/backend_mgr.py` | `udp_backend` 进程生命周期管理 |
| **HTTP/SSE 服务器** | `host/server.py` | 前端页面 + SSE 推送 + 状态轮询 |
| **数据后端** | `host/src/` → `udp_backend` | C++ UDP 接收器 |

## 架构

```
                    UDP (注册)           UDP (数据)
发送端 ──────────────────→ control.py ───→ backend_mgr → udp_backend
  │                            │                              │
  │                            │ poller 轮询                  │ stdout pipe
  │                            ↓                              ↓
  │                        server.py ←────────────────── sse_queue
  │                            │
  │                     SSE (state + data)
  │                            ↓
  │                         浏览器
  └── RemoteLogger API
```

**关键设计原则：**
- 前端 JS 不包含任何业务决策，只做渲染
- 状态统一通过 `{"type":"state",...}` SSE 消息推送
- poller 线程每秒轮询 `control.version()`，检测发件方变化后自动切换后端

## 快速开始

```bash
./host/run.sh
# 或
cd host && cmake -B build && make -C build -j
python3 server.py --port 8080 --control-port 15000
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

### backend_mgr.py — 后端管理器

管理单个 `udp_backend` 进程（host 一次只监听一个端口）。

```python
mgr = BackendManager("/path/to/udp_backend")
mgr.on_output = lambda line: sse_queue.put(line)   # 数据输出 → SSE
mgr.on_state = lambda: push_state()                 # 状态变化 → SSE

mgr.start(15001, "robot_alpha")   # 在端口 15001 监听
mgr.switch("robot_beta", 15002)   # 停止当前，启动新
mgr.stop()                        # 停止
```

属性：`active_sender`, `active_port`, `running`

### server.py — 编排

```
poller 线程 (1s 循环):
  if control.version() changed:
      if new senders and not backend_mgr.running:
          backend_mgr.switch(first_sender, port)
      push_state() → SSE

HTTP API:
  GET /select?sender=name  →  do_select(name) → backend_mgr.switch()
  GET /events               →  SSE 流
  GET /                     →  HTML 页面
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
| `--backend` | `./udp_backend` | 后端二进制路径 |
| `--port` | 8080 | HTTP 端口 |
| `--udp-port` | 9871 | 默认数据端口（备用） |
| `--control-port` | 15000 | 控制端口 |

## 使用示例

```bash
# 启动 host
./host/run.sh

# 单发送端测试
./build/remote_logger_test --host=127.0.0.1 --ctrl-port=15000 --name=mybot

# 多发送端测试
./build/multi_sender_test --ctrl-port=15000
```
