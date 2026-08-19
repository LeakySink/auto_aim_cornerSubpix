# Remote Debugger — Host 端文档

## 概述

Host 端是远程调试系统的接收与可视化端，运行在调试 PC 上（Linux / macOS / Windows，只需 Python 3.8+，无第三方包）。

`host/` 根目录只放启动脚本和文档，Python 包在 `host/rdbg/`。

```
host/
  watch.sh                 单车调试入口
  field.sh                 多车场控入口
  rlog.sh                  本地 .rlog 回放
  HOST.md                  本文件
  FIELD.md                 场控文档
  rdbg/                    Python 包（勿直接当入口）
    cli.py / control.py / udp.py / sse.py / httputil.py / rlog.py
    apps/debugger.py
    apps/field.py
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
- 纯 Python 标准库，用 `./host/watch.sh` / `./host/field.sh` 启动
- 前端 HTML / CSS / JS 与后端分离
- 状态通过 `{"type":"state",...}` SSE 推送

## 快速开始

```bash
./host/watch.sh          # 单车调试，http://localhost:8080
./host/field.sh          # 多车场控，http://localhost:8888
./host/rlog.sh logs/run_xxx.rlog   # 回放本地日志（含图像），同调试页
```

端口可用环境变量覆盖：`HTTP_PORT`、`CTRL_PORT`；场控另有 `DATA_PORT`。其余参数原样传给 Python，例如离线下载 JS：

```bash
./host/watch.sh --download-assets
```

## 模块详解

### control.py — 控制协议

UDP 端口 15000，JSON 协议。

**注册：** `{"type":"register","name":"robot_alpha"}` → `{"type":"register_ack","status":"ok","port":15001}`

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

### rlog.py — 本地 .rlog

`load(path)` 读 C++ `RemoteLogger` 写出的二进制日志。`./host/rlog.sh` 回放与直播类似：图像与曲线共用录制时间轴（滑动窗口右边缘=当前时刻）、可拖进度条/倍速/空格暂停；图像预加载并按实际速率播放，落后时丢中间帧防卡顿。

### apps/debugger.py — 单车编排

```
poller:
  发件方变化 → udp.switch(first_sender, port) → SSE state

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

## 参数

| 脚本 | 环境变量 | 默认 | 说明 |
|------|----------|------|------|
| watch.sh | `HTTP_PORT` | 8080 | HTTP |
| watch.sh | `CTRL_PORT` | 15000 | 注册端口 |
| field.sh | `HTTP_PORT` | 8888 | HTTP |
| field.sh | `DATA_PORT` | 20000 | 共享数据端口 |
| field.sh | `CTRL_PORT` | 15000 | 注册端口 |
| rlog.sh | `HTTP_PORT` | 8080 | HTTP |
| watch / field | `--download-assets` | off | 下载离线 JS |

## 使用示例

```bash
./host/watch.sh

./build/remote_logger_test --host=127.0.0.1 --ctrl-port=15000 --name=mybot
./build/multi_sender_test --ctrl-port=15000
```
