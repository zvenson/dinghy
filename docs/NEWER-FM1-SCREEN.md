# The newer FM-1s' screen – washed out, no reds, or blank

**For every FM-1 firmware, not only Dinghy's forks.**

## What people see

Newer FM-1s come with the FM-1 / M-VAVE logo under the screen, and most of them with the UC25HQ80 flash (JEDEC
`0xB36014`). On them, many firmwares show washed-out colours, no reds at all, or a blank screen. The first FM-1s look
fine with the same firmware.

## Why

Most firmwares go back to an early, minimal ST7789V setup: a reset, then only the colour format, the scan direction and
inversion. Everything else – porch, gate, VCOM, the supply voltages, the frame rate and both gamma curves – stays at
the panel's own power-on defaults. The first FM-1s' panels happen to look right with those. The newer panels do not.

Some firmwares also run the display's SPI clock fast (fm1-base: 60 MHz). M-VAVE's own firmware uses 12 MHz, and the
newer panels need it.

## The fix

After the reset, send M-VAVE's own register values, **in the order M-VAVE's firmware sends them**, and keep the display
clock at 12 MHz (at least on the `0xB36014` units).

```
0x01                      SWRESET            wait 150 ms
0x28                      DISPOFF            wait 100 ms
0x11                      SLPOUT             wait 120 ms
0xB2 0C 0C 0C 00 33       PORCTRL
0xB7 56                   GCTRL
0xBB 18                   VCOMS
0xC0 2C                   LCMCTRL
0xC2 01                   VDVVRHEN
0xC3 1F                   VRHS
0xC4 20                   VDVS
0xC6 0F                   FRCTRL2            60 Hz
0xD0 A6 A1                PWCTRL1
0xE0 D0 0D 14 0B 0B 07 3A 44 50 08 13 13 2D 32    PVGAMCTRL
0xE1 D0 0D 14 0B 0B 07 3A 44 50 08 13 13 2D 32    NVGAMCTRL
0x36 00                   MADCTL             top-left origin, RGB
0x3A 55                   COLMOD             RGB565
0xE7 00                   SPI2EN             one data line
0x51 FF                   WRDISBV            full brightness
0x21                      INVON              IPS panel
0x13                      NORON
… clear the screen, then 0x29 DISPON
```

The first FM-1s look exactly as before with it.

### Where to put it

- **Dinghy and its forks:** copy `LCD_SEQ` (the FM-1 branch, not the SMK-37 one) from
  [`firmware/src/lcd.c`](../firmware/src/lcd.c) of Dinghy 0.6.1. The clock is already 12 MHz (`LCD_BAUD 4`).
- **fm1-base firmwares:** set `lcd_panel = true` in `fm1.toml` (M-VAVE's register values after `lcd_init`). Run the
  panel at 12 MHz on the newer units (`fl_jedec() == 0xB36014`). For both, h1's `panel_setup` and `panel_clock` in
  [jakehandy/h1 `app/app.c`](https://github.com/jakehandy/h1) are a complete example.
- **Felucca / felucca-bsp forks:** the same table in your LCD init, after the reset, and the 12 MHz clock.

## Credit

Found and measured by **Jake Handy** for h1 (0.3.1, "Panel setup in M-VAVE's order and clock on newer FM-1s").
In Dinghy since 0.6.1 ([CHANGELOG](../CHANGELOG.md)).
