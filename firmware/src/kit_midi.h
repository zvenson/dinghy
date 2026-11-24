/* SPDX-License-Identifier: GPL-3.0-only */
/* MIDI in, the basics as OberVave has them: the channel, notes, pitch bend (slewed), the mod wheel, volume (CC 7),
 * the sustain pedal (CC 64), all notes off (CC 120 / 123), program change, and every other CC handed on.
 *
 * Define before the #include (all but MIDI_NOTE optional):
 *   MIDI_NOTE(on, note, vel)   a note starts (on 1, vel 1..127) or ends (on 0); the pedal is already applied
 *   MIDI_CC(n, v)              a CC this file does not take itself (not 1, 7, 64, 120, 123)
 *   MIDI_PROGRAM(n)            a program change, 0..127
 *   MIDI_PANIC()               all notes off: silence everything now (MIDI_NOTE(0, ..) came for the held ones)
 * Then:
 *   midi_ch                    0 OMNI (in: every channel, out: 1), 1..16 (keep it in your settings)
 *   midi_out_ch()              the channel your notes go out on (0..15)
 *   midi_poll(notes_only)      each main-loop pass (notes_only 0), and from LCD_IDLE (1: a program change waits for
 *                              the main loop, it may load a sound); clock and transport go to kit_clock.h by the hook
 *   key_note(on, note, vel)    your panel's notes through the same pedal (and out over USB on midi_out_ch)
 *   midi_src                   where the last note came from: 1 USB, 2 the TRS jack (0 the panel)
 *   midi_bend                  -8192..8191 as it came; bend_q12() in the ISR: the bend slewed, in 1/4096 semitone
 *                              (BEND_RANGE semitones at the end, default 2), pitch_q16() makes a frequency factor
 *   midi_mod, midi_vol         0..127 (the mod wheel; CC 7, 127 at power-on: midi_vol_q15() to scale your output)
 *   midi_sustain               the pedal is down */
#pragma once

#ifndef BEND_RANGE
#define BEND_RANGE 2                            /* semitones at the end of the wheel */
#endif

static uint8_t midi_ch;                         /* 0 OMNI, 1..16 */
static uint32_t midi_src;
static volatile int32_t midi_bend;              /* -8192..8191 */
static volatile uint32_t midi_mod, midi_vol = 127, midi_sustain;
static uint32_t mk_sus[4];                      /* notes let go while the pedal was down: bit n */
static uint8_t mk_back[4];                      /* a message left for the main loop (LCD_IDLE met a program change) */

static uint32_t midi_out_ch(void) { return midi_ch ? midi_ch - 1u : 0u; }

static void mk_note(uint32_t on, uint32_t note, uint32_t vel)   /* through the pedal */
{
    uint32_t bit = 1u << (note & 31u), *w = &mk_sus[note >> 5 & 3u];
    if (on) {
        *w &= ~bit;                             /* (struck again while held by the pedal: it plays anew) */
        MIDI_NOTE(1, note, vel);
    } else if (midi_sustain) *w |= bit;
    else MIDI_NOTE(0, note, 0);
}
static void mk_pedal(uint32_t down)
{
    uint32_t i, k;
    midi_sustain = down;
    if (down) return;
    for (i = 0; i < 4u; i++)                    /* let go: what it held ends now */
        for (k = 0; k < 32u; k++)
            if (mk_sus[i] >> k & 1u) { mk_sus[i] &= ~(1u << k); MIDI_NOTE(0, i * 32u + k, 0); }
}
static void mk_panic(void)
{
    mk_sus[0] = mk_sus[1] = mk_sus[2] = mk_sus[3] = 0;
    midi_sustain = 0;
#ifdef MIDI_PANIC
    MIDI_PANIC();
#endif
}

static void key_note(uint32_t on, uint32_t note, uint32_t vel)   /* a note from the panel: sounds, and goes out */
{
    note &= 127u;
    midi_send((on ? 0x90u : 0x80u) | midi_out_ch(), note, on ? vel : 0u);
    mk_note(on, note, vel);
}

static void midi_poll(uint32_t notes_only)
{
    uint8_t st, d1, d2;
    int src = 1;
    for (;;) {
        uint32_t ty;
        if (mk_back[3]) { st = mk_back[0]; d1 = mk_back[1]; d2 = mk_back[2]; src = mk_back[3]; mk_back[3] = 0; }
        else if ((src = midi_get(&st, &d1, &d2)) == 0) break;
        if (st >= 0xF0u) continue;              /* (clock and transport: kit_clock.h took them as they came) */
        if (midi_ch && (st & 15u) != midi_ch - 1u) continue;   /* not our channel */
        ty = st & 0xF0u;
        if (notes_only && ty == 0xC0u) {        /* a program change loads a sound: not in the middle of a draw */
            mk_back[0] = st; mk_back[1] = d1; mk_back[2] = d2; mk_back[3] = (uint8_t)src;
            break;
        }
        if (ty == 0x90u && d2) { mk_note(1, d1, d2); midi_src = (uint32_t)src; }
        else if (ty == 0x80u || ty == 0x90u) mk_note(0, d1, 0);
        else if (ty == 0xE0u) midi_bend = (int32_t)(d1 | d2 << 7) - 8192;
        else if (ty == 0xB0u) {
            if (d1 == 1u) midi_mod = d2;
            else if (d1 == 7u) midi_vol = d2;
            else if (d1 == 64u) mk_pedal(d2 >= 64u);
            else if (d1 == 120u || d1 == 123u) {
                uint32_t k;
                for (k = 0; k < 128u; k++) MIDI_NOTE(0, k, 0);   /* (cheap: a voice that does not play it ignores it) */
                mk_panic();
            }
#ifdef MIDI_CC
            else MIDI_CC(d1, d2);
#endif
        }
#ifdef MIDI_PROGRAM
        else if (ty == 0xC0u) MIDI_PROGRAM(d1);
#endif
    }
}

/* ---- the ISR's side */
static int32_t mk_bend_sm;                      /* Q8 of 1/4096 semitone */
static int32_t bend_q12(void)                   /* once per audio block: the bend, slewed over ~10 ms (a wheel in coarse
                                                 * 7-bit steps glides instead of stepping) */
{
    int32_t d = (midi_bend * BEND_RANGE) << 7;  /* 8192 -> BEND_RANGE * 4096 << 8 */
    mk_bend_sm += (d - mk_bend_sm) >> 1;
    if (mk_bend_sm - d < 256 && d - mk_bend_sm < 256) mk_bend_sm = d;
    return mk_bend_sm >> 8;
}

/* 2^(s / 12) in Q16 for s in 1/4096 semitone (-8 .. +8 octaves): a semitone table, linear between its steps
 * (at most ~0.04 %, under a cent) */
static uint32_t pitch_q16(int32_t s)
{
    static const uint32_t SEMI[13] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772,
                                      123715, 131072};
    int32_t oct = 0;
    uint32_t k, f, r;
    while (s < 0) { s += 12 * 4096; oct--; }
    while (s >= 12 * 4096) { s -= 12 * 4096; oct++; }
    k = (uint32_t)s >> 12; f = (uint32_t)s & 4095u;
    r = SEMI[k] + (((SEMI[k + 1u] - SEMI[k]) * f) >> 12);
    return oct >= 0 ? r << oct : r >> -oct;
}

static int32_t midi_vol_q15(void) { return (int32_t)((midi_vol * midi_vol * 32767u) / (127u * 127u)); }   /* (a square law, as OberVave) */
