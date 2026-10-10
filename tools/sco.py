#!/usr/bin/env python3
"""
sco.py - EVE burst error (Saturn) .SCO scenario inspector / disassembler.

Usage:
  python sco.py <file.SCO> [--mode info|text|disasm|menus|tokens] [--start IP] [--limit N]

Format (see docs/formats.md):
  * whole file XOR 0xFF
  * instruction stream of 16-bit big-endian words from offset 0; opcodes are 0x80xx
  * text/name operands are WORD addresses: byte offset = operand * 2 (same file)
  * 8035 addr flag [voice]   text; voice/SE token address present when flag != 0
  * 8002 N {k (var val)*k text target}*N   menu (entry shown when all vars match)
  * 8003 k (var val)*k target               branch when all vars match
  * 8005 target / 8007 var val / 8014 bg / 8020 script / 8039,800b sound effect

A file whose bytes 2337..2346 are zero after decryption is a RAW SECTOR DUMP
(2352-byte sectors, the old extraction bug). It is reported and auto-stripped
to the 2048 user bytes per sector, but such a file is truncated: re-extract it
with satdisc.py instead.
"""
import argparse
import sys
from pathlib import Path

# Operand counts measured over all 90 .SCO files (356k instructions); each opcode
# has exactly one count. Keep in sync with vm_fixed_operands() in src/vm.c.
FIXED = {0x00: 0, 0x04: 0, 0x06: 0, 0x09: 0, 0x0A: 0, 0x0C: 0, 0x0D: 0, 0x11: 0,
         0x16: 0, 0x17: 0, 0x18: 0, 0x1E: 0, 0x21: 0, 0x24: 0, 0x27: 0, 0x28: 0,
         0x2D: 0, 0x2E: 0, 0x34: 0, 0x3C: 0, 0x42: 0,
         0x01: 1, 0x05: 1, 0x08: 1, 0x0B: 1, 0x0E: 1, 0x0F: 1, 0x10: 1, 0x12: 1,
         0x13: 1, 0x14: 1, 0x19: 1, 0x1A: 1, 0x20: 1, 0x23: 1, 0x25: 1, 0x26: 1,
         0x29: 1, 0x2A: 1, 0x2B: 1, 0x2F: 1, 0x31: 1, 0x32: 1, 0x39: 1, 0x3A: 1,
         0x41: 1, 0x43: 1,
         0x07: 2, 0x22: 2, 0x3B: 2, 0x40: 2,
         0x36: 3,
         0x1F: 4, 0x33: 4}
# Opcodes whose single operand is a word address of a string (shown decoded).
STRING_OPS = (0x14, 0x20, 0x39, 0x0B, 0x08, 0x19, 0x43)
NAMES = {0x35: "TEXT", 0x02: "MENU", 0x03: "BRANCH", 0x05: "GOTO", 0x07: "SETVAR",
         0x14: "BG", 0x20: "SCRIPT", 0x39: "SE", 0x0B: "SE2",
         0x36: "SPRITE", 0x2F: "WAIT", 0x25: "TRANS", 0x26: "BGM", 0x0E: "BGMMUTE", 0x0F: "BGMFADE", 0x41: "TERM", 0x42: "TERMCLR", 0x1F: "TERMCUR", 0x12: "FADEOUT", 0x13: "FADEIN", 0x33: "PALETTE", 0x40: "TEXTPOS", 0x22: "TEXTCOL", 0x04: "OP04", 0x06: "OP06",
         0x00: "OP00", 0x01: "OP01", 0x28: "SAVE?", 0x1E: "LOAD?",
         0x08: "NAME08", 0x19: "NAME19", 0x43: "TERMTEXT"}


def decrypt(raw):
    """Return the decoded bytes of a .SCO file (every byte XOR 0xFF)."""
    raw = bytearray(b ^ 0xFF for b in raw)
    if len(raw) > 2347 and raw[2337:2347] == bytes(10):
        sys.stderr.write("WARNING: raw sector dump detected; stripping 304 bytes per 2352. "
                         "The tail of this file is missing, re-extract it.\n")
        out = bytearray()
        for k in range(0, len(raw), 2352):
            out += raw[k:k + 2048]
        raw = out
    return raw


def load(path):
    return decrypt(Path(path).read_bytes())


