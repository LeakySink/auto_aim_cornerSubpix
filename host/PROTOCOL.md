# Host ↔ 车 控制协议

用法见 [`HOST.md`](HOST.md)，host 模块 API 见 [`DESIGN.md`](DESIGN.md)，车上发送端见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

数据平面：plot/log JSON、图像 **`0xFE` UDP 分片**（兼容收旧 `0xFF` 单包）、`.rlog` RLG2。控制平面：车 DHCP；host 向车注册并排队；车只与 **队首** 发数据和心跳；后入调试机从队首拉流。车上数据面始终一份单播。**图像仅在有 host `img_subscribe` 时发送**；本地 `.rlog` 不受订阅影响。

车 yaml 里若仍留着 `remote_host` / `register_retry_ms`，会被忽略。

---

## 1. 模型

```
车 (DHCP，host_id 队列)
  beacon → LAN :15999
  plot / 图像 / hb  ──只──►  队首 Host A :15001
                               ├─ 本机浏览器 (SSE)
                               └─ 原样 UDP ──► Host B/C :15001

所有 host 向车 :15000 register（进队列）
后入者 role=follower，不向车要数据，向队首 :15100 subscribe
队首注销或超时 → 车出队，promote 新队首，其余改订
```

| 谁 | 负担 |
|---|---|
| 车 | 数据 1 份单播；全程 ~100B/s beacon（连上后不停）；成员变化时若干条 `promote` / `queue_update` |
| 队首 host | 收车包 + 转发给订阅者 |
| follower | 只跟队首，不跟车发 `head_alive` |

禁止：车对 N 个 host 发图像；非队首再转发（防环）；follower 向车发数据面。

---

## 2. 约定

| 项 | 值 |
|---|---|
| 传输 | UDP IPv4，局域网 |
| 控制 JSON | 必带 `"v": 1`、`"type"` |
| 车身份 | yaml `sender_name`（多车唯一）+ beacon `app`（程序类型） |
| host 身份 | 启动时生成 `host_id`（UUID 字符串）。**不以 IP 当主键**（双方都可能 DHCP） |
| 队列 | FIFO，下标 0 为队首。同 `host_id` 再 `register` 只更新地址，不换位置 |
| 字节序 | 图像头与现在相同：`ts` / 长度字段小端 |

### 端口

| 口 | 默认 | bind | 用途 |
|---|---|---|---|
| 车控制 | **15000** | 仅车 | host → 车：`register` / `deregister` / `head_alive` / `query_head` / `img_subscribe` |
| 发现 | **15999** | 每个 host | 车 → `255.255.255.255:15999`：`beacon`；host 可发 `who` |
| host 数据 | **15001** | 每个 host | 收车（队首）或队首转发的 plot / 图像 / `hb` |
| host 对等 | **15100** | 每个 host | `subscribe` / `promote` / `queue_update` / `peer_handoff` |

同一台机器开第二个 watch：用环境变量错开 `DATA_PORT` / `PEER_PORT`（实现时再接到脚本）。

车 yaml：

```yaml
remote_logger:
  control_port: 15000
  beacon_interval_ms: 1000
  heartbeat_interval_ms: 500
  head_timeout_ms: 2000
  sender_name: "sentry"
  app: "normal"              # normal=调试；calibrate=标定；tfviz=TF Viz（缺省 normal）
```

Beacon 还可带可选 `"feature":"tfviz"`（或 `watch` / `calibrate`）；host 优先用 `feature` 决定开哪一页。

---

## 3. 发现

车在 `enable_remote` 期间 **一直** 按 `beacon_interval_ms` 向 `255.255.255.255:15999` 发 beacon。队列空、已有队首、正在转发，都不停。不是「连上之前的握手」，而是常驻宣告。

必须持续广播的原因：

