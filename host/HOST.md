# Remote Debugger — Host 端

调试 PC 上的接收与可视化。Python 3.8+ 标准库，无 pip 依赖。

**内部构造、扩展方法和全部 API** 见 [`DESIGN.md`](DESIGN.md)。车/host 控制协议见 [`PROTOCOL.md`](PROTOCOL.md)。车上发送端见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

## 目录

```
host/
  watch.sh / replay.sh     Unix 入口
  watch.bat / replay.bat   Windows 入口
  run.py                   按平台选上面两者
  HOST.md / DESIGN.md / PROTOCOL.md
  rdbg/
    http/     路由 · SSE · 静态
    net/      发现 · 向车注册 · 数据 UDP · host 转发
    log/      .rlog 解析 · 预加载
    sources/  live · replay
    apps/     watch.py · replay.py
    static/   watch.html · replay.html · js/plugins/
```

watch 与 replay 是两个进程，共用 HTTP 壳和面板插件，数据源不同。

## 快速开始

Unix：

```bash
./host/watch.sh                          # http://localhost:8080  数据口 15001
./host/replay.sh logs/run_xxx.rlog       # 默认 http://127.0.0.1:8765
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

## 使用

```bash
./host/watch.sh
# 另开终端跑车上程序（不必写 remote_host；watch 靠 beacon 发现车）

./host/replay.sh logs/run_xxx.rlog
```

回放快捷键：空格播放/暂停，← / → 步进 0.05s。图像窗口用下拉框切 `meta.name`；拆分不会自动换路。

加数据源或面板的步骤见 `DESIGN.md` §3。
