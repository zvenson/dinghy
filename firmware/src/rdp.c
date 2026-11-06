/* SPDX-License-Identifier: GPL-3.0-only */
/* rdp.c: the CPU-B board of Roland's SA pianos (MKS-20, RD-1000, Rhodes MK-80) on the FM-1.
 *
 * A port of librdpiano (giulioz/rdpiano, Copyright (C) Giulio Zausa and contributors, GPL-3.0) to C for the FM-1:
 * no heap, no float, no C++, the ROMs read straight from flash (tools/pack_roms.py -> firmware/gen/rdp_rom.h).
 * The HD6301 core is MAME's m6800 (license BSD-3-Clause, copyright-holders Aaron Giles, Nicola Salmoria), as
 * rdpiano carries it; its opcode handlers below are rdpiano's mcu_ops.h, mechanically turned into C functions.
 *
 * What changed against librdpiano, and why:
 *   - the wave ROMs: not 1.1 MB of unpacked tables in RAM but 3 bytes an address in flash (pack_roms.py packs
 *     them exactly as SoundChip::load_samples unpacks them; tests/rdp_compare.sh checks it);
 *   - the exponential tables: folded to 8 KB + 2 KB (rdp_tables.h); the phase step is worked out when the CPU
 *     writes a pitch, not looked up every sample;
 *   - the parameter ROM: only its 32 KB window of the current patch, LZ4 in flash, unpacked into the pool;
 *   - the CPU's RAM: the 4 KB the board has (0x0000..0x0FFF), not 64 KB;
 *   - the command queue: a ring the main loop fills (the only writer) and the audio interrupt empties;
 *   - the timing (rdp_exact): librdpiano runs 100 *instructions* per 20 kHz sample, about four times the real
 *     CPU's 2 MHz. rdp_exact = 1 runs 100 (32 kHz: 62) *cycles* instead, as the board does; 0 = librdpiano's way,
 *     bit for bit (tests/rdp_compare.sh).
 *
 *   rdp_init()            once, in the main loop, before the sound starts: reset, the handshake, patch 0
 *   rdp_select(i)         main loop: unpack patch i's window into the spare buffer; the render takes it over
 *   rdp_take()            audio interrupt, before the block: switch to a patch rdp_select has unpacked
 *   rdp_poll()            main loop: once rdp_take has switched, the board's reload commands (0x31 0x30)
 *   rdp_midi(st, d1, d2)  main loop: note on/off, sustain (CC 64), program change, as the board understands them
 *   rdp_render()          audio interrupt: one sample at RDP_PATCH[i].khz (20 or 32 kHz), about +-2^17 */
#include <stdint.h>
#include "rdp_tables.h"
#include "rdp_rom.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef union {
    struct { u8 l, h, h2, h3; } b;
    struct { u16 l, h; } w;
    struct { s8 l, h, h2, h3; } sb;
    struct { s16 l, h; } sw;
    u32 d;
    s32 sd;
} PAIR;                                         /* (little endian: the FM-1 and the host) */

#ifndef RDP_LOG
#define RDP_LOG(...) ((void)0)
#endif
#ifndef RDP_POOL
#define RDP_POOL __attribute__((section(".pool")))
#endif
#ifndef RDP_FAST                                /* code the audio interrupt runs: in RAM (app.ld's .fast), not */
#define RDP_FAST __attribute__((section(".fast")))   /* from the flash, whose 32 KB cache it would thrash */
#endif

/* ---- the SA sound chip (librdpiano SoundChip)
 * The same arithmetic, bit for bit, arranged for a small CPU: what depends only on what the board wrote (the
 * envelope's speed and target, the loop, the wave bank, the pitch) is worked out at the write, and only the parts
 * that sound are visited (sa_live). tests/rdp_compare.sh holds it to librdpiano's samples. */
#define SA_NV 16u                               /* voices */
#define SA_NP 10u                               /* parts a voice sounds (of 16 it has registers for) */
typedef struct {
    u32 sub_phase, env_value;                   /* the state the chip keeps */
    u32 step;                                   /* IC11: the phase step of pitch */
    u32 env_add, dest20, dest_c, loop_c, high11;   /* from the writes: see sa_write */
    u16 pitch;
    u8 loop, high, env_dest, env_speed, env_offset, f0, f1;
    u8 en, inv, ci, pitch_hi;
} sa_part_t;
static sa_part_t sa[SA_NV][16];
/* (words, not halfwords, and the voice mask in two: the even voices are core 0's, the odd ones core 1's, and
 * neither writes a word the other does) */
static u32 sa_live[SA_NV];                      /* bit p: part p sounds (not: env_value == 0 and env_dest == 0) */
static u32 sa_zero[SA_NV];                      /* bit p: fell silent last sample: its sub phase goes to 0 */
static u32 sa_vm[2];                            /* bit v: voice v has a part in sa_live or sa_zero; [v & 1] */
static RDP_FAST void sa_vmark(u32 vi)
{
    if (sa_live[vi] | sa_zero[vi]) sa_vm[vi & 1u] |= 1u << vi;
    else sa_vm[vi & 1u] &= ~(1u << vi);
}
static u8 sa_irq_id, sa_irq;
static volatile u32 rdp_active;                 /* parts that sounded in the last sample (the screen shows it) */

static const u32 SA_ENV[256] = {               /* librdpiano env_table: the envelope's speed (decimal: build.py
                                                * takes some hex numbers here for register addresses) */
    0, 35, 38, 41, 45, 49, 54, 59, 64, 70, 76, 82,
    90, 98, 108, 118, 128, 140, 152, 164, 180, 196, 216, 236,
    260, 284, 308, 332, 364, 396, 436, 476, 512, 560, 608, 656,
    720, 784, 864, 944, 1024, 1120, 1216, 1312, 1440, 1568, 1728, 1888,
    2048, 2240, 2432, 2624, 2880, 3136, 3456, 3776, 4096, 4480, 4864, 5248,
    5760, 6272, 6912, 7552, 8192, 8960, 9728, 10496, 11520, 12544, 13824, 15104,
    16384, 17920, 19456, 20992, 23040, 25088, 27648, 30208, 32768, 35840, 38912, 41984,
    46080, 50176, 55296, 60416, 65536, 71680, 77824, 83968, 92160, 100352, 110592, 120832,
    131072, 143360, 155648, 167936, 184320, 200704, 221184, 241664, 262144, 286720, 311296, 335872,
    368640, 401408, 442368, 483328, 524288, 573440, 622592, 671744, 737280, 802816, 884736, 966656,
    1048576, 1146880, 1245184, 1343488, 1474560, 1605632, 1769472, 1933312, 0, 2097116, 2097113, 2097110,
    2097106, 2097102, 2097097, 2097092, 2097087, 2097081, 2097075, 2097069, 2097061, 2097053, 2097043, 2097033,
    2097023, 2097011, 2096999, 2096987, 2096971, 2096955, 2096935, 2096915, 2096891, 2096867, 2096843, 2096819,
    2096787, 2096755, 2096715, 2096675, 2096639, 2096591, 2096543, 2096495, 2096431, 2096367, 2096287, 2096207,
    2096127, 2096031, 2095935, 2095839, 2095711, 2095583, 2095423, 2095263, 2095103, 2094911, 2094719, 2094527,
    2094271, 2094015, 2093695, 2093375, 2093055, 2092671, 2092287, 2091903, 2091391, 2090879, 2090239, 2089599,
    2088959, 2088191, 2087423, 2086655, 2085631, 2084607, 2083327, 2082047, 2080767, 2079231, 2077695, 2076159,
    2074111, 2072063, 2069503, 2066943, 2064383, 2061311, 2058239, 2055167, 2051071, 2046975, 2041855, 2036735,
    2031615, 2025471, 2019327, 2013183, 2004991, 1996799, 1986559, 1976319, 1966079, 1953791, 1941503, 1929215,
    1912831, 1896447, 1875967, 1855487, 1835007, 1810431, 1785855, 1761279, 1728511, 1695743, 1654783, 1613823,
    1572863, 1523711, 1474559, 1425407, 1359871, 1294335, 1212415, 1130495, 1048575, 950271, 851967, 753663,
    622591, 491519, 327679, 163839,
};
static const u16 SA_ADDR[16] = {0x1e0, 0x080, 0x060, 0x04d, 0x040, 0x036, 0x02d, 0x026,   /* bits 5..8 of the sub phase */
                                0x020, 0x01b, 0x016, 0x011, 0x00d, 0x00a, 0x006, 0x003};

static RDP_FAST u32 sa_step(u32 pitch)                   /* IC11: librdpiano phase_exp_table[pitch] */
{
    u32 oct = pitch >> 12;
    return oct < 12u ? (262144u + ((u32)RDP_PHASE13[pitch & 4095u] << 5)) >> (11u - oct) : 0u;
}

static RDP_FAST s32 sa_exp(u32 t, u32 neg)               /* IC10: a 14-bit level -> a sample; librdpiano samples_exp_table */
{
    s32 v = (s32)(RDP_EXP[t & 1023u] >> (t >> 10));
    return neg ? ~v : v;
}

static RDP_FAST void sa_write(u32 offset, u8 data)
{
    u32 vi = offset >> 8, pi = (offset >> 4) & 15u, field = offset & 7u;
    sa_part_t *p;
    if (vi >= SA_NV) { RDP_LOG("rdp: SA write %03x\n", offset); return; }
    p = &sa[vi][pi];
    switch (field) {
    case 0: case 1:
        p->pitch = (u16)(field ? (p->pitch & 0xFF00u) | data : (p->pitch & 0x00FFu) | (u32)data << 8);
        p->step = sa_step(p->pitch);
        p->pitch_hi = ((p->pitch >> 14) & 3u) == 3u;
        break;
    case 2: p->loop = data; p->loop_c = (~(u32)data & 0xFFu) + 1u; break;
    case 3: p->high = data; p->high11 = (u32)data << 11; break;
    case 4:
        p->env_dest = data;
        p->dest20 = (u32)data << 20;
        p->dest_c = (~(u32)data & 0xFFu) + 1u;
        if (pi < SA_NP) {
            if (p->env_value || data) sa_live[vi] |= 1u << pi;
            else sa_live[vi] &= ~(1u << pi);
            sa_vmark(vi);
        }
        break;
    case 5:
        p->env_speed = data;
        p->en = (data & 0x7Fu) != 0;
        p->inv = (data >> 7) & 1u;
        p->ci = p->en && p->inv;
        p->env_add = SA_ENV[data] | (p->ci ? 0x7Fu << 21 : 0u);
        break;
    case 6: sa[vi][0].f0 = data & 1u; sa[vi][0].f1 = (data >> 1) & 1u; break;   /* the flags: one for all parts */
    default: p->env_offset = data; break;
    }
}

static const u8 SA_LOW[1024] = {                /* the lowest set bit of a 10-bit mask (the next part to do) */
    0,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    8,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    9,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    8,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
    5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,
};
typedef struct { s32 result; u32 active, irq, irq_id; u32 pad[12]; } sa_out_t;   /* (a cache line each) */
static sa_out_t sa_out[2] __attribute__((aligned(64)));

