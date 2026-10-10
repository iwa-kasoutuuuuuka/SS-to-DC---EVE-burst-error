#!/usr/bin/env python3
"""
satbgm.py - render the Saturn BGM of EVE burst error (MUSIC/*.SEQ + *.BIN) to WAV.

The Saturn disc stores the music as sequence banks for Sega's sound driver and
SCSP tone banks (see satseq.py). This is a small software synthesiser for that
data: PCM layers with loop points, the SCSP amplitude envelope (AR / D1R / DL /
D2R / RR with the chip's rate tables), total level, direct send level, the LFO,
pitch bend, and the driver's volume / expression controllers. FM layers, the
pitch envelope and the DSP effects (reverb) are not reproduced.

The song number is the operand of the scenario instruction 8026 (BGM). A song
lives in whichever bank holds data at that index:
  GAMEA 1-11, GAMEB 12-21 / 46 / 47, GAMEC 22-29, GAMED 38-45  (tones: GAME.BIN)
  JOIN 30 / 31, DIARY 32-36                                    (tones: same name)

Usage:
  python satbgm.py <MUSIC dir> --song 1 --out bgm1.wav
  python satbgm.py <MUSIC dir> --all out_dir          # BGM<n>.wav for every song
Output is 16-bit mono PCM at 44100 Hz. Needs numpy.
"""
import argparse
import wave
from pathlib import Path

import numpy as np

import satseq

SR = 44100
# (sequence bank, tone bank) in search order.
BANKS = [("GAMEA", "GAME"), ("GAMEB", "GAME"), ("GAMEC", "GAME"), ("GAMED", "GAME"),
         ("JOIN", "JOIN"), ("DIARY", "DIARY"), ("HELI", "HELI")]

# SCSP envelope times in milliseconds for the 64 effective rates (0-96 dB).
AR_MS = [1e9, 1e9, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860,
         760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55,
         47, 38, 31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8, 6.0, 5.1, 4.3, 3.4,
         3.0, 2.5, 2.0, 1.6, 1.5, 1.2, 1.0, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0]
DR_MS = [1e9, 1e9, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700,
         14800, 12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400,
         1100, 920, 790, 690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49,
         43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1]
LFO_HZ = [0.17, 0.19, 0.23, 0.27, 0.34, 0.39, 0.45, 0.55, 0.68, 0.78, 0.92, 1.10, 1.39, 1.60, 1.87, 2.27,
          2.87, 3.31, 3.92, 4.79, 6.15, 7.18, 8.60, 10.8, 14.4, 17.2, 21.5, 28.7, 43.1, 57.4, 86.1, 172.3]
PLFO_CENTS = [0, 7, 13.5, 27, 55, 112, 230, 494]
ALFO_DB = [0, 0.4, 0.8, 1.5, 3, 6, 12, 24]

TL_DB = 0.375            # one total-level step
CC_DB = 0.375            # one step of controller 7 / 11 below 127
FLOOR_DB = 84.0          # the envelope is treated as silent below this
MAX_RELEASE = 4.0        # seconds
MASTER = 0.22            # mix gain before the limiter


def find_song(music, number):
    """Return (Song, ToneBank) for a BGM number, or None."""
    music = Path(music)
    for seq, tone in BANKS:
        p = music / (seq + ".SEQ")
        if not p.exists():
            continue
        songs = satseq.bank_songs(p.read_bytes())
        if number in songs:
            song = satseq.Song(songs[number])
            if any(e[2] == "note" for e in song.events):
                return song, satseq.ToneBank((music / (tone + ".BIN")).read_bytes())
    return None


def song_numbers(music):
    found = set()
    for seq, _ in BANKS[:6]:
        p = Path(music) / (seq + ".SEQ")
        if p.exists():
            found |= set(satseq.bank_songs(p.read_bytes()))
    return sorted(n for n in found if n and find_song(music, n))


def step_curve(points, n, default):
    """Piecewise-constant curve of n samples from sorted (sample index, value) points."""
    if not points:
        return None
    idx = np.array([p[0] for p in points])
    val = np.array([default] + [p[1] for p in points], dtype=np.float32)
    return val[np.searchsorted(idx, np.arange(n), side="right")]


