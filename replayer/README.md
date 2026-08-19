# Replayer — 本地 `.rlog` 独立预览

读 RemoteLogger 写出的 `.rlog`，**预加载到内存**（默认上限 1GiB），打开独立预览窗口：

- **左侧**：当前时刻图像
- **右侧**：变量曲线（时间轴以当前时刻为中心对齐）
- **底部**：可拖动进度条，流畅 scrub
- 曲线支持滚轮缩放、拖拽平移（`重置缩放` 恢复居中跟随）

## 快速开始

```bash
./replayer/replay.sh logs/run_xxx.rlog
```

会自动打开浏览器窗口（优先 Chromium `--app` 无地址栏模式）。

## 参数

| 环境变量 / 参数 | 默认 | 说明 |
|----------------|------|------|
| `HOST` / `--host` | 127.0.0.1 | HTTP 绑定地址 |
| `PORT` / `--port` | 8765 | HTTP 端口 |
| `--max-mb` | 1024 | 预加载内存上限（MiB） |
| `--no-browser` | off | 不自动打开窗口 |

## 快捷键

| 键 | 作用 |
|----|------|
| 空格 | 播放 / 暂停 |
| ← / → | 后退 / 前进 0.05s |

## 文件

```
replayer/
  replay.sh          入口
  replay.py          加载 + 启动本地 HTTP 服务
  session.py         解析 .rlog 并预加载
  server.py          静态页 + /api/session
  rlog.py            RLG2 / 旧 RLOG 解析
  static/            预览 UI
  README.md
```

## `.rlog` 格式（本工具可读）

- magic `RLG2`（4B LE），旧版 `RLOG` 仅 JSON 也支持
- 每条：`type(1B) + ts_ns(8B) + payload`
  - `0x00` json：`len(4B) + utf-8 json`
  - `0x01` image：`meta_len(4B) + meta json + jpg_len(4B) + jpeg`

离线使用可将 Chart.js / Hammer / zoom 插件放入 `replayer/static/vendor/`（文件名见 `server.py` 的 `CDN` 字典），否则首次打开会走 CDN。
