# Remote Debugger — Host 端

调试 PC 上的接收与可视化。默认经 `host/*.sh` 自动使用 `host/.venv`（与系统 Python 隔离）；watch/replay 仍只需标准库，dump 导出视频依赖 `opencv-python-headless`（见 `requirements.txt`）。

**内部构造、扩展方法和全部 API** 见 [`DESIGN.md`](DESIGN.md)。车/host 控制协议见 [`PROTOCOL.md`](PROTOCOL.md)。车上发送端见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

## 目录

```
host/
  _env.sh                  自动创建/选用 .venv（离线无 venv 则回退系统 Python）
  requirements.txt         dump 视频等可选依赖
  .venv/                   本地虚拟环境（gitignore，首次联网脚本自动建）
  watch.sh / replay.sh / dump.sh   Unix 入口
  watch.bat / replay.bat           Windows 入口
  run.py                           按平台选 watch/replay/dump
  HOST.md / DESIGN.md / PROTOCOL.md
  rdbg/
    http/     路由 · SSE · 静态
    net/      发现 · 向车注册 · 数据 UDP · host 转发
    log/      .rlog 解析 · dump · 预加载
    sources/  live · replay
    apps/     watch.py · replay.py
    static/   watch.html · replay.html · js/plugins/
```

watch 与 replay 是两个进程，共用 HTTP 壳和面板插件，数据源不同。

## 环境

所有 Unix 入口脚本会 `source _env.sh`：

1. 已有 `host/.venv` → 用其中的 Python  
2. 否则能访问 PyPI → 创建 `.venv` 并 `pip install -r requirements.txt`  
3. 否则（离线且无 venv）→ 回退系统 `python3` / `python`

也可手动：`python3 -m venv host/.venv && host/.venv/bin/pip install -r host/requirements.txt`

## 快速开始

Unix：

```bash
./host/watch.sh                          # http://localhost:8080  数据口 15001
./host/replay.sh logs/run_xxx.rlog       # 默认 http://127.0.0.1:8765
./host/dump.sh logs/run_xxx.rlog         # → logs/run_xxx_dump/{log,plot}.txt + images.mp4
./host/netcheck.sh discover              # 听车上 beacon
./host/netcheck.sh echo --port 15050     # 一台开回显
./host/netcheck.sh ping <IP> --size 1200 # 另一台测丢包/RTT（可再试 --size 20000）
```

Windows：

```bat
host\watch.bat
host\replay.bat logs\run_xxx.rlog
```

不想记平台时：

```bash
python3 host/run.py                      # 等价于 watch
python3 host/run.py watch --no-browser
python3 host/run.py replay logs/run_xxx.rlog
```

```bash
./host/watch.sh --download-assets
./host/watch.sh --no-browser
HTTP_PORT=8081 DATA_PORT=15002 PEER_PORT=15101 ./host/watch.sh

./host/replay.sh logs/run_xxx.rlog --port 8766 --no-browser
```

## 参数

| 脚本 | 环境变量 / 参数 | 默认 | 说明 |
|------|----------|------|------|
| watch.sh | `HTTP_PORT` | 8080 | HTTP |
| watch.sh | `DATA_PORT` | 15001 | 收车/队首转发的数据 |
| watch.sh | `PEER_PORT` | 15100 | host 对等（subscribe） |
| watch.sh | `DISCOVER_PORT` | 15999 | 听车 beacon |
| watch.sh | `--no-browser` | off | 不自动打开浏览器 |
| watch.sh | `--download-assets` | off | 下载离线 Chart.js |
| replay.sh | `--port` | 8765 | HTTP（占用则自动 +1） |
| replay.sh | `--host` | 127.0.0.1 | HTTP 绑定 |
| replay.sh | `--max-mb` | 1024 | 预加载内存上限 |
| replay.sh | `--no-browser` | off | 不自动打开浏览器 |
| dump.sh | `-o` / `--output` | `<stem>_dump/` | 输出目录（内含 log.txt / plot.txt / images.mp4） |
| dump.sh | `--fps` | 10 | 图像视频帧率 |

## 使用

```bash
./host/watch.sh
# 另开终端跑车上程序（不必写 remote_host；watch 靠 beacon 发现车）

./host/replay.sh logs/run_xxx.rlog

./host/dump.sh logs/run_xxx.rlog
./host/dump.sh logs/run_xxx.rlog -o /tmp/run_dump --fps 15
```

`dump` 行格式：`[相对秒][LOG|PLOT]-----内容`；图像按时间序合成 `images.mp4`（需 opencv）。

回放快捷键：空格播放/暂停，← / → 步进 0.05s。图像窗口用下拉框切 `meta.name`；拆分不会自动换路。

加数据源或面板的步骤见 `DESIGN.md` §3。
