#!/usr/bin/env python3
"""
satseq.py - parser for the Sega Saturn sound driver sequence banks (MUSIC/*.SEQ)
and tone banks (MUSIC/*.BIN) used by EVE burst error.

Sequence bank (big-endian):
  u16 song count, then one u32 file offset per song.
Song:
  u16 ticks per quarter note
  u16 number of tempo entries
  u16 offset of the event track (from the song start)
  u16 offset of the first tempo entry of the loop
  tempo entries: u32 length in ticks, u32 microseconds per quarter note
  event track; every event carries the delta time that precedes it:
    0x00-0x7F  note: channel = low nibble, bit 6 = gate + 256, bit 5 = delta + 256;
               then key, velocity, gate, delta
    0x81       play `count` events at track offset u16, then return (u16 offset, u8 count)
    0x82       loop point (u8 delta); the second one ends the loop body
    0x83       end of track
    0x87-0x8B  add 256 / 512 / 2048 / 4096 / 8192 ticks to the next note's gate
    0x8C-0x8F  rest of 256 / 512 / 2048 / 4096 ticks
    0xB0       control change: controller, value (bit 7 = delta + 256), delta
    0xC0       program change: program, delta
    0xD0       channel pressure: value, delta
    0xE0       pitch bend: value (8 bit, 0x40 = centre), delta

Tone bank (big-endian):
  u16 offsets of the mixer, velocity, pitch-EG and pitch-LFO tables, then one
  u16 offset per voice. A voice is a 4-byte header (bend range, portamento,
  layer count - 1, volume bias) followed by 32-byte layers:
    +0 first key, +1 last key, +2..+24 image of the SCSP slot registers 0x00-0x16,
    +25 base key, +26 fine tune (signed), +27/+28 FM connections,
    +29 velocity table, +30 pitch-EG, +31 pitch-LFO
  The sample start address (SA) is a file offset; samples are signed 16-bit
  big-endian (or signed 8-bit when PCM8B is set).
"""
import struct

GATE_EXT = {0x87: 256, 0x88: 512, 0x89: 2048, 0x8A: 4096, 0x8B: 8192}
REST_EXT = {0x8C: 256, 0x8D: 512, 0x8E: 2048, 0x8F: 4096}


def bank_songs(data):
    """Return {index: song bytes} for the songs of a .SEQ bank that hold data."""
    n = struct.unpack_from(">H", data, 0)[0]
    offs = [struct.unpack_from(">I", data, 2 + 4 * i)[0] for i in range(n)]
    out = {}
    for i, o in enumerate(offs):
        end = min([x for x in offs if x > o] + [len(data)])
        if o and end - o > 8:
            out[i] = data[o:end]
    return out


class Song:
    def __init__(self, data):
        self.ppqn, ntempo, track, _ = struct.unpack_from(">HHHH", data, 0)
        self.tempo = []          # (tick, microseconds per quarter)
        t = 0
        for i in range(ntempo):
            length, mspb = struct.unpack_from(">II", data, 8 + 8 * i)
            self.tempo.append((t, mspb))
            t += length
        # Events in stream order: (tick, channel, kind, a, b, c) with kind
        # 'note' (key, velocity, gate) | 'cc' (controller, value) | 'pc' (program) | 'bend' (value)
        self.events = []
        self.loop_start = None   # tick of the first loop mark
        self.loop_end = None     # tick of the second loop mark
        self.end = 0
        self._parse(data, track)

    def _parse(self, d, base):
        pos = base
        tick = 0
        gate_ext = 0
        ret = None               # (return position, remaining events) of an 0x81 reference
        n = len(d)
        while pos < n:
            st = d[pos]
            counted = True
            if st < 0x80:
                key, vel, gate, delta = d[pos + 1:pos + 5]
                pos += 5
                gate += gate_ext + (256 if st & 0x40 else 0)
                gate_ext = 0
                tick += delta + (256 if st & 0x20 else 0)
                self.events.append((tick, st & 0x0F, "note", key, vel, gate))
            elif st == 0x81:
                target, count = struct.unpack_from(">HB", d, pos + 1)
                pos += 4
                counted = False
                if ret is None and count:
                    ret = [pos, count]
                    pos = base + target
                    continue
            elif st == 0x82:
                tick += d[pos + 1]
                pos += 2
                counted = False
                if self.loop_start is None:
                    self.loop_start = tick
                else:
                    self.loop_end = tick
                    break
            elif st == 0x83:
                break
            elif st in GATE_EXT:
                gate_ext += GATE_EXT[st]
                pos += 1
                counted = False
            elif st in REST_EXT:
                tick += REST_EXT[st]
                pos += 1
                counted = False
            elif (st & 0xF0) == 0xB0:
                ctl, val, delta = d[pos + 1:pos + 4]
                pos += 4
                tick += delta + (256 if val & 0x80 else 0)
                self.events.append((tick, st & 0x0F, "cc", ctl, val & 0x7F, 0))
            elif (st & 0xF0) == 0xC0:
                prog, delta = d[pos + 1:pos + 3]
                pos += 3
                tick += delta
                self.events.append((tick, st & 0x0F, "pc", prog & 0x7F, 0, 0))
            elif (st & 0xF0) == 0xD0:
                tick += d[pos + 2]
                pos += 3
            elif (st & 0xF0) == 0xE0:
                val, delta = d[pos + 1:pos + 3]
                pos += 3
                tick += delta
                self.events.append((tick, st & 0x0F, "bend", val, 0, 0))
            else:
                pos += 1
                counted = (st & 0xF0) != 0x80
            if ret is not None and counted:
                ret[1] -= 1
                if ret[1] == 0:
                    pos = ret[0]
                    ret = None
        self.end = self.loop_end if self.loop_end is not None else tick

    def seconds(self, tick):
        """Convert a tick position to seconds with the tempo track."""
        tempo = self.tempo or [(0, 500000)]
        sec = 0.0
        for i, (t0, mspb) in enumerate(tempo):
            t1 = tempo[i + 1][0] if i + 1 < len(tempo) else None
            if t1 is None or tick < t1:
                return sec + (tick - t0) * mspb / 1e6 / self.ppqn
            sec += (t1 - t0) * mspb / 1e6 / self.ppqn
        return sec


