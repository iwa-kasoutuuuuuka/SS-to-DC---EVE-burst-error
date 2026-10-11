# File formats of the Saturn version of EVE burst error

Notes on the data files the engine and `tools/convert.py` read. All multi-byte
values on the Saturn disc are big-endian unless stated otherwise. The tools in
`tools/` are the reference implementations.

## Disc layout

Each of the three game discs is an ISO9660 volume (`EVE`) with these directories:

| Directory | Contents |
|---|---|
| `SCENARIO/` | 90 scenario files `*.SCO` (the same set on every disc) |
| `GRAPHICS/` | pictures `*.B1` used by the part of the story on that disc |
| `MISC/` | UI pictures (`*.B1`), compressed sheets and font (`*.CMP`), UI sounds (`*.WAV`) |
| `MOVIES/` | Cinepak movies `*.CPK` (Sega FILM container; ffmpeg reads them) |
| `MUSIC/` | sequence banks `*.SEQ` and tone banks `*.BIN` |
| `SOUND/` | voices and sound effects `*.WAV` (not PCM, see below) |

An empty root file `DISC1` / `DISC2` / `DISC3` tells the discs apart. The story is
split as: disc 1 = first half of the Kojiro route, disc 2 = first half of the
Marina route, disc 3 = second halves and the finale.

**Extraction pitfall.** A raw image has 2352-byte sectors. Copying "file size"
bytes from the first sector of a file leaves 304 bytes of sync / header / EDC /
ECC inside every 2352 bytes and loses the end of the file. Only the 2048 user
bytes per sector belong to the file (`tools/satdisc.py`).

## Scenario: `.SCO`

- Every byte is XORed with 0xFF.
- From offset 0: a stream of 16-bit words. Instructions are `0x80xx` followed by
  their operand words. The code is followed by the string area (NUL-terminated
  Shift-JIS, padded to even offsets).
- String, name and jump operands are **word addresses**: byte offset = operand × 2.
- Every opcode has a fixed operand count except the three listed first.

| Opcode | Operands | Meaning |
|---|---|---|
| `8035` | text, n, n × voice | show text; n (0..2) voice file names follow |
| `8002` | N, then N × { k, k × (var, value), text, target } | menu; an entry is shown when all its k conditions hold |
| `8003` | k, k × (var, value), target | jump when all conditions hold |
| `8005` | target | jump |
| `8007` | var, value | set a variable (flags; all 0 at the start) |
| `8014` | name | background picture `GRAPHICS/<name>.B1` |
| `8036` | name, x, y | overlay picture at (x, y) in the 320x224 frame; cleared by the next `8014` |
| `8012` / `8013` | flag | fade out / fade in (0 = cut) |
| `8025` | n | speed of the next fade in |
| `802f` | n | wait n frames |
| `8026` | n | play music n (0 = stop) |
| `800f` | n | fade the music out (n = speed) |
| `800e` | n | 0 mutes the music, non-zero restores it |
| `8039`, `800b` | name | sound effect `SOUND/<name>.WAV` |
| `8008` | name | play movie `MOVIES/<name>.CPK`; `8009` waits for its end |
| `8020` | name | continue in another scenario file (variables are kept) |
| `8001` | id | block label (the title screen jumps to block 100 of `A01`) |
| `8006` | - | return to the top-level command menu |
| `8028` / `801e` | - | save / load |
| `802a` | n | route: 1 Kojiro, 2 Marina, 3 common |
| `803a` | n | disc n is required from here on |
| `8041` / `8043` / `8042` | frame / text / - | terminal screen: show, append text, clear |
| `801f` | y, x, ?, ? | text caret position on the terminal |
| `8040` | x, y | draw the following text at a free position |
| `8022` | colour, 0 | text colour |
| `8033` | slot, r, g, b | set a palette entry (5-bit components) |

Operand counts of the remaining opcodes are in `FIXED` in `tools/sco.py`.
Text strings may end in lower-case control letters: `w` (wait, then advance),
`k` (wait for a key), `n` (append / clear the window), `s` (line has two voice
files). Terminal text uses `\n`, `\q` (clear), `\cN` (colour), `\wN` (pause),
`\b`, `\u`.

Characters are not separate sprites: a background such as `01__03__` is the
room `01` with character `03` painted in, and expressions are small `8036`
overlays on top of it.

## Pictures: `.B1`

```
0x00  'b1'
0x02  u16 width, u16 height, u16 0
0x08  16-bit tokens until the end of the file
        bit 15 = 1 : one pixel, 1 BBBBB GGGGG RRRRR
        bit 15 = 0 : run; high byte = length n, low byte = source mask.
                     Pixel k of the run copies its left neighbour when mask bit
                     (7 - (k & 7)) is 1, and the pixel above when it is 0.
```

`0xFFFF` is the transparent colour of overlays. Backgrounds are 320x160 (the
lower 64 lines of the 320x224 screen belong to the message window).

