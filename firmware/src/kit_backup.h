/* SPDX-License-Identifier: GPL-3.0-only */
/* Backup, from OberVave 3.5 (asked for there by dmochen, zvenson/obervave#15): a page in the browser saves your
 * firmware's settings, blobs and slots to a file on the computer and writes them back later - after other firmwares
 * were on the FM-1 and may have used the same flash.
 *
 * Over USB-MIDI SysEx, F0 7D t0 t1 cmd ... F7 (7D = non-commercial; t0 t1 = BACKUP_TAG, Dinghy "DY", OberVave "OB":
 * give your fork its own two letters, so a page never writes one firmware's file into another; every request gets
 * one reply with the same cmd):
 *   0 HELLO                       -> 0, protocol (1), board (0 FM-1, 1 SMK-37), region count, per region: address
 *                                    (3 x 7 bit, LSB first), sectors (4 KiB each), then APP_NAME " " APP_VER, 0
 *   1 READ  addr(3) n(2, 1..256)  -> 1, addr(3), rc, pack7 data (rc: 0 ok, 1 not in a region, 2 flash)
 *   2 ERASE addr(3)               -> 2, addr(3), rc (one 4 KiB sector; the first one stops the instrument: RESTORE)
 *   3 WRITE addr(3) pack7 data    -> 3, addr(3), rc (<= 256 bytes, into an erased sector)
 *   4 DONE                        -> 4, 0, then the firmware starts again and reads what was written
 * pack7: groups of up to 7 bytes, each after one byte with their top bits (bit j = bit 7 of byte j), as zp12 / SLOOP.
 * Only the regions of store.h are reachable (and the platform's flash guard checks again). The data keeps its own
 * headers and CRCs: a file from another version is checked as it loads, as ever.
 *
 * Use: include it after platform.c, call backup_poll() each main-loop pass (after mirror_poll, before platform_poll):
 * while it returns 1 a restore is under way, do nothing else. BACKUP_STOP() (optional): silence everything, the
 * first erase came. */
#ifndef BACKUP_TAG0
#define BACKUP_TAG0 'D'
#define BACKUP_TAG1 'Y'
#endif
#ifndef BACKUP_STOP
#define BACKUP_STOP() ((void)0)
#endif

#if defined(DINGHY_HOST) && !defined(KIT_BACKUP_TEST)   /* the host keeps its storage in RAM: no flash to back up */
static int backup_poll(void) { return 0; }
#else
#define BK_HDR0 0x7Du
#define BK_HDR1 BACKUP_TAG0
#define BK_HDR2 BACKUP_TAG1

typedef struct { uint32_t at, sectors; } bk_region_t;
static const bk_region_t BK_REG[] = {
    {SETTINGS_AT, 1u},
    {BLOBS_AT, NBLOB},
#if BOARD_SMK37
    {OWNER_AT, 1u},                              /* (the owner tag: restored with the data, so the boot keeps it) */
#endif
    {SLOTS_AT, NSLOT * SLOT_SECT},
};
#define BK_NREG (sizeof BK_REG / sizeof BK_REG[0])

static uint8_t bk_lock;                          /* a restore began: the instrument stands still until DONE */
static uint8_t bk_drawn;
static uint8_t bk_buf[256];
static uint8_t bk_out[8 + 256 + 37 + 8];

static int bk_in_region(uint32_t at, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < BK_NREG; i++)
        if (at >= BK_REG[i].at && at + n <= BK_REG[i].at + BK_REG[i].sectors * 0x1000u && at + n >= at)
            return 1;
    return 0;
}

static uint32_t bk_pack7(uint8_t *o, const uint8_t *p, uint32_t n)
{
    uint32_t w = 0, i, j;
    for (i = 0; i < n; i += 7u) {
        uint32_t k = n - i < 7u ? n - i : 7u, hi = 0;
        for (j = 0; j < k; j++) hi |= (uint32_t)(p[i + j] >> 7) << j;
        o[w++] = (uint8_t)hi;
        for (j = 0; j < k; j++) o[w++] = p[i + j] & 0x7Fu;
    }
    return w;
}

static int bk_unpack7(uint8_t *o, uint32_t max, const uint8_t *p, uint32_t n)   /* the bytes, or -1 */
{
    uint32_t w = 0, i = 0, j;
    while (i < n) {
        uint32_t hi = p[i++], k = n - i < 7u ? n - i : 7u;
        for (j = 0; j < k; j++) {
            if (w >= max) return -1;
            o[w++] = (uint8_t)(p[i + j] | (hi >> j & 1u) << 7);
        }
        i += k;
    }
    return (int)w;
}

