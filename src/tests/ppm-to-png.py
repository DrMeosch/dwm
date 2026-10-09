#!/usr/bin/env python3
"""Convert the test harness's binary PPM root capture to a PNG artifact."""

import struct
import sys
import zlib
from pathlib import Path


def chunk(kind, data):
    return (
        struct.pack("!I", len(data))
        + kind
        + data
        + struct.pack("!I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} INPUT.ppm OUTPUT.png")

    source, target = map(Path, sys.argv[1:])
    with source.open("rb") as image:
        if image.readline().strip() != b"P6":
            raise SystemExit(f"{source}: expected binary PPM (P6)")
        width, height = map(int, image.readline().split())
        if image.readline().strip() != b"255":
            raise SystemExit(f"{source}: expected 8-bit RGB channels")
        pixels = image.read()

    if len(pixels) != width * height * 3:
        raise SystemExit(f"{source}: pixel data length does not match its dimensions")

    rows = b"".join(
        b"\0" + pixels[y * width * 3 : (y + 1) * width * 3]
        for y in range(height)
    )
    header = struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)
    target.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b"")
    )
    print(f"wrote {target} ({width}x{height})")


if __name__ == "__main__":
    main()
