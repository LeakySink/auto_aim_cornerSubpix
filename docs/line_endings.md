# 行尾约定：LF

本仓库所有源文件用 **LF**。`76f6893` 基线即如此，`core.autocrlf=false`。

## 历史上的污染（2026-11-22 ~ 11-24）

我用 Python 脚本批量改文件时（`Path.write_text`），在 Windows 上把 **25 个
源文件转成了 CRLF**，从 `718a463` 开始一路带进历史。

后果：`git diff` / `git log -p` 里整个文件显示为"全改"，真实改动被淹没在
几千行里。回查历史时几乎无法看出某次提交到底改了什么。

**一个值得记的对比**：`4295277`（CA 迁移）那次是干净的 LF —— 因为那次用
write 工具而非 Python 脚本。同一批工作里有的提交干净、有的不干净，
差别只在"用了哪个工具写入"。

## 已做的处理

1. 工作区 25 个文件全部转回 LF（内容零变化，`git diff` 忽略 CRLF 后为 0 行）
2. 新增 `.gitattributes`，强制 `*.cpp/*.hpp/*.md/*.yaml` 为 `eol=lf`

## 约定

- 新增/修改文件**保持 LF**
- 用脚本批量改文件时，写回必须显式处理：
  ```python
  p.write_bytes(p.read_text(encoding="utf-8").replace("\r\n", "\n").encode("utf-8"))
  ```
  或 `open(p, "w", encoding="utf-8", newline="")`
  —— 裸 `Path.write_text()` 在 Windows 上会写成 CRLF
- 提交前检查：
  ```bash
  git diff --check          # 行尾空白问题
  git ls-files --eol | grep -v "i/lf"   # 非 LF 的文件
  ```

## 注

`282d06d` 之后的历史提交（`718a463`…`282d06d`）里，这些文件的 diff 仍然
是 CRLF 的，**历史没有重写**（改写已推送历史风险太高）。但 `282d06d` 之后
新增的提交都是干净的 LF，diff 可读。
