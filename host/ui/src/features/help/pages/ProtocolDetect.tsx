/** Help 第 3–4 章：协议时序、Host 检测（发现/离线/队首）。 */
import {
  Callout,
  CardGrid,
  DocPath,
  FlowArrow,
  FlowRow,
  MiniTable,
  Node,
  PageHead,
  QueueViz,
  Section,
  SeqDiagram,
  Steps,
} from "../shared";

export function ProtocolPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="03 · 协议与通信"
        title="控制面改绑定，数据面推内容"
        lead="JSON 控制报文必带 v=1 与 type。图像默认不推：仅当 Host img_subscribe 了话题名才 UDP 分片。细节见 host/PROTOCOL.md。"
      />

      <Section id="seq" title="总时序">
        <SeqDiagram
          rows={[
            { from: "车", fromKind: "car", msg: "beacon(name, app, feature, ip, control)", to: "Host :15999", toKind: "host" },
            { from: "浏览器", fromKind: "ui", msg: "POST /api/open → 分配 data/peer 口", to: "Hub", toKind: "host" },
            { from: "Feature", fromKind: "host", msg: "register(host_id, data_port, peer_port)", to: "车 :15000", toKind: "car" },
            { from: "车", fromKind: "car", msg: "入队；队首开始收 plot/log/image/hb", to: "队首 data_port", toKind: "host" },
            { from: "Watch UI", fromKind: "ui", msg: "勾选话题 → img_subscribe", to: "车", toKind: "car" },
            { from: "车", fromKind: "car", msg: "0xFE JPEG 分片（仅已订阅流）", to: "队首", toKind: "host" },
          ]}
        />
      </Section>

      <Section id="planes" title="两平面">
        <CardGrid
          items={[
            {
              title: "控制面",
              tag: "ctrl",
              meta: ":15000 / peer",
              body: "register · deregister · head_alive · query_head · img_subscribe · calib_cmd · json 下行 · promote / queue_update",
            },
            {
              title: "数据面",
              tag: "data",
              meta: "data_port",
              body: "plot JSON（可含 markers / tf）· log · hb · 图像 0xFE 分片（兼容旧 0xFF）",
            },
            {
              title: "本地落盘",
              tag: "disk",
              meta: ".rlog",
              body: "enable_local 时 RLG2 会话；图像本地写不受 img_subscribe 影响。",
            },
          ]}
        />
      </Section>

      <Section id="msgs" title="报文角色（常用）">
        <MiniTable
          headers={["type", "方向", "作用"]}
          rows={[
            ["`beacon`", "车 → :15999", "常驻宣告 name/app/ip"],
            ["`who`", "Host → :15999", "加速发现；车单播回一条等价 beacon"],
            ["`register`", "Host → 车", "入队；同 host_id 再注册只更新地址"],
            ["`head_alive`", "队首 → 车", "保活；超时出队并 promote"],
            ["`img_subscribe`", "Host → 车", "声明要哪些图像话题"],
            ["`hb` / plot / log", "车 → 队首", "心跳与调试数据"],
            ["`subscribe`", "follower → 队首 peer", "后入调试机拉流"],
          ]}
        />
        <p className="hp-footnote">
          完整字段表见 <DocPath>host/PROTOCOL.md</DocPath>。yaml 里旧字段 <code>remote_host</code> / <code>register_retry_ms</code> 会被忽略。
        </p>
      </Section>

      <Section id="img-sub" title="图像订阅">
        <FlowRow>
          <Node title="Watch 侧栏" sub="勾选 meta.name" accent="ui" />
          <FlowArrow label="img_subscribe" animated />
          <Node title="车 RemoteLogger" sub="仅这些流 UDP" accent="car" />
          <FlowArrow label="0xFE ≤1200B" animated />
          <Node title="队首 → SSE" sub="浏览器出图" accent="host" />
        </FlowRow>
        <Callout tone="info" title="为什么默认不推图">
          省带宽。曲线/日志可一直走；图要你显式订阅。主线程还会按 ~30fps 相位选帧，未入选直接丢。
        </Callout>
      </Section>
    </div>
  );
}

