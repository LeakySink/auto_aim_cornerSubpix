/** Help 第 8–10 章：自瞄用法、排障、SSH/CLion。 */
import {
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

export function AimPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="08 · 自瞄代码使用"
        title="编译 · 配置 · 跑哪个 · 单测"
        lead="不写算法长文：把二进制选对、配置打开 RemoteLogger，就能和 Host Watch 联调。理论细节见仓库 readme。"
      />

      <Section id="build" title="编译">
        <Steps
          items={[
            {
              title: "依赖",
              body: "MindVision 或 HikRobot SDK、OpenVINO、Ceres；再 apt 装 git/g++/cmake/OpenCV/fmt/Eigen/spdlog/yaml-cpp/usb/nlohmann-json 等（见 readme §3.2）。",
            },
            {
              title: "构建",
              body: "cmake -B build && make -C build/ -j$(nproc)。产物落在 build/。",
            },
            {
              title: "冒烟",
              body: "./build/auto_aim_test 或对应兵种 debug；确认相机与 remote_logger 配置。",
            },
          ]}
        />
      </Section>

      <Section id="config" title="配置角色">
        <MiniTable
          headers={["路径 / 段", "角色"]}
          rows={[
            ["`configs/*.yaml`", "兵种/场景主配置入口"],
            ["相机 / 外参段", "内参、雷达到云台、手眼等"],
            ["`remote_logger`", "Host 调试：enable、sender_name、app、端口与间隔"],
          ]}
        />
      </Section>

      <Section id="bins" title="跑哪个（常用）">
        <MiniTable
          headers={["二进制", "场景"]}
          rows={[
            ["`sentry_debug` / `sentry`", "步兵哨兵调试 / 赛场"],
            ["`uav_debug` / `uav`", "无人机"],
            ["`mt_auto_aim_debug` / `mt_standard`", "多线程标准兵"],
            ["`auto_aim_debug_mpc` / `standard_mpc`", "MPC 相关调试"],
            ["`auto_buff_debug*`", "能量机关调试"],
            ["`auto_aim_test`", "tests 里自瞄联调入口"],
            ["`calibrate`", "内参标定（beacon app=calibrate）"],
            ["`tf_pub_test`", "TF 发布联调（可被 Hub spawn）"],
          ]}
        />
        <p className="hp-footnote">
          入口在 <DocPath>src/</DocPath> 与 <DocPath>tests/</DocPath>，按兵种选；CMake 用 auto_add_executables 生成同名产物。
        </p>
      </Section>

      <Section id="flow" title="数据流">
        <Pipeline stages={["相机+IMU", "识别", "解算/EKF", "决策", "电控"]} />
        <FlowRow>
          <Node title="主链路" sub="车上闭环" accent="car" />
          <FlowArrow label="旁路" animated />
          <Node title="RemoteLogger" accent="car" pulse />
          <FlowArrow label="UDP" animated />
          <Node title="Host Watch" sub="本机浏览器" accent="host" />
        </FlowRow>
        <Callout tone="ok" title="与门户">
          车上跑 *debug + 本机 Host Watch；断点可用第 10 章 CLion，与 Watch 并行。
        </Callout>
      </Section>

      <Section id="tests" title="模块单测（一句话）">
        <CardGrid
          items={[
            { title: "detector_* / camera_detect_*", tag: "检测", body: "模型与后处理是否正常出框。" },
            { title: "gimbal_* / fire_* / cboard_*", tag: "执行", body: "云台、击发、电控板通路。" },
            { title: "calibrate_* / handeye_*", tag: "标定", body: "内参/手眼流程。" },
            { title: "planner_*", tag: "规划", body: "轨迹/规划模块离线或在线测。" },
            { title: "markers_pub / tf_pub", tag: "Host 联调", body: "向门户发 3D / TF（markers_pub_test 默认不进 git）。" },
            { title: "usbcamera_* / publish_*", tag: "IO", body: "相机采集与话题发布冒烟。" },
          ]}
        />
        <p className="hp-footnote">算法与建模长文见 readme 理论节；这里只解决「先跑起来」。 </p>
      </Section>
    </div>
  );
}

