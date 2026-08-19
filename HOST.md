# Remote Debugger — Host 端文档

## 概述

Host 端是远程调试系统的接收与可视化端，运行在调试 PC 上（Linux / macOS / Windows，只需 Python 3.8+，无第三方包）。

```
host/
  __main__.py          python -m host 入口
  cli.py               debugger / field 子命令
  control.py           UDP 注册协议
  udp.py               UDP 数据解析
  sse.py               SSE 队列
  httputil.py          静态资源 / vendor CDN
  apps/
    debugger.py        单车调试
    field.py           多车场控
  static/
    debugger.html
    field.html
    css/  js/  vendor/
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
- 纯 Python 标准库，仓库根目录执行 `python3 -m host`
- 前端 HTML / CSS / JS 与后端分离，放在 `host/static/`
- 状态通过 `{"type":"state",...}` SSE 推送

## 快速开始

在仓库根目录：

```bash
python3 -m host                  # 单车调试，http://localhost:8080
python3 -m host field            # 多车场控，http://localhost:8888
./host/watch.sh                  # 同上 debugger
./host/field.sh                  # 同上 field
```

兼容旧命令：`python3 host/server.py`、`python3 host/field.py`。

离线使用（下载 Hammer / zoom 到 `host/static/vendor/`）：

```bash
python3 -m host --download-assets
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

## 命令行

```bash
python3 -m host debugger --port 8080 --control-port 15000
python3 -m host field --port 8888 --data-port 20000 --ctrl-port 15000
```

| 子命令 | 参数 | 默认 | 说明 |
|--------|------|------|------|
| debugger | `--port` | 8080 | HTTP |
| debugger | `--control-port` | 15000 | 注册端口 |
| field | `--port` | 8888 | HTTP |
| field | `--data-port` | 20000 | 共享数据端口 |
| field | `--ctrl-port` | 15000 | 注册端口 |
| 两者 | `--download-assets` | off | 下载离线 JS |

## 使用示例

```bash
python3 -m host

./build/remote_logger_test --host=127.0.0.1 --ctrl-port=15000 --name=mybot
./build/multi_sender_test --ctrl-port=15000
```
