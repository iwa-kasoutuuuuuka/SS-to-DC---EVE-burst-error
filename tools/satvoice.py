#!/usr/bin/env python3
"""
satvoice.py - decoder for the voice / sound-effect files of the Saturn version
of EVE burst error (SOUND/*.WAV and MISC/*.WAV, RIFF format tag 0xFF02).

The files are 6-bit logarithmic DPCM, mono, 22050 Hz:
  fmt chunk : tag 0xFF02, channels, sample rate, 0, 0, bits = 6, cbSize = 4,
              u32 number of samples
  data chunk: a little-endian bit stream of 6-bit codes (first code in the low
              bits of the first byte)
    code -32 (0b100000)   : escape; the next code, read as unsigned 0..63, is
                            the length of a run of unchanged samples (silence)
    any other code c      : sample += sign(c) * STEP[|c|], 16-bit saturated
STEP is the table the game itself uses (32 entries, 0 .. 32767).

Usage:
  python satvoice.py <in.WAV> <out.wav>          # 16-bit PCM WAV
Needs numpy.
"""
import struct
import sys
import wave

import numpy as np

STEP = np.array([0, 1, 5, 17, 44, 91, 163, 269, 413, 604, 849, 1155, 1530, 1981, 2517, 3145,
                 3873, 4711, 5665, 6746, 7961, 9319, 10829, 12500, 14341, 16361, 18570, 20976,
                 23590, 26420, 29475, 32767], dtype=np.int64)
ESCAPE = 32


def parse(data):
    """Return (sample rate, sample count, code bytes) of a 0xFF02 RIFF file."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    rate = count = None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt, _, rate = struct.unpack_from("<HHI", body, 0)
            if fmt != 0xFF02:
                raise ValueError(f"format tag {fmt:#06x}, expected 0xff02")
            count = struct.unpack_from("<I", body, 18)[0] if len(body) >= 22 else None
        elif tag == b"data":
            return rate, count, body
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def decode(data):
    """Decode a 0xFF02 file. Returns (sample rate, int16 numpy array)."""
    rate, count, body = parse(data)
    bits = np.unpackbits(np.frombuffer(body, dtype=np.uint8), bitorder="little")
    bits = bits[:len(bits) // 6 * 6].reshape(-1, 6)
    codes = (bits << np.arange(6, dtype=np.uint8)).sum(axis=1).astype(np.int64)

    # Escapes have to be found in order: the run length that follows one may be 32 too.
    repeat = np.ones(len(codes), dtype=np.int64)
    skip = -1
    silent = np.zeros(len(codes), dtype=bool)      # escape codes and their run lengths
    for i in np.flatnonzero(codes == ESCAPE):
        if i == skip:
            continue
        repeat[i] = 0
        silent[i] = True
        if i + 1 < len(codes):
            repeat[i + 1] = codes[i + 1]
            silent[i + 1] = True
            skip = i + 1

    magnitude = np.where(codes > 32, 64 - codes, codes)
    delta = np.where(codes > 32, -STEP[magnitude % 32], STEP[magnitude % 32])
    delta[silent] = 0
    delta = np.repeat(delta, repeat)
    pcm = np.cumsum(delta)
    if len(pcm) and (pcm.max() > 32767 or pcm.min() < -32768):      # rare: saturate step by step
        acc = 0
        out = np.empty(len(delta), dtype=np.int64)
        for k, dv in enumerate(delta.tolist()):
            acc = max(-32768, min(32767, acc + dv))
            out[k] = acc
        pcm = out
    if count:
        if len(pcm) < count:
            pcm = np.concatenate([pcm, np.full(count - len(pcm), pcm[-1] if len(pcm) else 0)])
        pcm = pcm[:count]
    return rate or 22050, pcm.astype("<i2")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rate, pcm = decode(open(sys.argv[1], "rb").read())
    with wave.open(sys.argv[2], "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())
    print(f"{sys.argv[1]}: {len(pcm)} samples at {rate} Hz -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
