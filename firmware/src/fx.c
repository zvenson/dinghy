/* SPDX-License-Identifier: GPL-3.0-only */
/* fx.c: what the MKS-20 does after its board, at 44.1 kHz, integers only:
 *   voice level -> bass shelf (100 Hz) -> middle bell (400 Hz..4 kHz, level, width) -> treble shelf (10 kHz),
 *   -10.5..+10.5 dB in 1.5 dB steps, as the MKS-20's (the shelves gentle: fx_tables.h)
 *   -> chorus (a BBD-like modulated delay, the two sides in antiphase) -> tremolo (stereo, in antiphase)
 * or, with the chorus type SPACE-D, rdpiano_juce's Space-D chorus (spaced.c) at the board's rate, before all this
 * (fx_board), and the equalizer on both sides
 * The equalizer is Andrew Simper's linear trapezoidal SVF; its constants are fx_tables.h (tools/gen_fx_tables.py),
 * the middle band's worked out here from them with one 32-bit division. The chorus and the tremolo are new
 * approximations of the MKS-20's (rdpiano_juce's are a DSP emulation too large for the FM-1, and a float LFO).
 *
 *   fx_set(&p)        main loop: the parameters -> the coefficients (the render takes them at its next sample)
 *   fx_board(s, &l, &r)     audio interrupt: one sample of the board's (its scale) -> a pair, Q15 (Space-D or not)
 *   fx_process(l, r, &l, &r) audio interrupt, at 44.1 kHz: the pair in (Q15; l == r unless Space-D), a pair out */
#include "fx_tables.h"
#include "spaced.c"

typedef struct {                                /* the panel's values, as the screen shows them */
    uint8_t vol;                                /* 0..15 */
    uint8_t bass, treble, mid_level;            /* 0..14: -10.5..+10.5 dB in 1.5 dB steps, 7 = 0 dB (FX_LEVEL0) */
    uint8_t mid_freq;                           /* 0..7: FX_MID_HZ, 400 Hz .. 4 kHz */
    uint8_t width;                              /* 0..7: 1..8 on the screen, higher = narrower */
    uint8_t cho_rate, cho_depth;                /* 1..15, 0..15 (0 = off) */
    uint8_t trm_rate, trm_depth;                /* 1..15, 0..15 (0 = off) */
    uint8_t cho_type;                           /* FX_BBD, FX_SPACED */
} fx_par_t;
enum { FX_BBD, FX_SPACED };
/* Space-D's rate and depth for the MKS-20's 1..15 and 0..15: rdpiano_juce's spaceDRateFromMs(1000 / period / 4)
 * over its chorusRateToMsPeriod, and spaceDDepthTable[depth / 15 * 128] (15: the table's last) */
static const int32_t SD_RATE[15] = {46, 90, 136, 183, 230, 276, 323, 371, 415, 469, 508, 565, 607, 655, 711};
static const int32_t SD_DEPTH[16] = {0, 17, 38, 58, 80, 102, 127, 150, 178, 204, 234, 262, 295, 326, 363, 397};

typedef struct { int32_t a1, a2, a3, m0, m1, m2; } svf_c_t;   /* Q24 */
typedef struct {
    svf_c_t bass, mid, treble;
    uint8_t on_bass, on_mid, on_treble;
    int32_t vol;                                /* Q12 */
    uint32_t cho_inc, cho_amp;                  /* LFO step; the delay's swing in 1/256 samples */
    uint32_t trm_inc, trm_depth;                /* LFO step; Q15 */
    uint32_t sd_on;                             /* Space-D instead of the BBD chorus */
    int32_t sd_rate, sd_depth;
} fx_c_t;
static fx_c_t fx_cs[2];
static volatile uint32_t fx_cur;                /* the set the render uses; fx_set writes the other one */

static void svf_load(svf_c_t *c, const int32_t *t)
{
    c->a1 = t[0]; c->a2 = t[1]; c->a3 = t[2]; c->m0 = t[3]; c->m1 = t[4]; c->m2 = t[5];
}

