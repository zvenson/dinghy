# RDPNO (experimental)

**Roland's SA pianos (MKS-20; also the Rhodes MK-80) on the M-VAVE FM-1: [rdpiano](https://github.com/giulioz/rdpiano)'s
emulation of the CPU-B board, ported to the FM-1 on [Dinghy](https://github.com/zvenson/dinghy).**

rdpiano emulates the board these pianos share: the HD6301 CPU running Roland's program and the SA sound chip,
from silicon analysis. RDPNO runs that emulation inside the FM-1's audio interrupt, reading the ROMs straight
from flash, and plays it from the keys and MIDI.

> **Status: v0.1, experimental. Plays on the FM-1** (old and new flash chip), in three builds: **RDPNO** (MKS-20 voices 1-3, the
> pianos), **RDPNO-B** (MKS-20 voices 4-8: harpsichord, clavi, vibraphone, e-piano 1 and 2) and **RDPNO-80** (the
> Rhodes MK-80's eight tones). The emulation gives librdpiano's samples bit for bit (`tests/rdp_compare.sh`), on
> one core or split over two.
>
> **Install it from the browser:** [r-jeb.github.io/rdpno-fm1](https://r-jeb.github.io/rdpno-fm1/) (Chrome or Edge,
> the FM-1 connected by USB). That firmware holds **no Roland data**: the ROMs are Roland's, so you load **your own
> dumps** into the FM-1 from the [load page](https://r-jeb.github.io/rdpno-fm1/load/), one set at a time (MKS-20 A,
> MKS-20 B or MK-80). The page makes the ROM pack in the browser; nothing leaves your computer except to the FM-1. A
> firmware update clears the ROMs; the installer loads them again in the same go. Everything the panel sets can also be
> set from the browser, live: the [editor](https://r-jeb.github.io/rdpno-fm1/edit/). What the three pages show:
> [the screens](https://r-jeb.github.io/rdpno-fm1/screens/).
>
> You can still build RDPNO yourself with the ROMs built in ([Build it yourself](#build-it-yourself)); please do not
> publish such packages: they contain the ROMs.

**How many notes:** the voices at 20 kHz (pianos, vibraphone, e-piano 1, the MK-80's Classic, Special, Blend,
A.Piano 1/2) play 6 to 8 notes clean, also with the pedal; the ones at 32 kHz (harpsichord, clavi, e-piano 2, the
MK-80's Contemporary and Clavi) cost 1.6 times as much: about 6 notes, and 4-note chords with the pedal are at the
limit. VOICES (ENV, knob 4) at 8 works best.

## How eight notes fit into the FM-1

The FM-1 has no floating point and 240 MHz; the board it emulates is a 2 MHz HD6301 CPU and a sound chip with
16 voices of 10 parts each, 20,000 samples a second, all run cycle for cycle inside the FM-1's audio interrupt.
At first a single note took most of the time. What brought it to eight:

| | |
| --- | --- |
| **Both cores** | The chip's second core (started as in Clipper's HAL) computes half of the sounding voices each sample; every 32 samples they are dealt out again by the parts they sound, so both finish together. With few parts one core does it all. |
| **Only what sounds** | The sound chip visits only the parts that sound (a mask per voice, and one over the voices), and parts more than ~60 dB down are not read from the wave ROM at all: their envelopes and phases go on, so the board sees no difference. |
| **The common case, straight** | Nearly every sounding voice has the same two flags; for it the part loop has its own version, without the general case's mask arithmetic. Envelope and phase steps are worked out when the board writes them, not every sample. |
| **The board's idle loop natively** | 94 % of the board's instructions are its scan over the voices and the main loop around it. These run as C code with the same registers, flags, RAM and cycles, instead of through the interpreter; other instructions skip the interpreter's interrupt checks while none could be taken. |
| **Space-D beside the board's CPU** | The chorus is computed by the second core while the first runs the board's CPU, so it costs next to nothing. |
| **Code and data in RAM** | The hot code (the `.fast` section), the board's program and the most used wave ROM pages are copied to RAM: the 32 KB flash cache would otherwise thrash. |
| **No bursts** | Notes go to the board at least 3 ms apart, the screen and LEDs redraw only what changed. |
| **Voices and the brake** | ENV, knob 4 sets how many voices the board uses (4..10, 8 by default; with the pedal down every key struck keeps one): it steals among them by its own rule. Over 90 % load an overload brake leaves out the faintest parts for a while (`*` after PARTS on the screen; at 100 % and over down to -48 dB) and puts them back under 70 %. Should the load stay over its time for 0.2 s anyway, the sound stops for ~60 ms and every note and the pedal are released: the main loop, which carries the note-offs, would otherwise starve, and the voices hang. |

The result does not change the sound: every step was checked sample for sample against the plain emulation.
Load on Joakim Eriksson's [FM-1 emulator](https://github.com/joakimeriksson/fm1-emu) (strict mode, 240 MHz; the
device is somewhat slower), `tools/emu_load.sh`:

| idle | one key | 4-note chord | 6 keys | 8-note chord | 16 notes |
| --- | --- | --- | --- | --- | --- |
| 6 % | 27 % peak | 47 % peak | 64 % peak | 73 % peak | 78 % peak |

## The panel, after the MKS-20's front

Three pages, as the MKS-20's edit parameters. **SELECT** turns through them, or **HOME**, **FX**, **ENV** jump to one
(its button lights). The screen shows the voice on top (as the MKS-20's LCD), the page, its four knobs with their
values (the last one turned in white), MIDI and the CPU load.

| | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- |
| **HOME** | voice level 0..15 | treble level (shelf, 4 kHz) | bass level (shelf, 200 Hz) | the keys' velocity |
| **FX** | tremolo rate 1..15 | tremolo depth (0 = off; 1.5 dB a step, 15: full) | chorus rate 1..15 | chorus depth (0 = off) |
| **ENV** | middle frequency 400 Hz..4 kHz (8 steps, as the MKS-20) | middle level | band width 1..8 (higher = narrower) | voices 4..10, 8 by default (the board's own voice stealing among them) |

**FX** pressed again on the FX page switches the chorus: **SPACE-D** (the default) or **BBD**; the page's title says
which. The tremolo is a stereo one, as rdpiano's: the two sides swing in antiphase.

**The equalizer:** bass and treble are shelving filters, gentle ones (Q 0.5) with their corner (half the gain) at
**200 Hz** and **4 kHz**: they reach nearly their full level at 100 Hz and 10 kHz, the MKS-20's own frequencies.
Shelves at 100 Hz and 10 kHz themselves did next to nothing here: the board's sound ends at 10 kHz (it samples at
20 kHz), and a piano has little at 100 Hz. The middle is a peak filter at 400, 560, 770 Hz, 1, 1.4, 2, 2.8 or 4 kHz,
its width 1..8 (Q 0.35..4). All three -10.5..+10.5 dB in 1.5 dB steps (`tools/gen_fx_tables.py`).

On every page: **PRESETS** the voice, **ALGORITHM** the MIDI receive channel (OMNI, 1..16), **OCT- / OCT+** the
octave, **PLAY** the sustain pedal (latched), **MASTER** the volume. Levels go -10.5..+10.5 dB in 1.5 dB steps, as on
the MKS-20. MIDI in: notes with velocity, CC 64, CC 120 / 123 (all notes off), program change (the voice); the keys go out
on USB-MIDI. On the DIN/TRS input aftertouch, pitch bend, the other controllers and clock are dropped at once (a
keyboard's aftertouch stream would fill the queue); a note-off or the pedal is never dropped on the way to the
board; if a message is lost anyway, every note is released and the MIDI line shows `E` and the count.
Everything but the pedal is kept in flash. The settings are global, not per voice (the MKS-20 keeps them per tone).

The board gives 16 voices. The SPACE-D chorus is rdpiano_juce's Space-D (`spaced.c`): a Roland LSP effect
program, Roland's digital Dimension-D-style chorus, two delay lines swung in antiphase by a triangle and crossed to
the two sides; it runs at the board's rate, right after it, sample for sample as rdpiano_juce's (checked on the
computer). The equalizer (Simper SVFs), the BBD chorus (a modulated delay, the two sides in antiphase) and the
stereo tremolo are new integer approximations in `fx.c`, not emulations of the MKS-20's analog circuits.

## Build it yourself

You need: an FM-1 (or an SMK-37), a computer with **Linux x86-64**, or **Windows with WSL** (Ubuntu from the
Microsoft Store), and about 1 GB of disk. In a terminal (under Windows: the Ubuntu window):

```sh
git clone https://github.com/R-Jeb/rdpno-fm1 ~/rdpno-fm1
cd ~/rdpno-fm1
sh tools/setup.sh
```

`tools/setup.sh` installs the packages (asks for your password once), the JieLi toolchain and JieLi's AC79 SDK,
fetches the ROM dumps into `../rdpiano/roms` (from [rdpiano](https://github.com/giulioz/rdpiano)'s repository) and
builds the three firmwares:

| Package | | Voices |
| --- | --- | --- |
| `build/rdpno.fwsc` | RDPNO, MKS-20 voices 1-3 | PIANO 1, PIANO 2, PIANO 3 |
| `build/rdpno-b.fwsc` | RDPNO-B, MKS-20 voices 4-8 | HARPSICHORD, CLAVI, VIBRAPHONE, E-PIANO 1, E-PIANO 2 |
| `build/rdpno-mk80.fwsc` | RDPNO-80, Rhodes MK-80 | CLASSIC, SPECIAL, BLEND, CONTEMPORARY, A.PIANO 1, A.PIANO 2, CLAVI, VIBRAPHONE |

Under WSL it copies them into Windows' `Downloads` too (or take them from `\\wsl.localhost\Ubuntu\home\<you>\rdpno-fm1\build`
in the Explorer).

**Install** one of them with M-VAVE's **M-UPGRADE**, or the [FM-1 Firmware Hub](https://thefirmwarehub.com)'s
local-file install in Chrome or Edge: FM-1 switched on, connected straight to the computer, **every other MIDI
program and browser tab closed** (Windows lets one program at a time open a MIDI port; else the installer says the
device "did not answer"). Or from the terminal: `python3 tools/fm1_install.py build/rdpno.fwsc`.

**Later:** `git pull`, then `./build.sh` (RDPNO), `./build.sh --set mks20b` (RDPNO-B) or `./build.sh --set mk80`
(RDPNO-80). The ROM data are packed for the set by themselves (`tools/pack_roms.py`; `RDP_ROMS=/path` if the dumps
are elsewhere, `RDP_PATCHES=0,1` for fewer voices if a build does not fit: 20..28 KB a parameter window).

**The ROMs:** Roland's. They are not in this repository, nor in anything GitHub builds: its workflow builds with
blank ROMs (`RDP_NO_ROMS=1`), and that firmware only says *NO ROMS: BUILD IT YOURSELF*. Keep the dumps and your
packages to yourself.

Without an FM-1:

```sh
tests/run_host.sh tests/scripts/rdpno.txt     # -> build/host/rdpno.wav and the screens as PNG
tests/rdp_compare.sh ../rdpiano               # the C port against librdpiano, sample for sample
sh tests/run_tests.sh                         # everything (after ./build.sh)
```

## If the FM-1 gets stuck

- **Back to another firmware:** the [FM-1 Firmware Hub](https://thefirmwarehub.com), or hold **OCT-** while
  switching on (the USB rescue: no sound, only the installer) and install from there.
- **The installer finds the FM-1 but it "does not answer"** (a firmware with broken USB-MIDI): close every other
  MIDI program first. If it still does not answer, [M-Flasher](https://github.com/kmplngj/m-flasher) (Windows, macOS,
  Linux): with the firmware running, its **DOWNLOAD MODE** button switches the FM-1 into the chip's own USB
  download mode (that way only needs the computer-to-FM-1 direction), it backs up the whole flash, and installs a
  `.fwsc`, e.g. M-VAVE's original `FM-1.fwsc`; from that, RDPNO again with M-UPGRADE.
- **Do not build with `-O2`:** USB-MIDI breaks with it, and no installer reaches the FM-1 any more (that is how
  the way above was found).

## How it works

| File | |
| --- | --- |
| `firmware/src/rdp.c` | the board: librdpiano's `Mcu` (MAME's HD6301) and `SoundChip`, in C, no heap, no float. The ROM tables live in flash (3 bytes a wave address), the parameter ROM's 32 KB window of the patch is unpacked (LZ4) into the pool, the exponential tables are folded to 10 KB (`rdp_tables.h`), and the sound chip only visits the parts that sound |
| `firmware/src/rdpno.c` | the app: keys, MIDI, the panel (HOME / FX / ENV), the screen; the board's 20/32 kHz resampled to 44.1 kHz |
| `firmware/src/fx.c`, `fx_tables.h` | voice level, bass/middle/treble EQ, BBD chorus, tremolo at 44.1 kHz; the constants from `tools/gen_fx_tables.py` |
| `firmware/src/spaced.c` | the Space-D chorus after rdpiano_juce (Giulio Zausa), at the board's rate |
| `tools/pack_roms.py` | the ROMs, descrambled as librdpiano does it, into `firmware/gen/rdp_rom.h` |
| `tests/rdp_test.c`, `rdp_ref.cpp`, `rdp_script.h`, `rdp_compare.sh` | the same script on the port and on librdpiano |
| everything else | Dinghy's platform: updates, USB rescue, USB-MIDI/audio, panel, screen, storage, the SMK-37 |

The package identity is `FM-1_95N` (Dinghy is `99N`); check the hub's list before you release under it.

## License

GPL-3.0-only. rdpiano is GPL-3.0 (Giulio Zausa and contributors), its HD6301 core is MAME's (BSD-3-Clause), the
platform is Felucca's, SLOOP's and Dinghy's. Details in [LICENSING.md](LICENSING.md).
