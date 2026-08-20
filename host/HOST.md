# Remote Debugger — Host 端

调试 PC 上的接收与可视化。Python 3.8+ 标准库，无 pip 依赖。Win / macOS / Linux 同一套入口。

**内部构造、扩展方法和全部 API** 见 [`DESIGN.md`](DESIGN.md)。车上发送端见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

## 目录

```
host/
  watch.py / replay.py     跨平台入口（推荐）
  watch.sh / replay.sh     Unix 包装
  watch.cmd / replay.cmd   Windows 包装
  HOST.md / DESIGN.md
  rdbg/
    http/     路由 · SSE · 静态
    net/      注册 UDP · 数据 UDP
    log/      .rlog 解析 · 预加载
    sources/  live · replay
    apps/     watch.py · replay.py
    static/   watch.html · replay.html · js/plugins/
```

watch 与 replay 是两个进程，共用 HTTP 壳和面板插件，数据源不同。

## 快速开始

任意系统（同一文件）：

```bash
python3 host/watch.py
python3 host/replay.py logs/run_xxx.rlog
```

Windows 若 `python3` 不在 PATH，用 `python` 或包装脚本：

```bat
python host\watch.py
host\watch.cmd
host\replay.cmd logs\run_xxx.rlog
```

Unix 也可继续用：

```bash
./host/watch.sh
./host/replay.sh logs/run_xxx.rlog
```

```bash
python3 host/watch.py --download-assets
python3 host/watch.py --no-browser
HTTP_PORT=8081 CTRL_PORT=15000 python3 host/watch.py

python3 host/replay.py logs/run_xxx.rlog --port 8766 --no-browser
```

## 参数

| 入口 | 环境变量 / 参数 | 默认 | 说明 |
|------|----------|------|------|
| watch | `HTTP_PORT` / `--port` | 8080 | HTTP |
| watch | `CTRL_PORT` / `--control-port` | 15000 | 注册端口 |
| watch | `--no-browser` | off | 不自动打开浏览器 |
| watch | `--download-assets` | off | 下载离线 Chart.js |
| replay | `--port` | 8765 | HTTP（占用则自动 +1） |
| replay | `--host` | 127.0.0.1 | HTTP 绑定 |
| replay | `--max-mb` | 1024 | 预加载内存上限 |
| replay | `--no-browser` | off | 不自动打开浏览器 |

命令行 `--port` 优先于环境变量。

## 使用

```bash
python3 host/watch.py
# 另开终端跑车上程序（yaml 里 remote_host 指向本机）

python3 host/replay.py logs/run_xxx.rlog
```

回放快捷键：空格播放/暂停，← / → 步进 0.05s。图像窗口用下拉框切 `meta.name`；拆分不会自动换路。

加数据源或面板的步骤见 `DESIGN.md` §3。
