#!/usr/bin/env python3
"""
cmpdec.py - decoder for the Saturn EVE burst error MISC/*.CMP files (SPR1, SPR2,
SPR3, COMFONT, FONT) and converter of the resulting 4/8-bit BMPs to EVB1 textures.

Format: a byte-oriented LZ stream of tokens
  c < 0x80        : copy the next c+1 bytes literally
  c >= 0x80, n    : copy n+3 bytes from (c & 0x7F) + 1 bytes back in the output
                    (overlapping copies allowed, so "80 01" after a 00 = four zeros)
SPR1/SPR2/SPR3/COMFONT decode to exact-size Windows BMPs (8 bpp / 4 bpp, bottom-up).
FONT.CMP decodes to 280192 raw bytes (no BMP header): 2189 glyphs of 16x16 pixels,
4 bits per pixel; CHARCODE lists their Shift-JIS codes (see convert.py).

Contents of the sheets:
  SPR1.CMP  192x364 8bpp : title / system menu labels, 14 rows of 26 px:
            0 KOJIROH, 1 MARINA, 2 PRESS START, 3 NEW GAME, 4 SAVE, 5 LOAD, 6 OPTION,
            7 EXIT, 8 VOICE <>, 9 BGM < FAST, 10 TEXT <NORMAL>, 11 SLOW >, 12 slider,
            13 slider knob row
  SPR2.CMP  288x432 8bpp : save/load screen (KOJIROH/MARINA tabs, digits, disc icons
            1/2/3, INTERNAL / CARTRIDGE / EXIT, frame ornaments)
  SPR3.CMP  232x378 4bpp : message window frame
  COMFONT.CMP 444x192 4bpp : 12x12 command font, 37 x 16 cells; COMFONT.SET lists the
            Shift-JIS code of each cell in order

Usage:
  python cmpdec.py <file.CMP> --bmp out.bmp
  python cmpdec.py <file.CMP> --evb1 out.BIN [--crop x,y,w,h] [--key 0] [--rows N --prefix out/NAME_R]
    --evb1 writes ARGB4444 (format 1) with palette index --key fully transparent
    --rows N splits the sheet into N equal horizontal strips <prefix>00.BIN ... (SPR1)
EVB1 layout: "EVB1" u16 w u16 h u16 fmt u16 0, then w*h*2 bytes (see convert.py).
"""
import argparse, struct, sys
from pathlib import Path

def decode(data):
    out = bytearray(); i = 0
    while i < len(data):
        c = data[i]; i += 1
        if c < 0x80:
            n = c + 1; out += data[i:i + n]; i += n
        else:
            if i >= len(data): break
            ln = data[i] + 3; i += 1
            start = len(out) - ((c & 0x7F) + 1)
            for j in range(ln):
                out.append(out[start + j] if start + j >= 0 else 0)
    return bytes(out)

def bmp_pixels(bmp):
    """Return (w, h, palette[(r,g,b)], rows top-down of palette indices) for 4/8 bpp BMPs."""
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18); bpp = struct.unpack_from("<H", bmp, 28)[0]
    ncol = struct.unpack_from("<I", bmp, 46)[0] or (1 << bpp)
    pal = [(bmp[54 + 4 * k + 2], bmp[54 + 4 * k + 1], bmp[54 + 4 * k]) for k in range(ncol)]
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(abs(h)):
        src = off + (abs(h) - 1 - y if h > 0 else y) * stride
        line = bmp[src:src + stride]
        if bpp == 8:
            rows.append(list(line[:w]))
        else:
            r = []
            for x in range(w):
                b = line[x // 2]; r.append(b >> 4 if x % 2 == 0 else b & 15)
            rows.append(r)
    return w, abs(h), pal, rows

def write_evb1(path, w, h, pal, rows, key, crop=None):
    x0, y0, cw, ch = crop if crop else (0, 0, w, h)
    px = bytearray()
    for y in range(y0, y0 + ch):
        for x in range(x0, x0 + cw):
            idx = rows[y][x]
            if idx == key:
                px += struct.pack("<H", 0)
            else:
                r, g, b = pal[idx]
                px += struct.pack("<H", 0xF000 | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"EVB1" + struct.pack("<HHHH", cw, ch, 1, 0) + px)
    return cw, ch

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmp")
    ap.add_argument("--bmp")
    ap.add_argument("--evb1")
    ap.add_argument("--crop", help="x,y,w,h")
    ap.add_argument("--key", type=int, default=0, help="transparent palette index (-1 = none)")
    ap.add_argument("--rows", type=int, default=0)
    ap.add_argument("--prefix")
    a = ap.parse_args()
    raw = decode(Path(a.cmp).read_bytes())
    print(f"{a.cmp}: {len(raw)} bytes decoded")
    if a.bmp:
        Path(a.bmp).write_bytes(raw); print("wrote", a.bmp)
    if raw[:2] != b"BM":
        return
    w, h, pal, rows = bmp_pixels(raw)
    print(f"BMP {w}x{h}, {len(pal)} colours")
    if a.rows and a.prefix:
        rh = h // a.rows
        for k in range(a.rows):
            p = f"{a.prefix}{k:02d}.BIN"
            write_evb1(p, w, h, pal, rows, a.key, (0, k * rh, w, rh)); print("wrote", p)
    elif a.evb1:
        crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None
        cw, ch = write_evb1(a.evb1, w, h, pal, rows, a.key, crop); print("wrote", a.evb1, cw, ch)

if __name__ == "__main__":
    main()
