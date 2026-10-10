# Host HTTP API

浏览器 / 外部工具调门户时用这份。车端 UDP 控制与数据面见 [`PROTOCOL.md`](PROTOCOL.md)。  
Python 内部模块 API 见 [`DESIGN.md`](DESIGN.md) §4。用法入口见 [`HOST.md`](HOST.md)。

约定：

- Base URL：`http://<host>:8080`（`start.sh` 默认）
- 除 SSE / 图像帧外，响应多为 `application/json`
- Feature 实例前缀：`/api/i/<id>/…`（`<id>` 为 `POST /api/open` 返回的短 hex）
- Help 页 `/help` **不**创建实例、不占 UDP

---

## 1. Hub（全局）

### `GET /api/features`

列出可打开的 Feature 种类（不是运行中实例）。

```json
{ "features": [ { "id": "watch", "title": "Watch", "description": "…" }, … ] }
```

种类来自 `rdbg/features/registry.py` 的 `KINDS`（含 watch / calibrate / tfviz / replay / dump / netcheck）。**不含** help。

### `POST /api/open`

新建实例并启动线程。

请求：

```json
{ "feature": "watch", "config": { "sender": "sentry" } }
```

| 字段 | 说明 |
|------|------|
| `feature` 或 `id` | 种类，必须在 `KINDS` |
| `config` | 传给 `Feature.start`；Watch/标定/TF Viz 常用 `sender`；TF Viz 可 `spawn: true`、`config_path`；Replay 可 `path` |

成功：

```json
{
  "ok": true,
  "id": "a1b2c3d4e5",
  "feature": "watch",
  "path": "/i/a1b2c3d4e5",
  "status": { "instance": "…", "state": "running", "sender": "sentry", … }
}
```

标定 `path` 为 `/calibrate.html?i=<id>`。未知种类 → `404` `{ "ok": false, "error": "unknown feature" }`。

### `GET /api/instances`

当前所有实例的公开状态列表。

```json
{ "instances": [ { "instance", "feature", "title", "state", "path", "sender?", "data_port?", … } ] }
```

### `GET /api/instances/<id>/status`

单实例状态（等同公开字段）。未知 id → `404`。

### `POST /api/instances/<id>/start`

对已存在实例再次 `start(config)`（body 为 JSON config）。少用；一般靠 `/api/open`。

### `POST /api/instances/<id>/stop`

停止线程。可选 query：`?forget=1` 从 registry 移除（关页 `sendBeacon` 常用）。

```json
{ "ok": true, "id": "…", "state": "idle" }
```

### `GET /api/robots`

Hub 发现目录（周期 beacon，约 3 个周期无则剔除）。由 `RobotFleet` 挂载。

```json
{
  "robots": [
    {
      "name": "sentry",
      "ip": "192.168.1.10",
      "control": 15000,
      "app": "normal",
      "feature": "watch",
      "data_port": 15001,
      "peer_port": 15100,
      "watches": 1
    }
  ]
}
```

| 字段 | 说明 |
|------|------|
| `feature` | 门户应开的页（beacon `feature` 优先，否则由 `app` 映射） |
| `data_port` / `peer_port` | 已为该车分配则非 0；尚无 Watch 绑定时为 0 |
| `watches` | 当前挂在该车 fan 上的 SSE 订阅数 |

---

## 2. 实例前缀约定

打开后业务 API 均在：

```text
/api/i/<id>/…
```

前端用 `useInstance().base`（即该前缀）拼路径。

---

## 3. FleetBound：Watch / Calibrate / TF Viz