- 后入的 watch 没有历史状态，只能靠当前 beacon 发现车和其 `ip`
- 车 DHCP 换地址后，`beacon.ip` 是 host 改打 `register` 的唯一来源
- 多车时 watch 用持续 beacon 维持「场上有哪些 `name`」；某车进程退出，beacon 停，host 才把它从名单里拿掉
- 队首已连上也不能停：停了第二台调试机就看不见这辆车

```json
{"v":1,"type":"beacon","name":"sentry","app":"normal","feature":"watch","ip":"<车当前IPv4>","control":15000,"ts":...}
```

| 字段 | 含义 |
|---|---|
| `name` | `sender_name`，多车唯一，显示用 |
| `app` | 程序身份。`normal` / `calibrate` / `tfviz` 等；**缺省 / 旧固件无此字段时按 `normal`** |
| `feature` | **可选**。门户要打开的功能 id：`watch` / `calibrate` / `tfviz` …。有则优先用；无则由 host 从 `app` 映射（`normal→watch`，`calibrate→calibrate`，`tfviz→tfviz`） |
| `ip` | 车此刻收控制包的地址 |
| `control` | 控制口，默认 15000 |

`ip` 必须是车 **此刻** 准备收控制包的地址（每次发前读网卡，不要缓存开机时的 IP）。host 听到后向 `beacon.ip:control` 单播 `register`。已在队列里的 host 若发现 `ip` 变了，用同一 `host_id` 再 `register` 一次（只更新地址，不换队序）。门户车辆列表按 `feature`（或由 `app` 映射）打开对应功能，**不要**用 `name` 猜身份。

host 先于车启动时，可向 `255.255.255.255:15999` 探一次（车若也 bind 15999 则回；否则等下一次周期 beacon）：

```json
{"v":1,"type":"who","host_id":"..."}
```

车对该 host **单播** 一条等价 `beacon` 到其源地址（发现口或控制口均可，实现时固定回源端口）。`who` 是加速，不能替代周期广播。

`shutdown()` / `enable_remote=false` 后停止 beacon。host 超过约 3 个周期（默认 3s）听不到某 `name`，将该车标为离线；**不要**因此替车清 host 队列——队列只活在车上。

---

## 4. 注册与队列（所有 host → 车 :15000）

### host → 车

```json
{"v":1,"type":"register","host_id":"...","name":"lbw-pc","data_port":15001,"peer_port":15100}
{"v":1,"type":"deregister","host_id":"..."}
{"v":1,"type":"query_head","host_id":"..."}
{"v":1,"type":"json","host_id":"...","data":{}}
{"v":1,"type":"calib_cmd","host_id":"...","cmd":"add|calibrate|save|drop|reset|undistort|quit"}
```

`json`：发送方须已入队；车端把 `data` 入队，应用侧 `RemoteLogger::poll_json` 消费。host 用 `RobotClient.send_json(ip, control, data)`。

`calib_cmd`：旧标定按钮协议，保留兼容；新代码优先用 `json` + `data.cmd`。详见 `calibration/calibration.md`。

车侧队列元素：`{host_id, ip, data_port, peer_port}`。`ip` 取 `recvfrom` 源地址。

### 车 → 该 host（打到其 `peer_port`）

`register_ack` / `query_head` 回复同一形状。`query_head` 若 `host_id` 不在队列：`status=error`。已在队列则不改顺序。

队首：

```json
{"v":1,"type":"register_ack","status":"ok","role":"head",
 "robot":"sentry","queue":["idA","idB"]}
```

后入：

```json
{"v":1,"type":"register_ack","status":"ok","role":"follower",
 "robot":"sentry",
 "head":{"host_id":"idA","ip":"192.168.1.10","peer_port":15100,"data_port":15001},
 "queue":["idA","idB"]}
```

失败：

```json
{"v":1,"type":"register_ack","status":"error","message":"..."}
```

注销成功：

```json
{"v":1,"type":"deregister_ack","status":"ok"}
```

### 队列规则

