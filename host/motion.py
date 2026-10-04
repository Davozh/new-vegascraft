#!/usr/bin/env python3
"""Analyses a Shift+F12 motion capture: for each synchronised ReShade screenshot, draws where the F12 probe pillar
should be at FNV's pose (magenta) and where Minecraft's frame put it (cyan), finds the real pillar by its diamond
colour, and prints the offsets in pixels. Usage: [PILLAR="x y z"] motion.py outdir [game dir]"""
import glob, os, re, sys
import numpy as np
from PIL import Image, ImageDraw
out = sys.argv[1]; game = sys.argv[2] if len(sys.argv) > 2 else "/mnt/juegos/SteamLibrary/steamapps/common/Fallout New Vegas"
os.makedirs(out, exist_ok=True)
vlog = open(os.path.join(game, "vegascraft.log")).read().splitlines()
probes = [l for l in vlog if l.startswith("probe: FNV hit")]
# the pillar stays in Minecraft's world across FNV restarts: PILLAR="x y z" when this session's log has no probe line
bx, by, bz = map(int, (probes[-1].split("pillar at block ")[1] if probes else os.environ["PILLAR"]).split())
caps = {}
for l in open(os.path.join(game, "ReShade.log"), errors="replace"):
    m = re.search(r"VegasCraft capture (vc\d+) host (\S+) (\S+) (\S+) pos (\S+) (\S+) (\S+) \| minecraft (\S+) (\S+) (\S+) pos (\S+) (\S+) (\S+)", l)
    if m:
        v = list(map(float, m.groups()[1:])); caps[m.group(1)] = (v[0:3], v[3:6], v[6:9], v[9:12])
D = np.pi / 180
def camrot(yaw, pitch):
    a, b = np.pi - yaw * D, -pitch * D
    ry = np.array([[np.cos(a), 0, np.sin(a)], [0, 1, 0], [-np.sin(a), 0, np.cos(a)]])
    rx = np.array([[1, 0, 0], [0, np.cos(b), -np.sin(b)], [0, np.sin(b), np.cos(b)]])
    return ry @ rx
corners = [np.array([x, y, z], float) for x in (bx, bx + 1) for y in (by, by + 2) for z in (bz, bz + 1)]
def project(rot, pos, w, h):
    yaw, pitch, fov = rot; R = camrot(yaw, pitch); t = np.tan(fov * D / 2); pts = []
    for c in corners:
        pc = R.T @ (c - np.array(pos))
        pts.append(((pc[0] / -pc[2] / (t * w / h) * 0.5 + 0.5) * w, (0.5 - pc[1] / -pc[2] / t * 0.5) * h))
    return pts
def box(d, pts, colour):
    for a in range(8):
        for b in range(a + 1, 8):
            if bin(a ^ b).count("1") == 1:
                d.line([pts[a], pts[b]], fill=colour, width=3)
for tag in sorted(caps):
    files = glob.glob(os.path.join(game, f"*{tag}*"))
    if not files:
        print(tag, "no screenshot"); continue
    img = Image.open(files[0]).convert("RGB"); w, h = img.size
    hr, hp, mr, mp = caps[tag]
    a = np.asarray(img).astype(int)
    mask = (a[..., 1] > 150) & (a[..., 2] > 140) & (a[..., 0] < 120) & (a[..., 1] - a[..., 0] > 70)  # diamond block
    ys, xs = np.nonzero(mask)
    host = project(hr, hp, w, h); mc = project(mr, mp, w, h)
    cx = lambda pts: sum(p[0] for p in pts) / 8
    real = xs.mean() if len(xs) > 200 else float("nan")
    print(f"{tag}: real x {real:7.1f} | FNV pose {cx(host):7.1f} | Minecraft pose {cx(mc):7.1f} | real-FNV {real - cx(host):6.1f} px | "
          f"pose lag yaw {((hr[0] - mr[0] + 540) % 360) - 180:6.2f} deg, pos {np.linalg.norm(np.array(hp) - np.array(mp)):.3f} m")
    d = ImageDraw.Draw(img); box(d, host, (255, 0, 255)); box(d, mc, (0, 255, 255))
    img.resize((w // 2, h // 2)).save(os.path.join(out, f"{tag}.png"))
