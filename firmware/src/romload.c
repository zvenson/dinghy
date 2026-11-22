/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's ROM loader (RDP_ROMS_IN_FLASH: tools/build.py --romload): the load page (docs/load/, tools/fm1_romload.py)
 * sends a ROM pack (tools/pack_roms.py: its layout) over USB MIDI; it goes into the flash at [ROMT_LO, ROMT_HI),
 * below the VM, where the app slot leaves room (store.h; tested by romtest.c), and is read there through the plain
 * XIP window. Each firmware update rewrites that area: the ROMs are loaded again after it (the installer does it).
 *
 * Frames: F0 7D 52 44 cmd body.. F7 (7D: non-commercial; "RD"). 0x10..0x12 end with the sum of cmd and body, two
 * 7-bit bytes, as SQ-80's loader (leonardolopezchavira/sq80-fm1) has it. Every frame is answered:
 *   F0 7D 52 44 7F cmd status echo0 echo1 [three numbers of 4 x 7 bits] F7
 *   01 hello                     -> 7F 01 0 0 0, protocol 1, has a pack (0/1), its set (0..2), its patches,
 *                                   the area's size (4 x 7 bits), the pack's CRC-32 (5 x 7), the set's name (16)
 *   10 start  length (4 x 7)     the sound stops; the old pack is gone with the first data frame
 *   11 data   offset (4 x 7), then the bytes in groups of 7 with their high bits first (8 for 7), at most 448
 *             in order; a frame sent again (its answer lost) is checked against the flash, not written again
 *   12 end    CRC-32 (5 x 7)     the whole pack read back over SPI, its CRC and its layout (rdp_open) checked;
 *                                   then the FM-1 restarts with it
 * status: 0 ok, 1 a frame out of place, 2 the flash refused (an erase / a write), 3 the CRC differs, 4 no load
 * running, 5 a frame sent again differs from what was written, 6 the pack is not a ROM pack, 7 damaged (send again).
 *
 * The app supplies: rl_stop_sound() (the render off), rl_restart(); and from platform.c: ota_frame_done,
 * ota_wire_send, st_read, fl_erase4k, fl_write, fm1_ms. */

#define RL_HEAD0 0x7Du
#define RL_HEAD1 0x52u
#define RL_HEAD2 0x44u
#define RL_CHUNK 448u                           /* bytes of the pack in one data frame (64 x 7) */
#define RL_AREA (ROMT_HI - ROMT_LO)

static struct {
    uint8_t active, failed, done, have;         /* a load runs; the last one failed; loaded; a pack at boot */
    uint32_t len, next, erased;                 /* the pack's length, bytes written, bytes erased (4 KiB steps) */
    uint32_t t_last;                            /* fm1_ms of the last frame */
} rl;
static uint8_t rl_in[640], rl_buf[RL_CHUNK];

static void rl_stop_sound(void);
static void rl_restart(void);

static uint32_t rl_u7(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0, i;
    for (i = 0; i < n; i++) v |= (uint32_t)(p[i] & 0x7Fu) << (7u * i);
    return v;
}
static void rl_put7(uint8_t *p, uint32_t v, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(v >> (7u * i) & 0x7Fu);
}
static void rl_reply(uint32_t cmd, uint32_t status, const uint8_t *echo, uint32_t a, uint32_t b, uint32_t c)
{
    uint8_t f[22];
    f[0] = 0xF0; f[1] = RL_HEAD0; f[2] = RL_HEAD1; f[3] = RL_HEAD2; f[4] = 0x7F;
    f[5] = (uint8_t)cmd; f[6] = (uint8_t)status;
    f[7] = echo ? echo[0] : 0u; f[8] = echo ? echo[1] : 0u;
    rl_put7(f + 9, a, 4); rl_put7(f + 13, b, 4); rl_put7(f + 17, c, 4);
    f[21] = 0xF7;
    ota_wire_send(f, sizeof f);
}
static void rl_hello(void)
{
    uint8_t f[39];
    uint32_t i;
    f[0] = 0xF0; f[1] = RL_HEAD0; f[2] = RL_HEAD1; f[3] = RL_HEAD2; f[4] = 0x7F; f[5] = 0x01;
    f[6] = 0; f[7] = 0; f[8] = 0;
    f[9] = 1;                                   /* the protocol */
    f[10] = rdp_npatch ? 1u : 0u;
    f[11] = (uint8_t)(rdp_npatch ? rdp_set_id : 0u);
    f[12] = (uint8_t)rdp_npatch;
    rl_put7(f + 13, RL_AREA, 4);
    rl_put7(f + 17, rdp_npatch ? rdp_pack_crc : 0u, 5);
    for (i = 0; i < 16u; i++) f[22 + i] = (uint8_t)(rdp_npatch ? rdp_set_name[i] & 0x7F : 0);
    f[38] = 0xF7;
    ota_wire_send(f, sizeof f);
}

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
    if (n < 4u) { rl_reply(0x11, 1, echo, 0, 0, 0); return; }
    off = rl_u7(p, 4);
    for (i = 4; i < n; i += 8u) {               /* 8 for 7: the high bits, then 7 bytes */
        uint32_t hi = p[i], k;
        for (k = 1; k < 8u && i + k < n; k++) {
            if (m >= RL_CHUNK) { rl_reply(0x11, 1, echo, off, m, 0); return; }
            rl_buf[m++] = (uint8_t)(p[i + k] | ((hi >> (k - 1u)) & 1u) << 7);
        }
    }
    if (!rl.active) { rl_reply(0x11, 4, echo, off, 0, 0); return; }
    if (!m || off > rl.len || m > rl.len - off) { rl_reply(0x11, 1, echo, off, m, rl.len); return; }
    if (off + m <= rl.next) {                   /* sent again: the same bytes are there already */
        static uint8_t got[RL_CHUNK];
        if (st_read(ROMT_LO + off, got, m)) { rl_reply(0x11, 2, echo, off, 0, 0); return; }
        for (i = 0; i < m; i++)
            if (got[i] != rl_buf[i]) { rl_reply(0x11, 5, echo, off + i, rl_buf[i], got[i]); return; }
        rl_reply(0x11, 0, echo, off, 0, 0);
        return;
    }
    if (off != rl.next) { rl_reply(0x11, 1, echo, off, rl.next, 0); return; }
    while (rl.erased < off + m) {               /* erase ahead of the write, a sector at a time */
        fm1_wdt_feed();
        if (fl_erase4k(ROMT_LO + rl.erased, &took)) { rl.failed = 1; rl.active = 0; rl_reply(0x11, 2, echo, rl.erased, 1, 0); return; }
        rl.erased += 0x1000u;
    }
    if (fl_write(ROMT_LO + off, rl_buf, m)) { rl.failed = 1; rl.active = 0; rl_reply(0x11, 2, echo, off, 2, 0); return; }
    rl.next = off + m;
    rl_reply(0x11, 0, echo, off, 0, 0);
}

