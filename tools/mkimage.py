#!/usr/bin/env python3
"""Склеить stage-2 + приложение в образ для стокового загрузчика.

Образ пишется с 0x08002800 и занимает ровно 0x3D800 (246 КиБ):
  +0x0000  stage-2 (8 КиБ, добит 0xFF)
  +0x2000  приложение (добито до 2048)
  +0x3D000 метаданные (magic/len/crc32 + crc32 метаданных)

Usage: mkimage.py <boot2.bin> <app.bin> <out.bin>
"""

import struct
import sys
import zlib

STOCK_BASE = 0x08002800
APP_BASE = 0x08004800
META_ADDR = 0x0803F800
META_MAGIC = 0x42324D45
BOOT2_LEN = 0x2000
IMAGE_LEN = 0x08040000 - STOCK_BASE
BLOCK = 2048


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 1

    boot2_path, app_path, out_path = sys.argv[1:4]
    boot2 = open(boot2_path, "rb").read()
    if len(boot2) > BOOT2_LEN:
        print(f"stage-2 too big: {len(boot2)} > {BOOT2_LEN}", file=sys.stderr)
        return 1

    app = bytearray(open(app_path, "rb").read())
    if not app:
        print("empty application", file=sys.stderr)
        return 1
    if len(app) > META_ADDR - APP_BASE:
        print(f"application too big: {len(app)} > {META_ADDR - APP_BASE}",
              file=sys.stderr)
        return 1
    app.extend(b"\xff" * (-len(app) % BLOCK))

    img = bytearray(b"\xff" * IMAGE_LEN)
    img[:len(boot2)] = boot2

    app_off = APP_BASE - STOCK_BASE
    img[app_off:app_off + len(app)] = app

    app_crc = zlib.crc32(app) & 0xFFFFFFFF
    meta = struct.pack("<III", META_MAGIC, len(app), app_crc)
    meta += struct.pack("<I", zlib.crc32(meta) & 0xFFFFFFFF)
    meta_off = META_ADDR - STOCK_BASE
    img[meta_off:meta_off + len(meta)] = meta

    with open(out_path, "wb") as f:
        f.write(img)
    print(f"{out_path}: stage-2 {len(boot2)} B, app {len(app)} B, "
          f"crc {app_crc:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