## Compressed data: `.CMP`

A byte-oriented LZ stream:

```
c < 0x80      copy the next c + 1 bytes
c >= 0x80, n  copy n + 3 bytes from (c & 0x7F) + 1 bytes back in the output (may overlap)
```

`SPR1`, `SPR2`, `SPR3` and `COMFONT` decode to Windows BMP files (4 or 8 bits per
pixel). `FONT.CMP` decodes to 2189 glyphs of 16x16 pixels, 4 bits per pixel
(128 bytes each); `CHARCODE` holds their Shift-JIS codes (u16 each, ascending),
glyph i belongs to code i.

## Voices and sound effects: `.WAV` (format tag 0xFF02)

RIFF files, but the samples are 6-bit logarithmic DPCM (mono, 22050 Hz):

```
fmt  : tag 0xFF02, channels 1, rate 22050, 0, 0, bits 6, cbSize 4, u32 sample count
data : little-endian bit stream of 6-bit codes (first code = low 6 bits of byte 0)
         code 0b100000 (-32) : escape; the next code, unsigned 0..63, is the number
                               of samples the output stays unchanged (silence)
         other code c        : sample += sign(c) * STEP[|c|]   (16-bit, saturated)
STEP = 0, 1, 5, 17, 44, 91, 163, 269, 413, 604, 849, 1155, 1530, 1981, 2517, 3145,
       3873, 4711, 5665, 6746, 7961, 9319, 10829, 12500, 14341, 16361, 18570,
       20976, 23590, 26420, 29475, 32767
```

## Music: `.SEQ` and tone banks `.BIN`

The data of Sega's standard Saturn sound driver.

A `.SEQ` bank starts with `u16 count` and `count` × `u32` song offsets. The index
of a song inside its bank is the number used by the scenario (`8026 n`); most
entries of a bank are empty. `GAMEA`..`GAMED` use the tone bank `GAME.BIN`, the
other banks the `.BIN` of the same name.

A song is: `u16` ticks per quarter note, `u16` tempo entry count, `u16` offset of
the event track, `u16` offset of the loop's first tempo entry, then the tempo
entries (`u32` length in ticks, `u32` microseconds per quarter note). Events
(each carries the delta time that precedes it):

| Status | Bytes after it | Meaning |
|---|---|---|
| `00`-`7F` | key, velocity, gate, delta | note; low nibble = channel, bit 6 adds 256 to gate, bit 5 adds 256 to delta |
| `81` | u16 offset, u8 count | play `count` events at that track offset, then return |
| `82` | delta | loop mark (first = loop start, second = loop end) |
| `83` | - | end of track |
| `87`-`8B` | - | add 256 / 512 / 2048 / 4096 / 8192 to the next gate time |
| `8C`-`8F` | - | rest of 256 / 512 / 2048 / 4096 ticks |
| `Bn` | controller, value, delta | control change (bit 7 of value adds 256 to delta) |
| `Cn` | program, delta | program change |
| `Dn` | value, delta | channel pressure |
| `En` | value, delta | pitch bend, 8 bits, 0x40 = centre |

A tone bank starts with four `u16` offsets (mixer, velocity curves, pitch
envelopes, pitch LFOs) followed by one `u16` offset per voice (= program). A
voice is a 4-byte header (bend range, portamento, layer count - 1, volume bias)
and 32-byte layers:

```
+0  first key      +1  last key
+2  23 bytes: image of the SCSP slot registers 0x00-0x16
      (loop mode, 8-bit flag, sample address, loop start / end, AR, D1R, D2R, RR,
       DL, key rate scaling, total level, LFO settings, direct send level and pan)
+25 base key       +26 fine tune (signed)
+27 FM settings (2 bytes)
+29 velocity curve +30 pitch envelope +31 pitch LFO
```

The sample address is an offset into the bank file; samples are signed 16-bit
big-endian (8-bit when the PCM8B bit is set). A sample plays at 44100 Hz on its
base key. `tools/satbgm.py` renders the songs from this data.

## Files written for the Dreamcast engine

| File | Layout (little-endian) |
|---|---|
| `graphics/**/*.BIN` (EVB1) | `"EVB1"`, u16 width, u16 height, u16 format (0 = RGB565, 1 = ARGB4444), u16 0, pixels top-down |
| `graphics/ui/SATFONT.BIN` (EVF1) | `"EVF1"`, u16 count, u16 0, count × u16 Shift-JIS code, count × 128-byte glyph |
| `audio/DiscN/*.adx` | CRI ADX, mono, 22050 Hz |
| `bgm/BGM<n>.wav` | RIFF, AICA (Yamaha) ADPCM, mono, 22050 Hz, looped as a whole |
| `se/*.wav` | UI sounds from `MISC/*.WAV`: RIFF, AICA (Yamaha) ADPCM, mono, 22050 Hz |
| `movies/*.mpg` | MPEG-1 program stream, 320x240, 30 fps, MP2 mono 32 kHz |
