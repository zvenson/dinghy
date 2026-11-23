/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's ROM loader (RDP_ROMS_IN_FLASH: tools/build.py --romload): the load page (docs/load/, tools/fm1_romload.py)
 * sends a ROM pack (tools/pack_roms.py: its layout) over USB MIDI; it goes into the flash at [ROMT_LO, ROMT_HI),
 * below the VM, where the app slot leaves room (store.h; tested by romtest.c), and is read there through the plain
 * XIP window. Each firmware update rewrites that area: the ROMs are loaded again after it (the installer does it).
 *
 * Its frames (sysex.c: the framing, the answers, the hello):
 *   10 start  length (4 x 7)     the sound stops; the old pack is gone with the first data frame
 *   11 data   offset (4 x 7), then the bytes in groups of 7 with their high bits first (8 for 7), at most 448
 *             in order; a frame sent again (its answer lost) is checked against the flash, not written again
 *   12 end    CRC-32 (5 x 7)     the whole pack read back over SPI, its CRC and its layout (rdp_open) checked;
 *                                   then the FM-1 restarts with it
 * status: 0 ok, 1 a frame out of place, 2 the flash refused (an erase / a write), 3 the CRC differs, 4 no load
 * running, 5 a frame sent again differs from what was written, 6 the pack is not a ROM pack.
 *
 * The app supplies: rl_stop_sound() (the render off), rl_restart(); sysex.c: sx_reply, sx_u7; platform.c:
 * st_read, fl_erase4k, fl_write, fm1_ms. */

#define RL_CHUNK 448u                           /* bytes of the pack in one data frame (64 x 7) */
#define RL_AREA (ROMT_HI - ROMT_LO)

static struct {
    uint8_t active, failed, done, have;         /* a load runs; the last one failed; loaded; a pack at boot */
    uint32_t len, next, erased;                 /* the pack's length, bytes written, bytes erased (4 KiB steps) */
    uint32_t t_last;                            /* fm1_ms of the last frame */
} rl;
static uint8_t rl_buf[RL_CHUNK];

static void rl_stop_sound(void);
static void rl_restart(void);

/* the pack read back over SPI: its CRC-32 over [0, n) */
static uint32_t rl_crc_flash(uint32_t n)
{
    static uint8_t b[256];
    uint32_t c = 0xFFFFFFFFu, off, i, k;
    for (off = 0; off < n; off += sizeof b) {
        uint32_t m = n - off < sizeof b ? n - off : sizeof b;
        if (st_read(ROMT_LO + off, b, m)) return 0;
        for (i = 0; i < m; i++) {
            c ^= b[i];
            for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
    }
    return ~c;
}

static void rl_data(const uint8_t *p, uint32_t n, const uint8_t *echo)
{
    uint32_t off, m = 0, i, took;
    rl.t_last = fm1_ms;
    if (n < 4u) { sx_reply(0x11, 1, echo, 0, 0, 0); return; }
    off = sx_u7(p, 4);
    for (i = 4; i < n; i += 8u) {               /* 8 for 7: the high bits, then 7 bytes */
        uint32_t hi = p[i], k;
        for (k = 1; k < 8u && i + k < n; k++) {
            if (m >= RL_CHUNK) { sx_reply(0x11, 1, echo, off, m, 0); return; }
            rl_buf[m++] = (uint8_t)(p[i + k] | ((hi >> (k - 1u)) & 1u) << 7);
        }
    }
    if (!rl.active) { sx_reply(0x11, 4, echo, off, 0, 0); return; }
    if (!m || off > rl.len || m > rl.len - off) { sx_reply(0x11, 1, echo, off, m, rl.len); return; }
    if (off + m <= rl.next) {                   /* sent again: the same bytes are there already */
        static uint8_t got[RL_CHUNK];
        if (st_read(ROMT_LO + off, got, m)) { sx_reply(0x11, 2, echo, off, 0, 0); return; }
        for (i = 0; i < m; i++)
            if (got[i] != rl_buf[i]) { sx_reply(0x11, 5, echo, off + i, rl_buf[i], got[i]); return; }
        sx_reply(0x11, 0, echo, off, 0, 0);
        return;
    }
    if (off != rl.next) { sx_reply(0x11, 1, echo, off, rl.next, 0); return; }
    while (rl.erased < off + m) {               /* erase ahead of the write, a sector at a time */
        fm1_wdt_feed();
        if (fl_erase4k(ROMT_LO + rl.erased, &took)) { rl.failed = 1; rl.active = 0; sx_reply(0x11, 2, echo, rl.erased, 1, 0); return; }
        rl.erased += 0x1000u;
    }
    if (fl_write(ROMT_LO + off, rl_buf, m)) { rl.failed = 1; rl.active = 0; sx_reply(0x11, 2, echo, off, 2, 0); return; }
    rl.next = off + m;
    sx_reply(0x11, 0, echo, off, 0, 0);
}

static void rl_end(const uint8_t *p, uint32_t n, const uint8_t *echo)
{
    uint32_t want, got;
    rl.t_last = fm1_ms;
    if (!rl.active) { sx_reply(0x12, 4, echo, 0, 0, 0); return; }
    if (n < 5u || rl.next != rl.len) { sx_reply(0x12, 1, echo, rl.next, rl.len, 0); return; }
    want = sx_u7(p, 5);
    got = rl_crc_flash(rl.len);
    if (got != want) {
        rl.failed = 1; rl.active = 0;
        sx_reply(0x12, 3, echo, got & 0x0FFFFFFFu, got >> 28, rl.len);
        return;
    }
    fl_inval(ROMT_LO, RL_AREA);                 /* (the XIP side sees what was written) */
    if (rdp_open(fm1_xip_ptr(ROMT_LO), RL_AREA)) {
        const uint8_t *x = fm1_xip_ptr(ROMT_LO);
        rl.failed = 1; rl.active = 0;
        sx_reply(0x12, 6, echo, (uint32_t)x[0] | (uint32_t)x[1] << 8 | (uint32_t)x[2] << 16, x[3], rl.len);
        return;
    }
    sx_reply(0x12, 0, echo, rl.len, 0, 0);
    rl.active = 0;
    rl.done = 1;
    rl_restart();                               /* (does not return) */
}

/* sysex.c: 10 start (the body: the length) */
static void rl_start(const uint8_t *p, uint32_t n, const uint8_t *echo)
{
    uint32_t len = n >= 4u ? sx_u7(p, 4) : 0u;
    rl.t_last = fm1_ms;
    if (len < RDP_PACK_HDR || len > RL_AREA) { sx_reply(0x10, 1, echo, len, RL_AREA, 0); return; }
    if (!rl.active) rl_stop_sound();
    rdp_npatch = 0;                             /* (the old pack goes now) */
    rl.active = 1; rl.failed = 0; rl.done = 0;
    rl.len = len; rl.next = 0; rl.erased = 0;
    sx_reply(0x10, 0, echo, len, 0, 0);
}