class Layer:
    def __init__(self, bank, o):
        L = bank[o:o + 32]
        r = L[2:25]

        def w(k):
            return (r[k] << 8) | r[k + 1]

        self.key_lo, self.key_hi = L[0], L[1]
        self.loop = (w(0) >> 5) & 3
        self.pcm8 = (w(0) >> 4) & 1
        self.sa = ((w(0) & 0xF) << 16) | w(2)
        self.lsa, self.lea = w(4), w(6)
        self.d2r, self.d1r, self.ar = w(8) >> 11, (w(8) >> 6) & 31, w(8) & 31
        self.krs, self.dl, self.rr = (w(10) >> 10) & 15, (w(10) >> 5) & 31, w(10) & 31
        self.tl = w(12) & 0xFF
        self.mdl = w(14) >> 12
        self.lfof = (w(18) >> 10) & 31
        self.plfows, self.plfos = (w(18) >> 8) & 3, (w(18) >> 5) & 7
        self.alfows, self.alfos = (w(18) >> 3) & 3, w(18) & 7
        self.disdl, self.dipan = r[22] >> 5, r[22] & 31
        self.base = L[25]
        self.fine = L[26] - 256 if L[26] & 0x80 else L[26]
        self.vl = L[29]


class ToneBank:
    def __init__(self, data):
        self.data = data
        mix, vl, peg, plfo = struct.unpack_from(">HHHH", data, 0)
        nvoice = (mix - 8) // 2
        self.vltab = [data[vl + 10 * i:vl + 10 * i + 10] for i in range((peg - vl) // 10)]
        self.voices = []         # (bend range, volume bias, [Layer])
        for i in range(nvoice):
            o = struct.unpack_from(">H", data, 8 + 2 * i)[0]
            bend, _, nlayer, bias = data[o:o + 4]
            bias = bias - 256 if bias & 0x80 else bias
            self.voices.append((bend & 0x7F, bias, [Layer(data, o + 4 + 32 * k) for k in range(nlayer + 1)]))

    def velocity(self, table, vel):
        """Velocity curve of the driver: three break points and four slope codes."""
        if table >= len(self.vltab):
            return vel
        t = self.vltab[table]
        rate = (t[0], t[3], t[6], t[9])
        point = (t[1], t[4], t[7])
        level = (t[2], t[5], t[8])
        seg = 0
        base = 0
        origin = 0
        for k in range(3):
            if vel > point[k]:
                seg, base, origin = k + 1, level[k], point[k]
        code = rate[seg]
        shift, boost, mode = code >> 4, code & 8, code & 7
        m = vel - origin
        steep = (((m & 0x7F) * 12) << shift) >> 3 if boost else m << shift
        v = {0: base, 4: base, 1: base + steep, 2: base + m, 3: base + (m >> shift),
             5: base - (m >> shift), 6: base - m, 7: base - steep}[mode]
        return max(0, min(127, v))
