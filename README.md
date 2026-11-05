# RDPNO

**Roland's SA pianos (MKS-20; also the Rhodes MK-80) on the M-VAVE FM-1: [rdpiano](https://github.com/giulioz/rdpiano)'s
emulation of the CPU-B board, ported to the FM-1 on [Dinghy](https://github.com/zvenson/dinghy).**

rdpiano emulates the board these pianos share: the HD6301 CPU running Roland's program and the SA sound chip,
from silicon analysis. RDPNO runs that emulation inside the FM-1's audio interrupt, reading the ROMs straight
from flash, and plays it from the keys and MIDI.

> **Status: experimental, not yet tried on an FM-1.** The emulation core gives librdpiano's samples bit for bit
> (`tests/rdp_compare.sh`, all three ROM sets). The firmware builds with the JieLi toolchain: the GitHub workflow
> builds the `.fwsc` packages, and the build's checks pass (the image fits the app slot with MKS-20 voices 1-3; no
> 64-bit division, no chip ROM calls, RAM within limits). What only the device can show: whether the CPU keeps up
> (see [What to check first](#what-to-check-first)).

## The panel, after the MKS-20's front

Three pages, as the MKS-20's edit parameters. **SELECT** turns through them, or **HOME**, **FX**, **ENV** jump to one
(its button lights). The screen shows the voice on top (as the MKS-20's LCD), the page, its four knobs with their
values (the last one turned in white), MIDI and the CPU load.

| | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- |
| **HOME** | voice level 0..15 | treble level (10 kHz) | bass level (100 Hz) | the keys' velocity |
| **FX** | tremolo rate 1..15 | tremolo depth (0 = off) | chorus rate 1..15 | chorus depth (0 = off) |
| **ENV** | middle frequency 200 Hz..8 kHz | middle level | band width 1..8 (higher = narrower) | |

On every page: **PRESETS** the voice, **ALGORITHM** the MIDI receive channel (OMNI, 1..16), **OCT- / OCT+** the
octave, **PLAY** the sustain pedal (latched), **MASTER** the volume. Levels go -12..+12 dB in 1.5 dB steps, as on
the MKS-20. MIDI in: notes with velocity, CC 64, program change (the voice); the keys go out on USB-MIDI.
Everything but the pedal is kept in flash. The settings are global, not per voice (the MKS-20 keeps them per tone).

The board gives 16 voices; the equalizer (Simper SVFs), the chorus (a modulated delay, the two sides in
antiphase) and the stereo tremolo are new integer approximations in `fx.c`, not emulations of the MKS-20's
analog circuits.

## The ROMs

The ROMs are Roland's. They are **not** in this repository, and a build contains them: build it for yourself, do
not publish the packages. `tools/pack_roms.py` takes them from rdpiano's `roms/` folder:

```sh
git clone --depth 1 https://github.com/giulioz/rdpiano ../rdpiano     # the ROMs are in ../rdpiano/roms
python3 tools/pack_roms.py                    # -> firmware/gen/rdp_rom.h: MKS-20 voices 1-3, PIANO 1..3
python3 tools/pack_roms.py --set mks20b       # MKS-20 voices 4-8: HARPSICHORD, CLAVI, VIBRAPHONE, E-PIANO 1, 2
python3 tools/pack_roms.py --set mk80         # Rhodes MK-80 (Roland's 1989 piano on the same board): 8 tones
python3 tools/pack_roms.py --patches 0,1      # only these voices of the set (saves flash, see below)
```

`./build.sh` runs it by itself when `firmware/gen/rdp_rom.h` is missing (`RDP_ROMS=/path/to/roms` if they are
elsewhere, `RDP_SET=mks20b`, `RDP_REPACK=1` to pack again). **One wave set per build:** the MKS-20 itself has two
(voices 1-3 and 4-8), each 384 KB, and an FM-1 app may have 568 KB in all. So all eight MKS-20 voices make two
firmwares (`mks20a`, `mks20b`); the MK-80 has its eight on one set.

## Build and install

As Dinghy (its README has the details): Linux x86-64 (or Docker), Python 3 with Pillow, the JieLi toolchain and
three files of the AC79 SDK.

```sh
pip3 install Pillow
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh                                    # -> build/rdpno.fwsc
python3 tools/fm1_install.py build/rdpno.fwsc # over USB
```

Without an FM-1:

```sh
tests/run_host.sh tests/scripts/rdpno.txt     # -> build/host/rdpno.wav and the screens as PNG
tests/rdp_compare.sh ../rdpiano               # the C port against librdpiano, sample for sample
sh tests/run_tests.sh                         # everything (after ./build.sh)
```

Back to another firmware: the [FM-1 Firmware Hub](https://thefirmwarehub.com), or hold **OCT-** while switching on
(the USB rescue) and install from there.

## What to check first

1. **Does it fit?** `./build.sh` prints `image N B of 581564`. MKS-20 voices 1-3 fit (the workflow's build passes).
   Estimated with another compiler: about 580 KB for the MK-80 with all 8 tones (on the edge). If the build says the image is too big, leave out
   voices: `RDP_PATCHES=0,1 RDP_REPACK=1 ./build.sh` (every parameter window left out: 20..28 KB).
2. **Is the CPU fast enough?** The screen shows the sound's share of the audio interrupt's 5.8 ms. On a computer
   the emulation costs about 12,000 x86 instructions per board sample (20 kHz) with ~57 of the 160 parts sounding,
   about 5,400 per 44.1 kHz output sample: several times what sloopDX's engines take. Play big sustained chords
   and watch the PEAK: over ~90 % the sound will crackle. If so, the next step is splitting the 16 voices over
   the chip's two cores.
3. **Does the timing hold?** `rdp_exact = 1` (the default, in `rdp.c`) runs the board's CPU at its real 2 MHz,
   100 cycles per sample; librdpiano runs 100 *instructions* per sample, about four times faster. The two sound
   the same on the test script (same envelopes; big chords start a little more staggered, as on the board), and
   `rdp_exact = 0` is there to compare.

## How it works

| File | |
| --- | --- |
| `firmware/src/rdp.c` | the board: librdpiano's `Mcu` (MAME's HD6301) and `SoundChip`, in C, no heap, no float. The ROM tables live in flash (3 bytes a wave address), the parameter ROM's 32 KB window of the patch is unpacked (LZ4) into the pool, the exponential tables are folded to 10 KB (`rdp_tables.h`), and the sound chip only visits the parts that sound |
| `firmware/src/rdpno.c` | the app: keys, MIDI, the panel (HOME / FX / ENV), the screen; the board's 20/32 kHz resampled to 44.1 kHz |
| `firmware/src/fx.c`, `fx_tables.h` | voice level, bass/middle/treble EQ, chorus, tremolo at 44.1 kHz; the constants from `tools/gen_fx_tables.py` |
| `tools/pack_roms.py` | the ROMs, descrambled as librdpiano does it, into `firmware/gen/rdp_rom.h` |
| `tests/rdp_test.c`, `rdp_ref.cpp`, `rdp_script.h`, `rdp_compare.sh` | the same script on the port and on librdpiano |
| everything else | Dinghy's platform: updates, USB rescue, USB-MIDI/audio, panel, screen, storage, the SMK-37 |

The package identity is `FM-1_95N` (Dinghy is `99N`); check the hub's list before you release under it.

## License

GPL-3.0-only. rdpiano is GPL-3.0 (Giulio Zausa and contributors), its HD6301 core is MAME's (BSD-3-Clause), the
platform is Felucca's, SLOOP's and Dinghy's. Details in [LICENSING.md](LICENSING.md).
