import {
  Callout,
  CardGrid,
  DocPath,
  FlowArrow,
  FlowRow,
  MiniTable,
  Node,
  PageHead,
  Section,
  SeqDiagram,
  Steps,
} from "../shared";

/** Help ch.11 — Hub / Feature HTTP API（细节以 host/API.md 为准）。 */
export function ApiPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="11 · HTTP API"
        title="门户怎么调后端"
        lead="Hub 管种类与实例；业务在 /api/i/<id>/…。车端 UDP 不在这里。完整字段表见 host/API.md。"
      />

      <Section id="layers" title="分层">
        <FlowRow>
          <Node title="浏览器" sub="SPA / fetch / SSE" accent="ui" />
          <FlowArrow label="HTTP :8080" animated />
          <Node title="Hub" sub="/api/features · open · robots" accent="host" />
          <FlowArrow label="/api/i/&lt;id&gt;" animated />
          <Node title="Feature 线程" sub="Watch / Replay / …" accent="host" pulse />
        </FlowRow>
        <Callout tone="info" title="Help 例外">
          <code>/help</code> 纯前端，不走 <code>POST /api/open</code>，无实例、无 UDP。
        </Callout>
      </Section>

      <Section id="open-seq" title="开页时序">
        <SeqDiagram
          rows={[
            { from: "UI", fromKind: "ui", msg: "POST /api/open { feature, config }", to: "Hub", toKind: "host" },
            { from: "Hub", fromKind: "host", msg: "new thread · attach /api/i/<id>/*", to: "Feature", toKind: "host" },
            { from: "UI", fromKind: "ui", msg: "打开 path（/i/<id> 或 calibrate.html）", to: "浏览器", toKind: "ui" },
            { from: "页", fromKind: "ui", msg: "GET …/events（SSE）· bind / load …", to: "实例", toKind: "host" },
            { from: "页", fromKind: "ui", msg: "关页 sendBeacon stop?forget=1", to: "Hub", toKind: "host" },
          ]}
        />
      </Section>

      <Section id="hub" title="Hub 全局">
        <MiniTable
          headers={["方法", "路径", "作用"]}
          rows={[
            ["GET", "`/api/features`", "种类列表（KINDS）"],
            ["POST", "`/api/open`", "新建实例；返回 id / path / status"],
            ["GET", "`/api/instances`", "运行中实例"],
            ["GET", "`/api/instances/<id>/status`", "单实例状态"],
            ["POST", "`/api/instances/<id>/stop`", "停线程；`?forget=1` 移除"],
            ["POST", "`/api/instances/<id>/start`", "再次 start(config)"],
            ["GET", "`/api/robots`", "beacon 车辆目录"],
          ]}
        />
        <Callout tone="ok" title="open 的 config 常用键">
          Watch/标定/TF：<code>sender</code>。TF：<code>spawn</code>、<code>config_path</code>。Replay：<code>path</code> / <code>rlog</code>。
        </Callout>
      </Section>

      <Section id="fleet" title="FleetBound（Watch / 标定 / TF Viz）">
        <MiniTable
          headers={["方法", "路径", "说明"]}
          rows={[
            ["GET", "`/events`", "SSE：plot / log / image / state …"],
            ["POST", "`/bind`", "`{ sender }`；Watch 不可换绑"],
            ["GET", "`/state`", "status + senders + selected"],
            ["GET", "`/select?sender=`", "允许 rebind 时换车"],
            ["GET", "`/img_subscribe?streams=`", "Watch；可选 max_width/quality/fps/level"],
            ["POST", "`/record/start|stop`", "Watch Host 侧 RLG2 录制"],
            ["GET", "`/record/status`", "recording · path · n_json/n_img"],
          ]}
        />
        <p className="hp-footnote">上表路径均相对 <code>/api/i/&lt;id&gt;</code>。标定另有 <code>/calib</code>、<code>/done</code>。</p>
        <CardGrid
          items={[
            {
              title: "SSE type",
              tag: "events",
              body: "state · status · plot（含 markers/tf）· log · image · img_streams",
            },
            {
              title: "robots 字段",
              tag: "fleet",
              body: "name · ip · control · app · feature · data_port · peer_port · watches",
            },
          ]}
        />
      </Section>

      <Section id="others" title="Replay / Dump / Netcheck">
        <div className="hp-dev-steps">
          <div className="hp-dev-step">
            <span className="hp-dev-n">R</span>
            <div>
              <strong>Replay</strong>
              <p>
                <code>POST /load</code> · <code>GET /meta|/session</code> · <code>GET /frame/&lt;idx&gt;</code> ·{" "}
                <code>GET /events</code>
              </p>
            </div>
          </div>
          <div className="hp-dev-step">
            <span className="hp-dev-n">D</span>
            <div>
              <strong>Dump</strong>
              <p>
                <code>POST /run</code> → <code>job_id</code>；<code>GET /jobs/&lt;id&gt;</code> 查 running/done/error
              </p>
            </div>
          </div>
          <div className="hp-dev-step">
            <span className="hp-dev-n">N</span>
            <div>
              <strong>Netcheck</strong>
              <p>
                <code>discover/start|stop|beacons</code> · <code>echo/*</code> · <code>POST /ping</code> — 不进 robots 目录
              </p>
            </div>
          </div>
        </div>
      </Section>

      <Section id="fe" title="前端封装">
        <Steps
          items={[
            {
              title: "shared/api.ts",
              body: "listFeatures / openFeature / listInstances / featureStatus / stopFeature / getJson / postJson",
            },
            {
              title: "实例页",
              body: "useInstance().base 拼 /events、/bind 等；关页 stop?forget=1",
            },
            {
              title: "仓库文档",
              body: "字段级以 host/API.md 为准；UDP 见 PROTOCOL.md；扩展见 DESIGN.md",
            },
          ]}
        />
        <p className="hp-footnote">
          链到 <DocPath>host/API.md</DocPath> · <DocPath>host/PROTOCOL.md</DocPath> · <DocPath>host/DESIGN.md</DocPath>
        </p>
      </Section>
    </div>
  );
}
