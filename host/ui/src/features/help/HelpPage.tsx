/**
 * Help 门户手册：一级分组 + 二级章节菜单；图解页与仓库 md 全文并存。
 * hash：`#guide/<chapter>[/<sub>]`；无 hash 时从 localStorage 恢复。
 * 同时只展开一个章节的二级菜单。不占 Feature 线程 / UDP。
 */
import { useEffect, useMemo, useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { NAV_GROUPS, findChapter, flatChapters, type Chapter } from "./catalog";
import { DocMarkdownPage } from "./pages/DocMarkdownPage";
import "./help.css";

type Loc = { chapter: string; sub?: string };

const STORAGE_KEY = "rdbg.help.nav.v1";

type StoredNav = {
  chapter: string;
  sub?: string;
  /** 当前展开二级菜单的章节 id；无则都不展开 */
  openChapter?: string;
  /** 展开的一级分组 */
  openGroup?: string;
};

function parseHash(): Loc | null {
  const raw = (window.location.hash || "").replace(/^#/, "").trim();
  if (!raw) return null;
  if (!raw.includes("/") && findChapter(raw)) return { chapter: raw };
  const parts = raw.split("/").filter(Boolean);
  if (parts[0] === "guide" || parts[0] === "doc") {
    const chapter = parts[1] || "overview";
    if (!findChapter(chapter)) return null;
    return { chapter, sub: parts[2] };
  }
  if (parts.length >= 1 && findChapter(parts[0])) {
    return { chapter: parts[0], sub: parts[1] };
  }
  return null;
}

function hashFor(chapter: string, sub?: string): string {
  const ch = findChapter(chapter);
  const prefix = ch?.kind === "doc" ? "doc" : "guide";
  return sub ? `#${prefix}/${chapter}/${sub}` : `#${prefix}/${chapter}`;
}

function loadStored(): StoredNav | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const j = JSON.parse(raw) as StoredNav;
    if (!j?.chapter || !findChapter(j.chapter)) return null;
    return j;
  } catch {
    return null;
  }
}

function saveStored(s: StoredNav) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(s));
  } catch {
    /* quota / private mode */
  }
}

function initialState(): { loc: Loc; openChapter: string; openGroup: string } {
  const fromHash = typeof window !== "undefined" ? parseHash() : null;
  const stored = typeof window !== "undefined" ? loadStored() : null;
  const loc = fromHash || (stored ? { chapter: stored.chapter, sub: stored.sub } : { chapter: "overview" });
  const group =
    NAV_GROUPS.find((g) => g.chapters.some((c) => c.id === loc.chapter))?.id || "guide";
  // 有 hash/存档时默认展开当前章二级；仅允许这一章
  const openChapter = fromHash
    ? loc.chapter
    : stored?.openChapter && findChapter(stored.openChapter)
      ? stored.openChapter
      : loc.chapter;
  return {
    loc,
    openChapter,
    openGroup: stored?.openGroup && NAV_GROUPS.some((g) => g.id === stored.openGroup)
      ? stored.openGroup
      : group,
  };
}

