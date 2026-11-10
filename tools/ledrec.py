#!/usr/bin/env python3
"""Record HyperHDR's per-LED output stream (JSON API) to a JSON-lines file.

usage: ledrec.py <seconds> <out.jsonl> [host] [port]
Each line: [mac_time_seconds, [r,g,b, r,g,b, ...]] in HyperHDR's LED order.
"""
import json
import socket
import sys
import time

secs, out = float(sys.argv[1]), sys.argv[2]
host = sys.argv[3] if len(sys.argv) > 3 else "192.168.129.249"
port = int(sys.argv[4]) if len(sys.argv) > 4 else 19444
s = socket.create_connection((host, port), timeout=5)
s.sendall(b'{"command":"ledcolors","subcommand":"ledstream-start","tan":1}\n')
buf, n, end = b"", 0, time.time() + secs
with open(out, "w") as f:
    while time.time() < end:
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
                m = json.loads(line)
            except ValueError:
                continue
            if m.get("command") == "ledcolors-ledstream-update":
                f.write(json.dumps([round(time.time(), 3), m["result"]["leds"]]) + "\n")
                n += 1
print("recorded", n, "LED frames")
