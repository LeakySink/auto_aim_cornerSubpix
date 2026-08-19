# Remote Debugger — Host 端文档

## 概述

Host 端是远程调试系统的接收与可视化端，运行在调试 PC 上（Linux / macOS / Windows，只需 Python 3.8+，无第三方包）。

`host/` 根目录只放启动脚本和文档，Python 包在 `host/rdbg/`。

```
host/
  watch.sh                 单车调试入口（自动打开浏览器）
  replay.sh                本地 .rlog 回放入口
  HOST.md                  本文件
  rdbg/                    Python 包（勿直接当入口）
    cli.py / control.py / udp.py / sse.py / httputil.py
    rlog.py / session.py   .rlog 解析与预加载
    apps/debugger.py
    apps/replay.py
    static/                HTML / CSS / JS / vendor
```

## 架构

```
                    UDP (注册)           UDP (数据)
发送端 ──────────────────→ control.py ───→ udp.py (线程)
  │                            │                    │
  │                            │ poller             │ on_output
  │                            ↓                    ↓
  │                     apps/debugger.py ←──── sse_queue
  │                            │
  │                     SSE + /static/*
  │                            ↓
  │                         浏览器
  └── RemoteLogger API
```

**关键设计原则：**
- 纯 Python 标准库，用 `./host/watch.sh` / `./host/replay.sh` 启动
- 前端 HTML / CSS / JS 与后端分离
- 状态通过 `{"type":"state",...}` SSE 推送

## 快速开始

```bash
./host/watch.sh                          # 单车调试，自动打开 http://localhost:8080
./host/replay.sh logs/run_xxx.rlog       # 本地回放，默认 http://127.0.0.1:8765
```

端口可用环境变量覆盖：`HTTP_PORT`、`CTRL_PORT`。其余参数原样传给 Python：

```bash
./host/watch.sh --download-assets
./host/watch.sh --no-browser
```

## 模块详解

### control.py — 控制协议

UDP 端口 15000，JSON 协议。

**注册：** `{"type":"register","name":"robot_alpha"}` → `{"type":"register_ack","status":"ok","port":15001}`

同一 `name` 重复注册是幂等的：返回已分配的数据口并更新来源地址（车上会周期性刷新，因此可先开程序再开 watch）。

**注销：** `{"type":"deregister","name":"robot_alpha"}` → `{"type":"deregister_ack","status":"ok"}`

**Host 关闭：** `{"type":"host_shutdown"}`

接口：`version()` / `get_senders()` / `get_sender_info(name)`

### udp.py — UDP 接收

进程内绑定数据端口，解析车上 `RemoteLogger` 的 UDP 包。

```python
rx = UdpBackend()
rx.on_output = lambda line: sse.put(line)
rx.start(15001, "robot_alpha")
rx.switch("robot_beta", 15002)
rx.stop()
```

### apps/debugger.py — 单车编排

```
poller:
  发件方变化 → SSE state
  有 sender 时确保 UDP 在听（bind 失败会每 0.5s 重试）

GET /                debugger.html
GET /events          SSE
GET /select?sender=  切换监听端口
GET /static/...      CSS / JS / vendor
```

### 前端

只做渲染：SSE `state` 更新下拉框，`plot` / `image` / `log` 画图，下拉框请求 `/select`。

| type | 说明 |
|------|------|
| `state` | `{active_sender, senders}` |
| `plot` | 变量数据 |
| `image` | base64 JPEG |
| `log` | 日志 |
| `status` | `{connected, sender}` |

### apps/replay.py — 本地 `.rlog` 回放

与 watch **独立进程**，不占用控制口、不收 UDP。预加载到内存（默认上限 1GiB），左图右曲线、底部进度条。

```
GET /                replay.html
GET /api/meta        会话元数据 + 曲线（无 JPEG）
GET /api/frame/N     第 N 帧 JPEG
GET /static/...      CSS / JS / vendor
```

```bash
./host/replay.sh logs/run_xxx.rlog
./host/replay.sh logs/run_xxx.rlog --port 8766 --no-browser
```

快捷键：空格播放/暂停，← / → 步进 0.05s。

## 参数

| 脚本 | 环境变量 / 参数 | 默认 | 说明 |
|------|----------|------|------|
| watch.sh | `HTTP_PORT` | 8080 | HTTP |
| watch.sh | `CTRL_PORT` | 15000 | 注册端口 |
| watch.sh | `--no-browser` | off | 不自动打开浏览器 |
| replay.sh | `--port` | 8765 | HTTP（占用则自动 +1） |
| replay.sh | `--host` | 127.0.0.1 | HTTP 绑定 |
| replay.sh | `--max-mb` | 1024 | 预加载内存上限 |
| replay.sh | `--no-browser` | off | 不自动打开浏览器 |
| watch.sh | `--download-assets` | off | 下载离线 JS |

## 使用示例

```bash
./host/watch.sh

./build/remote_logger_test --host=127.0.0.1 --ctrl-port=15000 --name=mybot
./build/multi_sender_test --ctrl-port=15000

./host/replay.sh logs/run_xxx.rlog
```