static RDP_FAST void sa_half(u32 c)             /* one sample of the even (c = 0) or the odd voices (c = 1) */
{
    s32 result = 0;
    u32 vi, active = 0, vm, prior = sa_irq, irq = 0, irq_id = 0;
    for (vi = c, vm = sa_vm[c] >> c; vm; vi += 2u, vm >>= 2) {   /* the voices with something to do */
        u32 live, m0, m1, gone = 0, todo;
        if (!(vm & 1u)) continue;
        live = sa_live[vi];
        if (sa_zero[vi]) {                      /* librdpiano zeroes a silent part's sub phase when it skips it */
            u32 z = sa_zero[vi] & ~live, pi;
            for (pi = 0; z; pi++, z >>= 1)
                if (z & 1u) sa[vi][pi].sub_phase = 0;
            sa_zero[vi] = 0;
            sa_vmark(vi);
        }
        m0 = 0u - (u32)sa[vi][0].f0;            /* the voice's flags as masks: no branches below */
        m1 = 0u - (u32)sa[vi][0].f1;
        for (todo = live; todo; todo &= todo - 1u) {
            u32 pi = SA_LOW[todo];
            sa_part_t *p = &sa[vi][pi];
            u32 volume, wa, sp, a1a, a, t1, t2, end, env, w0, w1, pa, pb, sel, ph, x;
            active++;
            /* IC19: the envelope */
            a1a = (p->env_value & m0) | ((1u << 25) & ~m0);
            x = 1u + (a1a >> 20) + p->env_offset;                   /* <= 511: bit 8 is the carry */
            volume = (((a1a >> 14) & 0x3Fu) & m0) | ((x & 0xFu) << 6) | (((x & 0xF0u) << 6) & (0u - ((x >> 8) & 1u)));
            volume = ~volume & 0x3FFFu;
            a = a1a + p->env_add + p->ci;                           /* < 2^29: bit 28 is the carry */
            end = p->en & ((((a >> 28) & 1u) ^ p->inv) | (p->inv ^ ((((a & 0xFFFFFFFu) >> 20) + p->dest_c) >> 8)));
            if (end && !prior && !irq) {        /* the first part to end (sa_update: of both halves) */
                irq_id = pi | vi << 4;
                irq = 1;
            }
            env = (p->dest20 & (0u - end)) | (a & 0xFFFFFFFu & (end - 1u));
            p->env_value = env;
            /* IC9: the phase and the wave address */
            a = (p->step + p->sub_phase) & 0xFFFFFFu;
            x = (a >> 16) + p->loop_c;                              /* <= 511: bit 8 is the carry */
            ph = ((x & 0xFFu) & (0u - (x >> 8))) | ((a >> 16) & ((x >> 8) - 1u));
            sp = ((a & 0xFFFFu) | ph << 16) & m1;
            p->sub_phase = sp;
            wa = p->high11 | ((sp >> 9) & 0x7FFu);
            /* IC8: the wave ROM, the level, the two exponentials */
            {
                const u8 *w = &RDP_WAVE[3u * (wa & 0x1FFFFu)];    /* (the board's program keeps it below) */
                w0 = w[0]; w1 = w[1];
                sel = ((wa >> 11) & 63u) > 4u;
                pa = (w0 | (w1 & 0x3Fu) << 8) | sel;
                pb = ((u32)w[2] << 1 | (w0 & 1u)) | (sel ^ 1u);
            }
            volume |= (0xFu << 10) & (0u - (p->pitch_hi | (((sp >> 20) & 15u) != 0) | (~m1 & 1u)));
            a = volume + pa;                                        /* bit 14: the carry */
            t1 = (a & 0x3FFFu) | (0x3C00u & (0u - (a >> 14)));
            a = SA_ADDR[(sp >> 5) & 15u] + (pb & 0x1FFu);           /* bit 9: the carry */
            a = (a & 0x1FFu) | (0x1E0u & (0u - (a >> 9)));
            a = volume + (a << 5);
            t2 = (a & 0x3FFFu) | (0x3C00u & (0u - (a >> 14)));
            {
                s32 e1 = (s32)(RDP_EXP[t1 & 1023u] >> (t1 >> 10)) ^ (s32)(0u - ((w1 >> 6) & 1u));
                s32 e2 = (s32)(RDP_EXP[t2 & 1023u] >> (t2 >> 10)) ^ (s32)(0u - (w1 >> 7));
                result += (e1 + e2) & (s32)(0u - (u32)(env != 0));   /* (librdpiano: no sound at level 0) */
            }
            gone |= (u32)(env == 0 && p->env_dest == 0) << pi;     /* silent from the next sample on */
        }
        if (gone) {
            sa_live[vi] = live & ~gone;
            sa_zero[vi] = gone;
            sa_vmark(vi);
        }
    }
    sa_out[c].result = result;
    sa_out[c].active = active;
    sa_out[c].irq = irq;
    sa_out[c].irq_id = irq_id;
}

static RDP_FAST void sa_job(uint32_t c) { sa_half(c); }   /* (core 1's job) */

/* the split: RDP_SPLIT_RUN(fn, arg) hands fn(arg) to the second core (1 = taken), RDP_SPLIT_WAIT() waits for it
 * (0 = done). The app defines them (rdpno.c: platform.c's cpu1_run, cpu1_wait); without, one core does both */
#ifndef RDP_SPLIT_RUN
#define RDP_SPLIT_RUN(fn, arg) 0
#define RDP_SPLIT_WAIT() 0
#endif
static volatile u32 rdp_split_on = 1;           /* (the app may switch it off) */
static volatile u32 rdp_split_used;             /* the last sample had core 1's help */

static RDP_FAST s32 sa_update(void)             /* one sample of all voices (SoundChip::update) */
{
    u32 split = rdp_split_on && sa_vm[1] && RDP_SPLIT_RUN(sa_job, 1u);
    sa_half(0);
    if (!split) sa_half(1);
    else if (RDP_SPLIT_WAIT() != 0) sa_out[1].result = 0, sa_out[1].active = 0, sa_out[1].irq = 0;   /* (given up) */
    rdp_split_used = split;
    if (!sa_irq && (sa_out[0].irq | sa_out[1].irq)) {   /* the first part in (voice, part) order, as one loop has it */
        u32 id = sa_out[0].irq ? sa_out[0].irq_id : sa_out[1].irq_id;
        if (sa_out[0].irq && sa_out[1].irq && sa_out[1].irq_id < id) id = sa_out[1].irq_id;
        sa_irq_id = (u8)id;
        sa_irq = 1;
    }
    rdp_active = sa_out[0].active + sa_out[1].active;
    return sa_out[0].result + sa_out[1].result;
}

/* ---- the parameter ROM: the current patch's 32 KB window (unpacked into the pool), and its 3-byte header */
static u8 rdp_win[2][0x8000] RDP_POOL;
static const u8 *rdp_params = rdp_win[0];       /* the render's (the audio interrupt's) */
static u8 rdp_hdr[3];
static u32 rdp_khz32;                           /* the patch runs at 32 kHz (62 cycles a sample) */

static u32 lz4_unpack(const u8 *s, u32 n, u8 *d, u32 cap)   /* an LZ4 block (tools/lz4blk.py) -> its length */
{
    const u8 *e = s + n;
    u32 o = 0;
    while (s < e) {
        u32 tok = *s++, lit = tok >> 4, ml = tok & 15u, off, k;
        if (lit == 15u) { u32 b; do { b = *s++; lit += b; } while (b == 255u && s < e); }
        if (o + lit > cap) return 0;
        for (k = 0; k < lit; k++) d[o++] = *s++;
        if (s >= e) break;                      /* the last sequence: literals only */
        off = s[0] | (u32)s[1] << 8;
        s += 2;
        if (ml == 15u) { u32 b; do { b = *s++; ml += b; } while (b == 255u && s < e); }
        ml += 4u;
        if (!off || off > o || o + ml > cap) return 0;
        for (k = 0; k < ml; k++, o++) d[o] = d[o - off];
    }
    return o;
}

/* ---- the HD6301 (librdpiano Mcu, MAME m6800) */
enum { M6800_WAI = 8, M6800_SLP = 0x10 };
enum { M6800_IRQ_LINE = 0, M6801_TIN_LINE = 1, INPUT_LINE_NMI = 32 };
enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
#define TCSR_IEDG 0x02
#define TCSR_EICI 0x10
#define TCSR_ICF 0x80

static PAIR m_ppc, m_pc, m_s, m_x, m_d, m_ea, m_counter;
static u8 m_cc, m_wai_state, m_nmi_state, m_nmi_pending, m_irq_state[5], m_tcsr, m_pending_tcsr, latch_val;
static u16 m_input_capture;
static int m_icount;
static u8 rdp_ram[0x1000];                     /* 0x0020..0x0FFF: the board's RAM */
static u8 rdp_prog[0x2000];                    /* the program, in RAM: every opcode is read from it */
static u32 rdp_exact = 1;                      /* 1: 100 cycles a sample (the real 2 MHz); 0: librdpiano's 100 insns */

/* the command queue to the board (librdpiano commands_queue): the main loop writes, the audio interrupt reads */
static volatile u8 rdp_q[256];
static volatile u32 rdp_qw, rdp_qr;
static RDP_FAST int rdp_q_empty(void) { return rdp_qw == rdp_qr; }

static u8 mcu_read(u32 addr);
static void mcu_write(u32 addr, u8 data);
static void mcu_check_irq_lines(void);
static void mcu_eat_cycles(void);
static void mcu_take_trap(void);
static void mcu_execute_one(void);
static void mcu_set_input(int line, int state);
static RDP_FAST u32 RM16(u32 a) { return (u32)mcu_read(a) << 8 | mcu_read((a + 1u) & 0xFFFFu); }
static RDP_FAST void WM16(u32 a, PAIR *p) { mcu_write(a, p->b.h); mcu_write((a + 1u) & 0xFFFFu, p->b.l); }
static RDP_FAST void increment_counter(int amount) { m_icount -= amount; }

/* ---- the opcodes: rdpiano's mcu_ops.h (MAME m6800, BSD-3-Clause, Aaron Giles), as C functions */
#define pPPC    m_ppc
#define pPC     m_pc
#define pS      m_s
#define pX      m_x
#define pD      m_d

#define PC      m_pc.w.l
#define PCD     m_pc.d
#define S       m_s.w.l
#define SD      m_s.d
#define X       m_x.w.l
#define D       m_d.w.l
#define A       m_d.b.h
#define B       m_d.b.l
#define CC      m_cc

#define EAD     m_ea.d
#define EA      m_ea.w.l

/* memory interface */

/****************************************************************************/
/* Read a byte from given memory location                                   */
/****************************************************************************/
// #define RM(Addr) (m_program.read_byte(Addr))
#define RM(Addr) (mcu_read(Addr))

/****************************************************************************/
/* Write a byte to given memory location                                    */
/****************************************************************************/
// #define WM(Addr,Value) (m_program.write_byte(Addr,Value))
#define WM(Addr,Value) (mcu_write(Addr,Value))

/****************************************************************************/
/* M6800_RDOP() is identical to M6800_RDMEM() except it is used for reading */
/* opcodes. In case of system with memory mapped I/O, this function can be  */
/* used to greatly speed up emulation                                       */
/****************************************************************************/
// #define M_RDOP(Addr) (m_copcodes.read_byte(Addr))
#define M_RDOP(Addr) (mcu_read(Addr))

/****************************************************************************/
/* M6800_RDOP_ARG() is identical to M6800_RDOP() but it's used for reading  */
/* opcode arguments. This difference can be used to support systems that    */
/* use different encoding mechanisms for opcodes and opcode arguments       */
/****************************************************************************/
// #define M_RDOP_ARG(Addr) (m_cprogram.read_byte(Addr))
#define M_RDOP_ARG(Addr) (mcu_read(Addr))

/* macros to access memory */
#define IMMBYTE(b)  b = M_RDOP_ARG(PCD); PC++
#define IMMWORD(w)  w.d = (M_RDOP_ARG(PCD)<<8) | M_RDOP_ARG((PCD+1)&0xffff); PC+=2

#define PUSHBYTE(b) WM(SD,b); --S
#define PUSHWORD(w) WM(SD,w.b.l); --S; WM(SD,w.b.h); --S
#define PULLBYTE(b) S++; b = RM(SD)
#define PULLWORD(w) S++; w.d = RM(SD)<<8; S++; w.d |= RM(SD)

/* CC masks                       HI NZVC
                                7654 3210   */
#define CLR_HNZVC   CC&=0xd0
#define CLR_NZV     CC&=0xf1
#define CLR_HNZC    CC&=0xd2
#define CLR_NZVC    CC&=0xf0
#define CLR_Z       CC&=0xfb
#define CLR_ZC      CC&=0xfa
#define CLR_C       CC&=0xfe

/* macros for CC -- CC bits affected should be reset before calling */
#define SET_Z(a)        if(!(a))SEZ
#define SET_Z8(a)       SET_Z((u8)(a))
#define SET_Z16(a)      SET_Z((u16)(a))
#define SET_N8(a)       CC|=(((a)&0x80)>>4)
#define SET_N16(a)      CC|=(((a)&0x8000)>>12)
#define SET_H(a,b,r)    CC|=((((a)^(b)^(r))&0x10)<<1)
#define SET_C8(a)       CC|=(((a)&0x100)>>8)
#define SET_C16(a)      CC|=(((a)>>16)&1)
#define SET_V8(a,b,r)   CC|=((((a)^(b)^(r)^((r)>>1))&0x80)>>6)
#define SET_V16(a,b,r)  CC|=((((a)^(b)^(r)^((r)>>1))&0x8000)>>14)


static const u8 flags8i[256]= /* increment */
{
0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x0a,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
};


static const u8 flags8d[256]= /* decrement */
{
0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
};

#define SET_FLAGS8I(a)      {CC|=flags8i[(a)&0xff];}
#define SET_FLAGS8D(a)      {CC|=flags8d[(a)&0xff];}