static uint32_t bk_addr(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14; }
static uint32_t bk_put_addr(uint8_t *o, uint32_t a)
{
    o[0] = a & 0x7Fu; o[1] = a >> 7 & 0x7Fu; o[2] = a >> 14 & 0x7Fu;
    return 3;
}

/* one request (the bytes between F0 and F7, header included) -> the reply in bk_out, its length (0: not ours) */
static uint32_t bk_handle(const uint8_t *p, uint32_t n, int *reboot)
{
    uint32_t w = 0, i, at, k;
    int rc;
    if (n < 4u || p[0] != BK_HDR0 || p[1] != BK_HDR1 || p[2] != BK_HDR2)
        return 0;
    bk_out[w++] = 0xF0; bk_out[w++] = BK_HDR0; bk_out[w++] = BK_HDR1; bk_out[w++] = BK_HDR2; bk_out[w++] = p[3];
    switch (p[3]) {
    case 0: {
        const char *v = APP_NAME " " APP_VER;
        bk_out[w++] = 1;
        bk_out[w++] = BOARD_SMK37 ? 1u : 0u;
        bk_out[w++] = (uint8_t)BK_NREG;
        for (i = 0; i < BK_NREG; i++) { w += bk_put_addr(bk_out + w, BK_REG[i].at); bk_out[w++] = (uint8_t)BK_REG[i].sectors; }
        for (i = 0; v[i] && i < 24u; i++) bk_out[w++] = (uint8_t)v[i] & 0x7Fu;
        bk_out[w++] = 0;
        break;
    }
    case 1:
        if (n != 9u) return 0;
        at = bk_addr(p + 4); k = (uint32_t)p[7] | (uint32_t)p[8] << 7;
        w += bk_put_addr(bk_out + w, at);
        if (k < 1u || k > 256u || !bk_in_region(at, k)) { bk_out[w++] = 1; break; }
        if (!flash_ok || st_read(at, bk_buf, k)) { bk_out[w++] = 2; break; }
        bk_out[w++] = 0;
        w += bk_pack7(bk_out + w, bk_buf, k);
        break;
    case 2:
        if (n != 7u) return 0;
        at = bk_addr(p + 4);
        w += bk_put_addr(bk_out + w, at);
        if ((at & 0xFFFu) || !bk_in_region(at, 0x1000u)) { bk_out[w++] = 1; break; }
        if (!bk_lock) { bk_lock = 1; bk_drawn = 0; BACKUP_STOP(); }
        fm1_wdt_feed();
        { uint32_t took; rc = flash_ok ? fl_erase4k(at, &took) : -1; }
        bk_out[w++] = rc ? 2u : 0u;
        break;
    case 3:
        if (n < 8u) return 0;
        at = bk_addr(p + 4);
        w += bk_put_addr(bk_out + w, at);
        rc = bk_unpack7(bk_buf, sizeof bk_buf, p + 7, n - 7u);
        if (rc < 1 || !bk_in_region(at, (uint32_t)rc) || !bk_lock) { bk_out[w++] = 1; break; }
        bk_out[w++] = flash_ok && !fl_write(at, bk_buf, (uint32_t)rc) ? 0u : 2u;
        break;
    case 4:
        bk_out[w++] = 0;
        *reboot = 1;
        break;
    default:
        return 0;
    }
    bk_out[w++] = 0xF7;
    return w;
}

#ifndef DINGHY_HOST
static void bk_restart(void)                     /* the reply out first, then the firmware starts afresh from the flash */
{
    uint32_t t0 = fm1_ms;
    while (so_w != so_r && fm1_ms - t0 < 500u) { fm1_wdt_feed(); platform_poll(); }
    fm1_delay_ms(50);
    fm1_reboot();
}
#else
static void bk_restart(void) {}
#endif

/* every main-loop pass, after mirror_poll, before platform_poll: 1 = a restore is under way (the loop does nothing else) */
static int backup_poll(void)
{
    const uint8_t *p;
    uint32_t n, w;
    int reboot = 0;
    if (ota_frame_get(&p, &n) && (w = bk_handle(p, n, &reboot)) != 0) {
        ota_frame_done();
#ifndef DINGHY_HOST
        if (usb.config) ota_wire_send(bk_out, w);
#endif
        if (reboot) bk_restart();
    }
    if (bk_lock && !bk_drawn) {
        bk_drawn = 1;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 84, 240, &FONT_L, "RESTORE", C_WHITE, 1);
        draw_text_box(0, 124, 240, &FONT_S, "from the backup page", RGB(160, 160, 160), 1);
        draw_text_box(0, 150, 240, &FONT_S, APP_NAME " starts again when done", RGB(110, 110, 110), 1);
    }
    return bk_lock;
}
#endif
