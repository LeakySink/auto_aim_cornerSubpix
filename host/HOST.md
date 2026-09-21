# Remote Debugger — Host 端

调试 PC 上的统一门户：一个进程挂载 Watch / Replay / Dump / Netcheck。  
Unix 入口经 `_env.sh` 自动使用 `host/.venv`（离线无 venv 则回退系统 Python）。  
dump 导出视频依赖 `opencv-python-headless`（`requirements.txt`）。

**内部构造与扩展** 见 [`DESIGN.md`](DESIGN.md)。协议见 [`PROTOCOL.md`](PROTOCOL.md)。车上见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

## 快速开始

```bash
./host/start.sh                          # 门户 http://127.0.0.1:8080
# 浏览器首页点选 Watch / Replay / Dump / Netcheck
```

构建前端（改 `host/ui` 后）：

```bash
./host/ui/build.sh                       # 需要 Node 18+（可用 host/ui/.tools/node）
# 产物写入 host/rdbg/static_ui/
```

兼容旧入口（均转到门户）：

```bash
./host/watch.sh                          # 打开门户首页，再点 Watch 新开页面
./host/replay.sh logs/run_xxx.rlog       # 打开门户；在新开的 Replay 页填路径
./host/dump.sh logs/run_xxx.rlog         # 仍可命令行 dump；也可用门户 Dump 页
./host/netcheck.sh discover              # 仍可命令行；也可用门户 Netcheck 页
```

## 目录

```
host/
  start.sh                 统一门户入口
  _env.sh / requirements.txt / .venv/
  ui/                      Vite + React + TS 源码
  rdbg/
    apps/hub_app.py        Hub 生命周期
    features/              Feature 插件（watch/replay/dump/netcheck）
    static_ui/             前端构建产物
  HOST.md / DESIGN.md / PROTOCOL.md
```

## 参数（start.sh / serve）

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 8080 | HTTP |
| `--host` | 0.0.0.0 | 绑定地址 |
| `--no-browser` | off | 不自动开浏览器 |
| `--open` | `/` | 打开路径（首页） |

## 使用提示

- 首页点功能会 **新开页面**，每个页面一条线程，可以多开。关掉页面（或点「关闭」）会停掉这条线程。
- 打开 Watch 时先选一辆车。Host 给每辆车分配不同的空闲 UDP 口（从 15001 起），这辆车的数据只打到那个口。同一辆车的多个 Watch 共用该口；最后一个关掉后才释放。
- Replay：在页面输入 `.rlog` 绝对/相对路径后点「加载」。
- Dump：导出目录含 `log.txt` / `plot.txt` / `images.mp4`。
