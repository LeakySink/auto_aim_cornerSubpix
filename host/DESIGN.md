# Host 内部设计

给改 host 的程序员和 Agent 用。用法入口见 [`HOST.md`](HOST.md)。控制协议见 [`PROTOCOL.md`](PROTOCOL.md)。车上发送端见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

本文约定：路径相对 `host/`。包名 `rdbg`。Unix 用 `watch.sh` / `replay.sh`，Windows 用 `watch.bat` / `replay.bat`，把 `host/` 加进 `PYTHONPATH` 后跑 `python -m rdbg …`。`run.py` 只看 `sys.platform`，Windows 调 `.bat`，否则调 `.sh`。

---

## 1. 分层

```
host/
  watch.sh / replay.sh     Unix 入口
  watch.bat / replay.bat   Windows 入口
  run.py                   按平台转发到上面
  HOST.md                  使用者
  DESIGN.md                本文件
  PROTOCOL.md              车/host 控制协议
  rdbg/                    Python 包（不要直接当脚本跑）
    cli.py                 子命令
    http/                  HTTP 壳：路由、SSE、静态
    net/                   发现、向车注册、数据 UDP、host 转发
      control.py           Discovery + RobotClient
      peer.py              subscribe / 原样转发
      udp.py               数据口解析
    log/                   .rlog 解析、dump、预加载
    sources/               数据源插件（live / replay）
    apps/                  把壳和数据源拼起来
    static/                浏览器资源
      watch.html / replay.html
      css/shell.css        共用壳样式
      css/replay.css       回放条
      js/registry.js       Rdbg 面板注册表
      js/shell.js          分屏 + SSE 分发
      js/replay.js         回放进度条（驱动壳的全局函数）
      js/plugins/          面板插件
      vendor/              Chart.js 等
```

不要把协议细节写进 `apps/`，不要把分屏写进某个面板插件。

| 层 | 职责 | 禁止 |
|---|---|---|
| `net/` | UDP 字节 ↔ JSON 事件 | 不知道 HTTP |
| `log/` | 文件字节 ↔ 会话 dict | 不知道浏览器 |
| `http/` | 端口、路由、静态、SSE | 不知道 plot/image 语义 |
| `sources/` | 把 net/log 接到壳上 | 不写 Handler 类 |
| `apps/` | `run()` 生命周期 | 不写业务解析 |
| `static/js/shell.js` | 分屏、类型切换、SSE | 不画曲线/图/日志 |
| `static/js/plugins/` | 一种视图 | 不改分屏算法 |

---

## 2. 运行时数据流

```
车上 RemoteLogger
  plot/log  → JSON UDP
  plot_image → 本地 .rlog；UDP 仅订阅流，0xFE 分片发给队首
  周期 img_streams 目录 JSON

watch:
  net/control  听 beacon，向车 register
  队首 net/udp 收车包；peer 原样转发
  follower 收队首转发 → JSON 行 → sources/live → shell.sse

replay:
  log/rlog + log/session 预加载
  sources/replay 提供 /api/meta 与 /api/frame/N
  replay.js 按时间轴调用与 watch 相同的全局函数
```

watch 与 replay **两个进程**。前端插件相同；差别只在数据源和 `replay.js`。

---

## 3. 怎么加能力

### 3.1 新数据源

1. `rdbg/sources/foo.py` 实现：

```python
class FooSource:
    id = "foo"
    def attach(self, shell): ...
    def start(self): ...
    def stop(self): ...
```

2. `attach` 里只用壳 API：`page` / `route` / `sse_route`。
3. `rdbg/apps/foo.py` 写 `run()`：`Source` + `Shell` + `bind` + `serve`。
4. `cli.py` 加子命令。根目录加 `foo.sh`（可选）。

### 3.2 新面板

1. `static/js/plugins/foo.js` 里 `Rdbg.registerPanel({...})`。
2. `watch.html` 和 `replay.html` 在 `shell.js` **之前**加一行 script。
3. 类型下拉框自动出现。不要改 `shell.js` 的 `if (type === …)`（已经没有）。

