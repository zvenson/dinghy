# FM-1 screen mirror (MIT)

Lets an M-VAVE FM-1 firmware send its screen over USB MIDI, so an app on a computer can show it big:
[FM-1 Mirror](https://github.com/pfkellogg/fm1-mirror) (free, Mac/Windows/Linux) shows it live, in
colour or high-contrast looks, for players who can't read the 1.54-inch display.

**MIT licensed** (see LICENSE), so any FM-1 firmware can use it, open source or not, under any licence.
That covers this folder and the same code in the screen-out branch's `app/screen.c` ("Screen mirror"
section) and the hello check in `app/transfer.c`, which I wrote. The rest of this repository is
M-VAVE's FM1 Open and keeps its own licence.

## The messages

| Direction | Message | Meaning |
|---|---|---|
| app → FM-1 | `F0 7D 46 53 00 F7` (`'F' 'S'`) | "keep sending your screen" — about once a second |
| app → FM-1 | `F0 7D 46 53 01 F7` | the same, and send the whole screen again (a new viewer) |
| FM-1 → app | `F0 7D 46 53 01 rl rh cl ch {n c0 c1 c2}… F7` | one screen row, from column `c` |

- Row and column are two 7-bit bytes each (`low = v & 127`, `high = v >> 7`).
- Each run is four bytes: a count 1–127, then the RGB565 colour as bits 0–6, 7–13 and 14–15.
- Messages stay at 70 bytes or less, so they share a small USB queue with note output.
- The firmware sends only what it paints (what changes), and stops 3 seconds after the last hello.
  With nobody asking it sends nothing, so playing is unaffected.
- `7D` is MIDI's non-commercial manufacturer ID.

## Using it

`screen_mirror.c` / `.h` are plain C with no dependencies:

1. Pass every incoming SysEx message to `sm_sysex()` (it returns 1 for a hello).
2. Call `sm_poll(now_ms, send)` often, with a function that sends one SysEx message over USB.
3. While `sm_on()`, give each block of rows you send to the LCD to `sm_rows()` as well. If it
   returns 0, the queue is full: try again after `sm_poll`, or repaint those rows later.
4. When `sm_take_full_request()` returns 1, repaint the whole screen.

The buffer is 16 KB (`-DSM_BUFFER=…` to change it). `cc screen_mirror.c test.c && ./a.out` runs the test.

Questions, or want it tested on a real FM-1? pfkellogg@gmail.com — Patrick F. Kellogg
