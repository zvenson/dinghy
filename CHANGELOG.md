# Changelog

Every Dinghy release. The packages and their build attestations are on the
[releases page](https://github.com/zvenson/dinghy/releases); the Firmware Hub installs them
([thefirmwarehub.com](https://thefirmwarehub.com)). Identity: FM-1_99Y (SMK-37 Pro_99Y) for release 0.Y.

## 0.6 – 2026-11-24

The basics OberVave has grown, brought over as the kit: headers you include after `platform.c` when you want them.
`dinghy.c` uses all of them, so a fork starts with an instrument that behaves like one.

- **MIDI clock sync** (`kit_clock.h`): the clock and transport are taken where they arrive (`MIDI_RT_HOOK` in
  `usb.c` and `midi_uart.c`), never when the main loop gets round to them, so a redraw cannot move a beat. The master's
  tempo is measured in rendered frames and our beat held on its quarters by OberVave 3.5.4's PI loop: 5 minutes at odd
  tempos with jitter, no drift (`tests/clock_sync_test.py`). A tick 24 times a quarter in the audio ISR, to the
  frame (`CLOCK_TICK`). SYNC AUTO (a clock that comes in leads) or INTERNAL. START / CONTINUE from the top, STOP.
- **Our own tempo and transport**: PLAY starts and stops it, tap tempo, 40..240 BPM, kept in flash.
- **MIDI basics** (`kit_midi.h`): the channel (OMNI or 1..16, in and out), pitch bend (slewed, +-2 semitones),
  the mod wheel (a vibrato in Dinghy), the sustain pedal (CC 64), volume (CC 7), all notes off (CC 120 / 123),
  program change and every other CC handed to the app.
- **FM-1 Mirror** (`kit_mirror.h`, Patrick F. Kellogg's `screen_mirror`, MIT): the screen live and big on a computer.
- **Backup** (`kit_backup.h`): settings, blobs and slots to a file and back over SysEx, OberVave's protocol with a tag
  of the firmware's own (Dinghy: `DY`).
- **VIBE LINK** (`kit_link.h`, FM-1): the FM-1 as the VIBE FM's controller, beside the sound (ON) or instead of it
  (CONTROL).
- **The GLOBAL page** in `dinghy.c` (GLO): MIDI channel, SYNC, tempo, a click on the beat, REC = tap tempo, SELECT =
  VIBE LINK. 0.5's settings (octave, channel) come over.
- Tests: `tests/scripts/basics.txt`, `tests/clock_sync_test.py`, `tests/backup_test.c` in `tests/run_tests.sh`.
- USB audio is unchanged: the FM-1 is a 16-bit stereo audio input at 44.1 or 48 kHz for any DAW or phone.

## 0.5 – 2026-11-07

- Newer FM-1s' flash chip, the UC25HQ80 (JEDEC 0xB36014), beside the Puya (0x856014): before, a firmware built on
  Dinghy saved to RAM only on them.
- CI: only the release builds are attached and attested; the `.fwsc` files reach the release again.

## 0.4 – 2026-10-23

OberVave's platform: the TRS MIDI IN, clock and transport to the app, the MIDI channel, blobs and slots, `LCD_IDLE`,
the update loader's screen, the SMK-37 (Pro, Elite, MKE-P37), the test suite, and a CI release with attestation.
The README's "Build your own", step by step.

## 0.3 – 2026-10-09

A key stops the note it started (an octave change while it was held left it hanging); host mode runs a script file
and films at a frame rate. Tested on the FM-1: install, MIDI in, USB audio at 44.1 / 48 kHz, keys, screen, the octave
in flash, and back to another firmware through the hub.

## 0.2 – 2026-10-09

The logo (the designburg signet in dark purple) and lilac as the screen's accent; the credits (based on Felucca, its
platform as SLOOP carries it).

## 0.1 – 2026-10-09

The first: a 4-voice sine on Felucca's platform, as SLOOP carries it.