| 事件 | 动作 |
|---|---|
| 队列空 + 第一条 `register` | 入队并成为队首；开始向其 `ip:data_port` 发数据 |
| `host_id` 已在队列 | 更新 `ip/data_port/peer_port`；按是否下标 0 再 ack `head`/`follower` |
| 新 `host_id` | 入队尾，`role=follower` |
| `deregister` | 删除；若是队首则 `promote` 新 `queue[0]`，其余 `queue_update` |
| 队列空 | 停数据 UDP；本地 `.rlog` 照写 |

---

## 5. 队首：数据 + 心跳

**车 → 队首 `:data_port`**（与现网完全相同）

```json
{"ts":..., "_from":"sentry", "yaw":0.1}
{"ts":..., "_from":"sentry", "level":"INFO", "msg":"..."}
{"hb":1, "_from":"sentry", "ts":...}
```

图像（默认，MTU-safe 分片，每片 ≤1200B）：

```
公共头: [1B 0xFE][8B ts][2B frame_seq][2B frag_idx][2B frag_cnt][1B from_len][from]
frag0+: [2B meta_len][meta][4B jpg_total][chunk...]
frag n: [chunk...]
```

字段一律小端。host 收齐后重组；缺片超时丢弃。旧版整包 `0xFF` 仍可解析。

目录（约 1s，有队首且车上出现过 `plot_image` 名时）：

```json
{"img_streams":["reprojection","result"],"_from":"sentry","ts":...}
```

**按需发图**：无人订阅时车不发图像 UDP（仍可写 `.rlog`）。host → 车：

```json
{"v":1,"type":"img_subscribe","host_id":"...","peer_port":15100,"streams":["reprojection"],
 "max_width":480,"max_quality":40,"max_fps":20,"max_level":1}
```

可选 **`max_width` / `max_quality` / `max_fps` / `max_level`**（整数）：远程 JPEG 上限，与车上自适应档位叠加（`max_level`≥0 表示至少降到该档）。本地 `.rlog` 仍只用 yaml `img_width`/`img_quality`。HTTP 侧见 [`API.md`](API.md) `GET …/img_subscribe?streams=&max_*`。

等价调试指令（队首已 `register`，`type=json` 的 `data`）：

```json
{"cmd":"set_img_tx","max_width":480,"max_quality":40,"max_fps":20,"level":1}
```

车端控制面收到后立即更新上限（不必等业务 `poll_json`）；业务侧 `io::RemoteDebug::install()` 通过 `set_json_callback` 同步处理同形 JSON。

`streams: []` 表示该 host 退订。车对队列内各 host 的订阅取并集；出队时清掉该 host 的订阅。ack：

```json
{"v":1,"type":"img_subscribe_ack","status":"ok","streams":["reprojection"]}
```

心跳间隔 `heartbeat_interval_ms`（默认 500）。host 侧仍丢弃带 `"hb"` 的 JSON，只当链路活着。

**远程 JPEG 档位 0–3**（仅 UDP；本地 `.rlog` 始终 yaml）：

| level | 典型宽×质×fps（相对 yaml 封顶） |
|-------|----------------------------------|
| 0 | yaml 宽/质，30fps |
| 1 | ≤480×≤40，20fps |
| 2 | ≤320×≤30，15fps |
| 3 | ≤320×≤25，10fps |

车上按 **1s 窗口**统计非阻塞发送成败自适应升降档；Host `max_*` / `max_level` 再封顶。与 yaml 参数相同时只编码一次 JPEG。实现细节见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

**数据面非阻塞**：车向队首 `sendto`/`sendmsg` 使用 `MSG_DONTWAIT`；缓冲满或弱网时 **丢远程包**，不阻塞本地 `.rlog`（disk-first 入队）。

**诊断 plot 键（~1Hz，普通 plot JSON）**：`cam_fps`、`loop_fps`（主循环）、`img_tx_fps`、`img_tx_level`（远程出图）。来源 `io::FpsMeter` / `io::RemoteDebug`，**不是** CAN Command。字段说明见 [`../REMOTE_LOGGER.md`](../REMOTE_LOGGER.md) §诊断曲线。

### plot.markers（marker_v1）

