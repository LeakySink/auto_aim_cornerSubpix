/**
 * 轻量 Markdown → 安全 HTML（Help 文档用，无第三方依赖）。
 * 支持：标题、段落、列表、表格、围栏代码、行内 code/粗体/链接、hr、引用。
 */

export type TocItem = { id: string; title: string; level: 2 | 3 };

export function slugify(title: string): string {
  return title
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\-_]+/gu, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "") || "sec";
}

/** 从 md 提取 ## / ### 作为二级菜单。 */
export function extractToc(md: string): TocItem[] {
  const out: TocItem[] = [];
  const seen = new Map<string, number>();
  for (const line of md.split("\n")) {
    const m = /^(#{2,3})\s+(.+)$/.exec(line);
    if (!m) continue;
    const level = m[1].length as 2 | 3;
    const title = m[2].replace(/#+$/, "").trim();
    let id = slugify(title);
    const n = (seen.get(id) || 0) + 1;
    seen.set(id, n);
    if (n > 1) id = `${id}-${n}`;
    out.push({ id, title, level });
  }
  return out;
}

function escapeHtml(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

export type RenderMdOptions = {
  /** 当前文档仓库路径，如 host/PROTOCOL.md，用于解析相对链接 */
  sourcePath?: string;
  /** 把 md 互链变成 Help 可识别的 <a> */
  resolveLink?: (href: string) => HelpLinkAttr;
};

export type HelpLinkAttr =
  | { kind: "doc"; chapter: string; hashFrag?: string }
  | { kind: "anchor"; frag: string }
  | { kind: "external"; url: string }
  | { kind: "path"; path: string };

function linkToAnchor(labelHtml: string, link: HelpLinkAttr): string {
  if (link.kind === "external") {
    return `<a href="${escapeHtml(link.url)}" target="_blank" rel="noreferrer">${labelHtml}</a>`;
  }
  if (link.kind === "doc") {
    const sub = link.hashFrag ? ` data-help-sub="${escapeHtml(link.hashFrag)}"` : "";
    return `<a href="#doc/${escapeHtml(link.chapter)}" class="md-help-link" data-help-doc="${escapeHtml(link.chapter)}"${sub}>${labelHtml}</a>`;
  }
  if (link.kind === "anchor") {
    return `<a href="#${escapeHtml(link.frag)}" class="md-help-link" data-help-anchor="${escapeHtml(link.frag)}">${labelHtml}</a>`;
  }
  // 仓库内非文档路径：不跳转，悬停看路径
  return `<span class="md-path-ref" title="${escapeHtml(link.path)}">${labelHtml}</span>`;
}

function inlineFormat(text: string, opts?: RenderMdOptions): string {
  let s = escapeHtml(text);
  // ![alt](url)
  s = s.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (_m, alt, url) => {
    const u = String(url);
    if (/^https?:\/\//i.test(u)) {
      return `<img class="md-img" src="${escapeHtml(u)}" alt="${escapeHtml(alt)}" loading="lazy" />`;
    }
    return `<span class="md-img">${escapeHtml(alt || u)}</span>`;
  });
  // [text](url)
  s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_m, label, href) => {
    const labelHtml = escapeHtml(label);
    if (opts?.resolveLink) {
      return linkToAnchor(labelHtml, opts.resolveLink(String(href)));
    }
    const h = String(href);
    if (/^https?:\/\//i.test(h)) {
      return `<a href="${escapeHtml(h)}" target="_blank" rel="noreferrer">${labelHtml}</a>`;
    }
    return `<span class="md-path-ref" title="${escapeHtml(h)}">${labelHtml}</span>`;
  });
  // `code`
  s = s.replace(/`([^`]+)`/g, "<code>$1</code>");
  // **bold**
  s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  // *italic*
  s = s.replace(/(^|[^*])\*([^*]+)\*(?!\*)/g, "$1<em>$2</em>");
  return s;
}

function isTableSep(line: string): boolean {
  return /^\|?[\s:|-]+\|[\s:|-]+/.test(line);
}

/**
 * 渲染全文；给 ##/### 打上与 extractToc 一致的 id，便于二级菜单锚点。
 */
export function renderMarkdown(md: string, opts?: RenderMdOptions): string {
  const lines = md.replace(/\r\n/g, "\n").split("\n");
  const html: string[] = [];
  const seen = new Map<string, number>();
  let i = 0;
  const fmt = (t: string) => inlineFormat(t, opts);

  const headingId = (title: string) => {
    let id = slugify(title);
    const n = (seen.get(id) || 0) + 1;
    seen.set(id, n);
    if (n > 1) id = `${id}-${n}`;
    return id;
  };

  while (i < lines.length) {
    const line = lines[i];

    // fenced code
    if (line.startsWith("```")) {
      const lang = line.slice(3).trim();
      const buf: string[] = [];
      i += 1;
      while (i < lines.length && !lines[i].startsWith("```")) {
        buf.push(lines[i]);
        i += 1;
      }
      i += 1; // closing ```
      html.push(
        `<pre class="md-pre"><code class="language-${escapeHtml(lang)}">${escapeHtml(buf.join("\n"))}</code></pre>`,
      );
      continue;
    }

    // hr
    if (/^---+$/.test(line.trim()) || /^\*\*\*+$/.test(line.trim())) {
      html.push("<hr class=\"md-hr\" />");
      i += 1;
      continue;
    }

    // headings
    const hm = /^(#{1,4})\s+(.+)$/.exec(line);
    if (hm) {
      const level = hm[1].length;
      const title = hm[2].replace(/#+$/, "").trim();
      const tag = `h${level}`;
      if (level === 1) {
        html.push(`<${tag} class="md-h1">${fmt(title)}</${tag}>`);
      } else if (level === 2 || level === 3) {
        const id = headingId(title);
        html.push(
          `<${tag} class="md-h${level}" id="${id}">${fmt(title)}</${tag}>`,
        );
      } else {
        html.push(`<${tag} class="md-h${level}">${fmt(title)}</${tag}>`);
      }
      i += 1;
      continue;
    }

    // blockquote
    if (line.startsWith(">")) {
      const buf: string[] = [];
      while (i < lines.length && lines[i].startsWith(">")) {
        buf.push(lines[i].replace(/^>\s?/, ""));
        i += 1;
      }
      html.push(`<blockquote class="md-quote">${fmt(buf.join(" "))}</blockquote>`);
      continue;
    }

    // table
    if (line.includes("|") && i + 1 < lines.length && isTableSep(lines[i + 1])) {
      const rows: string[][] = [];
      while (i < lines.length && lines[i].includes("|")) {
        if (isTableSep(lines[i])) {
          i += 1;
          continue;
        }
        const cells = lines[i]
          .replace(/^\|/, "")
          .replace(/\|$/, "")
          .split("|")
          .map((c) => c.trim());
        rows.push(cells);
        i += 1;
      }
      if (rows.length) {
        const head = rows[0];
        const body = rows.slice(1);
        html.push('<div class="md-table-wrap"><table class="md-table"><thead><tr>');
        head.forEach((c) => html.push(`<th>${fmt(c)}</th>`));
        html.push("</tr></thead><tbody>");
        body.forEach((r) => {
          html.push("<tr>");
          r.forEach((c) => html.push(`<td>${fmt(c)}</td>`));
          html.push("</tr>");
        });
        html.push("</tbody></table></div>");
      }
      continue;
    }

    // ul / ol
    if (/^\s*[-*]\s+/.test(line) || /^\s*\d+\.\s+/.test(line)) {
      const ordered = /^\s*\d+\.\s+/.test(line);
      const tag = ordered ? "ol" : "ul";
      html.push(`<${tag} class="md-list">`);
      while (i < lines.length) {
        const m = ordered
          ? /^\s*\d+\.\s+(.+)$/.exec(lines[i])
          : /^\s*[-*]\s+(.+)$/.exec(lines[i]);
        if (!m) break;
        html.push(`<li>${fmt(m[1])}</li>`);
        i += 1;
      }
      html.push(`</${tag}>`);
      continue;
    }

    // blank
    if (!line.trim()) {
      i += 1;
      continue;
    }

    // paragraph (merge consecutive)
    const buf: string[] = [line];
    i += 1;
    while (
      i < lines.length &&
      lines[i].trim() &&
      !lines[i].startsWith("#") &&
      !lines[i].startsWith("```") &&
      !lines[i].startsWith(">") &&
      !/^\s*[-*]\s+/.test(lines[i]) &&
      !/^\s*\d+\.\s+/.test(lines[i]) &&
      !(lines[i].includes("|") && i + 1 < lines.length && isTableSep(lines[i + 1]))
    ) {
      buf.push(lines[i]);
      i += 1;
    }
    html.push(`<p class="md-p">${fmt(buf.join(" "))}</p>`);
  }

  return html.join("\n");
}
