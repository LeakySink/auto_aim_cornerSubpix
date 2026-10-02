import { useEffect, useMemo, useRef } from "react";
import { resolveHelpLink } from "../catalog";
import { extractToc, renderMarkdown, slugify } from "../markdown";

/** 渲染打包进来的仓库 Markdown 全文；文档互链在 Help 内跳转。 */
export function DocMarkdownPage({
  title,
  source,
  markdown,
  activeSub,
  onNavigateDoc,
}: {
  title: string;
  source: string;
  markdown: string;
  activeSub?: string;
  /** 跳到另一篇已嵌入文档（及可选锚点） */
  onNavigateDoc?: (chapterId: string, sub?: string) => void;
}) {
  const articleRef = useRef<HTMLElement>(null);

  const html = useMemo(
    () =>
      renderMarkdown(markdown, {
        sourcePath: source,
        resolveLink: (href) => resolveHelpLink(href, source),
      }),
    [markdown, source],
  );

  const toc = useMemo(() => extractToc(markdown), [markdown]);

  const scrollToFrag = (frag: string) => {
    if (!frag) return;
    const root = articleRef.current;
    // 1) 直接 id
    let el = document.getElementById(frag);
    // 2) 我们的 slugify
    if (!el) el = document.getElementById(slugify(frag));
    // 3) 标题文本近似匹配
    if (!el && root) {
      const want = slugify(frag);
      el =
        Array.from(root.querySelectorAll("h2[id], h3[id]")).find((h) => {
          const id = h.id;
          const text = slugify(h.textContent || "");
          return id === want || text === want || id.includes(want) || want.includes(id);
        }) || null;
    }
    // 4) toc 标题包含
    if (!el) {
      const hit = toc.find(
        (t) => t.id === frag || t.id === slugify(frag) || t.title.includes(frag) || slugify(t.title) === slugify(frag),
      );
      if (hit) el = document.getElementById(hit.id);
    }
    el?.scrollIntoView({ behavior: "smooth", block: "start" });
  };

  useEffect(() => {
    if (!activeSub) return;
    const t = window.setTimeout(() => scrollToFrag(activeSub), 40);
    return () => clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeSub, markdown]);

  useEffect(() => {
    const root = articleRef.current;
    if (!root) return;

    const onClick = (e: MouseEvent) => {
      const a = (e.target as HTMLElement | null)?.closest?.("a") as HTMLAnchorElement | null;
      if (!a || !root.contains(a)) return;

      const docId = a.getAttribute("data-help-doc");
      const anchor = a.getAttribute("data-help-anchor");
      const subAttr = a.getAttribute("data-help-sub") || undefined;

      if (docId) {
        e.preventDefault();
        // hashFrag 可能是 github 风格；交给父级换章，再尽量滚到对应 h2
        const sub = subAttr ? slugify(subAttr) : undefined;
        onNavigateDoc?.(docId, sub);
        // 若仍在同章（少见），本地滚
        window.setTimeout(() => {
          if (subAttr) scrollToFrag(subAttr);
        }, 100);
        return;
      }

      if (anchor !== null && a.classList.contains("md-help-link")) {
        e.preventDefault();
        scrollToFrag(anchor);
      }
    };

    root.addEventListener("click", onClick);
    return () => root.removeEventListener("click", onClick);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [html, onNavigateDoc, source]);

  return (
    <div className="hp-page hp-doc-page">
      <header className="hp-head">
        <span className="hp-kicker">完整文档</span>
        <h1>{title}</h1>
        <p>
          源文件 <code className="hp-path">{source}</code>
          ，已完整嵌入本 Help；文内文档链接在手册内跳转。
        </p>
      </header>
      <article
        ref={articleRef}
        className="md-body"
        dangerouslySetInnerHTML={{ __html: html }}
      />
    </div>
  );
}