脚本顺序必须是：`registry.js` → 各 `plugins/*.js` → `shell.js` →（回放再）`replay.js`。

---

## 4. Python API

### 4.1 `rdbg.cli`

`python3 -m rdbg <cmd>`

| cmd | 含义 |
|---|---|
| `watch` | 实时。别名 `debugger`（旧脚本） |
| `replay <file.rlog>` | 本地回放 |
| `dump <file.rlog>` | 导出到目录：`log.txt` / `plot.txt` / `images.mp4` |

无参数或参数以 `-` 开头（且不是 `-h`）时，默认 `watch`。

`watch` 参数：`--port`(8080) `--data-port`(15001) `--peer-port`(15100) `--discover-port`(15999) `--download-assets` `--no-browser`  
`replay` 参数：`rlog` `--host`(127.0.0.1) `--port`(8765) `--max-mb`(1024) `--no-browser`  
`dump` 参数：`rlog` `-o/--output`（默认旁路 `<stem>_dump/`） `--fps`(10)

### 4.2 `rdbg.http.shell.Shell`

```python
Shell(name="rdbg")
```

| 成员 / 方法 | 说明 |
|---|---|
| `name` | 日志前缀，Ctrl+C 时打印 `[name] shutting down...` |
| `sse` | `SSEQueue`，数据源 `put` JSON 字符串 |
| `last_bind_error` | `bind` 失败时的最后一次 `OSError` |
| `page(path, html_name)` | `GET path` → `static/html_name`（无缓存） |
| `route(path, fn, methods=("GET",), prefix=False)` | 精确或前缀匹配。前缀按路径长度从长到短 |
| `sse_route(path="/events", on_connect=None)` | `GET` 后挂 SSE；`on_connect()` 在头写完后立刻调 |
| `bind(host, port, tries=1)` | 返回 `(httpd, bound_port)` 或 `(None, 0)` |
| `serve(httpd, *, threaded=False, on_stop=None)` | 阻塞。`threaded=True` 时在守护线程 `serve_forever`（replay 用，便于 Ctrl+C） |

`fn(handler)` 收到的 handler 额外字段：

| 字段 / 方法 | 说明 |
|---|---|
| `route_path` | URL path，不含 query |
| `query` | `parse_qs` 结果，值为 list |
| `send(code, body, ctype=..., cache="no-cache")` | `body` 可以是 `bytes` 或 `str`（utf-8） |

匹配顺序：注册的 html 页 → 精确路由 → 最长前缀 → `/static/` → 404。只实现了 `GET`。

### 4.3 `rdbg.http.sse`

```python
SSEQueue(maxsize=4096)
q.put(line)            # 满则丢最旧一条再放入
q.get(timeout=0.5)     # 超时返回 None
write_sse(handler, q, on_connect=None)  # 阻塞直到客户端断开
```

写出格式：`data: {line}\n\n`。`line` 必须是已经序列化的 JSON 字符串。

### 4.4 `rdbg.http.httputil`

| 符号 | 说明 |
|---|---|
| `STATIC_DIR` | `rdbg/static` |
| `VENDOR_DIR` | `static/vendor` |
| `ThreadingHTTPServer` | `ThreadingMixIn` + `HTTPServer`，`daemon_threads=True` |
| `serve_page(handler, name)` | 根目录 html，`Cache-Control: no-cache` |
| `try_serve_static(handler, raw_path)` | `/static/...`；vendor 可 302 到 CDN。命中或 404 返回 True，非 static 返回 False |
| `send_file(handler, path, cache=...)` | 按后缀设 MIME |
| `download_vendor()` | 把 Chart/Hammer/zoom 拉到 vendor |
| `open_browser(url)` | Chrome/Chromium/Edge `--app=` 且隔离 stdio；否则 `webbrowser.open` |

