#!/usr/bin/env python3
"""ledlog.py <seconds> [outfile]

Logs HyperHDR's LED output via the JSON API LED stream (port 19444) and reports
periods where the output is black or frozen. Used as the external measurement
for the success criteria (no black/frozen LEDs for longer than ~0.5 s).

Each output line: wall-clock time, mean RGB, max channel, frozen flag.
At the end, a summary lists every black or frozen period longer than 300 ms.
"""
import json
import socket
import sys
import time

HOST, PORT = "192.168.129.249", 19444
BLACK_MAX = 8  # a frame counts as black when no channel exceeds this value


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    out = open(sys.argv[2], "w") if len(sys.argv) > 2 else None
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(b'{"command":"ledcolors","subcommand":"ledstream-start","tan":1}\n')
    buf = b""
    t_end = time.time() + secs
    prev = None
    state = None  # ("black"|"frozen", since)
    periods = []
    frames = 0
    while time.time() < t_end:
        try:
            chunk = s.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("command") != "ledcolors-ledstream-update":
                continue
            leds = msg["result"]["leds"]
            now = time.time()
            frames += 1
            n = len(leds) // 3
            mean = [round(sum(leds[c::3]) / n, 1) for c in range(3)]
            mx = max(leds)
            frozen = prev is not None and leds == prev
            prev = leds
            kind = "black" if mx <= BLACK_MAX else ("frozen" if frozen else None)
            if kind != (state[0] if state else None):
                if state:
                    periods.append((state[0], state[1], now))
                state = (kind, now) if kind else None
            if out:
                out.write(f"{now:.3f} {mean[0]} {mean[1]} {mean[2]} {mx} {int(frozen)}\n")
    if state:
        periods.append((state[0], state[1], time.time()))
    s.close()
    long_ = [(k, a, b) for k, a, b in periods if b - a > 0.3]
    print(f"frames={frames} ({frames / secs:.1f}/s); black/frozen periods > 300 ms: {len(long_)}")
    for k, a, b in long_:
        print(f"  {k:6s} {time.strftime('%H:%M:%S', time.localtime(a))}.{int(a % 1 * 1000):03d} "
              f"duration {b - a:.2f} s")


if __name__ == "__main__":
    main()