static void fx_set(const fx_par_t *p)
{
    fx_c_t *c = &fx_cs[fx_cur ^ 1u];
    uint32_t lb = p->bass < FX_NLEVEL ? p->bass : FX_LEVEL0, lt = p->treble < FX_NLEVEL ? p->treble : FX_LEVEL0;
    uint32_t lm = p->mid_level < FX_NLEVEL ? p->mid_level : FX_LEVEL0;
    uint32_t f = p->mid_freq < FX_NMIDF ? p->mid_freq : 3u, w = p->width < FX_NWIDTH ? p->width : 3u;
    svf_load(&c->bass, &FX_BASS[6u * lb]);
    svf_load(&c->treble, &FX_TREBLE[6u * lt]);
    {   /* the bell: g = tan(pi f / fs), k = 1 / (Q A); a1 = 1 / (1 + g (g + k)), m1 = k (A^2 - 1) */
        int32_t g = FX_MID_G[f], k = (int32_t)(((int64_t)FX_MID_K[w] * FX_INV_A[lm]) >> 24);
        uint32_t d = (1u << 24) + (uint32_t)(((int64_t)g * (g + k)) >> 24);
        int32_t a1 = (int32_t)((0xFFFFFFFFu / (d >> 8)) << 8);   /* Q16 quotient, to Q24 */
        c->mid.a1 = a1;
        c->mid.a2 = (int32_t)(((int64_t)g * a1) >> 24);
        c->mid.a3 = (int32_t)(((int64_t)g * c->mid.a2) >> 24);
        c->mid.m0 = 1 << 24;
        c->mid.m1 = (int32_t)(((int64_t)k * FX_A2M1[lm]) >> 24);
        c->mid.m2 = 0;
    }
    c->on_bass = lb != FX_LEVEL0;
    c->on_treble = lt != FX_LEVEL0;
    c->on_mid = lm != FX_LEVEL0;
    c->vol = FX_VOL[p->vol & 15u];
    c->cho_inc = FX_CHO_INC[(p->cho_rate ? p->cho_rate : 1u) - 1u];
    c->sd_on = p->cho_type == FX_SPACED && p->cho_depth;
    c->cho_amp = c->sd_on ? 0u : (uint32_t)(p->cho_depth & 15u) * 9u * 256u;   /* up to 135 samples: +-3 ms */
    c->sd_rate = SD_RATE[(p->cho_rate ? p->cho_rate : 1u) - 1u];
    c->sd_depth = SD_DEPTH[p->cho_depth & 15u];
    c->trm_inc = FX_TRM_INC[(p->trm_rate ? p->trm_rate : 1u) - 1u];
    c->trm_depth = (uint32_t)(p->trm_depth & 15u) * 32767u / 15u;
    fx_cur ^= 1u;                               /* (a 32-bit store: the render sees the old set or the new) */
}

typedef struct { int32_t ic1, ic2; } svf_s_t;
static svf_s_t fx_sb, fx_sm, fx_st, fx_sbr, fx_smr, fx_str;   /* (..r: the right side, with Space-D) */

static RDP_FAST int32_t svf(const svf_c_t *c, svf_s_t *s, int32_t v0)
{
    int32_t v3 = v0 - s->ic2;
    int32_t v1 = (int32_t)(((int64_t)c->a1 * s->ic1 + (int64_t)c->a2 * v3) >> 24);
    int32_t v2 = s->ic2 + (int32_t)(((int64_t)c->a2 * s->ic1 + (int64_t)c->a3 * v3) >> 24);
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    return (int32_t)(((int64_t)c->m0 * v0 + (int64_t)c->m1 * v1 + (int64_t)c->m2 * v2) >> 24);
}

static RDP_FAST int32_t fx_sine(uint32_t ph)             /* a full cycle over 2^32 -> -32767..32767 (Dinghy's parabola) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

#define FX_DLEN 1024u                           /* the chorus' delay: 23 ms at 44.1 kHz */
#define FX_BASE (265u * 256u)                   /* its middle: 6 ms, in 1/256 samples */
static int16_t fx_dl[FX_DLEN];
static uint32_t fx_dw, fx_cho_ph, fx_trm_ph;

