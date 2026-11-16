#!/usr/bin/env python3
"""PPM из ./build/preview -> PNG (x2) + общий лист. Запуск: python3 topng.py out_dir"""
import sys, glob, os
from PIL import Image
d = sys.argv[1] if len(sys.argv) > 1 else "."
files = sorted(glob.glob(os.path.join(d, "*.ppm")))
for f in files:
    Image.open(f).resize((480, 640), Image.NEAREST).save(f[:-4] + ".png")
tags = sorted({os.path.basename(f).split("_")[0] for f in files})
names = ["vfo", "scanner", "analyser", "fc", "loot"]
for t in tags:
    sheet = Image.new("RGB", (240 * len(names) + 6 * (len(names) - 1), 320), (60, 60, 60))
    for i, n in enumerate(names):
        p = os.path.join(d, f"{t}_{n}.ppm")
        if os.path.exists(p):
            sheet.paste(Image.open(p), (i * 246, 0))
    sheet = sheet.resize((sheet.width * 2 // 1, sheet.height * 2 // 1), Image.NEAREST)
    sheet.save(os.path.join(d, f"sheet_{t}.png"))
print("ok", files and len(files))
