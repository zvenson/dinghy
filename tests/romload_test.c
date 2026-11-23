/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's ROM loader (firmware/src/romload.c) on a fake flash, fed the frames tools/fm1_romload.py makes:
 *   tools/fm1_romload.py frames PACK FRAMES && cc -Ifirmware/src -o romload_test tests/romload_test.c
 *   ./romload_test PACK FRAMES
 * A load as the page sends it (the pack in the flash, checked, the restart asked); frames sent twice; a frame out of
 * place, a damaged one, a wrong CRC, a pack that is not one; the hello before and after; nothing written outside
 * the area. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RDP_POOL
#define RDP_ROMS_IN_FLASH 1
#include "rdp.c"

#define ROMT_LO 0x0001A000u
#define ROMT_HI 0x00093000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
static uint8_t flash[0x100000];
static uint32_t fm1_ms, outside, erases, restarted, stopped;
static uint8_t reply[64];
static uint32_t reply_n;
static void fm1_wdt_feed(void) {}
static void ota_frame_done(void) {}
static int ota_wire_send(const uint8_t *p, uint32_t n) { memcpy(reply, p, n); reply_n = n; return 0; }
static int st_read(uint32_t off, void *d, uint32_t n) { memcpy(d, flash + off, n); return 0; }
static int fl_erase4k(uint32_t off, uint32_t *t)
{
    *t = 0;
    if (!FL_IN(off, 0x1000u, ROMT_LO, ROMT_HI) || off % 0x1000u) { outside++; return -1; }
    memset(flash + off, 0xFF, 0x1000);
    erases++;
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n)
{
    uint32_t i;
    if (!FL_IN(off, n, ROMT_LO, ROMT_HI)) { outside++; return -1; }
    for (i = 0; i < n; i++) flash[off + i] &= s[i];        /* NOR: only 1 -> 0 */
    return 0;
}
static void fl_inval(uint32_t off, uint32_t n) { (void)off; (void)n; }
static const uint8_t *fm1_xip_ptr(uint32_t off) { return flash + off; }
#include "romload.c"
static void rl_stop_sound(void) { stopped++; }
static void rl_restart(void) { restarted++; }

static uint8_t frames[1200][600];
static uint32_t flen[1200], nf;

