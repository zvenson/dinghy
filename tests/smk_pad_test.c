/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37 pads (hal/smk_input.h, smk__pad): one hit per strike. Feeds a pad the raw ADC values it reads every 2.4 ms
 * and counts the hit events: a strike that rebounds, a finger resting near the thresholds, a press let go slowly over
 * a noisy reading, and fast repeated strikes (which must all count: a pad re-arms ~45 ms after a strike).
 *
 *   cc -DBOARD_SMK37=1 -Wno-int-to-pointer-cast -Ifirmware/hal -o build/host/smk_pad_test tests/smk_pad_test.c && build/host/smk_pad_test */
#include <stdio.h>
#include <stdlib.h>
#include "smk_input.h"

static int fails;
static uint32_t last_vel;                       /* the velocity of the last hit hits() counted */

static uint32_t hits(void)                      /* the hit events queued since the last call (releases skipped) */
{
    uint32_t n = 0;
    int32_t e;
    while ((e = smk_pad_take()) >= 0)
        if (e & 0x7F) { n++; last_vel = (uint32_t)e & 0x7Fu; }
    return n;
}

/* one reading of every pad a round (2.4 ms), as smk__analog: pad 0 gets v, the others rest */
static void round_of(uint32_t p, uint32_t v, uint32_t q, uint32_t w)
{
    uint32_t i;
    smk_pad_clock++;
    for (i = 0; i < SMK_NPAD; i++)
        smk__pad(i, i == p ? v : i == q ? w : 100u + (i * 7u) % 5u);
}
static void feed(const uint16_t *v, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) round_of(0, v[i], 99, 0); }
static void rest(uint32_t n) { uint32_t i; for (i = 0; i < n; i++) round_of(0, 100u + (i * 7u) % 5u, 99, 0); }

static void expect(const char *what, uint32_t want)
{
    uint32_t got = hits();
    printf("%-58s %u hit%s%s\n", what, got, got == 1u ? "" : "s", got == want ? "  ok" : "  FAIL");
    if (got != want) fails++;
}