static void rl_end(const uint8_t *p, uint32_t n, const uint8_t *echo)
{
    uint32_t want, got;
    if (!rl.active) { rl_reply(0x12, 4, echo, 0, 0, 0); return; }
    if (n < 5u || rl.next != rl.len) { rl_reply(0x12, 1, echo, rl.next, rl.len, 0); return; }
    want = rl_u7(p, 5);
    got = rl_crc_flash(rl.len);
    if (got != want) {
        rl.failed = 1; rl.active = 0;
        rl_reply(0x12, 3, echo, got & 0x0FFFFFFFu, got >> 28, rl.len);
        return;
    }
    fl_inval(ROMT_LO, RL_AREA);                 /* (the XIP side sees what was written) */
    if (rdp_open(fm1_xip_ptr(ROMT_LO), RL_AREA)) {
        const uint8_t *x = fm1_xip_ptr(ROMT_LO);
        rl.failed = 1; rl.active = 0;
        rl_reply(0x12, 6, echo, (uint32_t)x[0] | (uint32_t)x[1] << 8 | (uint32_t)x[2] << 16, x[3], rl.len);
        return;
    }
    rl_reply(0x12, 0, echo, rl.len, 0, 0);
    rl.active = 0;
    rl.done = 1;
    rl_restart();                               /* (does not return) */
}

/* platform.c (APP_SYSEX): a received frame, the bytes between F0 and F7; 1 = ours (taken), 0 = someone else's */
static int rl_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t i, s = 0, cmd;
    const uint8_t *echo;
    if (n < 4u || p[0] != RL_HEAD0 || p[1] != RL_HEAD1 || p[2] != RL_HEAD2 || n > sizeof rl_in)
        return 0;
    for (i = 0; i < n; i++) rl_in[i] = p[i];
    ota_frame_done();                           /* the next frame may come while this one is worked on */
    p = rl_in;
    cmd = p[3];
    rl.t_last = fm1_ms;
    if (cmd == 0x01u) { rl_hello(); return 1; }
    if (cmd < 0x10u || cmd > 0x12u || n < 6u) { rl_reply(cmd, 1, 0, 0, 0, 0); return 1; }
    for (i = 3; i < n - 2u; i++) s += p[i];
    echo = p + n - 2u;
    if (echo[0] != (s & 0x7Fu) || echo[1] != (s >> 7 & 0x7Fu)) { rl_reply(cmd, 7, echo, 0, 0, 0); return 1; }
    p += 4;
    n -= 6u;                                    /* the body */
    if (cmd == 0x10u) {
        uint32_t len = n >= 4u ? rl_u7(p, 4) : 0u;
        if (len < RDP_PACK_HDR || len > RL_AREA) { rl_reply(cmd, 1, echo, len, RL_AREA, 0); return 1; }
        if (!rl.active) rl_stop_sound();
        rdp_npatch = 0;                         /* (the old pack goes now) */
        rl.active = 1; rl.failed = 0; rl.done = 0;
        rl.len = len; rl.next = 0; rl.erased = 0;
        rl_reply(cmd, 0, echo, len, 0, 0);
    } else if (cmd == 0x11u) {
        rl_data(p, n, echo);
    } else {
        rl_end(p, n, echo);
    }
    return 1;
}