### 4.5 `rdbg.net.control` / `peer`

消息格式见 [`PROTOCOL.md`](PROTOCOL.md)。

```python
Discovery(port=15999, host_id="")
d.on_beacon = lambda name, ip, control, addr: ...
d.start() / d.stop() / d.probe()   # probe 发 who

RobotClient(host_id, host_name, data_port, peer_port)
c.register(ip, control_port)
c.deregister(ip, control_port)
c.head_alive(ip, control_port)
c.query_head(ip, control_port)

PeerHub(host_id, peer_port, data_port)
p.on_msg = lambda msg, addr: ...
p.is_head = lambda robot: False    # 非队首拒绝 subscribe
p.start() / p.stop()
p.subscribe(robot, head_ip, head_peer_port)
p.unfollow(robot)
p.forward(robot, raw_bytes)        # 原样 UDP 到订阅者 data_port
p.handoff(robot, head_dict)
```

车 bind `:15000`。host **不听** 15000，听 `:15999`（beacon）和 `:15100`（peer）。

### 4.6 `rdbg.net.udp.UdpBackend`

```python
packet_sender(data) -> str         # 包里的 _from

UdpBackend(timeout_ms=3000)
rx.on_output = lambda json_line: ...
rx.on_state = lambda: ...
rx.on_raw = lambda data: ...       # 原始 UDP，供队首转发；在解析之前
rx.set_filter(sender_name)         # 空=不过滤；只影响 SSE，不影响 on_raw
rx.start(port, sender_name="default")
rx.switch(sender_name, port)
rx.stop()
rx.active_sender / rx.active_port / rx.running
```

`on_output` 收到的是 **JSON 字符串**（不是 dict）。类型：

| `type` | 字段 |
|---|---|
| `plot` | `ts`, `data`（原 JSON，含 `_from`） |
| `log` | `ts`, `level`, `msg`，可选 `_from` |
| `image` | `ts`, `meta`, `jpg_b64` |
| `img_streams` | `streams` 字符串列表，可选 `_from` |
| `status` | `connected` bool；连上时带 `sender` |

图像默认 `0xFE` 分片（见 `PROTOCOL.md`）；host 收齐后发 `image`。兼容旧整包 `0xFF`。含 `"img_streams"` 的 JSON 目录单独发出。含 `"hb"` 的心跳丢弃。无包超过 `timeout_ms` 发 `status.connected=false`。

常量：`IMG_FRAG_MARKER=0xFE`，`IMG_MARKER=0xFF`，`MAX_UDP=65536`。

### 4.7 `rdbg.log.rlog`

```python
iter_records(path, include_jpeg=True)  # generator
summarize(path) -> (n_json, n_img, sender_name)
```

记录：

```python
{"kind": "json", "ts": int, "obj": dict}
{"kind": "img",  "ts": int, "meta": dict, "jpeg": bytes, "jpeg_len": int}
# include_jpeg=False 时 jpeg=b""，jpeg_len 仍为文件中长度
```

Magic：v1 `0x524C4F47`（仅 JSON），v2 `0x32474C52`（`RLG2`）。v2：`type 0x00` json，`0x01` image。截断则打印 stderr 并停止。文件格式细节见 `REMOTE_LOGGER.md`。

### 4.7b `rdbg.log.dump`

```python
dump_rlog(path, out_dir, fps=10.0) -> dict
dump_rlog_to_path(rlog_path, output=None, fps=10.0) -> exit_code
```

输出目录内：`log.txt`、`plot.txt`（行格式 `[t][LOG|PLOT]-----...`），以及按时间序合成的 `images.mp4`（需 OpenCV）。相对时间相对文件首条 `ts`。

### 4.8 `rdbg.log.session`

```python
load_session(path, max_bytes=1<<30) -> dict
load_session_or_exit(path, max_bytes=...)  # 失败 SystemExit 1/2
```

返回：