export function HelpPage() {
  const navigate = useNavigate();
  const boot = useMemo(() => initialState(), []);
  const [loc, setLoc] = useState<Loc>(boot.loc);
  /** 同时只允许一个章节展开二级菜单 */
  const [openChapter, setOpenChapter] = useState<string>(boot.openChapter);
  const [openGroup, setOpenGroup] = useState<string>(boot.openGroup);
  const [animKey, setAnimKey] = useState(0);
  const [dir, setDir] = useState<"fwd" | "back">("fwd");

  const chapters = useMemo(() => flatChapters(), []);
  const current = findChapter(loc.chapter) ?? chapters[0];
  const index = chapters.findIndex((c) => c.id === current.id);

  // 位置 / 展开态写入 localStorage
  useEffect(() => {
    saveStored({
      chapter: loc.chapter,
      sub: loc.sub,
      openChapter,
      openGroup,
    });
  }, [loc.chapter, loc.sub, openChapter, openGroup]);

  const go = (chapter: string, sub?: string, direction?: "fwd" | "back") => {
    const nextIdx = chapters.findIndex((c) => c.id === chapter);
    setDir(direction ?? (nextIdx >= index ? "fwd" : "back"));
    const sameChapter = chapter === loc.chapter;
    setLoc({ chapter, sub });
    if (!sameChapter) setAnimKey((k) => k + 1);
    // 换章时只展开该章二级
    setOpenChapter(chapter);
    const group = NAV_GROUPS.find((g) => g.chapters.some((c) => c.id === chapter))?.id;
    if (group) setOpenGroup(group);
    navigate(`/help${hashFor(chapter, sub)}`, { replace: true });
  };

  useEffect(() => {
    const onHash = () => {
      const next = parseHash();
      if (!next) return;
      setLoc((prev) => {
        if (prev.chapter === next.chapter && prev.sub === next.sub) return prev;
        if (prev.chapter !== next.chapter) setAnimKey((k) => k + 1);
        return next;
      });
      setOpenChapter(next.chapter);
      const group = NAV_GROUPS.find((g) => g.chapters.some((c) => c.id === next.chapter))?.id;
      if (group) setOpenGroup(group);
    };
    window.addEventListener("hashchange", onHash);
    // 首次：无 hash 则用存档或 overview 写回 URL
    if (!window.location.hash) {
      navigate(`/help${hashFor(loc.chapter, loc.sub)}`, { replace: true });
    }
    return () => window.removeEventListener("hashchange", onHash);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [navigate]);

  useEffect(() => {
    if (!loc.sub || current.kind !== "guide") return;
    const t = window.setTimeout(() => {
      document.getElementById(loc.sub!)?.scrollIntoView({ behavior: "smooth", block: "start" });
    }, 80);
    return () => clearTimeout(t);
  }, [loc.sub, loc.chapter, animKey, current.kind]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return;
      if (e.key === "ArrowRight" || e.key === "ArrowDown") {
        if (index < chapters.length - 1) go(chapters[index + 1].id, undefined, "fwd");
      } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
        if (index > 0) go(chapters[index - 1].id, undefined, "back");
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [index, loc.chapter]);

  const toggleGroup = (gid: string) => {
    setOpenGroup((prev) => (prev === gid ? "" : gid));
  };

  /** 手风琴：点开 A 则关掉其它；再点 A 可收起 */
  const toggleChapter = (cid: string) => {
    setOpenChapter((prev) => (prev === cid ? "" : cid));
  };

  const renderPage = (ch: Chapter) => {
    if (ch.kind === "doc") {
      return (
        <DocMarkdownPage
          title={ch.label}
          source={ch.source}
          markdown={ch.markdown}
          activeSub={loc.chapter === ch.id ? loc.sub : undefined}
          onNavigateDoc={(chapterId, sub) => go(chapterId, sub)}
        />
      );
    }
    const Page = ch.Page;
    return <Page />;
  };

  return (
    <div className="app-shell help-shell">
      <header className="topbar">
        <Link to="/" className="brand">
          rdbg
        </Link>
        <span className="topbar-sep" />
        <span className="topbar-title">Help</span>
        <span className="pill help-progress">
          {current.num} · {current.label}
        </span>
        <div className="spacer" />
        <Link to="/" className="pill">
          ← 首页
        </Link>
      </header>

      <div className="help-layout">
        <aside className="help-nav" aria-label="目录">
          <div className="help-nav-brand">
            <span className="help-nav-mark" />
            <div>
              <strong>手册</strong>
              <em>
                {NAV_GROUPS.length} 组 · {chapters.length} 章
              </em>
            </div>
          </div>

          <div className="help-nav-list">
            {NAV_GROUPS.map((group) => {
              const open = openGroup === group.id;
              return (
                <div key={group.id} className="help-nav-group">
                  <button
                    type="button"
                    className="help-nav-group-btn"
                    onClick={() => toggleGroup(group.id)}
                    aria-expanded={open}
                  >
                    <span className={`help-nav-caret${open ? " open" : ""}`} />
                    {group.label}
                  </button>
                  {open &&
                    group.chapters.map((ch) => {
                      const chOpen = openChapter === ch.id;
                      const active = loc.chapter === ch.id;
                      return (
                        <div key={ch.id} className="help-nav-chapter">
                          <div className="help-nav-chapter-row">
                            <button
                              type="button"
                              className="help-nav-caret-btn"
                              onClick={() => toggleChapter(ch.id)}
                              aria-label="展开二级"
                              aria-expanded={chOpen}
                            >
                              <span className={`help-nav-caret${chOpen ? " open" : ""}`} />
                            </button>
                            <button
                              type="button"
                              className={active ? "help-nav-item on" : "help-nav-item"}
                              onClick={() => go(ch.id)}
                            >
                              <span className="help-nav-num">{ch.num}</span>
                              <span className="help-nav-label">{ch.label}</span>
                            </button>
                          </div>
                          {chOpen && ch.subs.length > 0 && (
                            <div className="help-nav-subs">
                              {ch.subs.map((sub) => {
                                const subId = sub.id;
                                const subLabel = "label" in sub ? sub.label : sub.title;
                                const subOn = active && loc.sub === subId;
                                return (
                                  <button
                                    key={subId}
                                    type="button"
                                    className={subOn ? "help-nav-sub on" : "help-nav-sub"}
                                    onClick={() => go(ch.id, subId)}
                                  >
                                    {subLabel}
                                  </button>
                                );
                              })}
                            </div>
                          )}
                        </div>
                      );
                    })}
                </div>
              );
            })}
          </div>
          <div className="help-nav-hint">位置已存盘 · 仅一章二级展开</div>
        </aside>

        <div className="help-stage">
          <div key={animKey} className={`help-page-wrap help-enter-${dir}`}>
            {renderPage(current)}
          </div>

          <footer className="help-pager">
            <button
              type="button"
              className="help-pager-btn"
              disabled={index <= 0}
              onClick={() => index > 0 && go(chapters[index - 1].id, undefined, "back")}
            >
              <span>上一章</span>
              <strong>{index > 0 ? chapters[index - 1].label : "—"}</strong>
            </button>
            <div className="help-pager-meta">{current.kind === "doc" ? "全文文档" : "图解指南"}</div>
            <button
              type="button"
              className="help-pager-btn next"
              disabled={index >= chapters.length - 1}
              onClick={() =>
                index < chapters.length - 1 && go(chapters[index + 1].id, undefined, "fwd")
              }
            >
              <span>下一章</span>
              <strong>{index < chapters.length - 1 ? chapters[index + 1].label : "—"}</strong>
            </button>
          </footer>
        </div>
      </div>
    </div>
  );
}
