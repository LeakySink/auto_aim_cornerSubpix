/** Help 第 5–7 章：功能用法、插件扩展、车上 RemoteLogger。 */
import {
  Callout,
  CardGrid,
  CodeBlock,
  DocPath,
  FlowArrow,
  FlowRow,
  MiniTable,
  Node,
  PageHead,
  Section,
  Steps,
} from "../shared";

export function FeaturesPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="05 · 功能用法"
        title="Watch 到 Netcheck"
        lead="绑车功能走 FleetBoundFeature；本地工具不绑车。关标签页 = stop 线程。Watch 3D 吃 marker_v1，与 TF Viz 分工不同。"
      />

      <Section id="list" title="功能一览">
        <CardGrid
          items={[
            {
              title: "Watch",
              tag: "绑车",
              meta: "主战场",
              body: "曲线、日志、图像；工作台可加 3D 面板。收到合法 markers 时自动插一格 3D。侧栏按 ns 开关图层，选 display_frame（默认 world）。",
            },
            {
              title: "Calibrate",
              tag: "绑车",
              meta: "app=calibrate",
              body: "与 Watch 同壳、标定布局与 calib_cmd。车上跑 calibrate，首页点车直达。",
            },
            {
              title: "TF Viz",
              tag: "绑车",
              meta: "可 spawn",
              body: "看相机/枪管外参与 TF。无发布者时 Hub 可本机拉起 build/tf_pub_test（默认 configs/tf_pub.yaml）。",
            },
            {
              title: "Replay",
              tag: "本地",
              body: "输入 .rlog 路径加载：时间轴、曲线、图像、日志。不占车上队列。",
            },
            {
              title: "Dump",
              tag: "本地",
              body: "导出 log.txt / plot.txt / images.mp4（需 opencv-python-headless）。也可命令行 dump.sh。",
            },
            {
              title: "Netcheck",
              tag: "探测",
              body: "discover / echo / ping。查链路，不替代 beacon 车辆列表。",
            },
          ]}
        />
      </Section>

      <Section id="watch3d" title="Watch 3D 要点">
        <Steps
          items={[
            { title: "面板类型", body: "工作台格子可选「3D」；有 marker_v1 时也会自动插入。" },
            { title: "坐标系", body: "marker.frame_id 缺省按 world；用 FrameStore / TF 变到 display_frame。" },
            { title: "图层", body: "按 ns 开关（例如关掉 kalman.vel）；与 TF Viz「专用外参页」互补。" },
          ]}
        />
        <Callout tone="info" title="和 TF Viz 怎么选">
          通用 Marker / 轨迹 / 文字 → Watch 3D。只盯相机相对世界、枪管外参调试 → TF Viz。
        </Callout>
      </Section>

      <Section id="lifecycle" title="生命周期（使用者）">
        <FlowRow>
          <Node title="点功能 / 点车" accent="ui" />
          <FlowArrow label="POST /api/open" animated />
          <Node title="新标签 /i/&lt;id&gt;" sub="独立线程" accent="host" />
          <FlowArrow label="关页 sendBeacon" animated />
          <Node title="stop" sub="释放口（末个）" accent="muted" />
        </FlowRow>
        <p className="hp-footnote">同一辆车多个 Watch 共用 data/peer 口；全部关掉后才释放。</p>
      </Section>
    </div>
  );
}

