/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's ROM area test (tools/build.py --romtest; included by rdpno.c with RDP_ROMTEST=1). The question before
 * a ROM-free RDPNO that takes the ROMs from a web page: can the app keep 474..494 KB of its own data in the flash
 * between its (now small) app slot and the VM, [ROMT_LO, ROMT_HI), and read it there through a plain XIP window?
 *
 *   first start (the area is not our pattern):  erase + write the pattern, read it back over SPI and over XIP:
 *                                               "WRITTEN ... POWER OFF AND ON"
 *   next start (the pattern is there):          read it back over SPI and XIP: "ROM AREA OK" or the first bad address
 *
 * Both also time random XIP reads in the area against reads of the app's own (encrypted) code. Nothing else runs:
 * no sound; the main loop keeps the USB and the installer's update request alive (a normal update afterwards
 * rewrites the app area and with it this area: the pattern is gone, as the ROMs will be after each update). */

#define RT_Y(n) (6u + 20u * (n))

static uint32_t rt_word(uint32_t off)           /* the pattern: a hash of the address, a new value every word */
{
    uint32_t h = (off ^ 0x524F4D54u) * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    return h ^ (h >> 13);
}

static void rt_line(uint32_t n, const char *s, uint16_t c) { draw_text_box(0, RT_Y(n), 240, &FONT_S, s, c, 1); }

static void rt_hex(char *b, uint32_t v)         /* 0x%05X */
{
    static const char HEX[] = "0123456789ABCDEF";
    uint32_t i;
    b[0] = '0'; b[1] = 'X';
    for (i = 0; i < 5u; i++) b[2u + i] = HEX[v >> (16u - 4u * i) & 15u];
    b[7] = 0;
}

static void rt_say(uint32_t n, const char *what, uint32_t bad, uint32_t at, uint16_t ok_c)
{
    char b[48], h[8];
    str_cpy(b, what, sizeof b);
    if (!bad) {
        str_cpy(b + str_len(b), " OK", sizeof b - str_len(b));
        rt_line(n, b, ok_c);
        return;
    }
    rt_hex(h, at);
    str_cpy(b + str_len(b), " BAD AT ", sizeof b - str_len(b));
    str_cpy(b + str_len(b), h, sizeof b - str_len(b));
    rt_line(n, b, C_LED);
}

/* over SPI (st_read, 0x0B): the number of bad words, the first one's address in *at */
static uint32_t rt_check_spi(uint32_t *at)
{
    static uint32_t buf[64];
    uint32_t off, i, bad = 0;
    for (off = ROMT_LO; off < ROMT_HI; off += sizeof buf) {
        if ((off & 0xFFFFu) == 0u) platform_poll();
        if (st_read(off, buf, sizeof buf)) { if (!bad++) *at = off; continue; }
        for (i = 0; i < 64u; i++)
            if (buf[i] != rt_word(off + 4u * i) && !bad++) *at = off + 4u * i;
    }
    return bad;
}

/* over the plain XIP window (fl_plain_window_init with FL_PLAIN_LO = ROMT_LO), as the emulation would read */
static uint32_t rt_check_xip(uint32_t *at)
{
    uint32_t off, bad = 0;
    for (off = ROMT_LO; off < ROMT_HI; off += 4u) {
        if ((off & 0xFFFFu) == 0u) platform_poll();
        if (fl_xip_word(off) != rt_word(off) && !bad++) *at = off;
    }
    return bad;
}

/* n random word reads in [lo, lo + len): microseconds (rt_sink keeps the reads) */
static volatile uint32_t rt_sink;
static uint32_t rt_time(uint32_t lo, uint32_t len, uint32_t n)
{
    uint32_t t = fm1_micros(), x = 0x2545F491u, s = 0, i;
    for (i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        s += fl_xip_word(lo + ((x >> 8) % (len >> 2)) * 4u);
    }
    rt_sink = s;
    return fm1_micros() - t;
}

