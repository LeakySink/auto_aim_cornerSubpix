import type { FeatureModule } from "./types";
import { WatchPage } from "./watch/WatchPage";
import { ReplayPage } from "./replay/ReplayPage";
import { DumpPage } from "./dump/DumpPage";
import { NetcheckPage } from "./netcheck/NetcheckPage";
import { TfVizPage } from "./tfviz/TfVizPage";
import { HelpPage } from "./help/HelpPage";

/**
 * 前端功能注册表。
 * - 多数项经首页 `openFeature` → Hub KINDS 起线程；
 * - `help` 仅路由 `/help`，Home 用 Link 打开，**不要** POST /api/open。
 */
export const FEATURE_MODULES: FeatureModule[] = [
  {
    id: "watch",
    title: "Watch",
    description: "实时接收车上 RemoteLogger：曲线、日志、图像",
    route: "/watch",
    Component: WatchPage,
  },
  {
    id: "calibrate",
    title: "Calibrate",
    description: "棋盘格相机内参标定：覆盖度、采样、一键标定",
    route: "/calibrate",
  },
  {
    id: "tfviz",
    title: "TF Viz",
    description: "可视化相机相对世界系（tf_pub_test）",
    route: "/tfviz",
    Component: TfVizPage,
  },
  {
    id: "replay",
    title: "Replay",
    description: "本地回放 .rlog：时间轴、曲线、图像、日志",
    route: "/replay",
    Component: ReplayPage,
  },
  {
    id: "dump",
    title: "Dump",
    description: "导出 log.txt / plot.txt / images.mp4",
    route: "/dump",
    Component: DumpPage,
  },
  {
    id: "netcheck",
    title: "Netcheck",
    description: "UDP discover / echo / ping",
    route: "/netcheck",
    Component: NetcheckPage,
  },
  {
    id: "help",
    title: "Help",
    description: "用法、协议、检测、插件、自瞄、SSH 与 HTTP API（无线程）",
    route: "/help",
    Component: HelpPage,
  },
];
