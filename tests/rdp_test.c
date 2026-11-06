/* SPDX-License-Identifier: GPL-3.0-only */
/* The C port (firmware/src/rdp.c) on a computer: tests/rdp_script.h -> raw samples (s32, the board's rate).
 *   cc -O2 -Ifirmware/src -Ifirmware/gen -o build/host/rdp_test tests/rdp_test.c
 *   build/host/rdp_test OUT.raw [exact 0|1] [quiet 0..16] [voices 1..16]      (tests/rdp_compare.sh runs it; DINGHY_PERF-like counts: perf stat) */
#include <stdio.h>
#include <stdlib.h>
#define RDP_POOL
#ifdef RDP_TEST_SPLIT                           /* the second core's half, run right away (its results stay apart) */
#define RDP_SPLIT_RUN(fn, arg) ((fn)(arg), 1)
#define RDP_SPLIT_WAIT() 0
#endif
#include "rdp.c"
#include "rdp_script.h"

int main(int argc, char **argv)
{
    FILE *f;
    unsigned t, e = 0;
    long long peak = 0, act = 0, act_max = 0;
    if (argc < 2) { fprintf(stderr, "usage: rdp_test OUT.raw [exact]\n"); return 2; }
    rdp_exact = argc > 2 ? (u32)atoi(argv[2]) : 1u;
    rdp_quiet = argc > 3 ? (u32)atoi(argv[3]) : 16u;   /* (16: every part, as librdpiano; the firmware: 10) */
    rdp_voices = argc > 4 ? (u32)atoi(argv[4]) : 16u;  /* (the board's 16; fewer: rdp.c rdp_voices) */
    if (rdp_init(0)) { fprintf(stderr, "rdp_init failed\n"); return 1; }
    f = fopen(argv[1], "wb");
    if (!f) return 1;
    for (t = 0; t < RDP_SCRIPT_END; t++) {
        s32 s;
        for (; e < RDP_SCRIPT_N && RDP_SCRIPT[e].t == t; e++) {
            if (RDP_SCRIPT[e].patch >= 0) {
                if (rdp_select((u32)RDP_SCRIPT[e].patch % RDP_NPATCH)) { fprintf(stderr, "rdp_select failed\n"); return 1; }
            } else {
                rdp_midi(RDP_SCRIPT[e].st, RDP_SCRIPT[e].d1, RDP_SCRIPT[e].d2);
            }
        }
        rdp_take();
        rdp_poll();
        s = rdp_render();
        if (llabs(s) > peak) peak = llabs(s);
        act += rdp_active;
        if (rdp_active > act_max) act_max = rdp_active;
        fwrite(&s, 4, 1, f);
    }
    fclose(f);
    printf("rdp_test: %u samples, peak %lld, parts sounding: mean %.1f, max %lld (of 160)\n", t, peak,
           (double)act / t, act_max);
    return 0;
}
