# Replayer — 本地 `.rlog` 回放

读 RemoteLogger 写出的 `.rlog`，按车上相同 UDP 协议向 Host 注册并发送数据。Host 侧用 `./host/watch.sh` 接收，与真车无异。

## 快速开始

```bash
./host/watch.sh                              # 终端 1
./replayer/replay.sh logs/run_xxx.rlog       # 终端 2
```

## 参数

| 环境变量 / 参数 | 默认 | 说明 |
|----------------|------|------|
| `HOST` / `--host` | 127.0.0.1 | Host IP |
| `CTRL_PORT` / `--ctrl-port` | 15000 | 注册端口 |
| `SPEED` / `--speed` | 1 | 回放倍速 |
| `--name` | 文件内 `_from` | 伪装的 sender 名 |
| `--gap-cap` | 0.5 | 记录间最大等待（秒），0=不限制 |
| `--hb` | 500 | 心跳间隔（ms），0=关闭 |
| `--loop` | off | 播完再循环 |

Ctrl+C 会发 `deregister`。

## 文件

```
replayer/
  replay.sh    入口脚本
  replay.py    注册 + 按时序 UDP 发送
  rlog.py      解析 RLG2 / 旧 RLOG
  README.md    本文件
```

协议细节见仓库根目录 [`REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。
