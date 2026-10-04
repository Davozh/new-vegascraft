#!/usr/bin/env python3
"""Captures ONLY Fallout: New Vegas's window, and only if it is on the visible workspace of its monitor (Hyprland).
Usage: fnvshot.py out.png [scale]"""
import json, os, socket, subprocess, sys
SOCK = f"/run/user/{os.getuid()}/hypr/{os.environ['HYPRLAND_INSTANCE_SIGNATURE']}/.socket.sock"
def q(c):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK); s.send(c.encode()); r = b""
    while True:
        d = s.recv(65536)
        if not d: break
        r += d
    return json.loads(r)
win = next((c for c in q("j/clients") if c["class"] == "steam_app_22380"), None)
if not win: sys.exit("FNV is not running")
mon = next(m for m in q("j/monitors") if m["id"] == win["monitor"])
if mon["activeWorkspace"]["id"] != win["workspace"]["id"]:
    sys.exit("FNV is not on the visible workspace: not capturing")
x, y = win["at"]; w, h = win["size"]
subprocess.run(["grim", "-g", f"{x},{y} {w}x{h}", "-s", sys.argv[2] if len(sys.argv) > 2 else "0.5", sys.argv[1]], check=True)
print("ok", sys.argv[1])