Watch 3D 用。仍走普通 plot JSON（Host 发射后仍是 `type:"plot"`），**不是**独立 UDP/SSE 类型。

```json
{
  "ts": 123, "_from": "sentry",
  "x": 1.0, "vx": 0.1,
  "markers": {
    "schema": "marker_v1",
    "frame_id": "world",
    "items": [
      {
        "ns": "kalman.center", "id": "c", "type": "sphere",
        "pose": { "p": [1, 0, 0.5], "q": [1, 0, 0, 0] },
        "scale": [0.04, 0.04, 0.04],
        "color": [1, 0.45, 0.1, 1]
      }
    ]
  }
}
```

| 字段 | 约束 |
|------|------|
| `schema` | 必须为 `"marker_v1"`；否则 Watch 忽略 `markers`（曲线字段照常） |
| `frame_id` | **可选**，默认 `"world"`；空串按默认 |
| `items` | 数组；可空 |
| `ns` / `id` / `type` | 单条必填；`ns` 为图层键 |
| 单条 `frame_id` | 可选；缺省继承 array；再缺省 `"world"` |
| `pose.p` / `pose.q` | 米；四元数 Eigen `(w,x,y,z)` |
| `dir` / `shaft_len` | `arrow` |
| `points` | `line_list`，成对点 |
| `scale` / `color` | 随 type；RGBA 0–1 |

图元：`sphere`、`arrow`、`box`、`line_list`（`axes` 预留）。未知 `type` 跳过该条。

**合并：** Watch 按 `ns` 缓存；本包出现的每个 `ns` 整组替换；未出现的 ns 保留（多发布者可交错）。根字段 `markers_reset: true` 可清空缓存。

**可选同包：**

- `frames`: `{ "gimbal": { "parent":"world", "R":[9], "t":[3] }, ... }`（child→parent）
- `tf`: 与 TF Viz 相同的外参包；Watch 可 ingest 进 FrameStore

车端 API：定义 [`tools/rdbg/markers/viz_markers.hpp`](../tools/rdbg/markers/viz_markers.hpp)；自瞄转换 [`tasks/auto_aim/kalman_markers.hpp`](../tasks/auto_aim/kalman_markers.hpp)。用法见 [`REMOTE_LOGGER.md`](../REMOTE_LOGGER.md)。

**仅队首 → 车 `:15000`**

```json
{"v":1,"type":"head_alive","host_id":"idA","data_port":15001,"peer_port":15100}
```

间隔与车心跳相同。车超过 `head_timeout_ms`（默认 2000，约 4 个心跳）未收到 → 队首死亡：出队，`promote` 下一个。

follower **禁止**发 `head_alive`。follower 可以呆在队列里不保活；轮到它当队首再因 `head_alive` 失败而出队。

---

## 6. 队首切换

`promote` / `queue_update` 打到对方 **`peer_port`**（host 不听 15000）。

车 → 新队首：

```json
{"v":1,"type":"promote","robot":"sentry","role":"head","queue":["idB","idC"]}
```

车 → 其余仍在队列的成员：

```json
{"v":1,"type":"queue_update","robot":"sentry",
 "head":{"host_id":"idB","ip":"...","peer_port":15100,"data_port":15001},
 "queue":["idB","idC"]}
```

新队首：若正在 subscribe 旧队首则立刻停；开始接受 `subscribe`；车已把数据 `sendto` 切过来。

旧队首 **干净退出** 时，先向已订阅者发，再向车 `deregister`：

```json
{"v":1,"type":"peer_handoff","robot":"sentry",
 "head":{"host_id":"idB","ip":"...","peer_port":15100}}
```

崩溃没有 `peer_handoff`：靠车超时 + `queue_update`；或 follower 数据口 3s 无包后 `query_head`。

---

## 7. Host 对等（后入者只跟队首）

follower 收到 `role=follower` 后 → 队首 `:peer_port`：

