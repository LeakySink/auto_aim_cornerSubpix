#!/usr/bin/env python3
"""UDP 网络通达度检测（stdlib only）。

典型用法（两台机器）：

  # 机器 A（例如笔记本）开回显
  ./host/netcheck.sh echo --port 15050

  # 机器 B（例如 NUC）对 A 探测
  ./host/netcheck.sh ping <A的IP> --port 15050 --size 1200 --count 100
  ./host/netcheck.sh ping <A的IP> --port 15050 --size 20000 --count 50

  # 听车上 RemoteLogger beacon
  ./host/netcheck.sh discover

对比 size=1200（MTU 安全）与 size=20000（易 IP 分片）的丢包，可判断 WiFi 大包是否脆弱。
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time

MAGIC = b"NCHK"
HDR = struct.Struct("<4sIQ")  # magic, seq, t_ns
BEACON_PORT = 15999


def _udp(broadcast=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return sock


def _now_ns():
    return time.time_ns()


def cmd_echo(args):
    sock = _udp()
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)
    print("[echo] listening UDP 0.0.0.0:%s  (Ctrl+C stop)" % args.port, flush=True)
    n = 0
    try:
        while True:
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            if len(data) < HDR.size or data[:4] != MAGIC:
                continue
            sock.sendto(data, addr)
            n += 1
            if n == 1 or n % 50 == 0:
                print("[echo] replied %d pkts  last=%s:%s len=%d" % (
                    n, addr[0], addr[1], len(data)), flush=True)
    except KeyboardInterrupt:
        print("\n[echo] stopped after %d replies" % n, flush=True)
    finally:
        sock.close()
    return 0


def cmd_ping(args):
    target = (args.host, args.port)
    size = max(args.size, HDR.size)
    payload = b"\x00" * (size - HDR.size)
    sock = _udp()
    sock.settimeout(args.timeout)
    # connect so ICMP unreachable surfaces on some systems
    try:
        sock.connect(target)
    except OSError as e:
        print("[ping] connect %s:%s failed: %s" % (args.host, args.port, e), file=sys.stderr)
        return 2

    print("[ping] %s:%s  size=%d  count=%d  interval=%.3fs  timeout=%.3fs" % (
        args.host, args.port, size, args.count, args.interval, args.timeout), flush=True)

    sent = 0
    recv = 0
    rtts = []
    losses = []
    t0 = time.monotonic()

    for seq in range(args.count):
        pkt = HDR.pack(MAGIC, seq, _now_ns()) + payload
        t_send = time.monotonic()
        try:
            sock.send(pkt)
            sent += 1
        except OSError as e:
            print("[ping] send failed seq=%d: %s" % (seq, e), flush=True)
            losses.append(seq)
            time.sleep(args.interval)
            continue

        got = False
        deadline = t_send + args.timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                break
            sock.settimeout(remain)
            try:
                data = sock.recv(65535)
            except socket.timeout:
                break
            except OSError as e:
                print("[ping] recv error: %s" % e, flush=True)
                break
            if len(data) < HDR.size or data[:4] != MAGIC:
                continue
            magic, rseq, _tns = HDR.unpack_from(data)
            if rseq != seq:
                continue
            rtt_ms = (time.monotonic() - t_send) * 1000.0
            rtts.append(rtt_ms)
            recv += 1
            got = True
            if args.verbose or seq < 3 or seq + 1 == args.count:
                print("  seq=%d  rtt=%.2f ms  len=%d" % (seq, rtt_ms, len(data)), flush=True)
            break

        if not got:
            losses.append(seq)
            if args.verbose or len(losses) <= 5:
                print("  seq=%d  TIMEOUT" % seq, flush=True)

        elapsed = time.monotonic() - t_send
        sleep_for = args.interval - elapsed
        if sleep_for > 0:
            time.sleep(sleep_for)

    elapsed = time.monotonic() - t0
    loss_pct = (100.0 * (sent - recv) / sent) if sent else 0.0
    print("", flush=True)
    print("=== summary ===", flush=True)
    print("sent=%d  recv=%d  loss=%.1f%%  elapsed=%.2fs" % (
        sent, recv, loss_pct, elapsed), flush=True)
    if rtts:
        rtts_sorted = sorted(rtts)
        avg = sum(rtts) / len(rtts)
        p50 = rtts_sorted[len(rtts_sorted) // 2]
        p95 = rtts_sorted[min(len(rtts_sorted) - 1, int(len(rtts_sorted) * 0.95))]
        print("rtt_ms  min=%.2f  avg=%.2f  p50=%.2f  p95=%.2f  max=%.2f" % (
            rtts_sorted[0], avg, p50, p95, rtts_sorted[-1]), flush=True)
    if losses and not args.verbose:
        show = losses[:10]
        more = "" if len(losses) <= 10 else (" ... +%d" % (len(losses) - 10))
        print("lost_seq  %s%s" % (show, more), flush=True)

    if size >= 1500 and loss_pct > 5:
        print("", flush=True)
        print("hint: size>=1500 且丢包偏高，多半是 WiFi IP 分片；试 --size 1200 对比。",
              flush=True)
    elif size <= 1200 and loss_pct < 2 and args.count >= 20:
        print("", flush=True)
        print("hint: 小包较稳。RemoteLogger 图像已按 ≤1200B 分片，应接近这个水平。",
              flush=True)

    sock.close()
    return 0 if loss_pct < 50 else 1


def cmd_discover(args):
    sock = _udp(broadcast=True)
    try:
        sock.bind(("0.0.0.0", args.port))
    except OSError as e:
        print("[discover] bind :%s failed: %s" % (args.port, e), file=sys.stderr)
        return 2
    sock.settimeout(1.0)
    print("[discover] listening beacon UDP 0.0.0.0:%s  (Ctrl+C stop)" % args.port,
          flush=True)
    seen = {}
    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            try:
                msg = json.loads(data.decode())
            except Exception:
                continue
            if not isinstance(msg, dict) or msg.get("type") != "beacon":
                continue
            name = (msg.get("name") or "").strip()
            ip = (msg.get("ip") or addr[0] or "").strip()
            control = int(msg.get("control") or 15000)
            key = name or ip
            now = time.monotonic()
            first = key not in seen
            seen[key] = now
            if first or args.verbose:
                print("[beacon] name=%-16s ip=%-15s control=%s  from=%s:%s" % (
                    name or "?", ip, control, addr[0], addr[1]), flush=True)
    except KeyboardInterrupt:
        print("\n[discover] saw %d robot(s)" % len(seen), flush=True)
    finally:
        sock.close()
    return 0


def cmd_burst(args):
    """单向灌包（无回显），看对端网卡/资源管理器速率；测丢包请用 ping。"""
    target = (args.host, args.port)
    size = max(args.size, HDR.size)
    payload = b"\x00" * (size - HDR.size)
    sock = _udp()
    print("[burst] -> %s:%s  size=%d  count=%d  rate=%d pkt/s" % (
        args.host, args.port, size, args.count, args.rate), flush=True)
    interval = 1.0 / max(args.rate, 1)
    t0 = time.monotonic()
    sent = 0
    for seq in range(args.count):
        pkt = HDR.pack(MAGIC, seq, _now_ns()) + payload
        try:
            sock.sendto(pkt, target)
            sent += 1
        except OSError as e:
            print("[burst] send failed: %s" % e, flush=True)
            break
        time.sleep(interval)
    elapsed = max(time.monotonic() - t0, 1e-6)
    bytes_total = sent * size
    print("[burst] sent=%d  %.2f pkt/s  %.2f KB/s  elapsed=%.2fs" % (
        sent, sent / elapsed, bytes_total / elapsed / 1024.0, elapsed), flush=True)
    sock.close()
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(
        description="UDP network reachability check for RemoteLogger / LAN")
    sub = p.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("echo", help="UDP echo server (run on one machine)")
    pe.add_argument("--port", type=int, default=15050)
    pe.set_defaults(func=cmd_echo)

    pp = sub.add_parser("ping", help="UDP echo client: loss / RTT / jitter")
    pp.add_argument("host", help="echo server IP")
    pp.add_argument("--port", type=int, default=15050)
    pp.add_argument("--size", type=int, default=1200,
                    help="datagram size bytes (try 1200 vs 20000)")
    pp.add_argument("--count", type=int, default=100)
    pp.add_argument("--interval", type=float, default=0.05)
    pp.add_argument("--timeout", type=float, default=0.5)
    pp.add_argument("-v", "--verbose", action="store_true")
    pp.set_defaults(func=cmd_ping)

    pd = sub.add_parser("discover", help="listen for RemoteLogger beacons")
    pd.add_argument("--port", type=int, default=BEACON_PORT)
    pd.add_argument("-v", "--verbose", action="store_true")
    pd.set_defaults(func=cmd_discover)

    pb = sub.add_parser("burst", help="one-way flood (no echo); watch TX rate")
    pb.add_argument("host")
    pb.add_argument("--port", type=int, default=15050)
    pb.add_argument("--size", type=int, default=1200)
    pb.add_argument("--count", type=int, default=500)
    pb.add_argument("--rate", type=int, default=200, help="packets per second")
    pb.set_defaults(func=cmd_burst)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(file=sys.stderr)
        sys.exit(130)
