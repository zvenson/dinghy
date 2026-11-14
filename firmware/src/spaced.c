/* SPDX-License-Identifier: GPL-3.0-only */
/* spaced.c: the "Space-D" chorus of rdpiano_juce (Giulio Zausa, rdpiano_juce/Source/lsp/spaced.cpp, GPL-3.0): a
 * Roland LSP effect program as C, Roland's digital take on the Dimension D. A triangle LFO swings two delay lines
 * in antiphase; their difference goes to the two sides, crossed, over the dry signal, then a fixed filter.
 * Here line for line the same arithmetic (24-bit words, integers only), with two changes for the FM-1:
 *   - the 64 Ki-word external RAM (256 KB) is two rings of 256 words: the program writes two lines at
 *     eramPos and eramPos + 16384 and reads them back 102..203 and 116..217 samples later (SD_A, SD_B below);
 *   - the rate and depth come from the MKS-20's 1..15 / 1..15 through rdpiano_juce's tables (fx.c: fx_set).
 * It runs at the board's rate (20 or 32 kHz), right after it, as in rdpiano_juce.
 *
 *   sd_reset()                     the state as rdpiano_juce's SpaceD::reset()
 *   sd_rate, sd_depth              (fx.c sets them)
 *   sd_process(in, &l, &r)         one sample (the board's, << 5) -> a stereo pair, the same scale */

static int32_t sd_iram[128];                    /* the LSP's internal RAM: 128 words, addressed from bufferPos */
#define SD_RING 256u                            /* (a power of two above 217 + 1) */
static int32_t sd_ea[SD_RING], sd_eb[SD_RING];  /* the external RAM's two lines (words >> 4, as stored) */
static uint32_t sd_bpos, sd_epos;               /* bufferPos (7 bits), eramPos (16 bits) */
static int32_t sd_rate = 46, sd_depth = 397;    /* (fx_set) */
#define SD_LEVEL 127
#define SD_WET 127
#define SD_DRY 127
#define SD_PHASE 8388608
#define SD_PRE1 (3342336 >> 6)
#define SD_PRE2 (8388608 + (3866624 >> 6))

static inline int32_t sd_clamp(int32_t v) { return v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v; }
static inline int32_t sd_sx(int32_t x) { x &= 0xFFFFFF; return (x & 0x800000) ? (int32_t)((uint32_t)x | 0xFF000000u) : x; }
#define RM(o) sd_iram[((uint32_t)(o) + sd_bpos) & 127u]
#define WM(o, v) (sd_iram[((uint32_t)(o) + sd_bpos) & 127u] = (v))

static void sd_reset(void)
{
    uint32_t i;
    for (i = 0; i < 128u; i++) sd_iram[i] = 0;
    for (i = 0; i < SD_RING; i++) sd_ea[i] = sd_eb[i] = 0;
    sd_bpos = 0;
    sd_epos = 0;
}

/* One tap of the line: base = the delay in 1/1024 samples (the program's eramSecondTapOffs before >> 10), read
 * between two words; the LSP's interpolation, verbatim. */
static RDP_FAST int32_t sd_tap(const int32_t *ring, uint32_t off, int32_t base)
{
    int32_t coef = ((base & 0x3FF) << 13) >> 16, t = base >> 10, a, b, acc;
    a = ring[(sd_epos + 1u + (uint32_t)t - off) & (SD_RING - 1u)] << 4;   /* (readMemOffs(123)) */
    b = ring[(sd_epos + 0u + (uint32_t)t - off) & (SD_RING - 1u)] << 4;   /* (readMemOffs(122)) */
    acc = (a * coef) >> 7;
    acc += (b * 32) >> 5;
    return -((b * coef) >> 7) + sd_clamp(acc);
}

/* the swing: |the LFO| * depth, as the program's two multiplications (depth's high and low byte) */
static RDP_FAST int32_t sd_swing(int32_t pre, int32_t lfo)
{
    int32_t a = (lfo * -128) >> 7, acc;
    if (a < 0) a = -a;
    a = sd_clamp(a);
    acc = pre;
    acc += (a * (sd_depth >> 8)) >> 7;
    acc += (int32_t)(((int64_t)a * (sd_depth & 0xFF)) >> 7) >> 8;
    return acc + pre;
}