| 键 | 类型 | 说明 |
|---|---|---|
| `file` | str | 文件名 |
| `path` | str | 绝对路径 |
| `sender` | str | 第一条带 `_from` 的记录，否则 stem |
| `t0_ns` | int | 第一条记录的 ts |
| `duration` | float | 秒，图像/曲线/日志最晚者 |
| `memory_bytes` | int | JPEG 合计 |
| `frames` | list | `{t, meta, jpeg}`，按 `t` 排序 |
| `logs` | list | `{t, ts, level, msg}` |
| `series` | dict | `name -> [[t, value], ...]` |
| `fields` | list | 曲线名排序 |

跳过：心跳 `hb`；键 `_from`/`ts`/`hb`/`level`/`msg` 不进曲线。超 `max_bytes` 抛 `MemoryError`。

### 4.9 Source 契约 + 两个实现

每个 source：`id`、`attach(shell)`、`start()`、`stop()`。

**`LiveSource(data_port=15001, peer_port=15100, discover_port=15999)`**

- 页：`/` → `watch.html`
- `GET /events` SSE，连接时 `push_state`
- `GET /select?sender=` 按 `_from` 过滤 SSE（数据口不变）
- 听 beacon；向每辆在线车 `register`；队首 `head_alive`，follower `subscribe`
- 3s 无 beacon 的车从 `senders` 拿掉，不清车上队列

**`ReplaySource(session)`**

- `/`、`/index.html` → `replay.html`
- `GET /events` 推假 `state`+`status`（避免壳显示断连）
- `GET /select` → `{"ok":true}`
- `GET /api/meta` 与 `/api/session`：`public_meta(session)`（frames **不含** jpeg，只含 `i,t,meta`）
- `GET /api/frame/<int>`：JPEG，`cache public max-age=86400`

```python
public_meta(session) -> dict
load_or_exit(rlog, max_mb=1024)  # 下限 64MiB
```

### 4.10 `rdbg.apps`

```python
apps.watch.run(http_port, data_port=15001, peer_port=15100, discover_port=15999, no_browser=False) -> 0|1
apps.replay.run(rlog, host="127.0.0.1", port=8765, max_mb=1024, no_browser=False) -> 0|1
```

watch 绑 `0.0.0.0`，口占用即失败。replay 从 `port` 起试 20 个口。

---

## 5. 前端 API

全局对象，**不是** ES module。`replay.js` 依赖下列名字，改插件时不要改签名。

### 5.1 `Rdbg`（`js/registry.js`）

```javascript
Rdbg.registerPanel(spec)  // 同 id 覆盖，order 只在首次插入
Rdbg.get(id) -> spec|null
Rdbg.list() -> spec[]     // 注册顺序
```

`spec`：

| 字段 | 必需 | 说明 |
|---|---|---|
| `id` | 是 | 写入 `p.type`，如下拉框 value |
| `title` | 建议 | 下拉框文字 |
| `setupHeader(p, hdr, typeSel)` | 否 | 类型框已在 hdr 里；在 `typeSel` 前后插控件 |
| `setupBody(p, body)` | 否 | 清空后的 `.panel-body` |
| `init(p)` | 否 | DOM 齐了之后 |
| `destroy(p)` | 否 | 切走或关闭前；自己摘掉 header 上加的节点 |
| `reset(p)` | 否 | 顶栏「重置」 |

面板对象 `p`（壳创建）：

| 字段 | 说明 |
|---|---|
| `id` | 整数，分屏用 |
| `type` | 当前插件 id |
| `el` | `.panel` 节点，未 mount 前可能没有 |

插件可在 `p` 上挂自己的字段（`chart`、`imgSel`、`logDiv`…）。切类型时壳会把常用引用置 `null`，但仍应在 `destroy` 里拆 DOM。

### 5.2 壳（`js/shell.js`）