/* combos */
#define SET_NZ8(a)          {SET_N8(a);SET_Z8(a);}
#define SET_NZ16(a)         {SET_N16(a);SET_Z16(a);}
#define SET_FLAGS8(a,b,r)   {SET_N8(r);SET_Z8(r);SET_V8(a,b,r);SET_C8(r);}
#define SET_FLAGS16(a,b,r)  {SET_N16(r);SET_Z16(r);SET_V16(a,b,r);SET_C16(r);}

/* for treating an u8 as a signed s16 */
#define SIGNED(b) ((s16)((b)&0x80?(b)|0xff00:(b)))

/* Macros for addressing modes */
#define DIRECT IMMBYTE(EAD)
#define IMM8 EA=PC++
#define IMM16 {EA=PC;PC+=2;}
#define EXTENDED IMMWORD(m_ea)
#define INDEXED {EA=X+(u8)M_RDOP_ARG(PCD);PC++;}

/* macros to set status flags */
#if defined(SEC)
#undef SEC
#endif
#define SEC CC|=0x01
#define CLC CC&=0xfe
#define SEZ CC|=0x04
#define CLZ CC&=0xfb
#define SEN CC|=0x08
#define CLN CC&=0xf7
#define SEV CC|=0x02
#define CLV CC&=0xfd
#define SEH CC|=0x20
#define CLH CC&=0xdf
#define SEI CC|=0x10
#define CLI CC&=~0x10

/* macros for convenience */
#define DIRBYTE(b) {DIRECT;b=RM(EAD);}
#define DIRWORD(w) {DIRECT;w.d=RM16(EAD);}
#define EXTBYTE(b) {EXTENDED;b=RM(EAD);}
#define EXTWORD(w) {EXTENDED;w.d=RM16(EAD);}

#define IDXBYTE(b) {INDEXED;b=RM(EAD);}
#define IDXWORD(w) {INDEXED;w.d=RM16(EAD);}

/* Macros for branch instructions */
#define BRANCH(f) {IMMBYTE(t);if(f){PC+=SIGNED(t);}}
#define NXORV  ((CC&0x08)^((CC&0x02)<<2))
#define NXORC  ((CC&0x08)^((CC&0x01)<<3))



// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/*

HNZVC

? = undefined
* = affected
- = unaffected
0 = cleared
1 = set
# = ccr directly affected by instruction
@ = special - carry set if bit 7 is set

*/

#define OP_HANDLER(_name) static RDP_FAST void op_##_name(void)


//OP_HANDLER( illegl1 )
OP_HANDLER( illegl1 )
{
	RDP_LOG("m6800: illegal 1-byte opcode: address %04X, op %02X\n",PC-1,(int) M_RDOP_ARG(PC-1)&0xFF);
}

//OP_HANDLER( illegl2 )
OP_HANDLER( illegl2 )
{
	RDP_LOG("m6800: illegal 2-byte opcode: address %04X, op %02X\n",PC-1,(int) M_RDOP_ARG(PC-1)&0xFF);
	PC++;
}

//OP_HANDLER( illegl3 )
OP_HANDLER( illegl3 )
{
	RDP_LOG("m6800: illegal 3-byte opcode: address %04X, op %02X\n",PC-1,(int) M_RDOP_ARG(PC-1)&0xFF);
	PC += 2;
}

/* HD63701 only */
//OP_HANDLER( trap )
OP_HANDLER( trap )
{
	RDP_LOG("m6800: illegal opcode: address %04X, op %02X\n",PC-1,(int) M_RDOP_ARG(PC-1)&0xFF);
	PC--;
	mcu_take_trap();
}

/* $00 ILLEGAL */

/* $01 NOP */
OP_HANDLER( nop )
{
}

/* $02 ILLEGAL */

/* $03 ILLEGAL */

/* $04 LSRD inherent -0*-* */
OP_HANDLER( lsrd )
{
	u16 t;
	CLR_NZVC; t = D; CC|=(t&0x0001);
	t>>=1; SET_Z16(t);
	if (NXORC) SEV;
	D=t;
}

/* $05 ASLD inherent ?**** */
OP_HANDLER( asld )
{
	int r;
	u16 t;
	t = D; r=t<<1;
	CLR_NZVC; SET_FLAGS16(t,t,r);
	D=r;
}

/* $06 TAP inherent ##### */
OP_HANDLER( tap )
{
	CC=A;

	// TAP temporarily sets the I flag and blocks IRQ until the next opcode
	// (if the next opcode is TAP, IRQ is blocked again)
	mcu_execute_one();
	mcu_check_irq_lines();
}

/* $07 TPA inherent ----- */
OP_HANDLER( tpa )
{
	A=CC;
}

/* $08 INX inherent --*-- */
OP_HANDLER( inx )
{
	++X;
	CLR_Z; SET_Z16(X);
}

/* $09 DEX inherent --*-- */
OP_HANDLER( dex )
{
	--X;
	CLR_Z; SET_Z16(X);
}

/* $0a CLV */
OP_HANDLER( clv )
{
	CLV;
}

/* $0b SEV */
OP_HANDLER( sev )
{
	SEV;
}

/* $0c CLC */
OP_HANDLER( clc )
{
	CLC;
}

/* $0d SEC */
OP_HANDLER( sec )
{
	SEC;
}

/* $0e CLI */
OP_HANDLER( cli )
{
	u8 i = CC & 0x10;
	CLI;

	// pending IRQ won't be triggered until next machine cycle
	if (i) mcu_execute_one();
	mcu_check_irq_lines();
}

/* $0f SEI */
OP_HANDLER( sei )
{
	SEI;
}

/* $10 SBA inherent -**** */
OP_HANDLER( sba )
{
	u16 t;
	t=A-B;
	CLR_NZVC; SET_FLAGS8(A,B,t);
	A=t;
}

/* $11 CBA inherent -**** */
OP_HANDLER( cba )
{
	u16 t;
	t=A-B;
	CLR_NZVC; SET_FLAGS8(A,B,t);
}

/* $12 ILLEGAL */
OP_HANDLER( undoc1 )
{
	X += RM( S + 1 );
}

/* $13 ILLEGAL */
OP_HANDLER( undoc2 )
{
	X += RM( S + 1 );
}


/* $14 ILLEGAL */

/* $15 ILLEGAL */

/* $16 TAB inherent -**0- */
OP_HANDLER( tab )
{
	B=A;
	CLR_NZV; SET_NZ8(B);
}

/* $17 TBA inherent -**0- */
OP_HANDLER( tba )
{
	A=B;
	CLR_NZV; SET_NZ8(A);
}

/* $18 XGDX inherent ----- */ /* HD63701Y0 only */
OP_HANDLER( xgdx )
{
	u16 t = X;
	X = D;
	D=t;
}

/* $19 DAA inherent (A) -**0* */
OP_HANDLER( daa )
{
	u8 msn, lsn;
	u16 t, cf = 0;
	msn=A & 0xf0; lsn=A & 0x0f;
	if (lsn>0x09 || CC&0x20) cf |= 0x06;
	if (msn>0x80 && lsn>0x09) cf |= 0x60;
	if (msn>0x90 || CC&0x01) cf |= 0x60;
	t = cf + A;
	CLR_NZV; // keep carry from previous operation
	SET_NZ8((u8)t); SET_C8(t);
	A = t;
}

/* $1a ILLEGAL */

/* $1a SLP */ /* HD63701Y0 only */
OP_HANDLER( slp )
{
	// wait for next IRQ (same as waiting of WAI)
	m_wai_state |= M6800_SLP;

	mcu_check_irq_lines();
	if (m_wai_state & M6800_SLP)
		mcu_eat_cycles();
}

/* $1b ABA inherent ***** */
OP_HANDLER( aba )
{
	u16 t;
	t=A+B;
	CLR_HNZVC; SET_FLAGS8(A,B,t); SET_H(A,B,t);
	A=t;
}

/* $1c ILLEGAL */

/* $1d ILLEGAL */

/* $1e ILLEGAL */

/* $1f ILLEGAL */

/* $20 BRA relative ----- */
OP_HANDLER( bra )
{
	u8 t;
	BRANCH(1);
}

/* $21 BRN relative ----- */
OP_HANDLER( brn )
{
	u8 t;
	BRANCH(0);
}

/* $22 BHI relative ----- */
OP_HANDLER( bhi )
{
	u8 t;
	BRANCH(!(CC&0x05));
}

/* $23 BLS relative ----- */
OP_HANDLER( bls )
{
	u8 t;
	BRANCH(CC&0x05);
}

/* $24 BCC relative ----- */
OP_HANDLER( bcc )
{
	u8 t;
	BRANCH(!(CC&0x01));
}

/* $25 BCS relative ----- */
OP_HANDLER( bcs )
{
	u8 t;
	BRANCH(CC&0x01);
}

/* $26 BNE relative ----- */
OP_HANDLER( bne )
{
	u8 t;
	BRANCH(!(CC&0x04));
}

/* $27 BEQ relative ----- */
OP_HANDLER( beq )
{
	u8 t;
	BRANCH(CC&0x04);
}

/* $28 BVC relative ----- */
OP_HANDLER( bvc )
{
	u8 t;
	BRANCH(!(CC&0x02));
}

/* $29 BVS relative ----- */
OP_HANDLER( bvs )
{
	u8 t;
	BRANCH(CC&0x02);
}

/* $2a BPL relative ----- */
OP_HANDLER( bpl )
{
	u8 t;
	BRANCH(!(CC&0x08));
}

/* $2b BMI relative ----- */
OP_HANDLER( bmi )
{
	u8 t;
	BRANCH(CC&0x08);
}

/* $2c BGE relative ----- */
OP_HANDLER( bge )
{
	u8 t;
	BRANCH(!NXORV);
}

/* $2d BLT relative ----- */
OP_HANDLER( blt )
{
	u8 t;
	BRANCH(NXORV);
}

/* $2e BGT relative ----- */
OP_HANDLER( bgt )
{
	u8 t;
	BRANCH(!(NXORV||CC&0x04));
}

/* $2f BLE relative ----- */
OP_HANDLER( ble )
{
	u8 t;
	BRANCH(NXORV||CC&0x04);
}

/* $30 TSX inherent ----- */
OP_HANDLER( tsx )
{
	X = ( S + 1 );
}

/* $31 INS inherent ----- */
OP_HANDLER( ins )
{
	++S;
}

/* $32 PULA inherent ----- */
OP_HANDLER( pula )
{
	PULLBYTE(m_d.b.h);
}

/* $33 PULB inherent ----- */
OP_HANDLER( pulb )
{
	PULLBYTE(m_d.b.l);
}

/* $34 DES inherent ----- */
OP_HANDLER( des )
{
	--S;
}

/* $35 TXS inherent ----- */
OP_HANDLER( txs )
{
	S = ( X - 1 );
}

/* $36 PSHA inherent ----- */
OP_HANDLER( psha )
{
	PUSHBYTE(m_d.b.h);
}

/* $37 PSHB inherent ----- */
OP_HANDLER( pshb )
{
	PUSHBYTE(m_d.b.l);
}

/* $38 PULX inherent ----- */
OP_HANDLER( pulx )
{
	PULLWORD(pX);
}

/* $39 RTS inherent ----- */
OP_HANDLER( rts )
{
	PULLWORD(pPC);
}

/* $3a ABX inherent ----- */
OP_HANDLER( abx )
{
	X += B;
}

/* $3b RTI inherent ##### */
OP_HANDLER( rti )
{
	PULLBYTE(CC);
	PULLBYTE(B);
	PULLBYTE(A);
	PULLWORD(pX);
	PULLWORD(pPC);

	mcu_check_irq_lines();
}

/* $3c PSHX inherent ----- */
OP_HANDLER( pshx )
{
	PUSHWORD(pX);
}

/* $3d MUL inherent --*-@ */
OP_HANDLER( mul )
{
	u16 t;
	t=A*B;
	CLR_C;
	if (t & 0x80) SEC;
	D=t;
}

/* $3e WAI inherent ----- */
OP_HANDLER( wai )
{
	// WAI stacks the entire machine state on the hardware stack,
	// then waits for an interrupt.
	m_wai_state |= M6800_WAI;

	PUSHWORD(pPC);
	PUSHWORD(pX);
	PUSHBYTE(A);
	PUSHBYTE(B);
	PUSHBYTE(CC);

	mcu_check_irq_lines();
	if (m_wai_state & M6800_WAI)
		mcu_eat_cycles();
}

/* $3f SWI absolute indirect ----- */
OP_HANDLER( swi )
{
	PUSHWORD(pPC);
	PUSHWORD(pX);
	PUSHBYTE(A);
	PUSHBYTE(B);
	PUSHBYTE(CC);

	SEI;
	PCD = RM16(0xfffa);
}