export function TroubleshootPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="09 · 排障"
        title="先问对问题"
        lead="把「看不见车」「看不見数」「看不见图」「3D 空」分开查，避免在队首和 beacon 之间绕圈。"
      />

      <Section id="decision" title="决策条">
        <div className="hp-decision">
          {[
            {
              q: "首页看不到车？",
              a: "同网段？车上 enable_remote？Host 是否在听 :15999？用 Netcheck discover。防火墙/多网卡选错也会导致只发不收。",
            },
            {
              q: "有车但 Watch 无曲线？",
              a: "你是不是队首？register 成功了吗？车上是否在 plot？多开 Watch 时后开的可能是 follower——应能跟队首拉流；若队首已挂，需 promote/重开。",
            },
            {
              q: "有曲线无图？",
              a: "侧栏勾选图像话题（img_subscribe）。查 JPEG 质量/分辨率与带宽；确认 meta.name 一致。",
            },
            {
              q: "3D 空白？",
              a: "plot 里是否有合法 marker_v1？ns 是否被关掉？frame_id 能否变到 display_frame？",
            },
            {
              q: "列表离线但仍像连着？",
              a: "离线只改目录；车上队列还在。关页 stop 或 drop_head / 超时 promote 才会换收数者。",
            },
            {
              q: "TF Viz 起不来？",
              a: "spawn 时检查 build/tf_pub_test 与 configs/tf_pub.yaml（或 RDBG_TF_PUB_CONFIG）。也可手工跑测试进程再从车辆列表进。",
            },
            {
              q: "Replay 打不开？",
              a: "路径是否本机可读的 .rlog；Dump 导出视频还需 opencv-python-headless。",
            },
          ].map((row) => (
            <div key={row.q} className="hp-dec-row">
              <div className="hp-dec-q">{row.q}</div>
              <div className="hp-dec-a">{row.a}</div>
            </div>
          ))}
        </div>
      </Section>

      <Section id="quick" title="快速对照">
        <MiniTable
          headers={["现象", "先查"]}
          rows={[
            ["名单空", "beacon / 网段 / :15999"],
            ["名单有、无 plot", "队首 · register · 车上 plot"],
            ["无图", "img_subscribe · 话题名"],
            ["无 3D", "markers · ns · frame"],
            ["通达不明", "Netcheck（不替代列表）"],
          ]}
        />
      </Section>
    </div>
  );
}

export function SshPage() {
  return (
    <div className="hp-page">
      <PageHead
        kicker="10 · SSH + CLion"
        title="断点在 IDE，观测量在门户"
        lead="本机 CLion，车上编译运行；RemoteLogger → Host Watch 可并行。二者不互斥。"
      />

      <Section id="flow" title="推荐流程">
        <Steps
          items={[
            {
              title: "SSH 打通",
              body: "车上 sshd；本机密钥登录。确认与 Host 同一可达网段。",
            },
            {
              title: "CLion Remote Host / Deployment",
              body: "映射远端工程目录，或在远端直接打开；工具链指向车上 g++/cmake/gdb。",
            },
            {
              title: "远端 CMake + Debug",
              body: "在车 build/ 编译；Run/Debug Configuration 用 Remote GDB 挂 *debug 二进制。",
            },
            {
              title: "并行 Watch",
              body: "本机 ./host/start.sh，点车开 Watch：看 plot / 图 / 3D / 日志，同时 IDE 断点单步。",
            },
          ]}
        />
      </Section>

      <Section id="topo" title="并行拓扑">
        <FlowRow>
          <Node title="CLion" sub="本机 · gdb" accent="ui" />
          <FlowArrow label="SSH" animated />
          <Node title="车进程" sub="sentry_debug 等" accent="car" pulse />
          <FlowArrow label="RemoteLogger" animated />
          <Node title="Host Watch" sub="本机浏览器" accent="host" />
        </FlowRow>
        <Callout tone="warn" title="注意">
          断点卡住时 plot/图像会停或变稀——属预期。队首 head_alive 也可能受影响，必要时先继续运行再观察门户。
        </Callout>
      </Section>

      <Section id="link" title="和自瞄章的关系">
        <CardGrid
          items={[
            {
              title: "先会跑二进制",
              tag: "ch.8",
              body: "选对 src/tests 入口与 yaml，再挂调试器。",
            },
            {
              title: "再开门户",
              tag: "ch.1–5",
              body: "Watch 验证传感器与解算中间量；3D/TF 看空间关系。",
            },
            {
              title: "排障顺序",
              tag: "ch.9",
              body: "先保证 beacon 与队首，再查「断点导致无数据」。",
            },
          ]}
        />
        <p className="hp-footnote">
          Help 无线程；从首页随时回来对照。协议细节仍以 <DocPath>host/PROTOCOL.md</DocPath> 为准。
        </p>
      </Section>
    </div>
  );
}
