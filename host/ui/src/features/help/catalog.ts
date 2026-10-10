/**
 * Help 目录：图解指南 + 仓库文档全文。
 * hash: #guide/overview[/sub] 或 #doc/protocol[/sec-id]
 */
import type { FC } from "react";
import hostMd from "@hostdocs/HOST.md?raw";
import apiMd from "@hostdocs/API.md?raw";
import protocolMd from "@hostdocs/PROTOCOL.md?raw";
import designMd from "@hostdocs/DESIGN.md?raw";
import remoteMd from "@repo/REMOTE_LOGGER.md?raw";
import readmeMd from "@repo/readme.md?raw";
import { OverviewPage, NetworkPage } from "./pages/OverviewNetwork";
import { ProtocolPage, DetectPage } from "./pages/ProtocolDetect";
import { FeaturesPage, PluginPage, VehiclePage } from "./pages/FeaturesPluginVehicle";
import { AimPage, TroubleshootPage, SshPage } from "./pages/AimTroubleshootSsh";
import { ApiPage } from "./pages/ApiPage";
import { extractToc, type TocItem } from "./markdown";

export type GuideChapter = {
  kind: "guide";
  id: string;
  label: string;
  num: string;
  Page: FC;
  /** 二级：页内锚点（与 Section id 对齐） */
  subs: { id: string; label: string }[];
};

export type DocChapter = {
  kind: "doc";
  id: string;
  label: string;
  num: string;
  source: string;
  markdown: string;
  subs: TocItem[];
};

export type Chapter = GuideChapter | DocChapter;

export type NavGroup = {
  id: string;
  label: string;
  chapters: Chapter[];
};

const guide = (
  id: string,
  label: string,
  num: string,
  Page: FC,
  subs: { id: string; label: string }[],
): GuideChapter => ({ kind: "guide", id, label, num, Page, subs });

const doc = (id: string, label: string, num: string, source: string, markdown: string): DocChapter => ({
  kind: "doc",
  id,
  label,
  num,
  source,
  markdown,
  subs: extractToc(markdown).filter((t) => t.level === 2),
});

export const NAV_GROUPS: NavGroup[] = [
  {
    id: "guide",
    label: "图解指南",
    chapters: [
      guide("overview", "总览", "01", OverviewPage, [
        { id: "steps", label: "上手三步" },
        { id: "system", label: "系统一张图" },
        { id: "home", label: "首页结构" },
        { id: "docs-map", label: "文档地图" },
        { id: "pipeline", label: "数据旁路" },
      ]),
      guide("network", "网络与角色", "02", NetworkPage, [
        { id: "topo", label: "拓扑" },
        { id: "ports", label: "端口速查" },
        { id: "app-map", label: "beacon → 开页" },
        { id: "burden", label: "负担怎么分" },
        { id: "beacon", label: "持续发现" },
      ]),
      guide("protocol", "协议图解", "03", ProtocolPage, [
        { id: "seq", label: "总时序" },
        { id: "planes", label: "两平面" },
        { id: "msgs", label: "报文角色" },
        { id: "img-sub", label: "图像订阅" },
      ]),
      guide("detect", "Host 检测", "04", DetectPage, [
        { id: "discover", label: "发现环" },
        { id: "offline", label: "列表离线" },
        { id: "bind", label: "开页绑定" },
        { id: "head", label: "队首存活" },
        { id: "netcheck", label: "Netcheck" },
        { id: "online-table", label: "在线对照" },
      ]),
      guide("features", "功能用法", "05", FeaturesPage, [
        { id: "list", label: "功能一览" },
        { id: "watch3d", label: "Watch 3D" },
        { id: "lifecycle", label: "生命周期" },
      ]),
      guide("plugin", "Host 插件", "06", PluginPage, [
        { id: "user", label: "使用者" },
        { id: "dev", label: "加插件三步" },
        { id: "kinds", label: "现有插件" },
      ]),
      guide("vehicle", "RemoteLogger 图解", "07", VehiclePage, [
        { id: "life", label: "生命周期" },
        { id: "api", label: "对外 API" },
        { id: "channels", label: "五种通道" },
        { id: "who", label: "谁收得到" },
        { id: "prio", label: "线程与优先级" },
        { id: "image", label: "图像与 .rlog" },
        { id: "markers", label: "Markers 与 TF" },
        { id: "yaml", label: "yaml 配置" },
      ]),
      guide("aim", "自瞄代码", "08", AimPage, [
        { id: "build", label: "编译" },
        { id: "config", label: "配置" },
        { id: "bins", label: "跑哪个" },
        { id: "flow", label: "数据流" },
        { id: "tests", label: "模块单测" },
      ]),
      guide("troubleshoot", "排障", "09", TroubleshootPage, [
        { id: "decision", label: "决策条" },
        { id: "quick", label: "快速对照" },
      ]),
      guide("ssh", "SSH + CLion", "10", SshPage, [
        { id: "flow", label: "推荐流程" },
        { id: "topo", label: "并行拓扑" },
        { id: "link", label: "与自瞄章" },
      ]),
      guide("api-guide", "HTTP API 图解", "11", ApiPage, [
        { id: "layers", label: "分层" },
        { id: "open-seq", label: "开页时序" },
        { id: "hub", label: "Hub 全局" },
        { id: "fleet", label: "FleetBound" },
        { id: "others", label: "Replay/Dump/Netcheck" },
        { id: "fe", label: "前端封装" },
      ]),
    ],
  },
  {
    id: "docs",
    label: "完整文档",
    chapters: [
      doc("host-md", "HOST.md", "D1", "host/HOST.md", hostMd),
      doc("api-md", "API.md", "D2", "host/API.md", apiMd),
      doc("protocol-md", "PROTOCOL.md", "D3", "host/PROTOCOL.md", protocolMd),
      doc("design-md", "DESIGN.md", "D4", "host/DESIGN.md", designMd),
      doc("remote-md", "REMOTE_LOGGER.md", "D5", "REMOTE_LOGGER.md", remoteMd),
      doc("readme-md", "readme.md", "D6", "readme.md", readmeMd),
    ],
  },
];