export function PluginPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="06 · Host 插件"
        title="怎么用 · 怎么加"
        lead="Hub 单进程；每种功能一类 Feature，每开一页一条线程。扩展清单在 DESIGN.md §0 与两边 registry。"
      />

      <Section id="user" title="使用者视角">
        <Steps
          items={[
            { title: "打开", body: "首页 → POST /api/open → 新窗口 /i/<id> → FeatureRegistry 起线程。" },
            { title: "多开", body: "每个标签独立实例与 API 前缀 /api/i/<id>/…。" },
            { title: "关闭", body: "关页 sendBeacon stop（可 forget）；线程退出。" },
            {
              title: "绑不绑车",
              body: "FleetBoundFeature：Watch / Calibrate / TF Viz。纯本地：Replay / Dump / Netcheck。Help 根本不走 open。",
            },
          ]}
        />
      </Section>

      <Section id="dev" title="开发者：加插件三步">
        <div className="hp-dev-steps">
          <div className="hp-dev-step">
            <span className="hp-dev-n">1</span>
            <div>
              <strong>后端</strong>
              <p>
                <DocPath>features/foo.py</DocPath> 继承 <code>Feature</code> 或 <code>FleetBoundFeature</code>，
                <code>attach</code> 里用 <code>self.api_prefix</code> 挂路由；写入{" "}
                <DocPath>features/registry.py</DocPath> 的 <code>KINDS</code>。
              </p>
            </div>
          </div>
          <div className="hp-dev-step">
            <span className="hp-dev-n">2</span>
            <div>
              <strong>前端</strong>
              <p>
                <DocPath>ui/src/features/foo/FooPage.tsx</DocPath> +{" "}
                <DocPath>registry.ts</DocPath> 的 <code>FEATURE_MODULES</code>。用{" "}
                <code>useInstance().base</code> 调本实例 API。
              </p>
            </div>
          </div>
          <div className="hp-dev-step">
            <span className="hp-dev-n">3</span>
            <div>
              <strong>构建运行</strong>
              <p>
                <code>./host/ui/build.sh</code> → <code>./host/start.sh</code>。产物在{" "}
                <DocPath>rdbg/static_ui/</DocPath>。
              </p>
            </div>
          </div>
        </div>
        <Callout tone="ok" title="FleetBoundFeature 共用">
          fleet 绑定、<code>/events</code> SSE、<code>/bind</code>、<code>/select</code>、<code>/state</code>。
          子类用 <code>allow_rebind</code> / <code>default_img_streams</code> / <code>use_source_push_state</code> 区分。
        </Callout>
      </Section>

      <Section id="kinds" title="现有插件">
        <MiniTable
          headers={["id", "绑车", "典型 API / 行为"]}
          rows={[
            ["`watch` / `calibrate`", "是", "status · layout · image_sub · SSE；标定另有 calib_cmd"],
            ["`tfviz`", "是", "TF 树 + 场景；可选 spawn tf_pub_test"],
            ["`replay`", "否", "加载本地 .rlog"],
            ["`dump`", "否", "导出 txt / mp4"],
            ["`netcheck`", "否", "discover / echo / ping"],
            ["`help`", "—", "纯前端 /help，无 Hub 线程"],
          ]}
        />
        <p className="hp-footnote">
          分层禁区见 <DocPath>host/DESIGN.md</DocPath>：<code>ui/</code> 不直接碰 UDP，<code>net/</code> 不知道 HTTP。
        </p>
      </Section>
    </div>
  );
}