class Renderer:
    def __init__(self, song, bank):
        self.song, self.bank = song, bank
        self.samples = {}

    def pcm(self, layer):
        key = (layer.sa, layer.lea, layer.pcm8)
        if key not in self.samples:
            d = self.bank.data
            if layer.pcm8:
                a = np.frombuffer(d, dtype=np.int8, count=layer.lea, offset=layer.sa).astype(np.float32) / 128.0
            else:
                count = min(layer.lea, (len(d) - layer.sa) // 2)
                a = np.frombuffer(d, dtype=">i2", count=count, offset=layer.sa).astype(np.float32) / 32768.0
            self.samples[key] = a
        return self.samples[key]

    def voice(self, out, start, layer, key, atten_db, hold, bend):
        """Mix one layer of one note into out[start:]. hold = key-on time in seconds;
        bend = per-sample pitch offset in semitones (array) or None."""
        pcm = self.pcm(layer)
        if len(pcm) < 2:
            return
        ratio = 2.0 ** ((key - layer.base) / 12.0 + layer.fine / 1200.0)
        octave = int(np.floor(np.log2(ratio)))
        scale = 0 if layer.krs == 15 else octave + 2 * layer.krs + (1 if ratio / 2.0 ** octave >= 1.5 else 0)

        def rate(r):
            return max(0, min(63, scale + 2 * r)) if r else 0

        t_att = AR_MS[rate(layer.ar)] / 1000.0
        r1 = 96.0 / (DR_MS[rate(layer.d1r)] / 1000.0)
        r2 = 96.0 / (DR_MS[rate(layer.d2r)] / 1000.0)
        rr = 96.0 / (DR_MS[rate(layer.rr)] / 1000.0)
        dl = layer.dl * 3.0

        def level(t):          # attenuation in dB while the key is held
            td = np.maximum(t - t_att, 0.0)
            first = r1 * td
            if r1 * 1e3 <= dl or dl == 0:
                return first if dl else r2 * td
            return np.where(first < dl, first, dl + r2 * (td - dl / r1))

        off_db = float(level(np.float64(hold)))
        release = min(MAX_RELEASE, max(0.0, FLOOR_DB - off_db) / rr) if off_db < FLOOR_DB else 0.0
        n = int((hold + release) * SR) + 1
        if not layer.loop:     # one-shot: the sample end stops the slot
            n = min(n, int(len(pcm) / ratio))
        n = min(n, len(out) - start)
        if n <= 0:
            return
        t = np.arange(n, dtype=np.float64) / SR

        cents = None
        lfo = None
        if layer.plfos or layer.alfos:
            ph = (t * LFO_HZ[layer.lfof]) % 1.0
            lfo = np.abs(4.0 * ph - 2.0) - 1.0
            if layer.plfos:
                cents = PLFO_CENTS[layer.plfos] * lfo
        if bend is not None:
            b = bend[start:start + n] * 100.0
            cents = b if cents is None else cents + b
        if cents is None:
            pos = t * (SR * ratio)
        else:
            inc = ratio * 2.0 ** (cents / 1200.0)
            pos = np.concatenate(([0.0], np.cumsum(inc[:-1])))
        end = len(pcm) - 1
        if layer.loop and layer.lea > layer.lsa:
            ls, span = layer.lsa, max(1, min(layer.lea, end) - layer.lsa)
            pos = np.where(pos < ls + span, pos, ls + np.mod(pos - ls, span))
        else:
            pos = np.minimum(pos, end - 1e-3)
        i = pos.astype(np.int64)
        f = (pos - i).astype(np.float32)
        sig = pcm[i] * (1.0 - f) + pcm[np.minimum(i + 1, end)] * f

        db = np.where(t < hold, level(t), off_db + rr * (t - hold)) + atten_db
        if layer.alfos and lfo is not None:
            db = db + ALFO_DB[layer.alfos] * (lfo + 1.0) * 0.5
        amp = 10.0 ** (-db / 20.0)
        if t_att > 1.0 / SR:
            amp = amp * np.minimum(t / t_att, 1.0)
        amp[-min(n, 64):] *= np.linspace(1.0, 0.0, min(n, 64))       # declick the tail
        out[start:start + n] += (sig * amp).astype(np.float32)

    def render(self):
        song, bank = self.song, self.bank
        total = int(song.seconds(song.end) * SR)
        size = total + int((MAX_RELEASE + 1) * SR)
        chans = {}
        prog = [0] * 16
        bend_range = [2.0] * 16
        rpn = [None] * 16
        vol = {c: [] for c in range(16)}       # (sample, value) for controller 7
        expr = {c: [] for c in range(16)}
        bends = {c: [] for c in range(16)}
        notes = []
        for tick, ch, kind, a, b, c in song.events:
            at = int(song.seconds(tick) * SR)
            if kind == "pc":
                prog[ch] = a
            elif kind == "cc":
                if a == 7:
                    vol[ch].append((at, b))
                elif a == 11:
                    expr[ch].append((at, b))
                elif a == 101:
                    rpn[ch] = b << 7 | (rpn[ch] or 0) & 0x7F
                elif a == 100:
                    rpn[ch] = (rpn[ch] or 0) & ~0x7F | b
                elif a == 6 and rpn[ch] == 0:
                    bend_range[ch] = float(b)
            elif kind == "bend":
                bends[ch].append((at, (a - 64) / 64.0 * bend_range[ch]))
            elif kind == "note" and tick < song.end:
                hold = song.seconds(tick + c) - song.seconds(tick)
                notes.append((at, ch, prog[ch], a, b, max(hold, 0.005)))

        bend_curve = {}
        for ch, pts in bends.items():
            if any(v for _, v in pts):
                bend_curve[ch] = step_curve(pts, size, 0.0)
        for at, ch, program, key, vel, hold in notes:
            if program >= len(bank.voices):
                continue
            _, bias, layers = bank.voices[program]
            buf = chans.setdefault(ch, np.zeros(size, dtype=np.float32))
            for layer in layers:
                if not (layer.key_lo <= key <= layer.key_hi) or layer.disdl == 0 or layer.mdl:
                    continue
                v = bank.velocity(layer.vl, vel)
                level = max(0, min(255, (((v + 1) * (256 - layer.tl) * 512 - 1) >> 16) + bias))
                atten = (255 - level) * TL_DB + (7 - layer.disdl) * 6.0
                self.voice(buf, at, layer, key, atten, hold, bend_curve.get(ch))

        mix = np.zeros(size, dtype=np.float32)
        ramp = np.ones(256, dtype=np.float32) / 256.0
        for ch, buf in chans.items():
            gain = None
            for pts in (vol[ch], expr[ch]):
                curve = step_curve(pts, size, 127.0)
                if curve is not None:
                    g = 10.0 ** (-(127.0 - curve) * CC_DB / 20.0)
                    gain = g if gain is None else gain * g
            if gain is not None:
                buf = buf * np.convolve(gain, ramp, mode="same")
            mix += buf
        # Fold the release tail back onto the start so the file loops seamlessly.
        tail = mix[total:]
        if song.loop_end is not None:
            k = min(len(tail), total)
            mix[:k] += tail[:k]
        else:
            last = np.nonzero(np.abs(tail) > 1e-4)[0]
            total += int(last[-1]) + 1 if len(last) else 0
        mix = mix[:total] * MASTER
        peak = float(np.max(np.abs(mix))) if total else 0.0
        if peak > 0.98:
            mix *= 0.98 / peak
        return mix, peak


def write_wav(path, mix):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(mix, -1.0, 1.0) * 32767.0).astype("<i2").tobytes())


def render_song(music, number, out):
    found = find_song(music, number)
    if not found:
        return None
    mix, peak = Renderer(*found).render()
    write_wav(out, mix)
    return len(mix) / SR, peak


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("music", help="directory with the Saturn MUSIC/*.SEQ and *.BIN files")
    ap.add_argument("--song", type=int)
    ap.add_argument("--out", help="output .wav (with --song)")
    ap.add_argument("--all", help="output directory: render every song to BGM<n>.wav")
    a = ap.parse_args()
    if a.all:
        Path(a.all).mkdir(parents=True, exist_ok=True)
        todo = [(n, Path(a.all) / f"BGM{n}.wav") for n in song_numbers(a.music)]
    elif a.song and a.out:
        todo = [(a.song, Path(a.out))]
    else:
        ap.error("give --song N --out FILE, or --all DIR")
    for n, path in todo:
        r = render_song(a.music, n, path)
        print(f"BGM{n}: " + (f"{r[0]:.1f} s, peak {r[1]:.2f} -> {path}" if r else "not found"))


if __name__ == "__main__":
    main()
