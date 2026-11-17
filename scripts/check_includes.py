#!/usr/bin/env python3
"""检查 src/*.cpp 是否**直接** include 了它用到的类型所在的头文件。

为什么需要这个：`standard_mpc.cpp` 曾经靠 `tasks/auto_buff/*.hpp` 间接拿到
`io::Gimbal` / `auto_aim::Planner` 的声明。2026-11 移除打符子系统时把那几个
include 删了，代码里却仍在用这两个类型 —— Windows 上编不了 OpenVINO 的入口，
Linux 上编到 55% 才报 `'Gimbal' is not a member of 'io'`。这类"靠别人顺带
include"的依赖只能在真机上暴露，所以用静态检查兜住。

用法：
    python3 scripts/check_includes.py            # 检查 src/
    python3 scripts/check_includes.py --all      # src/ + tests/
退出码 0 = 全部直接 include 到位。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 命名空间符号 -> 必须直接 include 的头文件
SYMBOLS = {
    "io::Camera": "io/camera.hpp",
    "io::Gimbal": "io/gimbal/gimbal.hpp",
    "io::GimbalMode": "io/gimbal/gimbal.hpp",
    "io::GimbalState": "io/gimbal/gimbal.hpp",
    "io::CBoard": "io/cboard.hpp",
    "io::Command": "io/command.hpp",
    "io::ShootMode": "io/cboard.hpp",
    "auto_aim::Planner": "tasks/auto_aim/planner/planner.hpp",
    "auto_aim::Aimer": "tasks/auto_aim/aimer.hpp",
    "auto_aim::Shooter": "tasks/auto_aim/shooter.hpp",
    "auto_aim::Solver": "tasks/auto_aim/solver.hpp",
    "auto_aim::Tracker": "tasks/auto_aim/tracker.hpp",
    "auto_aim::YOLO": "tasks/auto_aim/yolo.hpp",
    "auto_aim::Target": "tasks/auto_aim/target.hpp",
    "auto_aim::Armor": "tasks/auto_aim/armor.hpp",
    "auto_aim::CenterAimer": "tasks/auto_aim/center_aimer.hpp",
    "auto_aim::CommandGener": "tasks/auto_aim/multithread/commandgener.hpp",
    "tools::Exiter": "tools/exiter.hpp",
    "tools::RemoteLogger": "tools/remote_logger.hpp",
    "tools::ThreadSafeQueue": "tools/thread_safe_queue.hpp",
    "tools::Trajectory": "tools/trajectory.hpp",
}

INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def check(path: Path) -> list:
    text = path.read_text(encoding="utf-8", errors="replace")
    code = COMMENT_RE.sub("", text)          # 注释里的符号不算使用
    includes = set(INCLUDE_RE.findall(text))
    problems = []
    for sym, header in SYMBOLS.items():
        # 只按 `ns::Sym` 的完整词形匹配，避免 `io::Camera` 命中 `io::CameraX`
        if not re.search(re.escape(sym) + r"\b", code):
            continue
        if header in includes:
            continue
        # 若用了 io::Command 但 include 了 aimer.hpp（后者 typedef/包含 command）也算到位
        if sym == "io::Command" and "tasks/auto_aim/aimer.hpp" in includes:
            continue
        problems.append(f"{path.relative_to(ROOT)}: 用了 {sym}，但没有直接 #include \"{header}\"")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="同时检查 tests/")
    args = ap.parse_args()

    targets = sorted((ROOT / "src").glob("*.cpp"))
    if args.all:
        targets += sorted((ROOT / "tests").rglob("*.cpp"))

    problems = []
    for p in targets:
        problems += check(p)

    for msg in problems:
        print(msg)
    print(f"\n检查 {len(targets)} 个 TU，问题 {len(problems)} 处")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
