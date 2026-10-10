"""Check that every .SCO decodes with fixed instruction boundaries and that every
string/jump operand lands where it should (word address * 2).

    python verify_sco.py <directory of .SCO files> [more dirs]

Per file: ops, refs (string + jump operands), ok, stray (data words the VM would skip
inside the code segment), unknown (opcodes without a FIXED entry). The last lines give
the totals; a healthy set prints ok == refs, unknown == {} and files with bad refs: 0.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sco as sd  # noqa: E402


def is_boundary(d, a):
    """A string starts right after a NUL, optionally with one attribute byte between."""
    if a == 0 or d[a - 1] == 0:
        return True
    return a >= 2 and d[a - 2] == 0


def check_file(path, tot):
    d = sd.load(path)
    s = sd.Script(d)
    n, ce = s.n, s.code_end()
    ops = list(s.decode())
    covered = sum(o[3] for o in ops)
    stray = (ce - covered) // 2 if ce > covered else 0
    refs = ok = 0
    unk = {}
    bad = []

    def chk(a, what, ip):
        nonlocal refs, ok
        refs += 1
        b = a * 2
        if b < n and is_boundary(d, b):
            ok += 1
        else:
            bad.append((ip, what, b))

    def jump(t, what, ip):
        nonlocal refs, ok
        refs += 1
        if t * 2 < ce:
            ok += 1
        else:
            bad.append((ip, what, t * 2))

    for ip, code, a, ln in ops:
        if code == 0x35:
            chk(a[0], "text", ip)
            for x in a[2:]:
                chk(x, "voice", ip)
        elif code == 0x02:
            for conds, t, tgt in a:
                chk(t, "menu", ip)
                jump(tgt, "menu-target", ip)
        elif code == 0x03:
            jump(a[1], "branch-target", ip)
        elif code == 0x05:
            jump(a[0], "goto-target", ip)
        elif code in sd.STRING_OPS:
            chk(a[0], sd.NAMES.get(code, hex(code)), ip)
        elif code == 0x36:
            chk(a[0], "sprite", ip)
        elif code not in sd.FIXED:
            unk[code] = unk.get(code, 0) + 1

    tot["files"] += 1
    tot["ops"] += len(ops)
    tot["refs"] += refs
    tot["ok"] += ok
    tot["stray"] += stray
    for k, v in unk.items():
        tot["unknown"][k] = tot["unknown"].get(k, 0) + v
    flag = "" if ok == refs and not unk and stray == 0 else "  <--"
    print(f"{path:44s} size={n:6d} code_end={ce:6d} ops={len(ops):5d} refs={refs:5d} "
          f"ok={ok:5d} stray={stray:3d} unknown={{{', '.join(f'{k:#x}: {v}' for k, v in sorted(unk.items()))}}}{flag}")
    if bad:
        tot["bad_files"].append(path)
        for b in bad[:5]:
            print("     BAD", b)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    tot = {"files": 0, "refs": 0, "ok": 0, "unknown": {}, "stray": 0, "ops": 0, "bad_files": []}
    for arg in sys.argv[1:]:
        if os.path.isfile(arg):
            check_file(arg, tot)
            continue
        for dp, _, fn in os.walk(arg):
            for f in sorted(fn):
                if f.upper().endswith(".SCO"):
                    check_file(os.path.join(dp, f), tot)
    print()
    pct = 100 * tot["ok"] / max(1, tot["refs"])
    print(f"files={tot['files']} ops={tot['ops']} refs={tot['refs']} ok={tot['ok']} ({pct:.2f}%) stray_words={tot['stray']}")
    print("unknown opcodes:", {f"{k:#x}": v for k, v in sorted(tot["unknown"].items())})
    print("files with bad refs:", len(tot["bad_files"]))
    sys.exit(0 if tot["ok"] == tot["refs"] and not tot["unknown"] else 1)


if __name__ == "__main__":
    main()
