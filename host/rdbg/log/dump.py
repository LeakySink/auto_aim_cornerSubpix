"""Export .rlog into a folder: log.txt, plot.txt, and images.mp4."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from .rlog import iter_records


def _rel_t(ts, t0):
    return (ts - t0) / 1e9


def _is_log(obj):
    return isinstance(obj.get("level"), str) and "msg" in obj


def _fmt_line(t_rel, typ, body):
    return f"[{t_rel:.6f}][{typ}]-----{body}"


def _write_video(frames, out_path, fps):
    """Encode list of JPEG bytes to mp4. Returns True on success."""
    if not frames:
        return False
    try:
        import cv2
        import numpy as np
    except ImportError:
        print("[dump] opencv not installed; skip video "
              "(use host/.venv via dump.sh, or pip install opencv-python-headless)",
              file=sys.stderr)
        return False

    decoded = []
    for jpeg in frames:
        arr = np.frombuffer(jpeg, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is not None:
            decoded.append(img)
    if not decoded:
        print("[dump] no decodable JPEG frames; skip video", file=sys.stderr)
        return False

    h0 = max(im.shape[0] for im in decoded)
    w0 = max(im.shape[1] for im in decoded)
    # even dims for some encoders
    w0 += w0 % 2
    h0 += h0 % 2

    out_path = Path(out_path)
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(str(out_path), fourcc, float(fps), (w0, h0))
    if not writer.isOpened():
        print(f"[dump] VideoWriter failed: {out_path}", file=sys.stderr)
        return False

    for img in decoded:
        h, w = img.shape[:2]
        if (h, w) != (h0, w0):
            canvas = np.zeros((h0, w0, 3), dtype=img.dtype)
            canvas[:h, :w] = img
            img = canvas
        writer.write(img)
    writer.release()
    return out_path.is_file() and out_path.stat().st_size > 0


def dump_rlog(path, out_dir, fps=10.0):
    """Write log.txt / plot.txt / images.mp4 under out_dir.

    Returns dict with counts.
    """
    p = Path(path)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    log_path = out / "log.txt"
    plot_path = out / "plot.txt"
    video_path = out / "images.mp4"

    t0 = None
    n_log = n_plot = n_img = 0
    frames = []

    with log_path.open("w", encoding="utf-8") as log_fp, \
         plot_path.open("w", encoding="utf-8") as plot_fp:
        log_fp.write(f"# rlog dump log: {p.name}\n")
        plot_fp.write(f"# rlog dump plot: {p.name}\n")

        for rec in iter_records(p, include_jpeg=True):
            ts = int(rec.get("ts") or 0)
            if t0 is None:
                t0 = ts
                hdr = f"# source: {p}\n# t0_ns: {t0}\n"
                log_fp.write(hdr)
                plot_fp.write(hdr)
            t_rel = _rel_t(ts, t0)
            kind = rec.get("kind")

            if kind == "img":
                n_img += 1
                jpeg = rec.get("jpeg") or b""
                if jpeg:
                    frames.append(jpeg)
                continue

            obj = rec.get("obj") or {}
            if _is_log(obj):
                level = obj.get("level", "INFO")
                msg = obj.get("msg", "")
                log_fp.write(_fmt_line(t_rel, "LOG", f"[{level}] {msg}") + "\n")
                n_log += 1
            else:
                body = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
                plot_fp.write(_fmt_line(t_rel, "PLOT", body) + "\n")
                n_plot += 1

        if t0 is None:
            log_fp.write("# (empty or unreadable file)\n")
            plot_fp.write("# (empty or unreadable file)\n")

    video_ok = False
    if frames:
        video_ok = _write_video(frames, video_path, fps)
        if not video_ok and video_path.exists():
            try:
                video_path.unlink()
            except OSError:
                pass

    return {
        "n_log": n_log,
        "n_plot": n_plot,
        "n_img": n_img,
        "video": video_ok,
        "out_dir": str(out),
    }


def dump_rlog_to_path(rlog_path, output=None, fps=10.0):
    """CLI helper. output=None → <stem>_dump/ next to the rlog.

    Returns exit code 0/1.
    """
    src = Path(rlog_path).expanduser().resolve()
    if not src.is_file():
        print(f"[dump] file not found: {src}", file=sys.stderr)
        return 1

    if output is None or output == "":
        out_dir = src.with_name(src.stem + "_dump")
    else:
        out_dir = Path(output).expanduser()

    try:
        info = dump_rlog(src, out_dir, fps=fps)
    except OSError as e:
        print(f"[dump] failed: {e}", file=sys.stderr)
        return 1

    vid = "images.mp4" if info["video"] else "(no video)"
    print(
        f"[dump] {info['out_dir']}  "
        f"log={info['n_log']} plot={info['n_plot']} img={info['n_img']} {vid}",
        file=sys.stderr,
    )
    return 0