公共路由（`FleetBoundFeature.attach_fleet_routes`）：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/events` | **SSE**。连接后 `on_sse_connect`（推 state / 默认图像订阅） |
| POST | `/bind` | body `{ "sender" }` 或 `{ "robot" }`；Watch 绑一次不可换（`allow_rebind=false`）→ 冲突 `409` |
| GET | `/state` | 状态 + `senders` + `selected` |
| GET | `/select?sender=` | 可选重绑（仅 `allow_rebind`）；返回 `{ ok, sender, data_port? }` |
| GET | `/img_subscribe?streams=` | **仅 Watch**。逗号分隔话题名；空=退订。可选 query：`max_width`、`max_quality`、`max_fps`、`max_level`（远程 JPEG 上限，见 [`PROTOCOL.md`](PROTOCOL.md) §5）。触发车控面 `img_subscribe` |
| POST | `/record/start` | **仅 Watch**。body 可选 `{ "path" \| "dir" }`；默认写到 host 侧录制目录。返回 `{ ok, path, recording, … }` |
| POST | `/record/stop` | 停录制；返回 `{ ok, path, … }` |
| GET | `/record/status` | `{ recording, path, n_json, n_img, dropped, elapsed_s }` |

### SSE 事件（JSON 行，`data: …\n\n`）

常见 `type`：

| type | 含义 |
|------|------|
| `state` | `{ active_sender, senders[] }` |
| `status` | `{ connected, sender?, … }` |
| `plot` | `{ ts, data }` — `data` 可含标量、`markers`、`tf` / `frames` |
| `log` | 文本日志 |
| `image` | 图像元数据 + 载荷（实现相关） |
| `img_streams` | 当前订阅的图像流 |

### Watch

- `attach`：`select` + `img_subscribe` + `record/*`
- `config.sender`：开页时自动 bind
- `GET /api/instances/<id>/status` 的 `record`、`tx_profile`：录制状态与当前远程画质上限
- 工具栏：**录制**（SSE → 本机 RLG2，不占用车上队列）、**画质/流畅档**（改 `img_subscribe` 的 `max_*`）、标题栏 **cam / loop / tx / L*** 来自车上 ~1Hz plot 键

### Calibrate

- 另有：
  - `GET|POST /calib` — 标定命令（见 `calib_cmds`）
  - `GET|POST /done` — 结束/清理
- `default_img_streams = ["calibrate"]`
- `ui_path`：`/calibrate.html?i=<id>`（旧静态页）

### TF Viz

- 公共 fleet 路由；`config.spawn: true` 时可本机拉起 `build/tf_pub_test`
- 配置路径：`config.config_path` → env `RDBG_TF_PUB_CONFIG` → `configs/tf_pub.yaml`
- `status` 可含 `spawned`、配置路径等

---

## 4. Replay

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/events` | SSE（load 后推 state/status） |
| GET | `/meta` 或 `/session` | 会话公开元数据；无会话 `409` |
| GET | `/frame/<idx>` | JPEG 帧；无会话 `409`，越界 `404` |
| POST | `/load` | `{ "path" \| "rlog", "max_mb"? }` → `{ ok, meta }` |

`on_start` 若带 `path`/`rlog` 会直接加载。

---

## 5. Dump

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/run` | `{ "path"\|"rlog", "output"?\|"out_dir"?, "fps"? }` → `{ ok, job_id }` |
| GET | `/jobs/<job_id>` | `{ id, state: running\|done\|error, path, output, error, result }` |

默认输出目录：`<stem>_dump/`。`result` 为 `dump_rlog` 返回信息。

---

## 6. Netcheck

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/discover/start` | body 可选 `{ "port": 15999 }`；监听 beacon |
| POST | `/discover/stop` | 停 discover worker |
| GET | `/discover/beacons` | 已见 beacon 列表 |
| POST | `/echo/start` | 开 echo 服务（NCHK 协议） |
| POST | `/echo/stop` | |
| GET | `/echo/status` | |
| POST | `/ping` | 对目标发 ping，测往返 |
| GET | `/jobs/<id>` | 异步 job 状态（若使用） |

Netcheck **不**写入 `/api/robots`；通达度与日常发现目录是两套逻辑。

---

## 7. 前端封装

`host/ui/src/shared/api.ts`：

| 函数 | 对应 |
|------|------|
| `listFeatures` | `GET /api/features` |
| `listInstances` | `GET /api/instances` |
| `openFeature` | `POST /api/open` |
| `featureStatus` | `GET /api/instances/<id>/status` |
| `startFeature` / `stopFeature` | start / stop |
| `getJson` / `postJson` | 通用 |

实例页内再用 `base + "/events"` 等。Help 只用静态路由，不调用 `openFeature`。

---

## 8. 相关文档

| 文档 | 内容 |
|------|------|
| [`PROTOCOL.md`](PROTOCOL.md) | 车 ↔ Host UDP |
| [`DESIGN.md`](DESIGN.md) | Hub 结构、Python API |
| [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md) | 车上发送端 |
| 门户 Help `#api` | 图形化摘要 |