static RDP_FAST void sd_process(int32_t in, int32_t *outl, int32_t *outr)
{
    int32_t acc, accb, a0, a1, a3, a4, a6, a10, b13, a15, a16, b19, b21, a23, b25, a31, a64, a70, a103, a108;
    int32_t a114, a120, a132, a138, a144, a370, a371, a373, a374, a376, out_r, out_l;

    /* the right input's stage (the program's first half; in = audioInR = audioInL) */
    a0 = (RM(120) * SD_LEVEL) >> 7;
    WM(0x7e, in);
    a1 = (in * 127) >> 7;
    WM(117, sd_clamp(a0));
    a3 = (RM(117) * 127) >> 5;
    WM(117, sd_clamp(a1));
    a4 = (RM(117) * 32) >> 7;
    acc = 128;
    WM(117, sd_clamp(a3));
    acc += (RM(117) * 127) >> 7;
    a6 = acc;
    WM(126, sd_clamp(a4));
    out_r = sd_clamp(a6);
    WM(0x78, out_r);

    /* the two lines' inputs: a little filtering, into the external RAM */
    a10 = (RM(127) * 127) >> 7;
    accb = (RM(126) * 127) >> 7;
    acc = RM(8);
    WM(117, sd_clamp(a10));
    acc += (RM(117) * 127) >> 7;
    b13 = accb;
    acc += ((RM(117) * 192) >> 7) >> 8;
    accb = RM(10);
    a15 = acc;
    WM(117, sd_clamp(b13));
    accb += (RM(117) * 127) >> 7;
    a16 = acc;
    accb += ((RM(117) * 192) >> 7) >> 8;
    sd_ea[sd_epos & (SD_RING - 1u)] = sd_clamp(a15) >> 4;
    WM(117, sd_clamp(a16));
    acc = (RM(117) * -1) >> 7;
    b19 = accb;
    acc += ((RM(117) * 128) >> 7) >> 8;
    acc += RM(8);
    b21 = accb;
    WM(117, sd_clamp(b19));
    accb = (RM(117) * -1) >> 7;
    accb += ((RM(117) * 128) >> 7) >> 8;
    a23 = acc;
    sd_eb[sd_epos & (SD_RING - 1u)] = sd_clamp(b21) >> 4;
    accb += RM(10);
    b25 = accb;
    WM(7, sd_clamp(a23));
    WM(9, sd_clamp(b25));

    /* the LFO and the two taps, in antiphase */
    a31 = RM(22) + sd_rate;
    WM(21, sd_sx(a31));
    a64 = sd_tap(sd_ea, 0u, sd_swing(SD_PRE1, RM(21)));
    WM(16, sd_clamp(a64));
    a70 = RM(21) + SD_PHASE;
    WM(117, sd_sx(a70));
    a103 = sd_tap(sd_eb, 16384u, sd_swing(SD_PRE2, RM(117)));
    WM(17, sd_clamp(a103));

    /* left: tap 1 - tap 2, over the dry, then the filter */
    acc = RM(16);
    acc += (RM(17) * -128) >> 7;
    a108 = acc;
    WM(117, sd_clamp(a108));
    acc = (RM(127) * SD_DRY) >> 7;
    acc += (RM(117) * SD_WET) >> 7;
    a114 = acc;
    acc = (RM(97) * -118) >> 7;
    acc += ((RM(97) * 67) >> 7) >> 8;
    WM(96, sd_clamp(a114));
    acc += RM(96);
    acc += (RM(98) * 117) >> 7;
    acc += ((RM(98) * 189) >> 7) >> 8;
    a120 = acc;
    acc = (RM(98) * -54) >> 7;
    acc += ((RM(98) * 127) >> 7) >> 8;
    WM(97, sd_clamp(a120));
    acc += RM(97);
    acc += (RM(99) * 53) >> 7;
    acc += ((RM(99) * 129) >> 7) >> 8;
    WM(98, sd_clamp(acc));
    WM(121, sd_clamp(acc));

    /* right: tap 2 - tap 1, the same */
    acc = (RM(16) * -128) >> 7;
    acc += RM(17);
    a132 = acc;
    WM(117, sd_clamp(a132));
    acc = (RM(126) * SD_DRY) >> 7;
    acc += (RM(117) * SD_WET) >> 7;
    a138 = acc;
    acc = (RM(101) * -118) >> 7;
    acc += ((RM(101) * 67) >> 7) >> 8;
    WM(100, sd_clamp(a138));
    acc += RM(100);
    acc += (RM(102) * 117) >> 7;
    acc += ((RM(102) * 189) >> 7) >> 8;
    a144 = acc;
    acc = (RM(102) * -54) >> 7;
    acc += ((RM(102) * 127) >> 7) >> 8;
    WM(101, sd_clamp(a144));
    acc += RM(101);
    acc += (RM(103) * 53) >> 7;
    acc += ((RM(103) * 129) >> 7) >> 8;
    WM(102, sd_clamp(acc));
    WM(119, sd_clamp(acc));

    /* the left input's stage */
    a370 = (RM(121) * SD_LEVEL) >> 7;
    WM(0x7e, in);
    a371 = (in * 127) >> 7;
    WM(117, sd_clamp(a370));
    a373 = (RM(117) * 127) >> 5;
    WM(117, sd_clamp(a371));
    a374 = (RM(117) * 32) >> 7;
    acc = 128;
    WM(117, sd_clamp(a373));
    acc += (RM(117) * 127) >> 7;
    a376 = acc;
    WM(126, sd_clamp(a374));
    out_l = sd_clamp(a376);
    WM(0x78, out_l);

    sd_bpos = (sd_bpos - 1u) & 0x7Fu;
    sd_epos = (sd_epos - 1u) & 0xFFFFu;
    *outl = out_l;
    *outr = out_r;
}
#undef RM
#undef WM
