# Remote Debugger — Field Control 场控系统

## 概述

场控系统用于多机器人同时监控，每个机器人渲染为一个 tile（上方图像 + 下方绘图），横向自动铺满。纯 Python 标准库，无需编译。

| 模块 | 文件 | 职责 |
|------|------|------|
| **控制协议** | `host/control.py` | 注册协议，`reuse_ports` 模式统一端口 |
| **UDP 接收** | `host/udp_rx.py` | 单端口收包，按 `_from` 区分发送方 |
| **场控服务器** | `host/field.py` | HTTP/SSE + 网格前端 |
| **启动脚本** | `host/field.sh` | 启动（可选） |

## 快速开始

```bash
# 启动场控
./host/field.sh
# 或
python3 host/field.py

# 各机器人发送端（ctrl_port=15000 自动注册）
./build/remote_logger_test --name=robot_1 &
./build/remote_logger_test --name=robot_2 &
```

浏览器打开 `http://localhost:8888`。

## 架构

```
发送端A ──注册──→ control.py:15000 ──分配同端口 20000──→ udp_rx.py:20000
发送端B ──注册──→ control.py:15000 ──分配同端口 20000──→ udp_rx.py:20000
                                                              │
                                                              │ JSON 回调
                                                              ↓
                                                         field.py (SSE)
                                                              │
                                                    SSE: plot + image
                                                              ↓
                                                          浏览器 grid
```

**关键设计**：控制服务器 `reuse_ports=True`，所有发送方分配到同一数据端口。后端单端口接收，前端按 `_from` 字段分组为独立 tile。

## 前端功能

| 功能 | 说明 |
|------|------|
| **Tile 网格** | 自适应横向铺满，每个机器人一个 tile |
| **图像显示** | 上方区域，支持下拉切换图像源、滚轮缩放、拖动平移 |
| **绘图** | 下方区域，Chart.js 折线图，chartjs-plugin-zoom 支持 X/Y 缩放 + X 拖拽 |
| **分割条** | 中间可拖拽横条，调整图像/绘图高度比例 |
| **侧边栏** | 显示模式（滑动/暂停）、窗口大小（秒）、重置视角、数据字段勾选 |
| **图例** | 每个 tile 右上角，空心/实心色块切换曲线显隐 |
| **连接状态** | tile 头部绿色/红色圆点，4 秒无数据变红 |
| **帧率** | 图像帧率实时显示 |

## 侧边栏

点击任意 tile 即可激活，侧边栏显示该 tile 的设置：

- **显示模式**：滑动（自动跟随最新数据）/ 暂停（手动缩放浏览）
- **窗口**：X 轴可视范围（秒），默认 30
- **重置视角**：恢复默认缩放/平移 + 图像视角
- **数据字段**：勾选控制当前 tile 的绘图通道，新字段默认全选

## 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 8888 | HTTP 端口 |
| `--data-port` | 20000 | UDP 数据端口 |
| `--ctrl-port` | 15000 | 控制端口 |
| `--download-assets` | off | 下载前端 JS 供离线使用 |