```json
{"v":1,"type":"subscribe","host_id":"idB","robot":"sentry","data_port":15001}
{"v":1,"type":"unsubscribe","host_id":"idB","robot":"sentry"}
```

队首 → follower 的 `peer_port`：

```json
{"v":1,"type":"subscribe_ack","status":"ok","robot":"sentry"}
```

之后队首把 **从车上收到的每一个数据 UDP 原样** `sendto` 到该 follower 的 `ip:data_port`（不解码 JSON / JPEG）。follower 的收包路径与直接收车相同。

follower 每 1s 重发 `subscribe` 保活；队首 3s 无刷新则丢掉该订阅（不影响车上队列）。

可选，队首偶尔向 subscriber 的 `peer_port` 推：

```json
{"v":1,"type":"peer_state","robot":"sentry","queue":["idA","idB"]}
```

供本机 UI 显示队列。没有也可。

---

## 8. 时序

第一台 watch：

```
车 --beacon--> LAN :15999
H1 --register--> 车 :15000
车 --register_ack role=head--> H1 :15100
车 --plot/img/hb--> H1 :15001
H1 --head_alive--> 车 :15000
车 --beacon--> LAN          （不停，H2 靠这个发现）
```

第二台：

```
H2 --register--> 车
车 --register_ack role=follower, head=H1--> H2
H2 --subscribe--> H1 :15100
H1 --subscribe_ack--> H2
H1 --原样 UDP--> H2 :15001
（车仍然只打给 H1）
```

H1 注销：

```
H1 --peer_handoff--> 已订阅者
H1 --deregister--> 车
车出队 H1
车 --promote--> H2 :15100
车 --queue_update--> 其余
车改 sendto H2 :15001
H2 开始转发；其余改 subscribe H2
```

---

## 9. 状态机

### 车

| 状态 | 数据 sendto | `head_alive` | beacon |
|---|---|---|---|
| `queue empty` | 无 | 忽略 | 发 |
| `has head` | 仅 `queue[0].ip:data_port` | 刷新超时计时 | 发 |
| 超时或队首 `deregister` | pop；空则停；否则 `promote` + `queue_update` | — | 发 |

### host

| 状态 | 动作 |
|---|---|
| `idle` | 听 `:15999` beacon |
| `head` | bind 15001+15100；发 `head_alive`；转发到订阅者 |
| `follower` | bind 15001；向 head `subscribe`；不发 `head_alive` |
| 收到 `promote` | follower → head：停 subscribe，开始转发 |
| `lost_head` | 停转发流；`query_head`；按 ack 切 `head` / `follower` |

---

## 10. 多车

每辆车自己一条 host 队列、自己的 beacon `name`。控制 JSON 带 `"robot"`。一台 watch 可对多车分别 `register`（v1 实现可先只挂一辆）。

---

## 11. 代码落点

| 位置 | 职责 |
|---|---|
| `tools/remote_logger.hpp` | 对外 API（`init` / `plot` / `log` / `plot_image`） |
| `tools/rdbg/transport` | L0 UDP |
| `tools/rdbg/control` | L1 beacon + host 队列 |
| `tools/rdbg/data` | L2 只向队首发 JSON/分片图像/hb/目录 |
| `tools/rdbg/session` | L3 本地 `.rlog` |
| `tools/rdbg/image` + `engine` | 30fps 邮箱、订阅门控、worker |
| `host/rdbg/net/control.py` | 发现 + 向车 `register` / `head_alive` / `img_subscribe` |
| `host/rdbg/net/peer.py` | `subscribe` / 原样转发 / `handoff` |
| `host/rdbg/net/udp.py` | 分片重组 + 数据面解析 + `on_raw` 转发 |

---

## 12. 与旧版差异

| | 旧版 | 现在 |
|---|---|---|
| 谁监听 15000 | host | 车 |
| yaml `remote_host` | 必须 | 忽略 |
| 数据目的地 | 唯一那台 host | 当前队首 |
| 第二台 watch | 收不到 | 队首转发 |
| 发现 | 无 | beacon `:15999`（连上后不停） |