| 函数 | 说明 |
|---|---|
| `makePanel(type)` | 默认 `image` |
| `forEachPanel(fn)` | 所有叶子 |
| `splitPanel(panelId, side)` | `left/right/up/down`，嵌套 tiling |
| `removePanel(panelId)` | 至少留一格 |
| `switchPanelType(p, newType)` | destroy → 清 body → mount |
| `resetAllViews()` | 各插件 `reset` |
| `clearPlots()` | 清空曲线/日志缓冲（watch 顶栏） |
| `onSenderChange()` | `GET /select?sender=` |
| `setConnected(bool, sender?)` | 顶栏状态 |
| `relayout()` | 窗口尺寸变化 |

SSE：`new EventSource('/events')`。`msg.type`：

| type | 动作 |
|---|---|
| `plot` | `addPoint(msg.ts, msg.data)` |
| `image` | `setImage(msg.jpg_b64, msg.meta)` |
| `log` | `addLog(msg.ts, msg.level, msg.msg)` |
| `status` | `setConnected` |
| `state` | 刷新 `#sender-sel` |

### 5.3 plot 插件 — 全局（replay 会写这些变量）

| 名字 | 含义 |
|---|---|
| `plotData` | `[{x, fields:{name:number}}]` |
| `fieldMeta` | `{name: {color}}` |
| `firstTs` | 第一个 plot 的 ns 时间戳；replay 会设成 `t0_ns` |
| `lastX` | 当前 x（秒） |
| `addPoint(ts_ns, data)` | 数字字段入曲线；`ts/_from` 等非数字跳过 |
| `ensureField(name)` | 分配颜色 |
| `rebuildAllCharts()` | 全量按 `plotData` 重建 |
| `trimData()` / `onSettingsChange()` | 历史窗口 / 滑动模式 |

侧栏字段勾选只作用于 **当前激活的 plot 面板**。

### 5.4 image 插件

| 名字 | 含义 |
|---|---|
| `imgSources` | `{name: {b64, ts, kb}}`，`b64` 已带 `data:image/jpeg;base64,` |
| `setImage(b64_raw, meta)` | `meta.name` 缺省 `"default"` |

### 5.5 log 插件

| 名字 | 含义 |
|---|---|
| `LOG_CAP` | 500 |
| `logBuffer` | `[{ts, level, msg}]` |
| `addLog(ts, level, msg)` | FATAL/CRITICAL 显示为 ERROR |
| `refreshAllLogPanels()` | 按筛选重画 |

### 5.6 replay.js 额外 HTTP

- `GET /api/meta` → duration、`t0_ns`、`series`、`frames[{i,t,meta}]`、`logs`
- `GET /api/frame/{i}` → JPEG 字节  
按时间找每路 `meta.name` 的最新帧，再 `setImage`。

快捷键：空格播放，←/→ 步进 0.05s。

---

## 6. 车上 → Host 事件形状（前端最终看到的）

与 UDP/`on_output` 一致，再经 SSE 推给浏览器：

```json
{"type":"plot","ts":123,"data":{"yaw":0.1,"_from":"sentry"}}
{"type":"log","ts":123,"level":"INFO","msg":"...","_from":"sentry"}
{"type":"image","ts":123,"meta":{"name":"reprojection","_from":"sentry"},"jpg_b64":"..."}
{"type":"state","active_sender":"sentry","senders":["sentry"]}
{"type":"status","connected":true,"sender":"sentry"}
```

`meta.name` 是图像路名（`raw` / `reprojection` / …），不是发送方。发送方是 `_from`。

---

## 7. 改代码时别动的契约

- `.rlog` RLG2 布局、UDP `0xFE` 分片图像（及旧 `0xFF`）：车上 `RemoteLogger` 与 host `net/`+`log/` 必须一起改。控制平面（beacon / 队列 / 转发 / `img_subscribe`）见 `PROTOCOL.md`。
- `replay.js` 用的全局函数名。
- 静态 URL 前缀 `/static/`。
- 标准库 only，不要为 host 加 pip 依赖。