int main(void)
{
    static const uint16_t STRIKE[] = {300, 700, 900, 650, 300, 150, 105, 104, 103};
    static const uint16_t REBOUND[] = {140, 260, 180, 110, 104};      /* the rubber comes back up once */
    uint32_t i;
    smk_trim_defaults();
    smk_pad_events = 1;
    for (i = 0; i < SMK_NPAD; i++)
        fm1_in.pad_base[i] = 100;
    rest(50);
    hits();

    feed(STRIKE, sizeof STRIKE / 2u);
    rest(40);
    expect("a clean strike", 1);

    feed(STRIKE, sizeof STRIKE / 2u);
    feed(REBOUND, sizeof REBOUND / 2u);
    rest(40);
    expect("a strike that rebounds 12 ms later", 1);

    feed(STRIKE, sizeof STRIKE / 2u);           /* the rubber comes back up once more, after ~30 ms */
    rest(8);
    feed(REBOUND, sizeof REBOUND / 2u);
    rest(40);
    expect("a strike that rebounds again 30 ms later", 1);

    {   /* a light hit, then the finger rests on the pad: the reading hovers around the thresholds (base + 12 .. 24) */
        static const uint16_t LIGHT[] = {128, 132, 130, 118, 109, 106, 105, 104};
        feed(LIGHT, sizeof LIGHT / 2u);
        for (i = 0; i < 125; i++)               /* 300 ms */
            smk__pad(0, 104u + (uint32_t)(rand() % 24));   /* 104 .. 127 */
        rest(40);
        expect("a light hit, the finger resting near the thresholds", 1);
    }

    feed(STRIKE, 4);                            /* held, then let go slowly over a reading that jitters +-16 */
    for (i = 0; i < 120; i++) {
        uint32_t level = 600u - (i < 100u ? i : 100u) * 5u, noise = (uint32_t)(rand() % 33);   /* down to rest (100) */
        smk__pad(0, level + noise > 16u ? level + noise - 16u : 0u);
    }
    rest(40);
    expect("a press let go slowly, noisy reading", 1);

    feed(STRIKE, 4);                            /* the same with twice the jitter, +-32 */
    for (i = 0; i < 120; i++) {
        uint32_t level = 600u - (i < 100u ? i : 100u) * 5u, noise = (uint32_t)(rand() % 65);
        smk__pad(0, level + noise > 32u ? level + noise - 32u : 0u);
    }
    rest(40);
    expect("a press let go slowly, very noisy reading", 1);

    for (i = 0; i < 8; i++) {                   /* a fast roll: a strike every 50 ms (21 readings; two fingers on */
        feed(STRIKE, sizeof STRIKE / 2u);       /* one pad reach 15-20 a second) */
        rest(21 - sizeof STRIKE / 2u);
    }
    rest(40);
    expect("eight strikes 50 ms apart", 8);

    for (i = 0; i < 6; i++) {                   /* a roll: a strike every 60 ms (25 readings) */
        feed(STRIKE, sizeof STRIKE / 2u);
        rest(25 - sizeof STRIKE / 2u);
    }
    rest(40);
    expect("six strikes 60 ms apart", 6);

    {   /* a slow strike: the rubber takes 6 readings (~14 ms) to its peak; the velocity is the peak's */
        static const uint16_t SLOW[] = {200, 300, 420, 520, 600, 640, 600, 400, 200, 110, 104, 103, 102};
        uint32_t want = smk__pad_vel(0, 640);
        feed(SLOW, sizeof SLOW / 2u);
        rest(40);
        expect("a slow strike", 1);
        printf("%-58s vel %u (the peak's: %u)%s\n", "  ... its velocity", last_vel, want, last_vel == want ? "  ok" : "  FAIL");
        if (last_vel != want) fails++;
    }

    {   /* crosstalk: bottom pad 7 (hardware 14) hit hard, top pad 7 (6, the same mux address) picks a little up */
        static const uint16_t HARD[] = {300, 700, 900, 650, 300, 150, 105, 104, 103, 102, 101, 100};
        static const uint16_t ECHO[] = {110, 140, 170, 160, 130, 108, 104, 103, 102, 101, 100, 100};
        int32_t e;
        uint32_t n = 0, who = 99;
        for (i = 0; i < 12u; i++) round_of(14, HARD[i], 6, ECHO[i]);
        for (i = 0; i < 40u; i++) round_of(99, 0, 99, 0);
        while ((e = smk_pad_take()) >= 0)
            if (e & 0x7F) { n++; who = (uint32_t)e >> 8; }
        printf("%-58s %u hit%s on pad %u%s\n", "a hard hit on pad 14, its echo on pad 6", n, n == 1u ? "" : "s", who,
               n == 1u && who == 14u ? "  ok" : "  FAIL");
        if (n != 1u || who != 14u) fails++;
        for (i = 0; i < 12u; i++) round_of(14, HARD[i], 6, HARD[i] - 40u);   /* two pads at once, alike: both */
        for (i = 0; i < 40u; i++) round_of(99, 0, 99, 0);
        for (n = 0; (e = smk_pad_take()) >= 0;) if (e & 0x7F) n++;
        printf("%-58s %u hits%s\n", "pads 14 and 6 hit together, alike", n, n == 2u ? "  ok" : "  FAIL");
        if (n != 2u) fails++;
    }

    {   /* a light tap: not 1 (the factory curve, 220..950, made every light tap 1) */
        static const uint16_t TAP[] = {150, 175, 190, 170, 130, 104, 103, 102, 101, 100};
        feed(TAP, sizeof TAP / 2u);
        rest(40);
        expect("a light tap (peak 190, the baseline 100)", 1);
        printf("%-58s vel %u%s\n", "  ... its velocity", last_vel, last_vel > 1u ? "  ok" : "  FAIL");
        if (last_vel <= 1u) fails++;
    }

    {   /* the hardest hit reaches 127 (a unit's hardest: up to 984) */
        static const uint16_t FULL[] = {400, 900, 1010, 700, 300, 104, 103, 102, 101, 100};
        feed(FULL, sizeof FULL / 2u);
        rest(40);
        expect("a full hit (peak 1010)", 1);
        printf("%-58s vel %u%s\n", "  ... its velocity", last_vel, last_vel == 127u ? "  ok" : "  FAIL");
        if (last_vel != 127u) fails++;
    }

    printf(fails ? "FAILED\n" : "pads: one hit per strike\n");
    return fails ? 1 : 0;
}
