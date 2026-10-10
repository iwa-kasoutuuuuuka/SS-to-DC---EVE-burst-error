# Third-party code

## libADX (`src/adx/`)

`src/adx/adx.h`, `libadx.c`, `snddrv.c` and `snddrv.h` are a modified copy of
libADX from [kos-ports](https://github.com/KallistiOS/kos-ports):

- Copyright (C) 2011-2013 Josh "PH3NOM" Pearson
- Copyright (C) 2024-2025 The KOS Team and contributors
- Decoder algorithm: adv2wav (C) 2001 BERO

It is distributed under the BSD-style license printed at the top of each of
those files. Changes made for this project: the decoder reads the CD through a
32 KB read-ahead buffer, PCM is handed to the stream callback through a
lock-free ring (the callback never waits for the decoder), `adx_stop()` gives up
after a timeout instead of blocking, and the sound stream is not shut down when
a clip ends because it is shared with the music player.

## Libraries linked at build time (not included here)

- [KallistiOS](https://github.com/KallistiOS/KallistiOS) (KOS License, BSD-style)
- libmpeg from kos-ports (based on pl_mpeg by Dominic Szablewski, MIT)

The boot sector (`IP.BIN`) is generated at build time by KallistiOS's `makeip`
from `ip.txt`; none is stored in this repository.

## References used for the file formats

- Sega's Saturn sound driver and tone editor manuals (sequence and tone bank layout)
- [VGMTrans](https://github.com/vgmtrans/vgmtrans) (Saturn sequence events), used as documentation only
- The SCSP envelope and LFO rate tables are the ones published in the SCSP
  user's manual and used by emulators such as MAME
