#!/usr/bin/env python3
"""
b1.py - decoder for Saturn EVE burst error .B1 images (GRAPHICS/*.B1, MISC/*.B1).

Format:
  0x00  'b1'
  0x02  u16 BE width
  0x04  u16 BE height
  0x06  u16 0
  0x08  stream of 16-bit BE words until EOF (all 585 images of the game consume the
        stream exactly):
          bit15 = 1 : literal pixel, Saturn 15-bit colour (0b1 BBBBB GGGGG RRRRR)
          bit15 = 0 : run; high byte = run length n, low byte = SOURCE MASK.
                      For pixel k of the run, mask bit (7 - (k & 7)) selects
                      the predictor: 1 = copy the previous pixel (left),
                      0 = copy the pixel above (previous row). Bits beyond n
                      are unused padding.
                      mode "repeat" (plain RLE) and "repair" are kept for
                      comparison.
  Sprites (e.g. S10.B1) use 0xFFFF as the transparent colour and may come out
  a row short; missing pixels are filled with the last value.

Usage:
  python b1.py <file.B1> [--png out.png (needs Pillow)] [--mode mask|repeat|above|repair] [--zoom N]
"""
import argparse
import struct
from pathlib import Path

TRANSPARENT = 0xFFFF


def _ch(x):
    return (x & 0x1F, (x >> 5) & 0x1F, (x >> 10) & 0x1F)


def _dist(a, b):
    A, B = _ch(a), _ch(b)
    return abs(A[0] - B[0]) + abs(A[1] - B[1]) + abs(A[2] - B[2])


def decode(data, mode="mask"):
    """Return (w, h, pixels[list of 16-bit words])."""
    w, h = struct.unpack(">HH", data[2:6])
    target = w * h
    # The token stream starts right after the 8-byte header.
    px = []
    runs = []
    last = 0x8000   # black, used before any literal pixel exists
    i = 8
    while i + 1 < len(data) and len(px) < target:
        x = (data[i] << 8) | data[i + 1]
        i += 2
        if x & 0x8000:
            px.append(x)
            last = x
        else:
            n = x >> 8
            mask = x & 0xFF
            runs.append((len(px), n, last))
            if mode == "mask":
                for k in range(n):
                    p = len(px)
                    if (mask >> (7 - (k & 7))) & 1:
                        px.append(px[p - 1] if p >= 1 else last)
                    else:
                        px.append(px[p - w] if p >= w else (px[p - 1] if p >= 1 else last))
            elif mode == "above":
                for k in range(n):
                    p = len(px)
                    px.append(px[p - w] if p >= w else last)
            else:
                px.extend([last] * n)
    while len(px) < target:
        px.append(last)
    px = px[:target]

    if mode == "repair":
        for start, n, last in runs:
            if start < w or start + n + w > target:
                continue
            e_rep = e_abv = 0
            for k in range(n):
                p = start + k
                below = px[p + w]
                e_rep += _dist(last, below)
                e_abv += _dist(px[p - w], below)
            if e_abv + n < e_rep:
                for k in range(n):
                    px[start + k] = px[start + k - w]
    return w, h, px


def to_rgba(px, color_key=TRANSPARENT):
    out = []
    for x in px:
        if x == color_key:
            out.append((0, 0, 0, 0))
            continue
        r = (x & 0x1F) << 3
        g = ((x >> 5) & 0x1F) << 3
        b = ((x >> 10) & 0x1F) << 3
        out.append((r | (r >> 5), g | (g >> 5), b | (b >> 5), 255))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--png")
    ap.add_argument("--mode", default="mask", choices=["mask", "repeat", "above", "repair"])
    ap.add_argument("--zoom", type=int, default=1)
    a = ap.parse_args()
    data = Path(a.file).read_bytes()
    w, h, px = decode(data, a.mode)
    print(f"{a.file}: {w}x{h} pixels={len(px)}")
    if a.png:
        from PIL import Image
        im = Image.new("RGBA", (w, h))
        im.putdata(to_rgba(px))
        if a.zoom > 1:
            im = im.resize((w * a.zoom, h * a.zoom), Image.NEAREST)
        im.save(a.png)
        print("wrote", a.png)


if __name__ == "__main__":
    main()