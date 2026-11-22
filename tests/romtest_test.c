/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's ROM area test (firmware/src/romtest.c) on a fake flash: the first start writes, the next one finds the
 * pattern and reads it back OK, a flipped bit is found at its address, the erase stays inside the area.
 *   cc -Ifirmware/src -o romtest_test tests/romtest_test.c && ./romtest_test */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RDP_ROMTEST 1
#define ROMT_LO 0x0001A000u
#define ROMT_HI 0x00093000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))

static uint8_t flash[0x100000];
static int window_lo = 0x93000, polls, fails;
static jmp_buf done;
static char screen[12][48];
static uint32_t touched_lo = ~0u, touched_hi;

typedef struct { int h; } felucca_font_t;
static const felucca_font_t FONT_S = {16};
#define C_BLACK 0
#define C_WHITE 1
#define C_ACC 2
#define C_LED 3
#define C_GRAY 4
static uint8_t flash_ok = 1;
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; memset(screen, 0, sizeof screen); }
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s, uint16_t c, int a)
{
    (void)x; (void)w; (void)f; (void)c; (void)a;
    snprintf(screen[(y - 6) / 20], 48, "%s", s);
}
static void platform_poll(void) { if (++polls > 2000000) longjmp(done, 1); }
static void fl_plain_window_init(void) { window_lo = ROMT_LO; }
static int st_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int fl_erase4k(uint32_t off, uint32_t *t)
{
    *t = 0;
    if (!FL_IN(off, 0x1000u, ROMT_LO, ROMT_HI) || off % 0x1000u) { fails++; return -1; }
    if (off < touched_lo) touched_lo = off;
    if (off + 0x1000u > touched_hi) touched_hi = off + 0x1000u;
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n)
{
    uint32_t i;
    if (!FL_IN(off, n, ROMT_LO, ROMT_HI)) { fails++; return -1; }
    for (i = 0; i < n; i++) flash[off + i] &= s[i];
    return 0;
}
static uint32_t fl_xip_word(uint32_t off)
{
    uint32_t w;
    if (off >= ROMT_LO && (int)off < window_lo) { fails++; return 0; }   /* plain reads only above the window start */
    memcpy(&w, flash + off, 4);
    return w;
}
static uint32_t fm1_micros(void) { return 0; }

static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void str_cpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void fmt_int(char *b, int32_t v) { sprintf(b, "%d", (int)v); }

#include "romtest.c"

static void run(const char *what)
{
    int i;
    polls = 0;
    window_lo = 0x93000;
    if (!setjmp(done)) romtest_main();
    printf("-- %s\n", what);
    for (i = 0; i < 12; i++) if (screen[i][0]) printf("   %s\n", screen[i]);
}
static int has(const char *s) { int i; for (i = 0; i < 12; i++) if (strstr(screen[i], s)) return 1; return 0; }

int main(void)
{
    int bad = 0;
    memset(flash, 0xFF, sizeof flash);
    memset(flash + 0x4000, 0x5A, ROMT_LO - 0x4000);       /* the app area below */
    memset(flash + ROMT_HI, 0xA5, sizeof flash - ROMT_HI); /* the VM and up */
    run("first start, empty");
    bad |= !has("EMPTY: WRITING") && !has("WRITTEN");
    bad |= !has("READ SPI OK") || !has("READ XIP OK") || !has("POWER OFF AND ON");
    run("next start");
    bad |= !has("PATTERN FOUND") || !has("READ SPI OK") || !has("READ XIP OK") || !has("ROM AREA OK");
    flash[0x77777] ^= 0x10;
    run("a flipped bit at 0x77777");
    bad |= !has("READ SPI BAD AT 0X77774") || !has("READ XIP BAD AT 0X77774") || !has("PLEASE REPORT");
    memset(flash + ROMT_LO, 0x00, 0x100);
    run("other data at the start");
    bad |= !has("WRITTEN") || !has("READ SPI OK") || !has("READ XIP OK");
    for (uint32_t i = 0x4000; i < ROMT_LO; i++) bad |= flash[i] != 0x5A;
    for (uint32_t i = ROMT_HI; i < sizeof flash; i++) bad |= flash[i] != 0xA5;
    bad |= touched_lo != ROMT_LO || touched_hi != ROMT_HI || fails;
    printf("erased %#x..%#x, out-of-range calls %d\n", touched_lo, touched_hi, fails);
    printf(bad ? "FAIL\n" : "OK\n");
    return bad;
}