/* $40 NEGA inherent ?**** */
OP_HANDLER( nega )
{
	u16 r;
	r=-A;
	CLR_NZVC; SET_FLAGS8(0,A,r);
	A=r;
}

/* $41 ILLEGAL */

/* $42 ILLEGAL */

/* $43 COMA inherent -**01 */
OP_HANDLER( coma )
{
	A = ~A;
	CLR_NZV; SET_NZ8(A); SEC;
}

/* $44 LSRA inherent -0*-* */
OP_HANDLER( lsra )
{
	CLR_NZVC; CC|=(A&0x01);
	A>>=1; SET_Z8(A);
	if (NXORC) SEV;
}

/* $45 ILLEGAL */

/* $46 RORA inherent -**-* */
OP_HANDLER( rora )
{
	u8 r;
	r=(CC&0x01)<<7;
	CLR_NZVC; CC|=(A&0x01);
	r |= A>>1; SET_NZ8(r);
	if (NXORC) SEV;
	A=r;
}

/* $47 ASRA inherent ?**-* */
OP_HANDLER( asra )
{
	CLR_NZVC; CC|=(A&0x01);
	A>>=1; A|=((A&0x40)<<1);
	SET_NZ8(A);
	if (NXORC) SEV;
}

/* $48 ASLA inherent ?**** */
OP_HANDLER( asla )
{
	u16 r;
	r=A<<1;
	CLR_NZVC; SET_FLAGS8(A,A,r);
	A=r;
}

/* $49 ROLA inherent -**** */
OP_HANDLER( rola )
{
	u16 t,r;
	t = A; r = CC&0x01; r |= t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	A=r;
}

/* $4a DECA inherent -***- */
OP_HANDLER( deca )
{
	--A;
	CLR_NZV; SET_FLAGS8D(A);
}

/* $4b ILLEGAL */

/* $4c INCA inherent -***- */
OP_HANDLER( inca )
{
	++A;
	CLR_NZV; SET_FLAGS8I(A);
}

/* $4d TSTA inherent -**0- */
OP_HANDLER( tsta )
{
	CLR_NZVC; SET_NZ8(A);
}

/* $4e ILLEGAL */

/* $4f CLRA inherent -0100 */
OP_HANDLER( clra )
{
	A=0;
	CLR_NZVC; SEZ;
}

/* $50 NEGB inherent ?**** */
OP_HANDLER( negb )
{
	u16 r;
	r=-B;
	CLR_NZVC; SET_FLAGS8(0,B,r);
	B=r;
}

/* $51 ILLEGAL */

/* $52 ILLEGAL */

/* $53 COMB inherent -**01 */
OP_HANDLER( comb )
{
	B = ~B;
	CLR_NZV; SET_NZ8(B); SEC;
}

/* $54 LSRB inherent -0*-* */
OP_HANDLER( lsrb )
{
	CLR_NZVC; CC|=(B&0x01);
	B>>=1; SET_Z8(B);
	if (NXORC) SEV;
}

/* $55 ILLEGAL */

/* $56 RORB inherent -**-* */
OP_HANDLER( rorb )
{
	u8 r;
	r=(CC&0x01)<<7;
	CLR_NZVC; CC|=(B&0x01);
	r |= B>>1; SET_NZ8(r);
	if (NXORC) SEV;
	B=r;
}

/* $57 ASRB inherent ?**-* */
OP_HANDLER( asrb )
{
	CLR_NZVC; CC|=(B&0x01);
	B>>=1; B|=((B&0x40)<<1);
	SET_NZ8(B);
	if (NXORC) SEV;
}

/* $58 ASLB inherent ?**** */
OP_HANDLER( aslb )
{
	u16 r;
	r=B<<1;
	CLR_NZVC; SET_FLAGS8(B,B,r);
	B=r;
}

/* $59 ROLB inherent -**** */
OP_HANDLER( rolb )
{
	u16 t,r;
	t = B; r = CC&0x01; r |= t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	B=r;
}

/* $5a DECB inherent -***- */
OP_HANDLER( decb )
{
	--B;
	CLR_NZV; SET_FLAGS8D(B);
}

/* $5b ILLEGAL */

/* $5c INCB inherent -***- */
OP_HANDLER( incb )
{
	++B;
	CLR_NZV; SET_FLAGS8I(B);
}

/* $5d TSTB inherent -**0- */
OP_HANDLER( tstb )
{
	CLR_NZVC; SET_NZ8(B);
}

/* $5e ILLEGAL */

/* $5f CLRB inherent -0100 */
OP_HANDLER( clrb )
{
	B=0;
	CLR_NZVC; SEZ;
}

/* $60 NEG indexed ?**** */
OP_HANDLER( neg_ix )
{
	u16 r,t;
	IDXBYTE(t); r=-t;
	CLR_NZVC; SET_FLAGS8(0,t,r);
	WM(EAD,r);
}

/* $61 AIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( aim_ix )
{
	u8 t, r;
	IMMBYTE(t);
	IDXBYTE(r);
	r &= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $62 OIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( oim_ix )
{
	u8 t, r;
	IMMBYTE(t);
	IDXBYTE(r);
	r |= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $63 COM indexed -**01 */
OP_HANDLER( com_ix )
{
	u8 t;
	IDXBYTE(t); t = ~t;
	CLR_NZV; SET_NZ8(t); SEC;
	WM(EAD,t);
}

/* $64 LSR indexed -0*-* */
OP_HANDLER( lsr_ix )
{
	u8 t;
	IDXBYTE(t); CLR_NZVC; CC|=(t&0x01);
	t>>=1; SET_Z8(t);
	if (NXORC) SEV;
	WM(EAD,t);
}

/* $65 EIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( eim_ix )
{
	u8 t, r;
	IMMBYTE(t);
	IDXBYTE(r);
	r ^= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $66 ROR indexed -**-* */
OP_HANDLER( ror_ix )
{
	u8 t,r;
	IDXBYTE(t); r=(CC&0x01)<<7;
	CLR_NZVC; CC|=(t&0x01);
	r |= t>>1; SET_NZ8(r);
	if (NXORC) SEV;
	WM(EAD,r);
}

/* $67 ASR indexed ?**-* */
OP_HANDLER( asr_ix )
{
	u8 t;
	IDXBYTE(t); CLR_NZVC; CC|=(t&0x01);
	t>>=1; t|=((t&0x40)<<1);
	SET_NZ8(t);
	if (NXORC) SEV;
	WM(EAD,t);
}

/* $68 ASL indexed ?**** */
OP_HANDLER( asl_ix )
{
	u16 t,r;
	IDXBYTE(t); r=t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	WM(EAD,r);
}

/* $69 ROL indexed -**** */
OP_HANDLER( rol_ix )
{
	u16 t,r;
	IDXBYTE(t); r = CC&0x01; r |= t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	WM(EAD,r);
}

/* $6a DEC indexed -***- */
OP_HANDLER( dec_ix )
{
	u8 t;
	IDXBYTE(t); --t;
	CLR_NZV; SET_FLAGS8D(t);
	WM(EAD,t);
}

/* $6b TIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( tim_ix )
{
	u8 t, r;
	IMMBYTE(t);
	IDXBYTE(r);
	r &= t;
	CLR_NZV; SET_NZ8(r);
}

/* $6c INC indexed -***- */
OP_HANDLER( inc_ix )
{
	u8 t;
	IDXBYTE(t); ++t;
	CLR_NZV; SET_FLAGS8I(t);
	WM(EAD,t);
}

/* $6d TST indexed -**0- */
OP_HANDLER( tst_ix )
{
	u8 t;
	IDXBYTE(t); CLR_NZVC; SET_NZ8(t);
}

/* $6e JMP indexed ----- */
OP_HANDLER( jmp_ix )
{
	INDEXED; PC=EA;
}

/* $6f CLR indexed -0100 */
OP_HANDLER( clr_ix )
{
	INDEXED; WM(EAD,0);
	CLR_NZVC; SEZ;
}

/* $70 NEG extended ?**** */
OP_HANDLER( neg_ex )
{
	u16 r,t;
	EXTBYTE(t); r=-t;
	CLR_NZVC; SET_FLAGS8(0,t,r);
	WM(EAD,r);
}

/* $71 AIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( aim_di )
{
	u8 t, r;
	IMMBYTE(t);
	DIRBYTE(r);
	r &= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $72 OIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( oim_di )
{
	u8 t, r;
	IMMBYTE(t);
	DIRBYTE(r);
	r |= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $73 COM extended -**01 */
OP_HANDLER( com_ex )
{
	u8 t;
	EXTBYTE(t); t = ~t;
	CLR_NZV; SET_NZ8(t); SEC;
	WM(EAD,t);
}

/* $74 LSR extended -0*-* */
OP_HANDLER( lsr_ex )
{
	u8 t;
	EXTBYTE(t);
	CLR_NZVC;
	CC|=(t&0x01);
	t>>=1;
	SET_Z8(t);
	if (NXORC) SEV;
	WM(EAD,t);
}

/* $75 EIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( eim_di )
{
	u8 t, r;
	IMMBYTE(t);
	DIRBYTE(r);
	r ^= t;
	CLR_NZV; SET_NZ8(r);
	WM(EAD,r);
}

/* $76 ROR extended -**-* */
OP_HANDLER( ror_ex )
{
	u8 t,r;
	EXTBYTE(t); r=(CC&0x01)<<7;
	CLR_NZVC; CC|=(t&0x01);
	r |= t>>1; SET_NZ8(r);
	if (NXORC) SEV;
	WM(EAD,r);
}

/* $77 ASR extended ?**-* */
OP_HANDLER( asr_ex )
{
	u8 t;
	EXTBYTE(t); CLR_NZVC; CC|=(t&0x01);
	t>>=1; t|=((t&0x40)<<1);
	SET_NZ8(t);
	if (NXORC) SEV;
	WM(EAD,t);
}

/* $78 ASL extended ?**** */
OP_HANDLER( asl_ex )
{
	u16 t,r;
	EXTBYTE(t); r=t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	WM(EAD,r);
}

/* $79 ROL extended -**** */
OP_HANDLER( rol_ex )
{
	u16 t,r;
	EXTBYTE(t); r = CC&0x01; r |= t<<1;
	CLR_NZVC; SET_FLAGS8(t,t,r);
	WM(EAD,r);
}

/* $7a DEC extended -***- */
OP_HANDLER( dec_ex )
{
	u8 t;
	EXTBYTE(t); --t;
	CLR_NZV; SET_FLAGS8D(t);
	WM(EAD,t);
}

/* $7b TIM --**0- */ /* HD63701Y0 only */
OP_HANDLER( tim_di )
{
	u8 t, r;
	IMMBYTE(t);
	DIRBYTE(r);
	r &= t;
	CLR_NZV; SET_NZ8(r);
}

/* $7c INC extended -***- */
OP_HANDLER( inc_ex )
{
	u8 t;
	EXTBYTE(t); ++t;
	CLR_NZV; SET_FLAGS8I(t);
	WM(EAD,t);
}

/* $7d TST extended -**0- */
OP_HANDLER( tst_ex )
{
	u8 t;
	EXTBYTE(t); CLR_NZVC; SET_NZ8(t);
}

/* $7e JMP extended ----- */
OP_HANDLER( jmp_ex )
{
	EXTENDED; PC=EA;
}

/* $7f CLR extended -0100 */
OP_HANDLER( clr_ex )
{
	EXTENDED; WM(EAD,0);
	CLR_NZVC; SEZ;
}

/* $80 SUBA immediate ?**** */
OP_HANDLER( suba_im )
{
	u16    t,r;
	IMMBYTE(t); r = A-t;
	CLR_NZVC; SET_FLAGS8(A,t,r);
	A = r;
}

/* $81 CMPA immediate ?**** */
OP_HANDLER( cmpa_im )
{
	u16    t,r;
	IMMBYTE(t); r = A-t;
	CLR_NZVC; SET_FLAGS8(A,t,r);
}

/* $82 SBCA immediate ?**** */
OP_HANDLER( sbca_im )
{
	u16    t,r;
	IMMBYTE(t); r = A-t-(CC&0x01);
	CLR_NZVC; SET_FLAGS8(A,t,r);
	A = r;
}