static int feed(const uint8_t *f, uint32_t n)           /* one frame F0..F7; the answer's status, or -1: none */
{
    reply_n = 0;
    fm1_ms += 2;
    if (!rl_sysex(f + 1, n - 2)) return -2;
    if (reply_n < 9 || reply[0] != 0xF0 || reply[4] != 0x7F || reply[5] != f[4]) return -1;
    return reply[6];
}
static int hello(uint32_t *has, uint32_t *crc, char *name)
{
    static const uint8_t H[6] = {0xF0, 0x7D, 0x52, 0x44, 0x01, 0xF7};
    if (feed(H, 6) != 0 || reply_n != 39) return -1;
    *has = reply[10];
    *crc = rl_u7(reply + 17, 5);
    memcpy(name, reply + 22, 16);
    name[16] = 0;
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t pack[0x80000];
    FILE *f;
    char line[1300], name[17];
    uint32_t plen, i, has, crc, bad = 0;
    int r;
    if (argc < 3) { fprintf(stderr, "usage: romload_test PACK FRAMES\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    plen = (uint32_t)fread(pack, 1, sizeof pack, f);
    fclose(f);
    f = fopen(argv[2], "r");
    if (!f) return 2;
    while (nf < 1200 && fgets(line, sizeof line, f)) {
        uint32_t k;
        for (k = 0; line[2 * k] && line[2 * k] != '\n'; k++) {
            unsigned v;
            sscanf(line + 2 * k, "%2x", &v);
            frames[nf][k] = (uint8_t)v;
        }
        flen[nf++] = k;
    }
    fclose(f);
    memset(flash, 0xFF, sizeof flash);
    memset(flash + 0x4000, 0x5A, ROMT_LO - 0x4000);       /* the app below, the VM above: never touched */
    memset(flash + ROMT_HI, 0xA5, sizeof flash - ROMT_HI);

    bad |= hello(&has, &crc, name) || has;
    printf("hello before: pack %u\n", has);
    /* a data frame with no load running, then the load; every tenth frame sent twice (its answer lost) */
    bad |= feed(frames[1], flen[1]) != 4;
    for (i = 0; i < nf; i++) {
        r = feed(frames[i], flen[i]);
        if (r != 0) { printf("frame %u: status %d\n", i, r); bad = 1; break; }
        if (i && i + 1 < nf && i % 10 == 0 && feed(frames[i], flen[i]) != 0) { printf("frame %u again: bad\n", i); bad = 1; }
        if (i == 0) bad |= !stopped;
    }
    bad |= !restarted || memcmp(flash + ROMT_LO, pack, plen) || rdp_npatch == 0;
    bad |= hello(&has, &crc, name) || !has || crc != rdp_rd32(pack + 12);
    printf("loaded %u B in %u frames: restart %u, %u sectors erased, hello: pack %u \"%s\" crc %08x\n",
           plen, nf, restarted, erases, has, name, crc);

    /* the failures: out of place, damaged, a wrong CRC, a pack that is not one */
    bad |= feed(frames[0], flen[0]) != 0 || feed(frames[2], flen[2]) != 1;
    memcpy(line, frames[1], flen[1]);
    line[20] ^= 1;
    bad |= feed((uint8_t *)line, flen[1]) != 7;
    for (i = 1; i + 1 < nf; i++) bad |= feed(frames[i], flen[i]) != 0;
    flash[ROMT_LO + 1000] ^= 0x01;                       /* (a bit the flash got wrong) */
    r = feed(frames[nf - 1], flen[nf - 1]);
    printf("a flipped bit: status %d\n", r);
    bad |= r != 3 || rl.active;
    {                                                   /* a pack whose CRC is right but is not a ROM pack */
        static uint8_t junk[600];
        uint32_t k, s;
        memcpy(junk, (const uint8_t[]){0xF0, 0x7D, 0x52, 0x44, 0x10, 0, 8, 0, 0}, 9);   /* 1024 bytes */
        s = 0x10 + 8;
        junk[9] = (uint8_t)(s & 0x7F); junk[10] = (uint8_t)(s >> 7); junk[11] = 0xF7;
        bad |= feed(junk, 12) != 0;
        for (k = 0; k < 1024; k += 448) {
            uint32_t m = 1024 - k < 448 ? 1024 - k : 448, j, o = 0;
            uint8_t data[448];
            memset(data, 0x11, sizeof data);
            junk[o++] = 0xF0; junk[o++] = 0x7D; junk[o++] = 0x52; junk[o++] = 0x44; junk[o++] = 0x11;
            for (j = 0; j < 4; j++) junk[o++] = (uint8_t)(k >> (7 * j) & 0x7F);
            for (j = 0; j < m; j += 7) {
                uint32_t q, hi = 0;
                for (q = 0; q < 7 && j + q < m; q++) hi |= (uint32_t)(data[j + q] >> 7) << q;
                junk[o++] = (uint8_t)hi;
                for (q = 0; q < 7 && j + q < m; q++) junk[o++] = data[j + q] & 0x7F;
            }
            for (s = 0, j = 4; j < o; j++) s += junk[j];
            junk[o++] = (uint8_t)(s & 0x7F); junk[o++] = (uint8_t)(s >> 7 & 0x7F); junk[o++] = 0xF7;
            bad |= feed(junk, o) != 0;
        }
        crc = rl_crc_flash(1024);
        {
            uint8_t e[13] = {0xF0, 0x7D, 0x52, 0x44, 0x12};
            uint32_t j;
            for (j = 0; j < 5; j++) e[5 + j] = (uint8_t)(crc >> (7 * j) & 0x7F);
            for (s = 0, j = 4; j < 10; j++) s += e[j];
            e[10] = (uint8_t)(s & 0x7F); e[11] = (uint8_t)(s >> 7 & 0x7F); e[12] = 0xF7;
            r = feed(e, 13);
        }
        printf("not a pack: status %d\n", r);
        bad |= r != 6 || rdp_npatch;
    }
    bad |= hello(&has, &crc, name) || has;
    for (i = 0x4000; i < ROMT_LO; i++) bad |= flash[i] != 0x5A;
    for (i = ROMT_HI; i < sizeof flash; i++) bad |= flash[i] != 0xA5;
    bad |= outside != 0;
    printf("outside the area: %u calls; %s\n", outside, bad ? "FAIL" : "OK");
    return bad;
}