static RDP_FAST int32_t fx_tap(uint32_t lfo, uint32_t amp)   /* the delay line read at 6 ms +- amp, lfo a triangle */
{
    uint32_t tri = (lfo & 0x80000000u ? ~lfo : lfo) >> 15;   /* 0..65535 */
    int32_t d = (int32_t)FX_BASE + (int32_t)((int64_t)(int32_t)amp * ((int32_t)tri - 32768) >> 15);
    uint32_t i = fx_dw - 1u - ((uint32_t)d >> 8), fr = (uint32_t)d & 255u;
    int32_t a = fx_dl[i & (FX_DLEN - 1u)], b = fx_dl[(i - 1u) & (FX_DLEN - 1u)];
    return a + (((b - a) * (int32_t)fr) >> 8);
}

static uint32_t fx_sd_was;
static RDP_FAST void fx_board(int32_t s, int32_t *l, int32_t *r)
{
    const fx_c_t *c = &fx_cs[fx_cur];
    if (c->sd_on) {
        int32_t sl, sr;
        if (!fx_sd_was) { sd_reset(); fx_sd_was = 1; }   /* (switched on: no old sound in its lines) */
        sd_rate = c->sd_rate;
        sd_depth = c->sd_depth;
        sd_process(s << 5, &sl, &sr);           /* as rdpiano_juce: << 5 in, >> 6 out */
        *l = ((sl >> 8) * 5) >> 3;              /* >> 6, then >> 2 to Q15 as below, and -4 dB: as loud as dry */
        *r = ((sr >> 8) * 5) >> 3;
    } else {
        fx_sd_was = 0;
        *l = *r = s >> 3;                       /* +-2^17 or so -> Q15 with room for a full chord */
    }
}
static RDP_FAST int32_t fx_eq(const fx_c_t *c, svf_s_t *sb, svf_s_t *sm, svf_s_t *st, int32_t x)
{
    x = (x * c->vol) >> 12;
    x <<= 4;                                    /* (4 bits more for the filters' states) */
    if (c->on_bass) x = svf(&c->bass, sb, x);
    if (c->on_mid) x = svf(&c->mid, sm, x);
    if (c->on_treble) x = svf(&c->treble, st, x);
    return x >> 4;
}
static RDP_FAST void fx_process(int32_t xl, int32_t xr, int32_t *l, int32_t *r)
{
    const fx_c_t *c = &fx_cs[fx_cur];
    int32_t yl, yr, x;
    if (c->sd_on) {                             /* (Space-D did the chorus: both sides through the equalizer) */
        yl = fx_eq(c, &fx_sb, &fx_sm, &fx_st, xl);
        yr = fx_eq(c, &fx_sbr, &fx_smr, &fx_str, xr);
        goto tremolo;
    }
    x = fx_eq(c, &fx_sb, &fx_sm, &fx_st, xl);
    if (c->cho_amp) {
        int32_t wl, wr;
        fx_dl[fx_dw & (FX_DLEN - 1u)] = (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
        fx_dw++;
        fx_cho_ph += c->cho_inc;
        wl = fx_tap(fx_cho_ph, c->cho_amp);
        wr = fx_tap(fx_cho_ph + 0x80000000u, c->cho_amp);
        yl = ((x + wl) * 23170) >> 15;          /* dry + wet, -3 dB */
        yr = ((x + wr) * 23170) >> 15;
    } else {
        yl = yr = x;
    }
tremolo:
    if (c->trm_depth) {
        int32_t s = fx_sine(fx_trm_ph), d = (int32_t)c->trm_depth;
        int32_t gl = 32768 - ((d * ((32767 - s) >> 1)) >> 15), gr = 32768 - ((d * ((32767 + s) >> 1)) >> 15);
        fx_trm_ph += c->trm_inc;
        yl = (int32_t)(((int64_t)yl * gl) >> 15);
        yr = (int32_t)(((int64_t)yr * gr) >> 15);
    }
    *l = yl;
    *r = yr;
}
