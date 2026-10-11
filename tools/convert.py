#!/usr/bin/env python3
"""
convert.py - build the Dreamcast CD contents for the EVE burst error engine
from your own Sega Saturn discs.

Usage:
  python tools/convert.py <disc image> [<disc image> ...] [--out cd_root]
         [--only-disc N] [--skip voices,movies,bgm,graphics] [--jobs N] [--force]

Pass the data track of each Saturn disc (a .cue, the "Track 1" .bin of a
bin/cue dump with 2352-byte sectors, or a 2048-byte .iso). The order does not
matter: the disc number is read from the image. Give all three game discs so
that every picture and movie can be found; one Dreamcast disc is written per
Saturn disc, with the same split of the story:

  <out>/Disc<N>/scenario/Disc<1..3>/*.SCO  scenario files, copied as they are
  <out>/Disc<N>/graphics/bg|spr/*.BIN      backgrounds / overlays used by that disc (EVB1)
  <out>/Disc<N>/graphics/misc/bg/*.BIN     terminal frames and other MISC pictures
  <out>/Disc<N>/graphics/ui/*.BIN          text font (SATFONT.BIN) and UI sprite sheets
  <out>/Disc<N>/graphics/ui_assets/SPR3.bmp  message window (drawn by this script)
  <out>/Disc<N>/movies/*.mpg               Cinepak movies re-encoded to MPEG-1
  <out>/Disc<N>/audio/Disc<N>/*.adx        voices and sound effects (CRI ADX)
  <out>/Disc<N>/se/*.wav                   UI sounds (AICA ADPCM)
  <out>/Disc<N>/bgm/BGM<n>.wav             music rendered from the SCSP sequences (AICA ADPCM)

Then `make cdi DISC=<N>` turns <out>/Disc<N> into a bootable image.

Nothing is downloaded and nothing from the game is stored in this repository:
every output file is derived from the images you supply. Finished files are
kept on a second run (use --force to rebuild them).

Needs Python 3.8+, numpy, and ffmpeg on PATH (movies, voices, music).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: python -m pip install numpy")

import b1          # noqa: E402
import cmpdec      # noqa: E402
import satbgm      # noqa: E402
import satdisc     # noqa: E402
import satvoice    # noqa: E402
import sco         # noqa: E402

STEPS = ("scenario", "graphics", "movies", "voices", "bgm")
CD_LIMIT = 360000 * 2048                 # 80-minute CD-R
BOOT_MOVIES = {1: ("CSLOGO30", "OPENING"), 2: ("CSLOGO30",), 3: ("CSLOGO30",)}
# 320x160 clips are padded to the 320x224 Saturn frame and stretched to 320x240, so
# they land on the same screen area as the backgrounds; libmpeg wants CBR MPEG-1.
MOVIE_ARGS = ["-vf", "pad=320:224:0:0,scale=320:240,fps=30",
              "-c:v", "mpeg1video", "-b:v", "742k", "-minrate", "742k", "-maxrate", "742k",
              "-bufsize", "742k", "-pix_fmt", "yuv420p",
              "-ac", "1", "-ar", "32000", "-c:a", "mp2", "-b:a", "64k", "-f", "mpeg"]


def log(msg):
    print(msg, flush=True)


def ffmpeg(args, data=None):
    r = subprocess.run(["ffmpeg", "-v", "error", "-y"] + args, input=data, capture_output=True)
    return r.returncode == 0


# ---------------------------------------------------------------- disc set

class Game:
    """The Saturn discs that were given, keyed by disc number."""

    def __init__(self, images):
        self.discs = {}
        for image in images:
            try:
                disc = satdisc.SaturnDisc(image)
            except (OSError, ValueError) as e:
                log(f"skipping {image}: {e}")
                continue
            number = None
            for name, _, _ in disc.listdir(""):
                m = re.fullmatch(r"DISC(\d)", name.upper())
                if m:
                    number = int(m.group(1))
            names = {n.upper() for n, _, is_dir in disc.listdir("") if is_dir}
            if number not in (1, 2, 3) or not {"SCENARIO", "GRAPHICS", "SOUND"} <= names:
                log(f"skipping {image}: not a game disc of EVE burst error (Saturn)")
                continue
            self.discs[number] = disc
            log(f"disc {number}: {disc.path.name}")
        if not self.discs:
            sys.exit("no usable disc image")
        self._scripts = {}
        self._index = {}

    def order(self, first):
        return [first] + [n for n in sorted(self.discs) if n != first]

    def index(self, number, directory):
        """{UPPER NAME: real name} of one directory of one disc."""
        key = (number, directory)
        if key not in self._index:
            self._index[key] = {n.upper(): n for n, _, is_dir in self.discs[number].listdir(directory)
                                if not is_dir}
        return self._index[key]

    def find(self, directory, name, first):
        """Read <directory>/<name> from disc `first`, else from another disc."""
        for n in self.order(first):
            real = self.index(n, directory).get(name.upper())
            if real:
                return self.discs[n].read(f"{directory}/{real}")
        return None

    def scripts(self, number):
        """{file name: references} for every .SCO of a disc."""
        if number not in self._scripts:
            out = {}
            for real in self.index(number, "SCENARIO").values():
                if real.upper().endswith(".SCO"):
                    data = self.discs[number].read(f"SCENARIO/{real}")
                    out[real] = (data, references(data))
            self._scripts[number] = out
        return self._scripts[number]


def references(data):
    """Names a scenario file uses: voices / effects, backgrounds, overlays, movies."""
    s = sco.Script(sco.decrypt(data))
    ref = {"voice": set(), "bg": set(), "spr": set(), "movie": set()}
    for _, code, ops, _ in s.decode():
        if code == 0x35:
            names, kind = [s.string(x * 2) for x in ops[2:]], "voice"
        elif code in (0x39, 0x0B):
            names, kind = [s.string(ops[0] * 2)], "voice"
        elif code == 0x14:
            names, kind = [s.string(ops[0] * 2)], "bg"
        elif code == 0x36:
            names, kind = [s.string(ops[0] * 2)], "spr"
        elif code == 0x08:
            names, kind = [s.string(ops[0] * 2)], "movie"
        else:
            continue
        ref[kind] |= {n.strip().upper() for n in names if n and n.isascii() and n.strip()}
    return ref


def disc_references(game, number):
    """Backgrounds / overlays / movies of the scripts that are played from this disc.

    A script belongs to a disc when (nearly) all of its voice files are recorded
    on it; scripts without voices (title, chapter heads) belong to every disc."""
    voices = {Path(n).stem for n in game.index(number, "SOUND")}
    need = {"bg": set(), "spr": set(), "movie": set()}
    count = 0
    for _, ref in game.scripts(number).values():
        if not ref["voice"] or len(ref["voice"] & voices) >= 0.95 * len(ref["voice"]):
            count += 1
            for k in need:
                need[k] |= ref[k]
    return need, count


# ---------------------------------------------------------------- pictures

def evb1(width, height, fmt, pixels):
    """Engine texture file: "EVB1" u16 w, u16 h, u16 format (0 RGB565, 1 ARGB4444), u16 0, pixels."""
    return b"EVB1" + struct.pack("<HHHH", width, height, fmt, 0) + pixels


def b1_to_evb1(data, fmt):
    w, h, px = b1.decode(data, "mask")
    if w > 512 or h > 256:
        return None
    px = np.array(px, dtype=np.uint16)
    r, g, b = px & 0x1F, (px >> 5) & 0x1F, (px >> 10) & 0x1F       # Saturn: 1 BBBBB GGGGG RRRRR
    if fmt == 0:
        out = (r << 11) | (((g << 1) | (g >> 4)) << 5) | b
    else:
        out = np.where(px == b1.TRANSPARENT, 0, 0xF000 | ((r >> 1) << 8) | ((g >> 1) << 4) | (b >> 1))
    return evb1(w, h, fmt, out.astype("<u2").tobytes())


def message_window():
    """512x256 32-bit BMP of the message window: a translucent rounded panel."""
    def inside(x, y, inset, radius):
        x0, y0, x1, y1 = 8 + inset, 8 + inset, 504 - inset, 222 - inset
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + radius), x1 - radius)
        cy = min(max(y, y0 + radius), y1 - radius)
        return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius + radius

    fill, border, light, clear = b"\x1c\x12\x0f\xb4", b"\xff\xb4\x00\xff", b"\xff\xff\xff\x28", bytes(4)
    rows = []
    for y in range(256):
        row = bytearray()
        for x in range(512):
            if not inside(x, y, 0, 10):
                row += clear
            elif not inside(x, y, 3, 7):
                row += border
            elif not inside(x, y, 4, 6):
                row += light
            else:
                row += fill
        rows.append(bytes(row))
    header = b"BM" + struct.pack("<IHHI", 54 + 512 * 256 * 4, 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, 512, 256, 1, 32, 0, 512 * 256 * 4, 3780, 3780, 0, 0)
    return header + b"".join(reversed(rows))


def text_font(font_cmp, charcode):
    """SATFONT.BIN: "EVF1" u16 count, u16 0, count x u16 Shift-JIS code, count x 128-byte glyph
    (16x16 pixels, 4 bits per pixel)."""
    glyphs = cmpdec.decode(font_cmp)
    codes = [struct.unpack_from(">H", charcode, i)[0] for i in range(0, len(charcode) - 1, 2)]
    if len(glyphs) != len(codes) * 128 or codes != sorted(codes):
        return None
    return b"EVF1" + struct.pack("<HH", len(codes), 0) + b"".join(struct.pack("<H", c) for c in codes) + glyphs


def write(path, data, force):
    if data is None:
        return False
    if force or not path.exists() or path.stat().st_size != len(data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return True


def do_graphics(game, number, need, out, force):
    missing = []
    done = 0
    for kind, fmt in (("bg", 0), ("spr", 1)):
        for name in sorted(need[kind]):
            dst = out / "graphics" / kind / (name + ".BIN")
            if dst.exists() and not force:
                done += 1
                continue
            data = game.find("GRAPHICS", name + ".B1", number)
            if data is None or not write(dst, b1_to_evb1(data, fmt), force):
                missing.append(f"{kind}/{name}")
            else:
                done += 1
    for upper, real in sorted(game.index(number, "MISC").items()):
        if upper.endswith(".B1"):
            dst = out / "graphics" / "misc" / "bg" / (Path(upper).stem + ".BIN")
            if force or not dst.exists():
                write(dst, b1_to_evb1(game.discs[number].read(f"MISC/{real}"), 0), force)

    ui = out / "graphics" / "ui"
    misc = lambda name: game.find("MISC", name, number)       # noqa: E731
    font, codes = misc("FONT.CMP"), misc("CHARCODE")
    if font and codes:
        write(ui / "SATFONT.BIN", text_font(font, codes), force)
    else:
        missing.append("MISC/FONT.CMP")
    for sheet, rows in (("SPR1", 14), ("SPR2", 2), ("COMFONT", 1)):
        data = misc(sheet + ".CMP")
        if not data:
            continue
        raw = cmpdec.decode(data)
        if raw[:2] != b"BM":
            continue
        w, h, pal, pix = cmpdec.bmp_pixels(raw)
        strip = h // rows
        for k in range(rows):
            name = {"SPR1": f"SPR1_R{k:02d}", "SPR2": "SPR2_BOTTOM" if k else "SPR2_TOP"}.get(sheet, sheet)
            if force or not (ui / (name + ".BIN")).exists():
                cmpdec.write_evb1(ui / (name + ".BIN"), w, h, pal, pix, 0, (0, k * strip, w, strip))
    write(out / "graphics" / "ui_assets" / "SPR3.bmp", message_window(), force)
    return done, missing


# ---------------------------------------------------------------- movies

def movie_job(job):
    data, dst = job
    part = dst.with_name(dst.name + ".part")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "movie.cpk"
        src.write_bytes(data)
        ok = ffmpeg(["-i", str(src)] + MOVIE_ARGS + [str(part)])
    if ok:
        os.replace(part, dst)
    return ok


def do_movies(game, number, need, out, force, jobs):
    wanted = {}
    missing = []
    for name in sorted(need["movie"] | set(BOOT_MOVIES[number])):
        # A few scripts add a letter to the file name ("m1718m" plays M1718).
        for cand in [name] + ([name[:-1]] if name[-1:].isalpha() else []):
            if any(cand + ".CPK" in game.index(n, "MOVIES") for n in game.discs):
                wanted[cand] = None
                break
        else:
            missing.append("movie/" + name)
    todo = []
    for name in wanted:
        dst = out / "movies" / (name + ".mpg")
        if force or not dst.exists() or dst.stat().st_size == 0:
            dst.parent.mkdir(parents=True, exist_ok=True)
            todo.append((game.find("MOVIES", name + ".CPK", number), dst))
    with ThreadPoolExecutor(jobs) as pool:
        for (_, dst), ok in zip(todo, pool.map(movie_job, todo)):
            if not ok:
                missing.append("movie/" + dst.stem + " (ffmpeg failed)")
    return len(wanted), missing


# ---------------------------------------------------------------- voices

VOICE_BATCH = 48                         # clips per ffmpeg run: starting ffmpeg costs more than encoding


def voice_job(batch):
    """Decode a batch of (data, destination) clips and encode them with one ffmpeg run."""
    result = [False] * len(batch)
    with tempfile.TemporaryDirectory() as tmp:
        inputs, outputs, live = [], [], []
        for k, (data, dst) in enumerate(batch):
            try:
                rate, pcm = satvoice.decode(data)
            except (ValueError, IndexError, struct.error):
                continue
            if not len(pcm):
                continue
            raw = Path(tmp) / f"{k}.raw"
            raw.write_bytes(pcm.tobytes())
            inputs += ["-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", str(raw)]
            outputs += ["-map", f"{len(live)}:a", "-c:a", "adpcm_adx", "-f", "adx", str(dst) + ".part"]
            live.append(k)
        if live and ffmpeg(inputs + outputs):
            for k in live:
                os.replace(str(batch[k][1]) + ".part", batch[k][1])
                result[k] = True
        elif len(batch) > 1:             # find the clip ffmpeg refused
            return [voice_job([item])[0] for item in batch]
    return result


def do_ui_sounds(game, number, out, force):
    """MISC/*.WAV (cursor, confirm, cancel ...) -> se/<NAME>.wav, AICA ADPCM for snd_sfx."""
    failed = []
    for upper, real in sorted(game.index(number, "MISC").items()):
        dst = out / "se" / (Path(upper).stem + ".wav")
        if not upper.endswith(".WAV") or (dst.exists() and not force):
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        part = dst.with_name(dst.name + ".part")
        try:
            rate, pcm = satvoice.decode(game.discs[number].read(f"MISC/{real}"))
        except (ValueError, IndexError, struct.error):
            failed.append("se/" + dst.stem)
            continue
        if ffmpeg(["-f", "s16le", "-ar", str(rate), "-ac", "1", "-i", "-", "-ar", "22050",
                   "-c:a", "adpcm_yamaha", "-f", "wav", str(part)], pcm.tobytes()):
            os.replace(part, dst)
        else:
            failed.append("se/" + dst.stem)
    return failed


def do_voices(game, number, out, force, jobs):
    disc = game.discs[number]
    dst_dir = out / "audio" / f"Disc{number}"
    dst_dir.mkdir(parents=True, exist_ok=True)
    names = sorted(real for upper, real in game.index(number, "SOUND").items() if upper.endswith(".WAV"))
    todo = [n for n in names if force or not (dst_dir / (Path(n).stem.upper() + ".adx")).exists()]
    failed = []
    step = VOICE_BATCH * jobs            # read from the image in the main thread, encode in workers
    with ThreadPoolExecutor(jobs) as pool:
        for i in range(0, len(todo), step):
            clips = [(disc.read(f"SOUND/{n}"), dst_dir / (Path(n).stem.upper() + ".adx")) for n in todo[i:i + step]]
            batches = [clips[k:k + VOICE_BATCH] for k in range(0, len(clips), VOICE_BATCH)]
            for batch, oks in zip(batches, pool.map(voice_job, batches)):
                failed += ["voice/" + dst.stem for (_, dst), ok in zip(batch, oks) if not ok]
            log(f"  voices {min(i + step, len(todo))}/{len(todo)}")
    return len(names), failed


# ---------------------------------------------------------------- music

def bgm_job(job):
    music, number, dst = job
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "bgm.wav"
        if not satbgm.render_song(music, number, wav):
            return False
        # The engine streams mono AICA (Yamaha) ADPCM at 22050 Hz from RAM.
        part = dst.with_name(dst.name + ".part")
        ok = ffmpeg(["-i", str(wav), "-ac", "1", "-ar", "22050", "-c:a", "adpcm_yamaha", "-f", "wav", str(part)])
    if ok:
        os.replace(part, dst)
    return ok


def do_bgm(game, number, out, force, jobs, cache):
    dst_dir = out / "bgm"
    dst_dir.mkdir(parents=True, exist_ok=True)
    if cache.get("dir"):                 # rendered for an earlier disc: the music is the same
        for f in cache["dir"].glob("BGM*.wav"):
            if force or not (dst_dir / f.name).exists():
                shutil.copy2(f, dst_dir / f.name)
        return len(list(dst_dir.glob("BGM*.wav"))), []
    disc = game.discs[number]
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        disc.extract("MUSIC", tmp)
        songs = satbgm.song_numbers(tmp)
        todo = [(tmp, n, dst_dir / f"BGM{n}.wav") for n in songs
                if force or not (dst_dir / f"BGM{n}.wav").exists()]
        with ProcessPoolExecutor(max(1, min(jobs, len(todo) or 1))) as pool:
            for (_, n, _), ok in zip(todo, pool.map(bgm_job, todo)):
                if not ok:
                    failed.append(f"bgm/{n}")
    cache["dir"] = dst_dir
    return len(songs), failed


# ---------------------------------------------------------------- driver

def build_disc(game, number, out_root, skip, force, jobs, cache):
    out = out_root / f"Disc{number}"
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.rglob("*.part"):    # left by an interrupted run
        stale.unlink()
    problems = []
    log(f"== Disc {number} -> {out}")

    if "scenario" not in skip:
        # The program keeps reading scenario/Disc<n> of the disc it was started
        # from after a disc change, so every CD carries all three directories.
        count = 0
        for n in (1, 2, 3):
            scripts = game.scripts(n if n in game.discs else number)
            for name, (data, _) in scripts.items():
                write(out / "scenario" / f"Disc{n}" / name.upper(), data, force)
            count += len(scripts)
        log(f"  scenario: {count} files")
    # Placeholder background shown before the first scene, and the disc marker.
    write(out / "S0005_LINEAR.BIN", bytes(512 * 256 * 2), force)
    write(out / f"DISC{number}.ID", f"EVE burst error disc {number}\n".encode(), force)

    need, count = disc_references(game, number)
    if "graphics" not in skip:
        done, missing = do_graphics(game, number, need, out, force)
        problems += missing
        log(f"  graphics: {done} pictures for {count} scripts")
    if "movies" not in skip:
        done, missing = do_movies(game, number, need, out, force, jobs)
        problems += missing
        log(f"  movies: {done}")
    if "voices" not in skip:
        done, missing = do_voices(game, number, out, force, jobs)
        problems += missing + do_ui_sounds(game, number, out, force)
        log(f"  voices: {done}")
    if "bgm" not in skip:
        done, missing = do_bgm(game, number, out, force, jobs, cache)
        problems += missing
        log(f"  bgm: {done} songs")

    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    log(f"  total {size / 2**20:.1f} MiB" + ("  ** larger than an 80-minute CD-R **" if size > CD_LIMIT - 4 * 2**20 else ""))
    if problems:
        log(f"  not converted ({len(problems)}): " + ", ".join(problems[:20]) + (" ..." if len(problems) > 20 else ""))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="Saturn disc images (.cue, Track 1 .bin or .iso)")
    ap.add_argument("--out", default="cd_root")
    ap.add_argument("--only-disc", type=int, choices=(1, 2, 3), help="write only this Dreamcast disc")
    ap.add_argument("--skip", default="", help="comma separated steps to leave out: " + ", ".join(STEPS))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--force", action="store_true", help="rebuild files that already exist")
    a = ap.parse_args()
    skip = {s for s in a.skip.split(",") if s}
    if skip - set(STEPS):
        ap.error("unknown step in --skip: " + ", ".join(sorted(skip - set(STEPS))))
    if not shutil.which("ffmpeg") and {"movies", "voices", "bgm"} - skip:
        sys.exit("ffmpeg was not found on PATH (needed for movies, voices and music)")

    game = Game(a.images)
    targets = [a.only_disc] if a.only_disc else sorted(game.discs)
    if a.only_disc and a.only_disc not in game.discs:
        sys.exit(f"the image of disc {a.only_disc} was not given")
    if len(game.discs) < 3:
        log("note: not all three discs were given; pictures or movies stored on another disc will be missing")
    cache = {}
    problems = []
    for number in targets:
        problems += build_disc(game, number, Path(a.out), skip, a.force, a.jobs, cache)
    log("done" + (f" with {len(problems)} missing items" if problems else "") +
        f". Next: make cdi DISC=<n> for n in {targets}")


if __name__ == "__main__":
    main()