export function DetectPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="04 · Host 检测逻辑"
        title="发现 · 离线 · 队首，三件不同的事"
        lead="「检测」= 看见车 + 判定列表在线 + 决定谁收数。Netcheck 是主动通达度测试，不替代日常 beacon 目录。"
      />

      <Section id="discover" title="1. 发现环">
        <FlowRow>
          <Node title="车" sub="beacon_interval_ms（默认 1s）" accent="car" pulse />
          <FlowArrow label="UDP :15999" animated />
          <Node title="Hub 目录" sub="GET /api/robots" accent="host" />
          <FlowArrow label="~2s 刷新" animated />
          <Node title="首页车辆列表" accent="ui" />
        </FlowRow>
        <Callout tone="info" title="为什么连上了还要广播">
          后入 Watch 没有历史；车 DHCP 换 IP 只能靠 beacon.ip；多车名单靠持续宣告；第二台调试机也要看见这辆车。
        </Callout>
      </Section>

      <Section id="offline" title="2. 列表离线">
        <Steps
          items={[
            { title: "超时", body: "约 3 个 beacon 周期（默认约 3s）听不到同 name → 标离线。" },
            { title: "不清队列", body: "Host 不要因此替车清 host 队列——队列只活在车上。" },
            { title: "进程退出", body: "车停进程 → beacon 停 → 名单消失/离线。" },
          ]}
        />
      </Section>

      <Section id="bind" title="3. 开页绑定">
        <Steps
          items={[
            { title: "点车", body: "resolvePortalFeature(feature|app) → POST /api/open({ sender })。" },
            { title: "分配口", body: "该车一对 data_port / peer_port（同车多 Watch 共用；全关才释放）。" },
            { title: "register", body: "Feature 向 beacon.ip:control 注册；FIFO，下标 0 为队首。" },
            { title: "收数条件", body: "成为队首才直接收车数据面；否则角色 follower，向队首订阅。" },
          ]}
        />
      </Section>

      <Section id="head" title="4. 队首存活 ≠ 列表在线">
        <div className="hp-split">
          <div className="hp-split-pane">
            <strong>列表离线</strong>
            <p>Hub 听不见 beacon →「看不见车」。</p>
            <p className="hp-warn">目录语义；不动车上队列。</p>
          </div>
          <div className="hp-split-pane">
            <strong>队首存活</strong>
            <p>队首需 head_alive；超时 drop_head / promote →「谁收 plot」。</p>
            <p className="hp-warn">队列语义；与名单是否灰掉无关。</p>
          </div>
        </div>
        <QueueViz />
      </Section>

      <Section id="netcheck" title="5. Netcheck">
        <CardGrid
          items={[
            { title: "discover", tag: "主动", body: "扫网上谁在响应，用于通达度，不是日常车辆目录。" },
            { title: "echo / ping", tag: "主动", body: "确认 UDP 往返；排障「同网不通」时用。" },
            { title: "和 beacon 的关系", tag: "边界", body: "Netcheck 成功 ≠ 会出现在 /api/robots；日常列表只认周期 beacon。" },
          ]}
        />
      </Section>

      <Section id="online-table" title="谁在听 · 什么叫在线">
        <MiniTable
          headers={["谁", "听什么", "算「在线」"]}
          rows={[
            ["Hub", "`:15999` beacon", "近期收到同 `name`"],
            ["队首 Feature", "`data_port` + `head_alive`", "队列头且未超时"],
            ["Follower", "队首 peer 转发", "订到队首即可看流"],
            ["Netcheck", "主动探测", "通达度；不进日常列表逻辑"],
          ]}
        />
      </Section>
    </div>
  );
}
