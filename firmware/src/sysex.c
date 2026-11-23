/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's own SysEx over USB MIDI (platform.c APP_SYSEX): the web pages' way in (site/romload.js; tools/fm1_romload.py).
 *
 * Frames: F0 7D 52 44 cmd body.. F7 (7D: non-commercial; "RD"). Commands from 0x10 end with the sum of cmd and body,
 * two 7-bit bytes, as SQ-80's loader (leonardolopezchavira/sq80-fm1) has it. Every frame is answered:
 *   F0 7D 52 44 7F cmd status echo0 echo1 [three numbers of 4 x 7 bits, or the answer's own bytes] F7
 *   01 hello     -> 7F 01 0 0 0, protocol 1, has a pack (0/1), its set (0..2), its patches, the ROM area's size
 *                   (4 x 7 bits; 0: the ROMs are built in), the pack's CRC-32 (5 x 7), the set's name (16 bytes),
 *                   what it can do (bit 0 load the ROMs: romload.c, bit 1 the editor: SX_EDITOR)
 *   10..12       loading the ROMs (romload.c, RDP_ROMS_IN_FLASH)
 *   20 get       -> 7F 20 0 echo, the settings: one byte each, in the editor's order (rdpno.c ed_get)
 *   22 set       pairs (setting, value): taken as the panel's knobs take them (rdpno.c ed_set)
 * status: 0 ok, 1 not understood (or a value out of range), 7 damaged (send again); romload.c has its own.
 *
 * Needs: ota_frame_done, ota_wire_send (platform.c); rdp_npatch, rdp_set_id, rdp_pack_crc, rdp_set_name (rdp.c). */

#define SX_HEAD0 0x7Du
#define SX_HEAD1 0x52u
#define SX_HEAD2 0x44u

static uint8_t sx_in[640];
#if RDP_ROMS_IN_FLASH                           /* (romload.c) */
static void rl_start(const uint8_t *p, uint32_t n, const uint8_t *echo);
static void rl_data(const uint8_t *p, uint32_t n, const uint8_t *echo);
static void rl_end(const uint8_t *p, uint32_t n, const uint8_t *echo);
#endif
#ifdef SX_EDITOR                                /* (rdpno.c) */
static void ed_get(const uint8_t *echo);
static void ed_set(const uint8_t *p, uint32_t n, const uint8_t *echo);
#endif

static uint32_t sx_u7(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0, i;
    for (i = 0; i < n; i++) v |= (uint32_t)(p[i] & 0x7Fu) << (7u * i);
    return v;
}
static void sx_put7(uint8_t *p, uint32_t v, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(v >> (7u * i) & 0x7Fu);
}
static void sx_head(uint8_t *f, uint32_t cmd, uint32_t status, const uint8_t *echo)
{
    f[0] = 0xF0; f[1] = SX_HEAD0; f[2] = SX_HEAD1; f[3] = SX_HEAD2; f[4] = 0x7F;
    f[5] = (uint8_t)cmd; f[6] = (uint8_t)status;
    f[7] = echo ? echo[0] : 0u; f[8] = echo ? echo[1] : 0u;
}
static void sx_reply(uint32_t cmd, uint32_t status, const uint8_t *echo, uint32_t a, uint32_t b, uint32_t c)
{
    uint8_t f[22];
    sx_head(f, cmd, status, echo);
    sx_put7(f + 9, a, 4); sx_put7(f + 13, b, 4); sx_put7(f + 17, c, 4);
    f[21] = 0xF7;
    ota_wire_send(f, sizeof f);
}
/* an answer with its own bytes (each 0..127) after the head */
static void sx_reply_bytes(uint32_t cmd, const uint8_t *echo, const uint8_t *d, uint32_t n)
{
    uint8_t f[64];
    uint32_t i;
    if (n > sizeof f - 10u) n = sizeof f - 10u;
    sx_head(f, cmd, 0, echo);
    for (i = 0; i < n; i++) f[9 + i] = d[i] & 0x7Fu;
    f[9 + n] = 0xF7;
    ota_wire_send(f, 10u + n);
}
static void sx_hello(void)
{
    uint8_t f[40];
    uint32_t i;
    sx_head(f, 0x01, 0, 0);
    f[9] = 1;                                   /* the protocol */
    f[10] = rdp_npatch ? 1u : 0u;
    f[11] = (uint8_t)(rdp_npatch ? rdp_set_id : 0u);
    f[12] = (uint8_t)rdp_npatch;
#if RDP_ROMS_IN_FLASH
    sx_put7(f + 13, ROMT_HI - ROMT_LO, 4);
#else
    sx_put7(f + 13, 0, 4);
#endif
    sx_put7(f + 17, rdp_npatch ? rdp_pack_crc : 0u, 5);
    for (i = 0; i < 16u; i++) f[22 + i] = (uint8_t)(rdp_npatch ? rdp_set_name[i] & 0x7F : 0);
    f[38] = (uint8_t)((RDP_ROMS_IN_FLASH ? 1u : 0u)
#ifdef SX_EDITOR
                      | 2u
#endif
                      );
    f[39] = 0xF7;
    ota_wire_send(f, sizeof f);
}

/* platform.c (APP_SYSEX): a received frame, the bytes between F0 and F7; 1 = ours (taken), 0 = someone else's */
static int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t i, s = 0, cmd;
    const uint8_t *echo;
    if (n < 4u || p[0] != SX_HEAD0 || p[1] != SX_HEAD1 || p[2] != SX_HEAD2 || n > sizeof sx_in)
        return 0;
    for (i = 0; i < n; i++) sx_in[i] = p[i];
    ota_frame_done();                           /* the next frame may come while this one is worked on */
    p = sx_in;
    cmd = p[3];
    if (cmd == 0x01u) { sx_hello(); return 1; }
    if (cmd < 0x10u || n < 6u) { sx_reply(cmd, 1, 0, 0, 0, 0); return 1; }
    for (i = 3; i < n - 2u; i++) s += p[i];
    echo = p + n - 2u;
    if (echo[0] != (s & 0x7Fu) || echo[1] != (s >> 7 & 0x7Fu)) { sx_reply(cmd, 7, echo, 0, 0, 0); return 1; }
    p += 4;
    n -= 6u;                                    /* the body */
    switch (cmd) {
#if RDP_ROMS_IN_FLASH
    case 0x10u: rl_start(p, n, echo); break;
    case 0x11u: rl_data(p, n, echo); break;
    case 0x12u: rl_end(p, n, echo); break;
#endif
#ifdef SX_EDITOR
    case 0x20u: ed_get(echo); break;
    case 0x22u: ed_set(p, n, echo); break;
#endif
    default: sx_reply(cmd, 1, echo, 0, 0, 0); break;
    }
    return 1;
}
