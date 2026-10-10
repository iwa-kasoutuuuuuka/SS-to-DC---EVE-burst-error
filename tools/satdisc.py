#!/usr/bin/env python3
"""
satdisc.py - read files out of a Sega Saturn disc image (ISO9660).

Accepts a raw track image with 2352-byte sectors (the "Track 1 .bin" of a
bin/cue dump) or a plain 2048-byte .iso, and a .cue (its first data track is
used). Only the 2048 user bytes of each sector are returned: copying a file as
a run of raw sectors leaves 304 bytes of sync / header / EDC / ECC inside every
2352 bytes and cuts the end off, which corrupts every multi-sector file.

Usage:
  python satdisc.py <image> --list [DIR]
  python satdisc.py <image> --extract DIR <out_dir> [--ext .SCO]
"""
import argparse
import re
import struct
from pathlib import Path

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class SaturnDisc:
    def __init__(self, image):
        image = Path(image)
        if image.suffix.lower() == ".cue":
            image = self._data_track(image)
        self.path = image
        self.f = open(image, "rb")
        head = self.f.read(16)
        if head[:12] == SYNC:
            self.raw, self.skip = 2352, 24 if head[15] == 2 else 16
        else:
            self.raw, self.skip = 2048, 0
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise ValueError(f"{image}: no ISO9660 volume descriptor (not a data track?)")
        self.system_id = pvd[8:40].decode("ascii", "replace").strip()
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        root = pvd[156:190]
        self.root = (struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])
        self._dirs = {}

    @staticmethod
    def _data_track(cue):
        for line in cue.read_text(errors="replace").splitlines():
            m = re.match(r'\s*FILE\s+"(.+)"\s+BINARY', line)
            if m:
                return cue.parent / m.group(1)
        raise ValueError(f"{cue}: no FILE entry")

    def sector(self, lba):
        self.f.seek(lba * self.raw + self.skip)
        return self.f.read(2048)

    def read_extent(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:size])

    def _entries(self, extent):
        if extent in self._dirs:
            return self._dirs[extent]
        buf = self.read_extent(*extent)
        out = {}
        i = 0
        while i < len(buf):
            ln = buf[i]
            if ln == 0:                      # records never span sectors
                i = (i // 2048 + 1) * 2048
                continue
            rec = buf[i:i + ln]
            name = rec[33:33 + rec[32]]
            if name not in (b"\x00", b"\x01"):
                name = name.decode("ascii", "replace").split(";")[0].rstrip(".")
                out[name.upper()] = (struct.unpack_from("<I", rec, 2)[0],
                                     struct.unpack_from("<I", rec, 10)[0], bool(rec[25] & 2), name)
            i += ln
        self._dirs[extent] = out
        return out

    def _lookup(self, path):
        extent = self.root
        entry = None
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            entry = self._entries(extent).get(part.upper())
            if entry is None:
                return None
            extent = (entry[0], entry[1])
        return entry

    def listdir(self, path=""):
        """Return [(name, size, is_dir)] of a directory ('' = root)."""
        extent = self.root
        if path.strip("/"):
            e = self._lookup(path)
            if e is None or not e[2]:
                return []
            extent = (e[0], e[1])
        return sorted((v[3], v[1], v[2]) for v in self._entries(extent).values())

    def read(self, path):
        e = self._lookup(path)
        if e is None or e[2]:
            raise FileNotFoundError(path)
        return self.read_extent(e[0], e[1])

    def extract(self, directory, out_dir, ext=""):
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        n = 0
        for name, size, is_dir in self.listdir(directory):
            if is_dir or (ext and not name.upper().endswith(ext.upper())):
                continue
            (out / name).write_bytes(self.read(f"{directory}/{name}"))
            n += 1
        return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--list", nargs="?", const="", metavar="DIR")
    ap.add_argument("--extract", nargs=2, metavar=("DIR", "OUT"))
    ap.add_argument("--ext", default="")
    a = ap.parse_args()
    disc = SaturnDisc(a.image)
    print(f"volume '{disc.volume_id}' ({disc.raw} bytes/sector)")
    if a.list is not None:
        for name, size, is_dir in disc.listdir(a.list):
            print(f"{'<DIR>' if is_dir else size:>10}  {name}")
    if a.extract:
        print(f"extracted {disc.extract(a.extract[0], a.extract[1], a.ext)} files to {a.extract[1]}")


if __name__ == "__main__":
    main()
