# Remote Debugger — Field Control 场控系统

## 概述

场控系统用于多机器人同时监控，每个机器人渲染为一个 tile（上方图像 + 下方绘图），横向自动铺满。纯 Python 标准库，无需编译。

实现位于 `host/apps/field.py`，前端在 `host/static/field.html`。

## 快速开始

仓库根目录：

```bash
python3 -m host field
# 或
./host/field.sh

./build/remote_logger_test --name=robot_1 &
./build/remote_logger_test --name=robot_2 &
```

浏览器打开 `http://localhost:8888`。

## 架构

```
发送端A ──注册──→ control.py:15000 ──分配同端口 20000──→ udp.py:20000
发送端B ──注册──→ control.py:15000 ──分配同端口 20000──→ udp.py:20000
                                                              │
                                                         apps/field.py
                                                              │ SSE
                                                          浏览器 grid
```

控制服务器 `reuse_ports=True`，所有发送方共用数据端口。前端按 `_from` 分成独立 tile。

## 前端功能

| 功能 | 说明 |
|------|------|
| **Tile 网格** | 自适应横向铺满 |
| **图像显示** | 滚轮缩放、拖动平移、下拉切换源 |
| **绘图** | Chart.js + zoom 插件 |
| **分割条** | 调整图像/绘图高度 |
| **侧边栏** | 滑动/暂停、窗口秒数、字段勾选 |
| **连接状态** | 4 秒无数据变红 |

## 参数

```bash
python3 -m host field --port 8888 --data-port 20000 --ctrl-port 15000
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 8888 | HTTP 端口 |
| `--data-port` | 20000 | UDP 数据端口 |
| `--ctrl-port` | 15000 | 控制端口 |
| `--download-assets` | off | 下载离线 JS |