export function flatChapters(): Chapter[] {
  return NAV_GROUPS.flatMap((g) => g.chapters);
}

export function findChapter(id: string): Chapter | undefined {
  return flatChapters().find((c) => c.id === id);
}

/** 将 md 相对路径解析为 Help 文档章 id（如 host-md）。 */
const DOC_FILE_TO_CHAPTER: Record<string, string> = {
  "host.md": "host-md",
  "api.md": "api-md",
  "protocol.md": "protocol-md",
  "design.md": "design-md",
  "remote_logger.md": "remote-md",
  "readme.md": "readme-md",
};

function normalizeFsPath(parts: string[]): string {
  const out: string[] = [];
  for (const p of parts) {
    if (!p || p === ".") continue;
    if (p === "..") {
      out.pop();
      continue;
    }
    out.push(p);
  }
  return out.join("/");
}

/** fromSource 如 host/HOST.md；href 如 ../REMOTE_LOGGER.md 或 PROTOCOL.md。 */
export function resolveRepoPath(fromSource: string, href: string): string {
  const clean = href.split(/[?#]/)[0].replace(/^\.\//, "");
  if (!clean) return fromSource;
  if (clean.startsWith("/")) return clean.replace(/^\/+/, "");
  const baseDir = fromSource.includes("/")
    ? fromSource.slice(0, fromSource.lastIndexOf("/"))
    : "";
  const joined = baseDir ? `${baseDir}/${clean}` : clean;
  return normalizeFsPath(joined.split("/"));
}

export function chapterIdForDocPath(repoPath: string): string | undefined {
  const base = repoPath.split("/").pop()?.toLowerCase() || "";
  return DOC_FILE_TO_CHAPTER[base];
}

export type HelpLink =
  | { kind: "doc"; chapter: string; hashFrag?: string }
  | { kind: "anchor"; frag: string }
  | { kind: "external"; url: string }
  | { kind: "path"; path: string };

/** 解析 md 链接：文档互跳 / 页内锚点 / 外链 / 仓库路径提示。 */
export function resolveHelpLink(href: string, fromSource: string): HelpLink {
  const raw = href.trim();
  if (!raw || raw === "#") return { kind: "anchor", frag: "" };
  if (/^(https?:|mailto:)/i.test(raw)) return { kind: "external", url: raw };

  const hashIdx = raw.indexOf("#");
  const pathPart = hashIdx >= 0 ? raw.slice(0, hashIdx) : raw;
  const hashFrag = hashIdx >= 0 ? decodeURIComponent(raw.slice(hashIdx + 1)) : undefined;

  if (!pathPart || pathPart === "." || pathPart === "./") {
    return { kind: "anchor", frag: hashFrag || "" };
  }

  const repoPath = resolveRepoPath(fromSource, pathPart);
  const chapter = chapterIdForDocPath(repoPath);
  if (chapter) return { kind: "doc", chapter, hashFrag };

  return { kind: "path", path: repoPath };
}

export function helpHashForDoc(chapter: string, sub?: string): string {
  return sub ? `#doc/${chapter}/${sub}` : `#doc/${chapter}`;
}