export function VehiclePage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="07 · RemoteLogger"
        title="车上 API 与特性"
        lead="单例 tools::RemoteLogger：只 include remote_logger.hpp。Host 来排队，数据只发给队首；本地 .rlog 与 UDP 可分开开关。"
      />

      <Section id="life" title="生命周期">
        <Steps
          items={[
            {
              title: "init",
              body: "init(yaml路径) 或 init(Config)。读 remote_logger 段；缺键则失败退出。启动 var/img/disk/ctrl worker。",
            },
            {
              title: "发送 / 轮询",
              body: "plot / log / plot_image；主循环里 poll_json（或旧 poll_calib_cmd）。",
            },
            {
              title: "shutdown",
              body: "停 beacon、排空落盘、强制 sync。名单约 3s 后离线。析构也会走清理。",
            },
          ]}
        />
        <CodeBlock>{`#include "tools/remote_logger.hpp"

tools::RemoteLogger::instance().init("configs/sentry.yaml");

auto& rl = tools::RemoteLogger::instance();
rl.plot({{"pitch", 0.15}, {"yaw", -0.3}});
rl.log("INFO", "target locked");
rl.log("ERROR", "motor {} fail, code={}", 3, 0x1F);
rl.plot_image(frame, {{"name", "front"}});

nlohmann::json msg;
while (rl.poll_json(msg)) { /* Host type=json 的 data */ }

rl.shutdown();`}
        </CodeBlock>
      </Section>

      <Section id="api" title="对外 API">
        <MiniTable
          headers={["API", "作用"]}
          rows={[
            ["`instance()`", "进程内单例"],
            ["`init(Config|path)`", "读配置、起 worker、可选开 beacon"],
            ["`plot(json)`", "标量/嵌套 JSON → UDP(队首) + .rlog；可含 markers/tf"],
            ["`log(level, fmt, …)`", "stderr + 远程；fmt；level 决定落盘优先级"],
            ["`plot_image(Mat, meta)`", "按 meta.name ~30fps 选帧 → JPEG；UDP 仅已订阅流"],
            ["`poll_json(json&)`", "消费 Host 下行 type=json"],
            ["`poll_calib_cmd(string&)`", "旧标定指令，兼容保留"],
            ["`shutdown()`", "停远程、冲刷落盘"],
          ]}
        />
        <Callout tone="info" title="头文件边界">
          调用方只碰 <DocPath>tools/remote_logger.hpp</DocPath>。实现在 <DocPath>tools/rdbg/</DocPath>
          （engine / session / data / control / transport）。
        </Callout>
      </Section>

      <Section id="channels" title="五种数据通道">
        <CardGrid
          items={[
            {
              title: "plot（变量）",
              tag: "var",
              body: "任意 JSON。自动注入 ts、_from。扁平数字进 Watch 曲线；保留键 markers / tf / frames 给 3D。",
            },
            {
              title: "log（文本）",
              tag: "text",
              body: "内部转成带 level/msg 的 plot。同时打 stderr。ERROR/FATAL 落盘紧急 sync。",
            },
            {
              title: "plot_image",
              tag: "img",
              body: "主线程相位锁选帧；clone 进深 1 邮箱；img_worker JPEG。未订阅不发 UDP；.rlog 仍可写。",
            },
            {
              title: "heartbeat",
              tag: "hb",
              body: "heartbeat_interval_ms>0 时自动 {hb:1,_from,ts} 给队首。",
            },
            {
              title: "下行 JSON",
              tag: "ctrl",
              body: "Host → 车:15000 type=json；车上 poll_json 取 data。标定另有 calib_cmd。",
            },
          ]}
        />
      </Section>

      <Section id="who" title="特性：谁收得到">
        <FlowRow>
          <Node title="enable_remote" sub="beacon + 队列" accent="car" pulse />
          <FlowArrow label="Host register" animated />
          <Node title="队首" sub="唯一数据 UDP" accent="host" />
          <FlowArrow label="可选转发" animated />
          <Node title="follower" sub="跟队首拉流" accent="ui" />
        </FlowRow>
        <Callout tone="warn" title="队列空不发数据 UDP">
          没有 host register 时 plot/log 仍可写本地 .rlog（enable_local），但不往网上推。图像：无订阅则无 UDP 图。
        </Callout>
      </Section>

      <Section id="prio" title="特性：线程与优先级">
        <MiniTable
          headers={["线程", "职责"]}
          rows={[
            ["`var_worker`", "JSON：先 UDP，再入落盘队列"],
            ["`img_worker`", "邮箱 → JPEG → 先 UDP（订阅）→ 落盘队列"],
            ["`disk_worker`", "写 RLG2；可落后；满则丢低优先级"],
            ["`ctrl_worker`", "beacon、host 队列、队首超时（enable_remote）"],
          ]}
        />
        <MiniTable
          headers={["prio", "来源", "落盘策略"]}
          rows={[
            ["0", "图像", "队列满时最先被丢"],
            ["1", "普通 plot / INFO 等", "正常"],
            ["2", "WARN", "高于普通"],
            ["3", "ERROR / FATAL", "最高；写后 urgent sync"],
          ]}
        />
        <p className="hp-footnote">约每 1s fdatasync；kill -9 / 掉电仍可能丢最近约 1s。</p>
      </Section>

      <Section id="image" title="特性：图像与 .rlog">
        <Steps
          items={[
            {
              title: "选帧",
              body: "按 meta.name 独立对齐 ~30Hz；未入选只比时间戳后 return，无拷贝。",
            },
            {
              title: "编码发送",
              body: "resize 到 img_width → JPEG(img_quality) → 0xFE 分片 ≤1200B 给队首（仅订阅流）。",
            },
            {
              title: "落盘",
              body: "log_dir/run_<ts_ns>.rlog，magic RLG2；json 与 image 交错。Replay/Dump 读此文件。",
            },
          ]}
        />
      </Section>

      <Section id="markers" title="特性：3D Markers 与 TF">
        <FlowRow>
          <Node title="MarkerArray" sub="viz_markers.hpp" accent="car" />
          <FlowArrow label="plot.markers" animated />
          <Node title="Watch 3D" sub="ns 图层开关" accent="host" />
        </FlowRow>
        <CodeBlock>{`tools::viz::MarkerArray arr;
arr.sphere("demo.point", "p0", {1,0,0.5}, 0.05);
data["markers"] = arr.to_json();  // schema marker_v1
tools::RemoteLogger::instance().plot(data);`}
        </CodeBlock>
        <CardGrid
          items={[
            {
              title: "marker_v1",
              tag: "3d",
              body: "ns 必填；frame_id 默认 world；sphere/arrow/box/line_list。同 ns 整组替换。",
            },
            {
              title: "分层",
              tag: "arch",
              body: "定义在 tools/rdbg/markers；转换在 task（如 kalman_markers）；src debug 挂到 plot。",
            },
            {
              title: "TF",
              tag: "tf",
              body: "配置 init 一次外参 + Gimbal 周期姿态（tf_auto_pub）→ plot.tf；Watch FrameStore / TF Viz。",
            },
          ]}
        />
      </Section>

      <Section id="yaml" title="yaml 配置">
        <MiniTable
          headers={["键", "默认", "说明"]}
          rows={[
            ["`control_port`", "15000", "车控制口"],
            ["`enable_remote`", "true", "UDP / beacon"],
            ["`enable_local`", "true", "写 .rlog"],
            ["`log_dir`", "./logs", "本地目录"],
            ["`var_buffer_size`", "1024", "变量缓冲条数"],
            ["`img_width` / `img_quality`", "640 / 50", "JPEG 参数"],
            ["`heartbeat_interval_ms`", "0", "0=关 hb"],
            ["`sender_name`", "空→自动", "多车必须唯一"],
            ["`app`", "normal", "门户开 watch/calibrate/tfviz"],
            ["`beacon_interval_ms`", "1000", "连上后也一直发"],
            ["`head_timeout_ms`", "2000", "队首失联出队"],
          ]}
        />
        <p className="hp-footnote">
          旧键 <code>remote_host</code> / <code>register_retry_ms</code> 忽略。全文见{" "}
          <DocPath>REMOTE_LOGGER.md</DocPath>。
        </p>
      </Section>
    </div>
  );
}