static void romtest_main(void)
{
    static uint32_t page[64];
    char b[48], h[8], v[16];
    uint32_t off, i, first = 1, blank = 1, took, bad_s, bad_x, at_s = 0, at_x = 0, t_plain, t_enc, werr = 0;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    rt_line(0, "RDPNO ROM AREA TEST", C_ACC);
    rt_hex(h, ROMT_LO); str_cpy(b, h, sizeof b); str_cpy(b + str_len(b), " - ", sizeof b - str_len(b));
    rt_hex(h, ROMT_HI); str_cpy(b + str_len(b), h, sizeof b - str_len(b));
    rt_line(1, b, C_WHITE);
    if (!flash_ok) {
        rt_line(3, "FLASH CHIP UNKNOWN", C_LED);
        for (;;) platform_poll();
    }
    /* our pattern at the start of the area: a later start, check it; else write it */
    if (st_read(ROMT_LO, page, sizeof page) == 0)
        for (i = 0; i < 64u; i++) {
            first &= page[i] != rt_word(ROMT_LO + 4u * i);
            blank &= page[i] == 0xFFFFFFFFu;
        }
    if (first) {
        rt_line(2, blank ? "EMPTY: WRITING..." : "OTHER DATA: WRITING...", C_WHITE);
        for (off = ROMT_LO; off < ROMT_HI && !werr; off += 0x1000u) {
            uint32_t p;
            platform_poll();                    /* (the watchdog, the USB) */
            if (fl_erase4k(off, &took)) { werr = off | 1u; break; }
            for (p = off; p < off + 0x1000u; p += sizeof page) {
                for (i = 0; i < 64u; i++) page[i] = rt_word(p + 4u * i);
                if (fl_write(p, (const uint8_t *)page, sizeof page)) { werr = p | 2u; break; }
            }
            if ((off & 0xFFFFu) == 0u) {
                fmt_int(v, (int32_t)((off - ROMT_LO) * 100u / (ROMT_HI - ROMT_LO)));
                str_cpy(b, "WRITING ", sizeof b); str_cpy(b + str_len(b), v, sizeof b - str_len(b));
                str_cpy(b + str_len(b), " %", sizeof b - str_len(b));
                rt_line(2, b, C_WHITE);
            }
        }
        if (werr) {
            rt_hex(h, werr & ~3u);
            str_cpy(b, werr & 1u ? "ERASE FAILED AT " : "WRITE FAILED AT ", sizeof b);
            str_cpy(b + str_len(b), h, sizeof b - str_len(b));
            rt_line(2, b, C_LED);
            for (;;) platform_poll();
        }
        rt_line(2, "WRITTEN", C_WHITE);
    } else {
        rt_line(2, "PATTERN FOUND (NEXT START)", C_WHITE);
    }
    bad_s = rt_check_spi(&at_s);
    rt_say(3, "READ SPI", bad_s, at_s, C_ACC);
    fl_plain_window_init();
    bad_x = rt_check_xip(&at_x);
    rt_say(4, "READ XIP", bad_x, at_x, C_ACC);

    /* random reads in 64 KiB of the area (plain) and of the app's code (encrypted, from 0x4120), 64 Ki words each */
    t_plain = rt_time(ROMT_LO, 65536u, 65536u);
    t_enc = rt_time(0x4120u, 65536u, 65536u);
    fmt_int(v, (int32_t)t_plain); str_cpy(b, "64K RD AREA ", sizeof b); str_cpy(b + str_len(b), v, sizeof b - str_len(b));
    str_cpy(b + str_len(b), " US", sizeof b - str_len(b));
    rt_line(6, b, C_GRAY);
    fmt_int(v, (int32_t)t_enc); str_cpy(b, "64K RD CODE ", sizeof b); str_cpy(b + str_len(b), v, sizeof b - str_len(b));
    str_cpy(b + str_len(b), " US", sizeof b - str_len(b));
    rt_line(7, b, C_GRAY);

    if (bad_s || bad_x)
        rt_line(9, "ERROR: PLEASE REPORT", C_LED);
    else if (first) {
        rt_line(9, "NOW POWER OFF AND ON,", C_WHITE);
        rt_line(10, "THEN READ THIS AGAIN", C_WHITE);
    } else
        rt_line(9, "ROM AREA OK", C_ACC);
    for (;;)                                    /* (an update stays possible: the installer reaches the unit) */
        platform_poll();
}
