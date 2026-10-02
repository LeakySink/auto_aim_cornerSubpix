/** Help 第 1–2 章：总览、网络与角色。 */
import {
  BeaconViz,
  Callout,
  CardGrid,
  DocPath,
  FlowArrow,
  FlowRow,
  MiniTable,
  Node,
  PageHead,
  Pipeline,
  Section,
  Steps,
} from "../shared";

export function OverviewPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="01 · 总览"
        title="先跑通三条线"
        lead="车端自瞄 + RemoteLogger，本机 Host 门户，浏览器看曲线/图/3D。Help 本身不占 Feature 线程、不占 UDP。"
      />

      <Section id="steps" title="上手三步">
        <Steps
          items={[
            {
              title: "车上开 debug",
              body: "例如 ./build/sentry_debug，yaml 里 remote_logger.enable_remote: true，并设好 sender_name。",
            },
            {
              title: "本机起 Host",
              body: "./host/start.sh → 浏览器打开 http://127.0.0.1:8080（改过 ui 需先 ./host/ui/build.sh）。",
            },
            {
              title: "点车或点功能",
              body: "车辆列表按 beacon 的 app/feature 开 Watch/标定/TF Viz；Replay、Dump、Netcheck、Help 从功能区进。",
            },
          ]}
        />
      </Section>

      <Section id="system" title="系统一张图">
        <FlowRow>
          <Node title="车端" sub="自瞄进程 + RemoteLogger" accent="car" pulse />
          <FlowArrow label="UDP beacon / data" animated />
          <Node title="Host Hub" sub="发现 · 开 Feature 线程" accent="host" />
          <FlowArrow label="HTTP / SSE" animated />
          <Node title="浏览器" sub="门户 SPA" accent="ui" />
        </FlowRow>
        <Callout tone="info" title="两个「本机」常见布局">
          调试 PC 同时跑 Host 与浏览器；车在同一局域网。手机只浏览也可以，但发现与 UDP 仍落在跑 Host 的那台机器上。
        </Callout>
      </Section>

      <Section id="home" title="首页结构">
        <CardGrid
          items={[
            {
              title: "功能区",
              tag: "launcher",
              body: "TF Viz / Replay / Dump / Netcheck / Help。除 Help 外每次 open 都新开标签 + 独立线程。",
            },
            {
              title: "车辆区",
              tag: "fleet",
              body: "来自 GET /api/robots（beacon 目录）。点一项 = open(feature) 并绑 sender=车名。",
            },
            {
              title: "已开页面",
              tag: "instances",
              body: "顶栏显示当前实例数；关标签页会 sendBeacon stop，对应线程退出。",
            },
          ]}
        />
      </Section>

      <Section id="docs-map" title="文档地图">
        <MiniTable
          headers={["想查什么", "去哪"]}
          rows={[
            ["怎么启动 / 参数", "`host/HOST.md`"],
            ["HTTP / 协议 / 设计 / 车上 / 自瞄", "左侧「完整文档」嵌全文"],
            ["图解流程", "左侧「图解指南」"],
          ]}
        />
        <p className="hp-footnote">
          本 Help 偏图形与流程；字段级细节以仓库 md 为准（<DocPath>host/*.md</DocPath>）。
        </p>
      </Section>

      <Section id="pipeline" title="数据旁路直觉">
        <Pipeline stages={["相机+IMU", "识别", "解算/EKF", "决策", "电控"]} />
        <p className="hp-footnote">主链路在车上；RemoteLogger 旁路把 plot / log / image / markers / tf 送到 Host Watch。</p>
      </Section>
    </div>
  );
}

export function NetworkPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="02 · 网络与角色"
        title="谁绑什么口、谁发什么"
        lead="车 DHCP；Host 用 host_id（UUID）进队列，不以 IP 当主键。门户 HTTP 与 UDP 发现/数据是两条线。"
      />

      <Section id="topo" title="拓扑">
        <div className="hp-topo">
          <div className="hp-topo-pane car">
            <h3>车</h3>
            <ul>
              <li>一直广播 beacon → LAN <code>:15999</code></li>
              <li>控制口默认 <code>:15000</code> 收 register / img_subscribe / head_alive</li>
              <li>数据只向<strong>队首</strong>单播一份（plot / log / 图 / hb）</li>
              <li>维护 host_id FIFO 队列</li>
            </ul>
          </div>
          <div className="hp-topo-pulse" aria-hidden>
            <span />
            <span />
            <span />
          </div>
          <div className="hp-topo-pane host">
            <h3>Host</h3>
            <ul>
              <li>Hub：全局发现口 <code>:15999</code> + HTTP <code>:8080</code></li>
              <li>每个 Watch 实例：数据口自 15001、对等口自 15100</li>
              <li>队首收车包并可转发给 follower</li>
              <li>follower 跟队首订阅，不向车要数据面</li>
            </ul>
          </div>
        </div>
      </Section>

      <Section id="ports" title="端口速查">
        <MiniTable
          headers={["口", "默认", "谁 bind", "用途"]}
          rows={[
            ["车控制", "`15000`", "仅车", "register / deregister / head_alive / img_subscribe / calib_cmd"],
            ["发现", "`15999`", "每个 Host", "车 beacon；Host 可发 who"],
            ["Host 数据", "`15001+`", "每个 Feature", "收 plot / 图像分片 / hb"],
            ["Host 对等", "`15100+`", "每个 Feature", "subscribe / promote / queue_update"],
            ["门户 HTTP", "`8080`", "Hub", "SPA + `/api/*` + SSE"],
          ]}
        />
      </Section>

      <Section id="app-map" title="beacon → 打开哪一页">
        <Callout tone="ok" title="优先级">
          有 <code>feature</code> 字段则直接用；否则由 <code>app</code> 映射：<code>normal→watch</code>，
          <code>calibrate→calibrate</code>，<code>tfviz→tfviz</code>。
        </Callout>
        <MiniTable
          headers={["app / feature", "首页点车打开", "典型二进制"]}
          rows={[
            ["`normal` / 缺省", "Watch", "`sentry_debug` 等"],
            ["`calibrate`", "标定页", "`./build/calibrate`"],
            ["`tfviz`", "TF Viz", "`tf_pub_test`（也可由 Hub spawn）"],
          ]}
        />
      </Section>

      <Section id="burden" title="负担怎么分">
        <CardGrid
          items={[
            {
              title: "车",
              tag: "light",
              body: "数据 1 份单播；beacon ~100B/s 常驻；成员变化时若干 promote / queue_update。",
            },
            {
              title: "队首 Host",
              tag: "heavy",
              body: "收车包 + 可选转发给订阅者；要维持 head_alive。",
            },
            {
              title: "Follower",
              tag: "peer",
              body: "只跟队首拉流；不向车发数据面，也不发 head_alive。",
            },
          ]}
        />
        <Callout tone="warn" title="禁止">
          车对 N 个 Host 发图像；非队首再转发（防环）；follower 向车灌数据面。
        </Callout>
      </Section>

      <Section id="beacon" title="发现仍在持续">
        <BeaconViz />
      </Section>
    </div>
  );
}
