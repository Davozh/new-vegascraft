#!/usr/bin/env python3
"""Draws on an FNV screenshot (magenta) where the F12 probe pillar SHOULD be, from FNV's camera in the last F10 dump.
Usage: predict.py shot.png out.png [path to vegascraft.log]"""
import re, sys
import numpy as np
from PIL import Image, ImageDraw
path = sys.argv[3] if len(sys.argv) > 3 else "/mnt/juegos/SteamLibrary/steamapps/common/Fallout New Vegas/vegascraft.log"
log = open(path).read().splitlines()
yoff = float(re.search(r"yOffset (-?\d+\.\d+)", [l for l in log if l.startswith("levelled")][-1]).group(1))
bx, by, bz = map(int, [l for l in log if l.startswith("probe: FNV hit")][-1].split("pillar at block ")[1].split())
nums = list(map(float, re.findall(r"-?\d+\.\d+", [l for l in log if l.startswith("at present: camera pos")][-1])))
cam, fwd, t, r = np.array(nums[0:3]), np.array(nums[3:6]), nums[6], nums[7]
W = np.array([list(map(float, l.split(":")[2].split())) for l in [l for l in log if l.startswith("at present: worldToCam row")][-4:]])
right, up = W[0, :3] / np.linalg.norm(W[0, :3]), W[1, :3] / np.linalg.norm(W[1, :3])
U = 70.0  # Minecraft block (x, y, z): FNV x [x, x+1] m, y [-(z+1), -z] m, z [y - yOffset, +2] m
corners = [np.array([X * U, -Z * U, (Y - yoff) * U]) for X in (bx, bx + 1) for Z in (bz, bz + 1) for Y in (by, by + 2)]
img = Image.open(sys.argv[1]).convert("RGB"); w, h = img.size; d = ImageDraw.Draw(img)
pts = []
for c in corners:
    v = c - cam; zf = v @ fwd
    pts.append((((v @ right) / zf / r * 0.5 + 0.5) * w, (0.5 - (v @ up) / zf / t * 0.5) * h))
for a in range(8):
    for b in range(a + 1, 8):
        if bin(a ^ b).count("1") == 1:
            d.line([pts[a], pts[b]], fill=(255, 0, 255), width=2)
img.save(sys.argv[2])
print("pillar", (bx, by, bz), "yOffset", yoff, "camera", cam.round(1), "distance m", round(float(np.linalg.norm(corners[0] - cam)) / U, 2))
