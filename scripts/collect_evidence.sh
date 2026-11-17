#!/usr/bin/env bash
# 一键采样：编译 → 自检 → 录制 → 出指标 → 打包"证明包"
#
# 用法：
#   scripts/collect_evidence.sh [all|build|check|record|bundle] [选项]
#
# 子命令（默认 all = build + check + record + bundle）：
#   build   只编译（cmake configure + build），日志留在 .evidence/
#   check   只跑 U 层自检（纯算法测试 + check_metrics.py）
#   record  只录制（需要相机/云台）
#   bundle  只打包（用 --rlog 指定已有的 .rlog）
#
# 选项：
#   -c, --config YAML      默认 configs/sentry.yaml
#   -r, --recorder K       debug | fullres | both，默认 debug
#                            debug   = auto_aim_debug_mpc（全量 plot：NIS/armor_id/w）
#                            fullres = eval_record（全分辨率原图，可重跑检测）
#                            both    = 先 fullres 再 debug
#   -d, --duration SEC     默认 60（debug 用超时发 SIGINT 停；fullres 换算成 --max-frames）
#   -o, --out DIR          默认 .evidence/proof_<stem>
#       --rlog FILE        bundle 子命令必填
#   -j, --jobs N           默认 $(nproc)
#       --replay           bundle 时顺带跑 rlog_replay（低分辨率录包会警告）
#       --allow-dirty      工作区有改动也继续（默认拒绝：证明包必须能归因到某个 commit）
#   -h, --help
#
# 产物：
#   .evidence/build.cmake.log、build.log、toolchain.txt
#   logs/run_<ts>.rlog | logs/eval_<ts>.rlog        ← 按 yaml 的 remote_logger.log_dir
#   .evidence/proof_<stem>/
#     MANIFEST.md           git rev / dirty / 配置与录包 sha256 / 判据清单
#     raw/*.rlog            原始录包副本
#     configs/*.yaml        当时的配置副本
#     metrics/metrics.md    rdbg metrics 报告
#     metrics/derived.csv   逐帧派生序列
#     replay/replay.csv     rlog_replay 逐帧 CSV（--replay）
#     replay/dump/          log.txt / plot.txt / images.mp4
#     logs/toolchain.txt    编译器与产物信息
#     SHA256SUMS            整包校验和
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CONFIG="configs/sentry.yaml"
RECORDER="debug"
DURATION=60
OUT=""
RLOG=""
JOBS=""
DO_REPLAY=0
ALLOW_DIRTY=0
CMD="all"

RED=$'\033[31m'; YEL=$'\033[33m'; CYA=$'\033[36m'; GRN=$'\033[32m'; RST=$'\033[0m'
say()  { printf '%s[evidence]%s %s\n' "$CYA" "$RST" "$*"; }
warn() { printf '%s[evidence][warn]%s %s\n' "$YEL" "$RST" "$*" >&2; }
die()  { printf '%s[evidence][fatal]%s %s\n' "$RED" "$RST" "$*" >&2; exit 1; }

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ------------------------------------------------------------------ 参数
while [ $# -gt 0 ]; do
  case "$1" in
    all|build|check|record|bundle) CMD="$1"; shift ;;
    -c|--config)   CONFIG="${2:?}"; shift 2 ;;
    -r|--recorder) RECORDER="${2:?}"; shift 2 ;;
    -d|--duration) DURATION="${2:?}"; shift 2 ;;
    -o|--out)      OUT="${2:?}"; shift 2 ;;
    --rlog)        RLOG="${2:?}"; shift 2 ;;
    -j|--jobs)     JOBS="${2:?}"; shift 2 ;;
    --replay)      DO_REPLAY=1; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) die "未知参数：$1（--help 看用法）" ;;
  esac
done

EVID="$ROOT/.evidence"
mkdir -p "$EVID"
[ -n "$JOBS" ] || { JOBS="$( (nproc 2>/dev/null || echo 4) )"; }

PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  if command -v python3 >/dev/null 2>&1; then PY=python3; else PY=python; fi
fi

# 仓库没设 RUNTIME_OUTPUT_DIRECTORY，产物按源码目录分放；两种布局都找
find_bin() {
  local n="$1" p
  for p in "$ROOT/build/$n" "$ROOT/build/src/$n" "$ROOT/build/tests/$n" \
           "$ROOT/build/calibration/$n"; do
    [ -x "$p" ] && { printf '%s\n' "$p"; return 0; }
  done
  return 1
}

yaml_get() {  # yaml_get <key> [默认值]
  local key="$1" def="${2:-}"
  local v
  v="$(sed -n "s/^[[:space:]]*${key}[[:space:]]*:[[:space:]]*\([^[:space:]#]*\).*/\1/p" "$CONFIG" | head -1)"
  printf '%s\n' "${v:-$def}"
}

git_rev()   { git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown; }
git_branch(){ git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown; }
git_dirty() { git -C "$ROOT" status --porcelain 2>/dev/null | wc -l | tr -d ' '; }

preflight() {
  [ -f "$CONFIG" ] || die "找不到配置：$CONFIG"
  command -v cmake >/dev/null 2>&1 || die "缺 cmake"
  command -v c++   >/dev/null 2>&1 || die "缺 c++/g++"
  if [ "$ALLOW_DIRTY" -eq 0 ] && [ "$(git_dirty)" != "0" ]; then
    die "工作区有 $(git_dirty) 个未提交改动；证明包必须能归因到某个 commit（确认无误可加 --allow-dirty）"
  fi
  local ovino
  ovino="$(yaml_get openvino_dir "")"
  if [ -z "$ovino" ] && [ ! -d /opt/intel/openvino_2024.6.0/runtime/cmake ]; then
    warn "没找到 OpenVINO（CMakeLists 里写死 /opt/intel/openvino_2024.6.0）；如路径不同请 cmake -DOpenVINO_DIR=..."
  fi
  # 相机 SDK 运行时依赖（仓里自带 .so，运行时要能被找到）
  export LD_LIBRARY_PATH="$ROOT/io/hikrobot/lib/amd64:$ROOT/io/mindvision/lib/amd64:${LD_LIBRARY_PATH:-}"
}

# ------------------------------------------------------------------ build
do_build() {
  say "编译（-j$JOBS）"
  {
    echo "git: $(git_rev) ($(git_branch))  dirty=$(git_dirty)"
    cmake --version | head -1
    c++ --version | head -1
  } > "$EVID/toolchain.txt"

  cmake -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$EVID/build.cmake.log"
  set +e
  cmake --build build -j"$JOBS" 2>&1 | tee "$EVID/build.log"
  local rc=${PIPESTATUS[0]}
  set -e
  [ "$rc" -eq 0 ] || die "编译失败（rc=$rc），看 $EVID/build.log"

  local nerr
  nerr="$(grep -c 'error:' "$EVID/build.log" 2>/dev/null || true)"
  [ "${nerr:-0}" = "0" ] || die "build.log 里有 $nerr 条 error:"
  say "编译通过（0 error）"

  local b miss=0
  for b in auto_aim_debug_mpc eval_record rlog_replay; do
    if b="$(find_bin "$b")"; then
      echo "$(basename "$b"): ${b#$ROOT/}  $(stat -c '%s bytes' "$b" 2>/dev/null)" >> "$EVID/toolchain.txt"
    else
      echo "$b: 缺失" >> "$EVID/toolchain.txt"; miss=1
    fi
  done
  [ "$miss" -eq 0 ] || die "关键产物缺失，见 $EVID/toolchain.txt"
}

# ------------------------------------------------------------------ check
do_check() {
  say "U 层自检"
  local fail=0 b
  # include 自洽性：Windows 上编不了 OpenVINO 的入口，靠静态检查兜住
  # "用了 io::Gimbal / auto_aim::Planner 却没直接 include"这类在 Linux 上
  # 要到 55% 才炸的问题（2026-11 移除打符子系统时踩过一次）。
  if [ -f "$ROOT/scripts/check_includes.py" ]; then
    if "$PY" "$ROOT/scripts/check_includes.py" >/dev/null 2>&1; then
      say "  OK   check_includes.py"
    else
      printf '%s  FAIL check_includes.py（运行它看具体缺哪行）%s\n' "$RED" "$RST" >&2
      fail=1
    fi
  fi
  for b in ballistics_poly_check limit_rad_check ekf_update_check trajectory_nan_check; do
    if b="$(find_bin "$b")"; then
      if "$b" >/dev/null 2>&1; then say "  OK   $b"; else
        printf '%s  FAIL %s%s\n' "$RED" "$b" "$RST" >&2; fail=1; fi
    else
      warn "  跳过 $b（未编译）"
    fi
  done
  if [ -f "$ROOT/tests/eval/check_metrics.py" ]; then
    if (cd "$ROOT" && "$PY" tests/eval/check_metrics.py >/dev/null 2>&1); then
      say "  OK   check_metrics.py"
    else
      printf '%s  FAIL check_metrics.py%s\n' "$RED" "$RST" >&2; fail=1
    fi
  fi
  [ "$fail" -eq 0 ] || die "自检失败：指标工具本身不可信时，后面的结论都不用看"
}

# ------------------------------------------------------------------ record
newest_rlog() { ls -t "$ROOT"/logs/*.rlog 2>/dev/null | head -1; }

do_record() {
  case "$RECORDER" in debug|fullres|both) ;; *) die "--recorder 只能是 debug|fullres|both" ;; esac
  mkdir -p "$ROOT/logs"
  local before after
  before="$(newest_rlog || true)"

  if [ "$RECORDER" = "fullres" ] || [ "$RECORDER" = "both" ]; then
    local bin out
    bin="$(find_bin eval_record)" || die "eval_record 未编译"
    out="$ROOT/logs/eval_$(date +%Y%m%d_%H%M%S).rlog"
    say "录制（全分辨率）→ ${out#$ROOT/}  ${DURATION}s"
    "$bin" "$CONFIG" --out "$out" --every 1 --max-frames "$((DURATION * 100))" --quality 90
    before="$out"
  fi

  if [ "$RECORDER" = "debug" ] || [ "$RECORDER" = "both" ]; then
    local bin
    bin="$(find_bin auto_aim_debug_mpc)" || die "auto_aim_debug_mpc 未编译"
    say "录制（主链路 + 估计指标）→ logs/  ${DURATION}s（到时自动 SIGINT 收尾）"
    # Exiter 处理 SIGINT；timeout 会返回 124，属正常
    timeout -s INT "$DURATION" "$bin" "$CONFIG" || true
    sleep 1
    after="$(newest_rlog || true)"
    [ -n "$after" ] || die "没找到新录的 .rlog（检查 yaml 的 remote_logger.log_dir）"
    before="$after"
  fi

  [ -n "$before" ] || die "没有可打包的 .rlog"
  RLOG="$before"
  say "本次录包：${RLOG#$ROOT/}"
}

# ------------------------------------------------------------------ bundle
do_bundle() {
  [ -n "$RLOG" ] || RLOG="$(newest_rlog || true)"
  [ -n "$RLOG" ] && [ -f "$RLOG" ] || die "bundle 需要 --rlog <file.rlog>"
  local stem
  stem="$(basename "$RLOG" .rlog)"
  OUT="${OUT:-$EVID/proof_$stem}"
  mkdir -p "$OUT"/{raw,configs,metrics,replay,logs}
  # 必须转成绝对路径：下面要在 `cd "$ROOT/host"` 的子 shell 里往外写，
  # 相对路径会解析到 host/ 下面（曾导致 metrics.md 写不进、打包只剩 5 个文件）。
  OUT="$(cd "$OUT" && pwd)"

  say "打包 → ${OUT#$ROOT/}"
  local rlog_abs
  rlog_abs="$(cd "$(dirname "$RLOG")" && pwd)/$(basename "$RLOG")"
  cp -f "$rlog_abs" "$OUT/raw/"
  cp -f "$CONFIG"   "$OUT/configs/"

  { cat "$EVID/toolchain.txt" 2>/dev/null || true
    echo "rlog: $(basename "$rlog_abs")  $(du -h "$rlog_abs" | cut -f1)"
    echo "rlog sha256: $(sha256sum "$rlog_abs" | cut -d' ' -f1)"
    echo "config sha256: $(sha256sum "$CONFIG" | cut -d' ' -f1)"
  } > "$OUT/logs/toolchain.txt"

  local imgw
  imgw="$(yaml_get img_width 0)"
  if [ "${imgw:-0}" -lt 1000 ] 2>/dev/null; then
    warn "录包图片宽度 img_width=$imgw（<1000）→ rlog_replay 的距离量纲不可用，只能当'检出与否'看；要重跑检测请用 --recorder fullres"
  fi

  ( cd "$ROOT/host" && "$PY" -m rdbg metrics "$rlog_abs" --derived "$OUT/metrics/derived.csv" \
      > "$OUT/metrics/metrics.md" ) || warn "metrics 失败（不影响其余产物）"
  [ -s "$OUT/metrics/metrics.md" ] || die "metrics 报告为空：先用一条 rlog 手跑 'cd host && python -m rdbg metrics <rlog>' 看真实报错"
  ( cd "$ROOT/host" && "$PY" -m rdbg dump "$rlog_abs" -o "$OUT/replay/dump" --fps 10 >/dev/null ) \
      || warn "dump 失败（不影响指标结论）"

  if [ "$DO_REPLAY" -eq 1 ]; then
    local rb
    if rb="$(find_bin rlog_replay)"; then
      "$rb" "$CONFIG" "$rlog_abs" --out "$OUT/replay/replay.csv" --tracker --quiet \
        || warn "rlog_replay 失败"
    else
      warn "rlog_replay 未编译，跳过 --replay"
    fi
  fi

  {
    echo "# 证明包 $stem"
    echo
    echo "| 项 | 值 |"
    echo "|---|---|"
    echo "| 生成时间 | $(date -Iseconds) |"
    echo "| 主机 | $(hostname) $(uname -m) / $(uname -r) |"
    echo "| git HEAD | \`$(git_rev)\` |"
    echo "| git 分支 | \`$(git_branch)\` |"
    echo "| 工作区改动 | $(git_dirty) |"
    echo "| 配置 | \`$CONFIG\` sha256=$(sha256sum "$CONFIG" | cut -d' ' -f1) |"
    echo "| 录包 | \`$(basename "$rlog_abs")\` $(du -h "$rlog_abs" | cut -f1) sha256=$(sha256sum "$rlog_abs" | cut -d' ' -f1) |"
    echo
    echo "## 判据（逐条打勾再用）"
    echo
    echo "- [ ] \`logs/toolchain.txt\` 里 4 个产物齐全，\`build.log\` 0 error"
    echo "- [ ] 上面 git HEAD 就是你要证明的 commit，且工作区改动=0"
    echo "- [ ] \`metrics/metrics.md\`：NIS 越限率、armor_id 每圈折返、w 过零 三个数符合预期"
    echo "- [ ] 同一 rlog 跑两次 metrics 结果逐字一致"
  } > "$OUT/MANIFEST.md"

  ( cd "$OUT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )
  say "完成：${OUT#$ROOT/}"
  ( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null && echo "  SHA256SUMS 校验通过（$(grep -c . SHA256SUMS) 个文件）" )
}

# ------------------------------------------------------------------ main
case "$CMD" in
  build)  preflight; do_build ;;
  check)  do_check ;;
  record) preflight; do_record ;;
  bundle) do_bundle ;;
  all)    preflight; do_build; do_check; do_record; do_bundle ;;
esac