class Script:
    def __init__(self, data):
        self.d = data
        self.n = len(data)

    def w(self, off):
        if off + 1 >= self.n:
            return 0
        return (self.d[off] << 8) | self.d[off + 1]

    def string(self, addr):
        if addr >= self.n:
            return None
        end = self.d.find(b"\x00", addr)
        if end < 0:
            end = self.n
        return bytes(self.d[addr:end]).decode("shift_jis", errors="replace")

    def code_end(self):
        """Byte offset where the text segment begins: the lowest string address
        referenced by the code, found by decoding from offset 0 and stopping as
        soon as ip reaches the current minimum (so text-segment garbage is
        never decoded). Searching for the first '【' does not work: files over
        64 KB have word addresses >= 0x8000 and 0x8179 occurs as an operand."""
        if hasattr(self, "_ce"):
            return self._ce
        lo = self.n  # start from EOF: a '【' search fails on files > 64 KB (word address 0x8179)
        for ip, code, a, ln in self.decode(0, lo):
            if ip >= lo:
                break
            if code == 0x35:
                refs = [a[0]] + list(a[2:])
            elif code == 0x02:
                refs = [t for _, t, _ in a]
            elif code in STRING_OPS or code == 0x36:
                refs = [a[0]]
            else:
                refs = []
            for r in refs:
                if 0 < r * 2 < lo:
                    lo = r * 2
        self._ce = lo
        return lo

    def decode(self, start=0, stop=None):
        """Yield (ip, code, operands(list of words), length_bytes)."""
        ip = start
        stop = self.code_end() if stop is None else stop
        while ip + 1 < stop:
            op = self.w(ip)
            if (op >> 8) != 0x80:
                ip += 2
                continue
            code = op & 0xFF
            if code == 0x35:
                flag = self.w(ip + 4)          # number of voice tokens (0..2)
                ln = 6 + 2 * flag
                yield ip, code, [self.w(ip + 2 + 2 * i) for i in range((ln - 2) // 2)], ln
            elif code == 0x02:
                n = self.w(ip + 2)
                off = ip + 4
                entries = []
                for _ in range(n):
                    k = self.w(off)
                    conds = [(self.w(off + 2 + 4 * i), self.w(off + 4 + 4 * i)) for i in range(k)]
                    off += 2 + 4 * k
                    entries.append((conds, self.w(off), self.w(off + 2)))
                    off += 4
                yield ip, code, entries, off - ip
                ln = off - ip
            elif code == 0x03:
                k = self.w(ip + 2)
                conds = [(self.w(ip + 4 + 4 * i), self.w(ip + 6 + 4 * i)) for i in range(k)]
                ln = 4 + 4 * k + 2
                yield ip, code, [conds, self.w(ip + 4 + 4 * k)], ln
            elif code in FIXED:
                ln = 2 + 2 * FIXED[code]
                yield ip, code, [self.w(ip + 2 + 2 * i) for i in range(FIXED[code])], ln
            else:
                j = ip + 2
                while j + 1 < stop and (self.w(j) >> 8) != 0x80:
                    j += 2
                ln = j - ip
                yield ip, code, [self.w(ip + 2 + 2 * i) for i in range((ln - 2) // 2)], ln
            ip += ln


def fmt(s, ip, code, a):
    name = NAMES.get(code, f"OP{code:02X}")
    if code == 0x35:
        voice = (" voice=" + ",".join(repr(s.string(x * 2)) for x in a[2:])) if len(a) > 2 else ""
        return f"TEXT {s.string(a[0] * 2)!r}{voice}"
    if code == 0x02:
        items = []
        for conds, text, target in a:
            c = "".join(f"[v{v}=={x}]" for v, x in conds)
            items.append(f"{c}{s.string(text * 2)!r}->{target * 2}")
        return "MENU " + " | ".join(items)
    if code == 0x03:
        return "BRANCH if " + " and ".join(f"v{v}=={x}" for v, x in a[0]) + f" goto {a[1] * 2}"
    if code == 0x05:
        return f"GOTO {a[0] * 2}"
    if code == 0x07:
        return f"SETVAR v{a[0]} = {a[1]}"
    if code in STRING_OPS:
        return f"{name} {s.string(a[0] * 2)!r}"
    if code == 0x36:
        return f"{name} {s.string(a[0] * 2)!r} x={a[1]} y={a[2]}"
    if code == 0x2F:
        return f"WAIT {a[0]} frames"
    if code == 0x33:
        return f"PALETTE slot {a[0]} = ({a[1]},{a[2]},{a[3]})"
    if code == 0x40:
        return f"TEXTPOS x={a[0]} y={a[1]}"
    return f"{name} " + " ".join(f"{x:04x}" for x in a)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--mode", choices=["info", "text", "disasm", "menus", "tokens"], default="text")
    ap.add_argument("--start", type=int, default=0, help="start IP (byte offset)")
    ap.add_argument("--limit", type=int, default=0, help="max lines (0 = all)")
    a = ap.parse_args()
    if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    s = Script(load(a.file))
    n = 0
    if a.mode == "info":
        ops = list(s.decode())
        texts = [o for o in ops if o[1] == 0x35]
        menus = [o for o in ops if o[1] == 0x02]
        scripts = sorted(set(s.string(o[2][0] * 2) for o in ops if o[1] == 0x20))
        bgs = sorted(set(s.string(o[2][0] * 2) for o in ops if o[1] == 0x14))
        print(f"size {s.n}  code_end {s.code_end()}  instructions {len(ops)}")
        print(f"TEXT {len(texts)} (voiced {sum(1 for o in texts if len(o[2]) > 2)})  MENU {len(menus)}")
        print(f"chained scripts: {scripts}")
        print(f"backgrounds: {bgs}")
        return
    for ip, code, ops, ln in s.decode(a.start):
        if a.mode == "text" and code != 0x35:
            continue
        if a.mode == "menus" and code != 0x02:
            continue
        if a.mode == "tokens":
            if code == 0x35 and len(ops) > 2:
                for x in ops[2:]:
                    print(s.string(x * 2))
            elif code in (0x39, 0x0B):
                print(s.string(ops[0] * 2))
            continue
        print(f"ip={ip:6d}: {fmt(s, ip, code, ops)}")
        n += 1
        if a.limit and n >= a.limit:
            break


if __name__ == "__main__":
    main()