/* $83 SUBD immediate -**** */
OP_HANDLER( subd_im )
{
	u32 r,d;
	PAIR b;
	IMMWORD(b);
	d = D;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $84 ANDA immediate -**0- */
OP_HANDLER( anda_im )
{
	u8 t;
	IMMBYTE(t); A &= t;
	CLR_NZV; SET_NZ8(A);
}

/* $85 BITA immediate -**0- */
OP_HANDLER( bita_im )
{
	u8 t,r;
	IMMBYTE(t); r = A&t;
	CLR_NZV; SET_NZ8(r);
}

/* $86 LDA immediate -**0- */
OP_HANDLER( lda_im )
{
	IMMBYTE(A);
	CLR_NZV; SET_NZ8(A);
}

/* is this a legal instruction? */
/* $87 STA immediate -**0- */
OP_HANDLER( sta_im )
{
	CLR_NZV; SET_NZ8(A);
	IMM8; WM(EAD,A);
}

/* $88 EORA immediate -**0- */
OP_HANDLER( eora_im )
{
	u8 t;
	IMMBYTE(t); A ^= t;
	CLR_NZV; SET_NZ8(A);
}

/* $89 ADCA immediate ***** */
OP_HANDLER( adca_im )
{
	u16 t,r;
	IMMBYTE(t); r = A+t+(CC&0x01);
	CLR_HNZVC; SET_FLAGS8(A,t,r); SET_H(A,t,r);
	A = r;
}

/* $8a ORA immediate -**0- */
OP_HANDLER( ora_im )
{
	u8 t;
	IMMBYTE(t); A |= t;
	CLR_NZV; SET_NZ8(A);
}

/* $8b ADDA immediate ***** */
OP_HANDLER( adda_im )
{
	u16 t,r;
	IMMBYTE(t); r = A+t;
	CLR_HNZVC; SET_FLAGS8(A,t,r); SET_H(A,t,r);
	A = r;
}

/* $8c CMPX immediate -***- */
OP_HANDLER( cmpx_im )
{
	PAIR r,d,b;
	IMMWORD(b);
	d.d = X;
	r.w.l = d.b.h - b.b.h;
	CLR_NZV;
	SET_N8(r.b.l);
	SET_V8(d.b.h, b.b.h, r.w.l);
	r.d = d.d - b.d;
	SET_Z16(r.d);
}

/* $8c CPX immediate -**** (6803) */
OP_HANDLER( cpx_im )
{
	u32 r,d;
	PAIR b;
	IMMWORD(b);
	d = X;
	r = d - b.d;
	CLR_NZVC; SET_FLAGS16(d,b.d,r);
}


/* $8d BSR ----- */
OP_HANDLER( bsr )
{
	u8 t;
	IMMBYTE(t);
	PUSHWORD(pPC);
	PC += SIGNED(t);
}

/* $8e LDS immediate -**0- */
OP_HANDLER( lds_im )
{
	IMMWORD(m_s);
	CLR_NZV;
	SET_NZ16(S);
}

/* $8f STS immediate -**0- */
OP_HANDLER( sts_im )
{
	CLR_NZV;
	SET_NZ16(S);
	IMM16;
	WM16(EAD,&m_s);
}

/* $90 SUBA direct ?**** */
OP_HANDLER( suba_di )
{
	u16    t,r;
	DIRBYTE(t); r = A-t;
	CLR_NZVC; SET_FLAGS8(A,t,r);
	A = r;
}

/* $91 CMPA direct ?**** */
OP_HANDLER( cmpa_di )
{
	u16    t,r;
	DIRBYTE(t); r = A-t;
	CLR_NZVC; SET_FLAGS8(A,t,r);
}

/* $92 SBCA direct ?**** */
OP_HANDLER( sbca_di )
{
	u16    t,r;
	DIRBYTE(t); r = A-t-(CC&0x01);
	CLR_NZVC; SET_FLAGS8(A,t,r);
	A = r;
}

/* $93 SUBD direct -**** */
OP_HANDLER( subd_di )
{
	u32 r,d;
	PAIR b;
	DIRWORD(b);
	d = D;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D=r;
}

/* $94 ANDA direct -**0- */
OP_HANDLER( anda_di )
{
	u8 t;
	DIRBYTE(t); A &= t;
	CLR_NZV; SET_NZ8(A);
}

/* $95 BITA direct -**0- */
OP_HANDLER( bita_di )
{
	u8 t,r;
	DIRBYTE(t); r = A&t;
	CLR_NZV; SET_NZ8(r);
}

/* $96 LDA direct -**0- */
OP_HANDLER( lda_di )
{
	DIRBYTE(A);
	CLR_NZV;
	SET_NZ8(A);
}

/* $97 STA direct -**0- */
OP_HANDLER( sta_di )
{
	CLR_NZV;
	SET_NZ8(A);
	DIRECT;
	WM(EAD,A);
}

/* $98 EORA direct -**0- */
OP_HANDLER( eora_di )
{
	u8 t;
	DIRBYTE(t);
	A ^= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $99 ADCA direct ***** */
OP_HANDLER( adca_di )
{
	u16 t,r;
	DIRBYTE(t);
	r = A+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $9a ORA direct -**0- */
OP_HANDLER( ora_di )
{
	u8 t;
	DIRBYTE(t);
	A |= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $9b ADDA direct ***** */
OP_HANDLER( adda_di )
{
	u16 t,r;
	DIRBYTE(t);
	r = A + t;
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $9c CMPX direct -***- */
OP_HANDLER( cmpx_di )
{
	PAIR r,d,b;
	DIRWORD(b);
	d.d = X;
	r.w.l = d.b.h - b.b.h;
	CLR_NZV;
	SET_N8(r.b.l);
	SET_V8(d.b.h, b.b.h, r.w.l);
	r.d = d.d - b.d;
	SET_Z16(r.d);
}

/* $9c CPX direct -**** (6803) */
OP_HANDLER( cpx_di )
{
	u32 r,d;
	PAIR b;
	DIRWORD(b);
	d = X;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
}

/* $9d JSR direct ----- */
OP_HANDLER( jsr_di )
{
	DIRECT;
	PUSHWORD(pPC);
	PC = EA;
}

/* $9e LDS direct -**0- */
OP_HANDLER( lds_di )
{
	DIRWORD(m_s);
	CLR_NZV;
	SET_NZ16(S);
}

/* $9f STS direct -**0- */
OP_HANDLER( sts_di )
{
	CLR_NZV;
	SET_NZ16(S);
	DIRECT;
	WM16(EAD,&m_s);
}

/* $a0 SUBA indexed ?**** */
OP_HANDLER( suba_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = A - t;
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
	A = r;
}

/* $a1 CMPA indexed ?**** */
OP_HANDLER( cmpa_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = A - t;
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
}

/* $a2 SBCA indexed ?**** */
OP_HANDLER( sbca_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = A - t - (CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
	A = r;
}

/* $a3 SUBD indexed -**** */
OP_HANDLER( subd_ix )
{
	u32 r,d;
	PAIR b;
	IDXWORD(b);
	d = D;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $a4 ANDA indexed -**0- */
OP_HANDLER( anda_ix )
{
	u8 t;
	IDXBYTE(t); A &= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $a5 BITA indexed -**0- */
OP_HANDLER( bita_ix )
{
	u8 t,r;
	IDXBYTE(t); r = A&t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $a6 LDA indexed -**0- */
OP_HANDLER( lda_ix )
{
	IDXBYTE(A);
	CLR_NZV;
	SET_NZ8(A);
}

/* $a7 STA indexed -**0- */
OP_HANDLER( sta_ix )
{
	CLR_NZV;
	SET_NZ8(A);
	INDEXED;
	WM(EAD,A);
}

/* $a8 EORA indexed -**0- */
OP_HANDLER( eora_ix )
{
	u8 t;
	IDXBYTE(t);
	A ^= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $a9 ADCA indexed ***** */
OP_HANDLER( adca_ix )
{
	u16 t,r;
	IDXBYTE(t);
	r = A + t + (CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $aa ORA indexed -**0- */
OP_HANDLER( ora_ix )
{
	u8 t;
	IDXBYTE(t);
	A |= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $ab ADDA indexed ***** */
OP_HANDLER( adda_ix )
{
	u16 t,r;
	IDXBYTE(t);
	r = A+t;
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $ac CMPX indexed -***- */
OP_HANDLER( cmpx_ix )
{
	PAIR r,d,b;
	IDXWORD(b);
	d.d = X;
	r.w.l = d.b.h - b.b.h;
	CLR_NZV;
	SET_N8(r.b.l);
	SET_V8(d.b.h, b.b.h, r.w.l);
	r.d = d.d - b.d;
	SET_Z16(r.d);
}

/* $ac CPX indexed -**** (6803)*/
OP_HANDLER( cpx_ix )
{
	u32 r,d;
	PAIR b;
	IDXWORD(b);
	d = X;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
}

/* $ad JSR indexed ----- */
OP_HANDLER( jsr_ix )
{
	INDEXED;
	PUSHWORD(pPC);
	PC = EA;
}

/* $ae LDS indexed -**0- */
OP_HANDLER( lds_ix )
{
	IDXWORD(m_s);
	CLR_NZV;
	SET_NZ16(S);
}

/* $af STS indexed -**0- */
OP_HANDLER( sts_ix )
{
	CLR_NZV;
	SET_NZ16(S);
	INDEXED;
	WM16(EAD,&m_s);
}

/* $b0 SUBA extended ?**** */
OP_HANDLER( suba_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = A - t;
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
	A = r;
}

/* $b1 CMPA extended ?**** */
OP_HANDLER( cmpa_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = A-t;
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
}

/* $b2 SBCA extended ?**** */
OP_HANDLER( sbca_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = A-t-(CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(A,t,r);
	A = r;
}

/* $b3 SUBD extended -**** */
OP_HANDLER( subd_ex )
{
	u32 r,d;
	PAIR b;
	EXTWORD(b);
	d = D;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D=r;
}

/* $b4 ANDA extended -**0- */
OP_HANDLER( anda_ex )
{
	u8 t;
	EXTBYTE(t);
	A &= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $b5 BITA extended -**0- */
OP_HANDLER( bita_ex )
{
	u8 t,r;
	EXTBYTE(t);
	r = A&t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $b6 LDA extended -**0- */
OP_HANDLER( lda_ex )
{
	EXTBYTE(A);
	CLR_NZV;
	SET_NZ8(A);
}

/* $b7 STA extended -**0- */
OP_HANDLER( sta_ex )
{
	CLR_NZV;
	SET_NZ8(A);
	EXTENDED;
	WM(EAD,A);
}

/* $b8 EORA extended -**0- */
OP_HANDLER( eora_ex )
{
	u8 t;
	EXTBYTE(t);
	A ^= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $b9 ADCA extended ***** */
OP_HANDLER( adca_ex )
{
	u16 t,r;
	EXTBYTE(t);
	r = A+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $ba ORA extended -**0- */
OP_HANDLER( ora_ex )
{
	u8 t;
	EXTBYTE(t);
	A |= t;
	CLR_NZV;
	SET_NZ8(A);
}

/* $bb ADDA extended ***** */
OP_HANDLER( adda_ex )
{
	u16 t,r;
	EXTBYTE(t);
	r = A+t;
	CLR_HNZVC;
	SET_FLAGS8(A,t,r);
	SET_H(A,t,r);
	A = r;
}

/* $bc CMPX extended -***- */
OP_HANDLER( cmpx_ex )
{
	PAIR r,d,b;
	EXTWORD(b);
	d.d = X;
	r.w.l = d.b.h - b.b.h;
	CLR_NZV;
	SET_N8(r.b.l);
	SET_V8(d.b.h, b.b.h, r.w.l);
	r.d = d.d - b.d;
	SET_Z16(r.d);
}

/* $bc CPX extended -**** (6803) */
OP_HANDLER( cpx_ex )
{
	u32 r,d;
	PAIR b;
	EXTWORD(b);
	d = X;
	r = d - b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
}

/* $bd JSR extended ----- */
OP_HANDLER( jsr_ex )
{
	EXTENDED;
	PUSHWORD(pPC);
	PC = EA;
}

/* $be LDS extended -**0- */
OP_HANDLER( lds_ex )
{
	EXTWORD(m_s);
	CLR_NZV;
	SET_NZ16(S);
}

/* $bf STS extended -**0- */
OP_HANDLER( sts_ex )
{
	CLR_NZV;
	SET_NZ16(S);
	EXTENDED;
	WM16(EAD,&m_s);
}

/* $c0 SUBB immediate ?**** */
OP_HANDLER( subb_im )
{
	u16    t,r;
	IMMBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $c1 CMPB immediate ?**** */
OP_HANDLER( cmpb_im )
{
	u16    t,r;
	IMMBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
}

/* $c2 SBCB immediate ?**** */
OP_HANDLER( sbcb_im )
{
	u16    t,r;
	IMMBYTE(t);
	r = B-t-(CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $c3 ADDD immediate -**** */
OP_HANDLER( addd_im )
{
	u32 r,d;
	PAIR b;
	IMMWORD(b);
	d = D;
	r = d + b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $c4 ANDB immediate -**0- */
OP_HANDLER( andb_im )
{
	u8 t;
	IMMBYTE(t);
	B &= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $c5 BITB immediate -**0- */
OP_HANDLER( bitb_im )
{
	u8 t,r;
	IMMBYTE(t);
	r = B&t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $c6 LDB immediate -**0- */
OP_HANDLER( ldb_im )
{
	IMMBYTE(B);
	CLR_NZV;
	SET_NZ8(B);
}

/* is this a legal instruction? */
/* $c7 STB immediate -**0- */
OP_HANDLER( stb_im )
{
	CLR_NZV;
	SET_NZ8(B);
	IMM8;
	WM(EAD,B);
}

/* $c8 EORB immediate -**0- */
OP_HANDLER( eorb_im )
{
	u8 t;
	IMMBYTE(t);
	B ^= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $c9 ADCB immediate ***** */
OP_HANDLER( adcb_im )
{
	u16 t,r;
	IMMBYTE(t);
	r = B+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $ca ORB immediate -**0- */
OP_HANDLER( orb_im )
{
	u8 t;
	IMMBYTE(t);
	B |= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $cb ADDB immediate ***** */
OP_HANDLER( addb_im )
{
	u16 t,r;
	IMMBYTE(t);
	r = B+t;
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $CC LDD immediate -**0- */
OP_HANDLER( ldd_im )
{
	IMMWORD(m_d);
	CLR_NZV;
	SET_NZ16(D);
}

/* is this a legal instruction? */
/* $cd STD immediate -**0- */
OP_HANDLER( std_im )
{
	IMM16;
	CLR_NZV;
	SET_NZ16(D);
	WM16(EAD,&m_d);
}

/* $ce LDX immediate -**0- */
OP_HANDLER( ldx_im )
{
	IMMWORD(m_x);
	CLR_NZV;
	SET_NZ16(X);
}

/* $cf STX immediate -**0- */
OP_HANDLER( stx_im )
{
	CLR_NZV;
	SET_NZ16(X);
	IMM16;
	WM16(EAD,&m_x);
}

/* $d0 SUBB direct ?**** */
OP_HANDLER( subb_di )
{
	u16    t,r;
	DIRBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $d1 CMPB direct ?**** */
OP_HANDLER( cmpb_di )
{
	u16    t,r;
	DIRBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
}

/* $d2 SBCB direct ?**** */
OP_HANDLER( sbcb_di )
{
	u16    t,r;
	DIRBYTE(t);
	r = B-t-(CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $d3 ADDD direct -**** */
OP_HANDLER( addd_di )
{
	u32 r,d;
	PAIR b;
	DIRWORD(b);
	d = D;
	r = d + b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $d4 ANDB direct -**0- */
OP_HANDLER( andb_di )
{
	u8 t;
	DIRBYTE(t);
	B &= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $d5 BITB direct -**0- */
OP_HANDLER( bitb_di )
{
	u8 t,r;
	DIRBYTE(t);
	r = B&t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $d6 LDB direct -**0- */
OP_HANDLER( ldb_di )
{
	DIRBYTE(B);
	CLR_NZV;
	SET_NZ8(B);
}

/* $d7 STB direct -**0- */
OP_HANDLER( stb_di )
{
	CLR_NZV;
	SET_NZ8(B);
	DIRECT;
	WM(EAD,B);
}

/* $d8 EORB direct -**0- */
OP_HANDLER( eorb_di )
{
	u8 t;
	DIRBYTE(t);
	B ^= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $d9 ADCB direct ***** */
OP_HANDLER( adcb_di )
{
	u16 t,r;
	DIRBYTE(t);
	r = B+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $da ORB direct -**0- */
OP_HANDLER( orb_di )
{
	u8 t;
	DIRBYTE(t);
	B |= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $db ADDB direct ***** */
OP_HANDLER( addb_di )
{
	u16 t,r;
	DIRBYTE(t);
	r = B+t;
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $dc LDD direct -**0- */
OP_HANDLER( ldd_di )
{
	DIRWORD(m_d);
	CLR_NZV;
	SET_NZ16(D);
}

/* $dd STD direct -**0- */
OP_HANDLER( std_di )
{
	DIRECT;
	CLR_NZV;
	SET_NZ16(D);
	WM16(EAD,&m_d);
}

/* $de LDX direct -**0- */
OP_HANDLER( ldx_di )
{
	DIRWORD(m_x);
	CLR_NZV;
	SET_NZ16(X);
}

/* $dF STX direct -**0- */
OP_HANDLER( stx_di )
{
	CLR_NZV;
	SET_NZ16(X);
	DIRECT;
	WM16(EAD,&m_x);
}

/* $e0 SUBB indexed ?**** */
OP_HANDLER( subb_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $e1 CMPB indexed ?**** */
OP_HANDLER( cmpb_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
}

/* $e2 SBCB indexed ?**** */
OP_HANDLER( sbcb_ix )
{
	u16    t,r;
	IDXBYTE(t);
	r = B-t-(CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $e3 ADDD indexed -**** */
OP_HANDLER( addd_ix )
{
	u32 r,d;
	PAIR b;
	IDXWORD(b);
	d = D;
	r = d + b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $e4 ANDB indexed -**0- */
OP_HANDLER( andb_ix )
{
	u8 t;
	IDXBYTE(t);
	B &= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $e5 BITB indexed -**0- */
OP_HANDLER( bitb_ix )
{
	u8 t,r;
	IDXBYTE(t);
	r = B&t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $e6 LDB indexed -**0- */
OP_HANDLER( ldb_ix )
{
	IDXBYTE(B);
	CLR_NZV;
	SET_NZ8(B);
}

/* $e7 STB indexed -**0- */
OP_HANDLER( stb_ix )
{
	CLR_NZV;
	SET_NZ8(B);
	INDEXED;
	WM(EAD,B);
}

/* $e8 EORB indexed -**0- */
OP_HANDLER( eorb_ix )
{
	u8 t;
	IDXBYTE(t);
	B ^= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $e9 ADCB indexed ***** */
OP_HANDLER( adcb_ix )
{
	u16 t,r;
	IDXBYTE(t);
	r = B+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $ea ORB indexed -**0- */
OP_HANDLER( orb_ix )
{
	u8 t;
	IDXBYTE(t);
	B |= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $eb ADDB indexed ***** */
OP_HANDLER( addb_ix )
{
	u16 t,r;
	IDXBYTE(t);
	r = B+t;
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $ec LDD indexed -**0- */
OP_HANDLER( ldd_ix )
{
	IDXWORD(m_d);
	CLR_NZV;
	SET_NZ16(D);
}

/* $ec ADCX immediate -****    NSC8105 only.  Flags are a guess - copied from op_addb_im() */
// actually this is ADDX, causes garbage in nightgal.cpp otherwise
OP_HANDLER( adcx_im )
{
	u16 t,r;
	IMMBYTE(t);
	r = X+t;
	CLR_HNZVC;
	SET_FLAGS8(X,t,r);
	SET_H(X,t,r);
	X = r;
}

/* $ed STD indexed -**0- */
OP_HANDLER( std_ix )
{
	INDEXED;
	CLR_NZV;
	SET_NZ16(D);
	WM16(EAD,&m_d);
}

/* $ee LDX indexed -**0- */
OP_HANDLER( ldx_ix )
{
	IDXWORD(m_x);
	CLR_NZV;
	SET_NZ16(X);
}

/* $ef STX indexed -**0- */
OP_HANDLER( stx_ix )
{
	CLR_NZV;
	SET_NZ16(X);
	INDEXED;
	WM16(EAD,&m_x);
}

/* $f0 SUBB extended ?**** */
OP_HANDLER( subb_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $f1 CMPB extended ?**** */
OP_HANDLER( cmpb_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = B-t;
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
}

/* $f2 SBCB extended ?**** */
OP_HANDLER( sbcb_ex )
{
	u16    t,r;
	EXTBYTE(t);
	r = B-t-(CC&0x01);
	CLR_NZVC;
	SET_FLAGS8(B,t,r);
	B = r;
}

/* $f3 ADDD extended -**** */
OP_HANDLER( addd_ex )
{
	u32 r,d;
	PAIR b;
	EXTWORD(b);
	d = D;
	r = d + b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	D = r;
}

/* $f4 ANDB extended -**0- */
OP_HANDLER( andb_ex )
{
	u8 t;
	EXTBYTE(t);
	B &= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $f5 BITB extended -**0- */
OP_HANDLER( bitb_ex )
{
	u8 t,r;
	EXTBYTE(t);
	r = B & t;
	CLR_NZV;
	SET_NZ8(r);
}

/* $f6 LDB extended -**0- */
OP_HANDLER( ldb_ex )
{
	EXTBYTE(B);
	CLR_NZV;
	SET_NZ8(B);
}

/* $f7 STB extended -**0- */
OP_HANDLER( stb_ex )
{
	CLR_NZV;
	SET_NZ8(B);
	EXTENDED;
	WM(EAD,B);
}

/* $f8 EORB extended -**0- */
OP_HANDLER( eorb_ex )
{
	u8 t;
	EXTBYTE(t);
	B ^= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $f9 ADCB extended ***** */
OP_HANDLER( adcb_ex )
{
	u16 t,r;
	EXTBYTE(t);
	r = B+t+(CC&0x01);
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $fa ORB extended -**0- */
OP_HANDLER( orb_ex )
{
	u8 t;
	EXTBYTE(t);
	B |= t;
	CLR_NZV;
	SET_NZ8(B);
}

/* $fb ADDB extended ***** */
OP_HANDLER( addb_ex )
{
	u16 t,r;
	EXTBYTE(t);
	r = B+t;
	CLR_HNZVC;
	SET_FLAGS8(B,t,r);
	SET_H(B,t,r);
	B = r;
}

/* $fc LDD extended -**0- */
OP_HANDLER( ldd_ex )
{
	EXTWORD(m_d);
	CLR_NZV;
	SET_NZ16(D);
}

/* $fc ADDX extended -****    NSC8105 only.  Flags are a guess */
OP_HANDLER( addx_ex )
{
	u32 r,d;
	PAIR b;
	EXTWORD(b);
	d = X;
	r = d + b.d;
	CLR_NZVC;
	SET_FLAGS16(d,b.d,r);
	X = r;
}

/* $fd STD extended -**0- */
OP_HANDLER( std_ex )
{
	EXTENDED;
	CLR_NZV;
	SET_NZ16(D);
	WM16(EAD,&m_d);
}

/* $fe LDX extended -**0- */
OP_HANDLER( ldx_ex )
{
	EXTWORD(m_x);
	CLR_NZV;
	SET_NZ16(X);
}

/* $ff STX extended -**0- */
OP_HANDLER( stx_ex )
{
	CLR_NZV;
	SET_NZ16(X);
	EXTENDED;
	WM16(EAD,&m_x);
}

/* NSC8105 specific, guessed opcodes (tested by Night Gal Summer) */
// $bb - $mask & [X + $disp8]
OP_HANDLER( btst_ix )
{
	u8 val;
	u8 mask = M_RDOP_ARG(PCD);
	{EA=X+(M_RDOP_ARG(PCD+1));PC+=2;}
	val = RM(EAD) & mask;
	CLR_NZVC; SET_NZ8(val);
}

// $b2 - assuming correct, store first byte to (X + $disp8)
OP_HANDLER( stx_nsc )
{
	IMM8;
	u8 val = RM(EAD);
	IMM8;
	EA = X + RM(EAD);
	CLR_NZV;
	SET_NZ8(val);
	WM(EAD,val);
}



static void (*const hd63701_insn[0x100])(void) = {
op_trap,   op_nop,    op_trap,   op_trap,   op_lsrd,   op_asld,   op_tap,    op_tpa,    // 0
op_inx,    op_dex,    op_clv,    op_sev,    op_clc,    op_sec,    op_cli,    op_sei,
op_sba,    op_cba,    op_undoc1, op_undoc2, op_trap,   op_trap,   op_tab,    op_tba,    // 1
op_xgdx,   op_daa,    op_slp,    op_aba,    op_trap,   op_trap,   op_trap,   op_trap,
op_bra,    op_brn,    op_bhi,    op_bls,    op_bcc,    op_bcs,    op_bne,    op_beq,    // 2
op_bvc,    op_bvs,    op_bpl,    op_bmi,    op_bge,    op_blt,    op_bgt,    op_ble,
op_tsx,    op_ins,    op_pula,   op_pulb,   op_des,    op_txs,    op_psha,   op_pshb,   // 3
op_pulx,   op_rts,    op_abx,    op_rti,    op_pshx,   op_mul,    op_wai,    op_swi,
op_nega,   op_trap,   op_trap,   op_coma,   op_lsra,   op_trap,   op_rora,   op_asra,   // 4
op_asla,   op_rola,   op_deca,   op_trap,   op_inca,   op_tsta,   op_trap,   op_clra,
op_negb,   op_trap,   op_trap,   op_comb,   op_lsrb,   op_trap,   op_rorb,   op_asrb,   // 5
op_aslb,   op_rolb,   op_decb,   op_trap,   op_incb,   op_tstb,   op_trap,   op_clrb,
op_neg_ix, op_aim_ix, op_oim_ix, op_com_ix, op_lsr_ix, op_eim_ix, op_ror_ix, op_asr_ix, // 6
op_asl_ix, op_rol_ix, op_dec_ix, op_tim_ix, op_inc_ix, op_tst_ix, op_jmp_ix, op_clr_ix,
op_neg_ex, op_aim_di, op_oim_di, op_com_ex, op_lsr_ex, op_eim_di, op_ror_ex, op_asr_ex, // 7
op_asl_ex, op_rol_ex, op_dec_ex, op_tim_di, op_inc_ex, op_tst_ex, op_jmp_ex, op_clr_ex,
op_suba_im,op_cmpa_im,op_sbca_im,op_subd_im,op_anda_im,op_bita_im,op_lda_im, op_trap,   // 8
op_eora_im,op_adca_im,op_ora_im, op_adda_im,op_cpx_im ,op_bsr,    op_lds_im, op_trap,
op_suba_di,op_cmpa_di,op_sbca_di,op_subd_di,op_anda_di,op_bita_di,op_lda_di, op_sta_di, // 9
op_eora_di,op_adca_di,op_ora_di, op_adda_di,op_cpx_di ,op_jsr_di, op_lds_di, op_sts_di,
op_suba_ix,op_cmpa_ix,op_sbca_ix,op_subd_ix,op_anda_ix,op_bita_ix,op_lda_ix, op_sta_ix, // A
op_eora_ix,op_adca_ix,op_ora_ix, op_adda_ix,op_cpx_ix ,op_jsr_ix, op_lds_ix, op_sts_ix,
op_suba_ex,op_cmpa_ex,op_sbca_ex,op_subd_ex,op_anda_ex,op_bita_ex,op_lda_ex, op_sta_ex, // B
op_eora_ex,op_adca_ex,op_ora_ex, op_adda_ex,op_cpx_ex ,op_jsr_ex, op_lds_ex, op_sts_ex,
op_subb_im,op_cmpb_im,op_sbcb_im,op_addd_im,op_andb_im,op_bitb_im,op_ldb_im, op_trap,   // C
op_eorb_im,op_adcb_im,op_orb_im, op_addb_im,op_ldd_im, op_trap,   op_ldx_im, op_trap,
op_subb_di,op_cmpb_di,op_sbcb_di,op_addd_di,op_andb_di,op_bitb_di,op_ldb_di, op_stb_di, // D
op_eorb_di,op_adcb_di,op_orb_di, op_addb_di,op_ldd_di, op_std_di, op_ldx_di, op_stx_di,
op_subb_ix,op_cmpb_ix,op_sbcb_ix,op_addd_ix,op_andb_ix,op_bitb_ix,op_ldb_ix, op_stb_ix, // E
op_eorb_ix,op_adcb_ix,op_orb_ix, op_addb_ix,op_ldd_ix, op_std_ix, op_ldx_ix, op_stx_ix,
op_subb_ex,op_cmpb_ex,op_sbcb_ex,op_addd_ex,op_andb_ex,op_bitb_ex,op_ldb_ex, op_stb_ex, // F
op_eorb_ex,op_adcb_ex,op_orb_ex, op_addb_ex,op_ldd_ex, op_std_ex, op_ldx_ex, op_stx_ex
};

#define XX 4 // illegal opcode unknown cycle count
static const u8 cycles_63701[256] =
{
    /* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
  /*0*/ XX, 1,XX,XX, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  /*1*/  1, 1,XX,XX,XX,XX, 1, 1, 2, 2, 4, 1,XX,XX,XX,XX,
  /*2*/  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  /*3*/  1, 1, 3, 3, 1, 1, 4, 4, 4, 5, 1,10, 5, 7, 9,12,
  /*4*/  1,XX,XX, 1, 1,XX, 1, 1, 1, 1, 1,XX, 1, 1,XX, 1,
  /*5*/  1,XX,XX, 1, 1,XX, 1, 1, 1, 1, 1,XX, 1, 1,XX, 1,
  /*6*/  6, 7, 7, 6, 6, 7, 6, 6, 6, 6, 6, 5, 6, 4, 3, 5,
  /*7*/  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 4, 6, 4, 3, 5,
  /*8*/  2, 2, 2, 3, 2, 2, 2,XX, 2, 2, 2, 2, 3, 5, 3,XX,
  /*9*/  3, 3, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3, 4, 5, 4, 4,
  /*A*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
  /*B*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 6, 5, 5,
  /*C*/  2, 2, 2, 3, 2, 2, 2,XX, 2, 2, 2, 2, 3,XX, 3,XX,
  /*D*/  3, 3, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
  /*E*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
  /*F*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5
};


/* ---- the CPU around the opcodes (librdpiano Mcu) */
static RDP_FAST void mcu_enter_interrupt(u16 vector)
{
    int eat;
    if (m_wai_state & M6800_WAI) {
        eat = 4;
        m_wai_state &= ~M6800_WAI;
    } else {
        PUSHWORD(pPC);
        PUSHWORD(pX);
        PUSHBYTE(A);
        PUSHBYTE(B);
        PUSHBYTE(CC);
        eat = 12;
    }
    SEI;
    PCD = RM16(vector);
    increment_counter(eat);
}
static RDP_FAST void mcu_take_trap(void) { mcu_enter_interrupt(0xFFEE); }
static RDP_FAST void mcu_eat_cycles(void) { if (m_icount > 0) increment_counter(m_icount); }

static RDP_FAST void mcu_check_irq_lines(void)
{
    if (m_nmi_pending) {
        m_wai_state &= ~M6800_SLP;
        m_nmi_pending = 0;
        mcu_enter_interrupt(0xFFFC);
    } else if (m_irq_state[M6800_IRQ_LINE] != CLEAR_LINE) {
        m_wai_state &= ~M6800_SLP;
        if (!(CC & 0x10)) mcu_enter_interrupt(0xFFF8);   /* IRQ1: the SA chip */
    } else if ((m_tcsr & (TCSR_EICI | TCSR_ICF)) == (TCSR_EICI | TCSR_ICF)) {
        m_wai_state &= ~M6800_SLP;
        if (!(CC & 0x10)) mcu_enter_interrupt(0xFFF6);   /* ICI: a command waits */
    }
}

static RDP_FAST void mcu_set_input(int line, int state)
{
    if (line == INPUT_LINE_NMI) {
        if (!m_nmi_state && state != CLEAR_LINE) m_nmi_pending = 1;
        m_nmi_state = (u8)state;
    } else if (line == M6801_TIN_LINE) {
        if (state != m_irq_state[M6801_TIN_LINE]) {
            m_irq_state[M6801_TIN_LINE] = (u8)state;
            if (((m_tcsr & TCSR_IEDG) ^ (state == CLEAR_LINE ? TCSR_IEDG : 0)) == 0) return;
            m_tcsr |= TCSR_ICF;                 /* the active edge */
            m_pending_tcsr |= TCSR_ICF;
            m_input_capture = m_counter.w.l;
        }
    } else {
        m_irq_state[line] = (u8)state;
    }
}

static RDP_FAST u8 mcu_read(u32 addr)
{
    addr &= 0xFFFFu;
    if (addr >= 0xC000u) return rdp_prog[(addr - 0xC000u) & 0x1FFFu];
    if (addr >= 0x4000u) {                      /* the parameter ROM, 4 banks of 32 KB by the latch */
        u32 i = (addr - 0x4000u) | ((u32)latch_val & 3u) << 15;
        if (i - 0x8000u < 0x8000u) return rdp_params[i - 0x8000u];
        return i < 3u ? rdp_hdr[i] : 0xFFu;
    }
    if (addr >= 0x2000u) { RDP_LOG("%04x: unk read %04x\n", PCD, addr); return 0xFFu; }
    if (addr >= 0x1000u) return sa_irq_id;      /* the SA chip: which part ended */
    if (addr >= 0x20u) return rdp_ram[addr];
    if (addr == 0x02u) {                        /* port 1: the command bus (librdpiano: works with RD200_B) */
        u8 v = 0xFF;
        if (!rdp_q_empty() && (PCD == 0xE12Bu || PCD == 0xE15Eu || PCD == 0xE168u)) {
            v = rdp_q[rdp_qr & 255u];
            rdp_qr++;
        }
        return v;
    }
    if (addr == 0x03u) return PCD == 0xE15Au ? 0xFF : 0x00;   /* port 2 */
    if (addr == 0x08u) { m_pending_tcsr = 0; return m_tcsr; }
    if (addr == 0x0Du) {
        if (!(m_pending_tcsr & TCSR_ICF)) m_tcsr &= ~TCSR_ICF;
        return (u8)m_input_capture;
    }
    if (addr == 0x0Eu) return (u8)(m_input_capture >> 8);
    RDP_LOG("%04x: unk device read %04x\n", addr, PCD);
    return 0xFF;
}

static RDP_FAST void mcu_write(u32 addr, u8 data)
{
    addr &= 0xFFFFu;
    if (addr == 0x03u) {                        /* port 2: the command was taken */
        mcu_set_input(M6801_TIN_LINE, CLEAR_LINE);
    } else if (addr == 0x08u) {                 /* TCSR */
        data &= 0x1F;
        m_tcsr = data | (m_tcsr & 0xE0);
        m_pending_tcsr &= m_tcsr;
        mcu_check_irq_lines();
    } else if (addr < 0x20u) {
        /* ports 0, 1 and the rest: nothing to do */
    } else if (addr < 0x1000u) {
        rdp_ram[addr] = data;
    } else if (addr < 0x2000u) {
        sa_write(addr - 0x1000u, data);
        if (sa_irq) {
            sa_irq = 0;
            mcu_set_input(M6800_IRQ_LINE, CLEAR_LINE);
        }
    } else {
        latch_val = data;
    }
}

static RDP_FAST void mcu_execute_one(void)
{
    u8 ireg;
    pPPC = pPC;
    ireg = M_RDOP(PCD);
    PC++;
    hd63701_insn[ireg]();
    increment_counter(cycles_63701[ireg]);
}

static RDP_FAST void mcu_run_one(void)                   /* librdpiano Mcu::execute_run */
{
    if (!rdp_q_empty()) mcu_set_input(M6801_TIN_LINE, ASSERT_LINE);
    if (sa_irq) mcu_set_input(M6800_IRQ_LINE, ASSERT_LINE);
    mcu_check_irq_lines();
    mcu_execute_one();
}

/* ---- the API */
static void rdp_push(u8 b)                      /* the main loop only: the one writer */
{
    rdp_q[rdp_qw & 255u] = b;
    rdp_qw++;
}
static int rdp_room(u32 n) { return (u32)(rdp_qw - rdp_qr) + n <= 256u; }   /* (a whole command, or none) */

/* The board's idle work, run natively. RD200_B's main loop spends nearly all its time (94 % of the instructions,
 * idle or playing) in a scan over the voices at E379..E38E:
 *   E379 SEI / LDAA $50,X / BITA #1 / BEQ E386 / LDAB $70,X / BNE E388 / BRA E38F / E386 CLR $70,X /
 *   E388 CLI / INX / CPX $92 / BCS E379 / E38E RTS
 * Emulated, each of those costs as much as any instruction; mcu_fast runs them natively, one at a time with the
 * same registers, flags, RAM and cycles, and every other instruction without the interpreter's per-instruction
 * interrupt checks. Both only while nothing can interrupt the CPU (mcu_quiet: no IRQ, no command waiting; those
 * arrive between samples, or from a port or TCSR write, after which it hands back). The samples come out the same
 * as with the plain interpreter (tests: rdp_exact 1 before and after). rdp_init checks the program bytes. */
static u32 rdp_fast;
static const u8 RDP_SCAN[23] = {0x0F, 0xA6, 0x50, 0x85, 0x01, 0x27, 0x06, 0xE6, 0x70, 0x26, 0x04, 0x20, 0x09,
                                0x6F, 0x70, 0x0E, 0x08, 0x9C, 0x92, 0x25, 0xEB, 0x39, 0x7A};
#define RDP_SCAN_PC 0xE379u

static RDP_FAST int mcu_quiet(void)                      /* nothing can interrupt the CPU before the next sample */
{
    return rdp_q_empty() && !sa_irq && m_irq_state[M6800_IRQ_LINE] == CLEAR_LINE && !m_nmi_pending &&
           (m_tcsr & (TCSR_EICI | TCSR_ICF)) != (TCSR_EICI | TCSR_ICF);
}

static RDP_FAST void mcu_fast(void)                      /* the rest of the sample, while nothing can interrupt the CPU */
{
    u32 x = X, cc = CC;
    while (m_icount > 0) {                      /* (one instruction a turn, as the interpreter: same stops) */
        switch (PC) {
        case 0xE379:                                                                         /* SEI */
            while (m_icount >= 26 && x + 0x70u < 0x1000u) {     /* whole passes at once, while they fit */
                u32 a = rdp_ram[0x50u + x], lim, r;
                int ic = m_icount;
                A = (u8)a;                                      /* SEI, LDAA, BITA: N = 0, Z = !(a & 1) */
                if (!(a & 1u)) {                                /* BEQ, CLR $70,X */
                    rdp_ram[0x70u + x] = 0;
                    ic -= 1 + 4 + 2 + 3 + 5;
                } else {
                    u32 b = rdp_ram[0x70u + x];                 /* LDAB, BNE */
                    B = (u8)b;
                    if (!b) {                                   /* BRA E38F: out (the flags of LDAB) */
                        cc = (cc & 0xF1u) | 0x10u | 4u;
                        m_icount = ic - (1 + 4 + 2 + 3 + 4 + 3 + 3);
                        PC = 0xE38F;
                        goto out;
                    }
                    ic -= 1 + 4 + 2 + 3 + 4 + 3;
                }
                x = (x + 1u) & 0xFFFFu;                         /* CLI, INX, CPX $92: N Z V C; BCS */
                lim = (u32)rdp_ram[0x92] << 8 | rdp_ram[0x93];
                r = x - lim;
                cc = (cc & 0xE0u) | ((r & 0x8000u) >> 12) | ((r & 0xFFFFu) ? 0u : 4u) |
                     (((x ^ lim ^ r ^ (r >> 1)) & 0x8000u) >> 14) | ((r >> 16) & 1u);
                m_icount = ic - (1 + 1 + 4 + 3);
                if (!(cc & 1u)) { PC = 0xE38E; goto next; }
            }
            if (m_icount <= 0) goto out;
            cc |= 0x10u; PC = 0xE37A; m_icount -= 1; continue;
        case 0xE37A: {                                                                       /* LDAA $50,X */
            u32 a;
            if (x + 0x70u >= 0x1000u) break;
            a = rdp_ram[0x50u + x];
            A = (u8)a;
            cc = (cc & 0xF1u) | ((a & 0x80u) >> 4) | (a ? 0u : 4u);
            PC = 0xE37C; m_icount -= 4; continue;
        }
        case 0xE37C: cc = (cc & 0xF1u) | ((A & 1u) ? 0u : 4u); PC = 0xE37E; m_icount -= 2; continue;   /* BITA #1 */
        case 0xE37E: PC = (cc & 4u) ? 0xE386 : 0xE380; m_icount -= 3; continue;              /* BEQ E386 */
        case 0xE380: {                                                                       /* LDAB $70,X */
            u32 b;
            if (x + 0x70u >= 0x1000u) break;
            b = rdp_ram[0x70u + x];
            B = (u8)b;
            cc = (cc & 0xF1u) | ((b & 0x80u) >> 4) | (b ? 0u : 4u);
            PC = 0xE382; m_icount -= 4; continue;
        }
        case 0xE382: PC = (cc & 4u) ? 0xE384 : 0xE388; m_icount -= 3; continue;              /* BNE E388 */
        case 0xE386:                                                                         /* CLR $70,X */
            if (x + 0x70u >= 0x1000u) break;
            rdp_ram[0x70u + x] = 0;
            cc = (cc & 0xF0u) | 4u;
            PC = 0xE388; m_icount -= 5; continue;
        case 0xE388: cc &= ~0x10u; PC = 0xE389; m_icount -= 1; continue;                    /* CLI */
        case 0xE389: x = (x + 1u) & 0xFFFFu; cc = (cc & 0xFBu) | (x ? 0u : 4u); PC = 0xE38A; m_icount -= 1; continue;   /* INX */
        case 0xE38A: {                                                                       /* CPX $92 */
            u32 lim = (u32)rdp_ram[0x92] << 8 | rdp_ram[0x93], r = x - lim;
            cc &= 0xF0u;
            cc |= (r & 0x8000u) >> 12;
            cc |= (r & 0xFFFFu) ? 0u : 4u;
            cc |= ((x ^ lim ^ r ^ (r >> 1)) & 0x8000u) >> 14;
            cc |= (r >> 16) & 1u;
            PC = 0xE38C; m_icount -= 4; continue;
        }
        case 0xE38C: PC = (cc & 1u) ? 0xE379 : 0xE38E; m_icount -= 3; continue;              /* BCS E379 */
        default: break;
        }
        if (0) { next: continue; }
        {   /* any other instruction: the interpreter's, without its interrupt checks (nothing is pending) */
            u8 ireg;
            X = (u16)x; CC = (u8)cc;
            pPPC = pPC;
            ireg = PC >= 0xC000u ? rdp_prog[(PC - 0xC000u) & 0x1FFFu] : mcu_read(PCD);
            PC++;
            hd63701_insn[ireg]();
            increment_counter(cycles_63701[ireg]);
            x = X; cc = CC;
            if (!mcu_quiet()) break;            /* (a port or TCSR write may have raised one: back to the checks) */
        }
    }
out:
    X = (u16)x; CC = (u8)cc;
}

static RDP_FAST s32 rdp_render(void)                     /* one sample; the audio interrupt */
{
    s32 s = sa_update();
    int n = rdp_khz32 ? 62 : 100;
    if (rdp_exact) {
        m_icount += n;                          /* what the last instruction overran is owed */
        if (m_icount > n) m_icount = n;
        while (m_icount > 0) {
            if (rdp_fast && mcu_quiet()) {
                mcu_fast();
                if (m_icount <= 0) break;
            }
            mcu_run_one();
        }
    } else {
        int i;
        for (i = 0; i < n; i++) mcu_run_one();
        m_icount = 0;
    }
    return s;
}

static void rdp_midi(u32 st, u32 d1, u32 d2)   /* librdpiano Mcu::sendMidiCmd: what the board understands */
{
    u32 ty = st >> 4;
    if (!rdp_room(3)) return;                   /* the board is that far behind: drop it rather than garble it */
    if (ty == 0xCu) {
        rdp_push((u8)(0x30u | (d1 & 15u)));
    } else if (ty == 0x8u || (ty == 0x9u && d2 == 0u)) {
        rdp_push(0xB0); rdp_push((u8)d1); rdp_push(0x00);
    } else if (ty == 0x9u) {
        rdp_push(0xC0); rdp_push((u8)d1); rdp_push((u8)d2);
    } else if (ty == 0xBu && d1 == 64u) {
        rdp_push((u8)(0x50u | (d2 >= 64u ? 0xFu : 0u)));
    }
}

static volatile u32 rdp_spare;                  /* the window buffer the render does not read */
static volatile u32 rdp_pending;                /* 1 + patch: unpacked into rdp_win[rdp_spare], for rdp_take */
static volatile u32 rdp_taken;                  /* rdp_take switched: rdp_poll sends the board its reload */
static u32 rdp_cur;

static int rdp_unpack(u32 i, u8 *d)             /* patch i's window -> d; 0 = done */
{
    u32 w = RDP_PATCH[i].win;
    return lz4_unpack(RDP_WIN[w], RDP_WIN_LEN[w], d, 0x8000u) == 0x8000u ? 0 : -1;
}

static void rdp_set(u32 i, const u8 *win)       /* the render's side of a patch change (MCU::loadSounds) */
{
    u32 target = RDP_PATCH[i].off + 0x4000u;
    rdp_params = win;
    rdp_hdr[0] = 0x01;
    rdp_hdr[1] = (u8)(target >> 8);
    rdp_hdr[2] = (u8)target;
    rdp_khz32 = RDP_PATCH[i].khz == 32u;
    rdp_cur = i;
}

static int rdp_select(u32 i)                    /* main loop: 0 = queued (the render takes it), -1 = busy or bad */
{
    if (i >= RDP_NPATCH || rdp_pending || rdp_taken || rdp_unpack(i, rdp_win[rdp_spare])) return -1;
    rdp_pending = 1u + i;
    return 0;
}

static RDP_FAST void rdp_take(void)                      /* audio interrupt, before rendering: a patch change that waits */
{
    u32 p = rdp_pending;
    if (!p) return;
    rdp_set(p - 1u, rdp_win[rdp_spare]);
    rdp_spare ^= 1u;
    rdp_taken = 1;
    rdp_pending = 0;
}

static void rdp_poll(void)                      /* main loop, after rdp_take ran: the board reloads the patch */
{
    if (!rdp_taken || !rdp_room(2)) return;
    rdp_push(0x31);                             /* (rdpiano_juce setCurrentProgram) */
    rdp_push(0x30);
    rdp_taken = 0;
}

static void rdp_reset(void)                     /* librdpiano Mcu::reset */
{
    u32 i;
    m_ppc.d = m_pc.d = m_s.d = m_x.d = m_d.d = 0;
    m_cc = 0xC0;
    m_wai_state = m_nmi_state = m_nmi_pending = 0;
    for (i = 0; i < 5u; i++) m_irq_state[i] = 0;
    SEI;
    PCD = RM16(0xFFFE);
    for (i = 0; i < 1024u * 8u; i++) mcu_execute_one();   /* the board needs a moment before it takes commands */
    m_icount = 0;
}

static int rdp_init(u32 patch)                  /* main loop, before the render runs: 0 = ready */
{
    u32 i;
    for (i = 0; i < sizeof rdp_prog; i++) rdp_prog[i] = RDP_PROG[i];
    rdp_fast = 1;                               /* the scan's bytes, as mcu_scan knows them */
    for (i = 0; i < sizeof RDP_SCAN; i++)
        if (rdp_prog[(RDP_SCAN_PC & 0x1FFFu) + i] != RDP_SCAN[i]) rdp_fast = 0;
    if (patch >= RDP_NPATCH) patch = 0;
    if (rdp_unpack(patch, rdp_win[0])) return -1;
    rdp_set(patch, rdp_win[0]);
    rdp_spare = 1;
    rdp_reset();
    rdp_push(0x30); rdp_push(0xE0); rdp_push(0x00); rdp_push(0x00);   /* the handshake, master tune 0 */
    for (i = 0; i < 1024u; i++) rdp_render();
    rdp_push(0x31); rdp_push(0x30);
    return 0;
}

/* the CPU's macros stay in here */
#undef A
#undef B
#undef BRANCH
#undef CC
#undef CLC
#undef CLH
#undef CLI
#undef CLN
#undef CLR_C
#undef CLR_HNZC
#undef CLR_HNZVC
#undef CLR_NZV
#undef CLR_NZVC
#undef CLR_Z
#undef CLR_ZC
#undef CLV
#undef CLZ
#undef D
#undef DIRBYTE
#undef DIRECT
#undef DIRWORD
#undef EA
#undef EAD
#undef EXTBYTE
#undef EXTENDED
#undef EXTWORD
#undef IDXBYTE
#undef IDXWORD
#undef IMM16
#undef IMM8
#undef IMMBYTE
#undef IMMWORD
#undef INDEXED
#undef M_RDOP
#undef M_RDOP_ARG
#undef NXORC
#undef NXORV
#undef OP_HANDLER
#undef PC
#undef PCD
#undef PULLBYTE
#undef PULLWORD
#undef PUSHBYTE
#undef PUSHWORD
#undef RM
#undef S
#undef SD
#undef SEC
#undef SEH
#undef SEI
#undef SEN
#undef SET_C16
#undef SET_C8
#undef SET_FLAGS16
#undef SET_FLAGS8
#undef SET_FLAGS8D
#undef SET_FLAGS8I
#undef SET_H
#undef SET_N16
#undef SET_N8
#undef SET_NZ16
#undef SET_NZ8
#undef SET_V16
#undef SET_V8
#undef SET_Z
#undef SET_Z16
#undef SET_Z8
#undef SEV
#undef SEZ
#undef SIGNED
#undef WM
#undef X
#undef XX
#undef pD
#undef pPC
#undef pPPC
#undef pS
#undef pX
#undef TCSR_IEDG
#undef TCSR_EICI
#undef TCSR_ICF
