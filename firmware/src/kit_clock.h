/* SPDX-License-Identifier: GPL-3.0-only */
/* Tempo and transport, from OberVave 3.5.4: our own tempo, or a MIDI clock's (USB or the TRS jack), locked to the
 * master so a song in the DAW and your firmware stay together for as long as it plays.
 *
 * Include it after platform.c (dinghy.c shows how). What you get:
 *   in the audio ISR    clock_audio(n) once per app_audio, first: it runs the transport and calls
 *                       CLOCK_TICK(tick, ofs) 24 times a quarter (tick: since START, 0 = the downbeat; ofs: the frame
 *                       in this block where it falls). tick % 24 == 0 is a beat, tick % 6 == 0 a 16th
 *   in the main loop    clock_start(), clock_stop(), clock_toggle(), clock_tap() (tap tempo), clock_poll() (each pass:
 *                       a clock that stopped coming gives the tempo back), clock_bpm10() (the tempo now, x10)
 *   variables           clk_tempo (our BPM, 40..240), clk_sync_int (1: SYNC INTERNAL, MIDI clock and transport are
 *                       ignored), clk_run (the transport runs), clk_ext() (a MIDI clock leads), clk_ui_beat (counts
 *                       the beats: the PLAY light)
 *
 * How the lock works (OberVave #10, #17, #46). The clock's F8 is taken where it arrives (MIDI_RT_HOOK: usb.c,
 * midi_uart.c, in TIMER5), never when the main loop gets round to it, so a screen redraw cannot move the beats. The
 * master's quarter is measured in rendered frames (four quarters, rounded to 48 frames; taken only when it moves by
 * more than 0.3 %, so its jitter cannot wobble the steps). That alone would let our beat slide off the master's
 * (~0.4 s in 5 min), so where the master's quarter falls against our beat is measured too, and each next quarter of
 * ours is stretched or shortened by part of it: a PI loop, at most ~3 % a quarter (no audible lurch). One quarter is
 * latched on each beat: a tempo change inside a beat never puts the ticks already counted off it.
 *
 * START and CONTINUE start the transport from the top (no song position), STOP stops it. Without a master, PLAY on
 * the panel (clock_toggle) runs our own tempo. Costs: a few compares per 16 frames. */
#pragma once

#ifndef CLOCK_TICK
#define CLOCK_TICK(tick, ofs) ((void)(tick), (void)(ofs))
#endif
#define CLK_CTL 16u                             /* the transport's step in the ISR: frames */
#define CLK_PPQN 24u

/* the main loop writes */
static volatile uint32_t clk_tempo = 120;       /* our own tempo, BPM */
static volatile uint32_t clk_sync_int;          /* 1: SYNC INTERNAL (a MIDI clock is ignored); 0: SYNC AUTO */
static volatile uint32_t clk_req;               /* 1 start, 2 stop: taken by the ISR on its next block */
/* TIMER5 (clock_rt) writes, the ISR reads */
static volatile uint32_t clk_ext_quarter;       /* the master's quarter in frames, 0 = no MIDI clock */
static volatile uint32_t clk_ext_lock;          /* the transport came from the master's START: its phase is locked on */
static volatile uint32_t clk_ext_beats;         /* the master's quarters (24 clocks) since its START */
static volatile uint32_t clk_n, clk_t0, clk_last, clk_ext_cnt;
/* the ISR writes, the main loop reads */
static volatile uint32_t clk_frames;            /* frames rendered so far: the MIDI clock is measured in them */
static volatile uint32_t clk_run, clk_tick, clk_ui_beat;

static int clk_ext(void) { return clk_ext_quarter != 0u; }

/* ---- TIMER5: MIDI clock and transport as they arrive (MIDI_RT_HOOK) */
static void clock_rt(uint32_t st)
{
    if (clk_sync_int) return;
    if (st == 0xF8u) {                          /* 24 a quarter */
        if (clk_ext_lock && clk_ext_cnt++ % CLK_PPQN == 0u) clk_ext_beats++;   /* (a master's quarter: the phase lock) */
        if (!clk_n || fm1_ms - clk_last > 250u) { clk_n = 0; clk_t0 = clk_frames; }
        clk_last = fm1_ms;
        if (++clk_n == 25u && !clk_ext_quarter) {   /* no tempo yet: one quarter's guess, so a master that starts its
                                                 * clock with PLAY leads from the second beat, not the fifth */
            uint32_t q = (clk_frames - clk_t0 + 24u) / 48u * 48u;
            if (q > FS / 5u && q < FS * 8u / 5u) clk_ext_quarter = q;
        }
        if (clk_n == 97u) {                     /* four quarters: the tempo */
            uint32_t q = ((clk_frames - clk_t0) / 4u + 24u) / 48u * 48u, d;
            if (q > FS / 5u && q < FS * 8u / 5u) {
                d = q > clk_ext_quarter ? q - clk_ext_quarter : clk_ext_quarter - q;
                if (!clk_ext_quarter || d * 1000u > 3u * q) clk_ext_quarter = q;
            }
            clk_n = 1; clk_t0 = clk_frames;
        }
    } else if (st == 0xFAu || st == 0xFBu) {    /* START, CONTINUE: from the top; the next clock is the downbeat */
        clk_n = 0;
        clk_ext_lock = 1; clk_ext_cnt = 0;
        clk_req = 1;
    } else if (st == 0xFCu) {                   /* STOP */
        clk_ext_lock = 0;
        clk_req = 2;
    }
}

/* ---- the ISR: the beat, the lock, the ticks */
static uint32_t clk_q;                          /* the quarter latched on this beat, frames (0: stopped) */
static uint32_t clk_beat_cnt, clk_tick_in;      /* frames into this beat; the next tick in it (0..23) */
static uint32_t clk_seen, clk_q0;
static int32_t clk_err, clk_i;                  /* frames, + = our beat came early */

static int32_t clk_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a quarter in frames: the master's, else our tempo rounded to 48 frames (~1 ms), so 1/2, 1/3, 1/4, 1/6, 1/8, 1/12,
 * 1/16 (and 1/24) of it come out whole and nothing drifts off the beat (the tempo is off by at most 0.1 % for it) */
static uint32_t clock_quarter(void)
{
    uint32_t bpm = clk_tempo < 40u ? 40u : clk_tempo > 240u ? 240u : clk_tempo;
    if (clk_ext_quarter) return clk_ext_quarter;
    return (60u * FS / bpm + 24u) / 48u * 48u;
}

static void clk_measure(void)                   /* a master's quarter began: how far off our beat is */
{
    uint32_t q = clk_q ? clk_q : clock_quarter();
    if (clk_seen == clk_ext_beats) return;
    clk_seen = clk_ext_beats;
    if (clk_ext_lock && clk_ext_quarter)
        clk_err = clk_beat_cnt < q / 2u ? (int32_t)clk_beat_cnt : (int32_t)clk_beat_cnt - (int32_t)q;
}

static void clk_latch(void)                     /* on our beat: the quarter that starts now */
{
    int32_t q = (int32_t)clk_ext_quarter, c = 0, lim;
    clk_q = clock_quarter();
    if (!q) { clk_err = clk_i = 0; return; }
    lim = q / 64;
    if (clk_ext_quarter != clk_q0) {            /* measured again: a small step is the measurement's own jitter (the
                                                 * quarter goes on as it was), a big one a new tempo (its offset new) */
        int32_t d = (int32_t)clk_q0 - q;
        clk_i = clk_q0 && d >= -lim && d <= lim ? clk_i + d : 0;
        clk_q0 = clk_ext_quarter;
    }
    if (clk_ext_lock) {
        clk_i = clk_clamp(clk_i + clk_err / 8, -lim, lim);
        lim = q / 32;
        c = clk_clamp(clk_i + clk_err / 2, -lim, lim);
    }
    clk_err = 0;
    clk_q = (uint32_t)(q + c);
}

static void clk_block(uint32_t ofs)             /* CLK_CTL frames from ofs in this audio block */
{
    uint32_t q, r = clk_req;
    if (r) {
        clk_req = 0;
        if (r == 1u) {                          /* start: tick 0 in this block */
            clk_run = 1; clk_tick = 0; clk_beat_cnt = 0; clk_tick_in = 0;
            clk_q = clk_q0 = 0; clk_err = clk_i = 0; clk_seen = clk_ext_beats;
        } else clk_run = 0;
    }
    if (!clk_run) { clk_q = 0; return; }
    clk_measure();
    if (clk_beat_cnt < CLK_CTL) {
        clk_latch();
#ifdef TRACE
        TRACE('B', clk_tick / CLK_PPQN, clk_frames - clk_beat_cnt);   /* (tests/clock_sync_test.py: the beat's frame) */
#endif
        clk_ui_beat++;
    }
    q = clk_q;
    while (clk_tick_in < CLK_PPQN) {            /* the ticks of this beat that fall in these frames */
        uint32_t at = clk_tick_in * q / CLK_PPQN;
        if (at >= clk_beat_cnt + CLK_CTL) break;
        CLOCK_TICK(clk_tick, ofs + (at > clk_beat_cnt ? at - clk_beat_cnt : 0u));
        clk_tick++;
        clk_tick_in++;
    }
    clk_beat_cnt += CLK_CTL;
    if (clk_beat_cnt >= q) {                    /* the next beat (a DAW's quarter is any length) */
        clk_beat_cnt = clk_beat_cnt - q < CLK_CTL ? clk_beat_cnt - q : 0u;
        while (clk_tick_in < CLK_PPQN) { CLOCK_TICK(clk_tick, ofs + CLK_CTL - 1u); clk_tick++; clk_tick_in++; }   /* (none left, unless q shrank) */
        clk_tick_in = 0;
    }
}

static void clock_audio(uint32_t n)             /* the audio ISR, once per block, before you render */
{
    uint32_t f;
    for (f = 0; f < n; f += CLK_CTL) {
        clk_block(f);
        clk_frames += CLK_CTL;
    }
}

/* ---- the main loop */
static void clock_start(void) { clk_req = 1; }
static void clock_stop(void) { clk_req = 2; }
static void clock_toggle(void) { clk_req = clk_run ? 2u : 1u; }

static void clock_set_sync(uint32_t internal)   /* SYNC INTERNAL (1) or AUTO (0) */
{
    clk_sync_int = internal ? 1u : 0u;
    if (internal) { clk_ext_quarter = 0; clk_ext_lock = 0; clk_n = 0; }
}

static void clock_poll(void)                    /* each pass: a master that went quiet gives the tempo back */
{
    if (clk_ext_quarter && fm1_ms - clk_last > 1000u) { clk_ext_quarter = 0; clk_ext_lock = 0; clk_n = 0; }
}

static uint32_t clock_bpm10(void)               /* the tempo now, BPM x 10 */
{
    uint32_t q = clk_ext_quarter;
    return q ? (600u * FS + q / 2u) / q : clk_tempo * 10u;
}

static void clock_tap(void)                     /* tap tempo: the mean of the last taps (a pause > 2 s starts again) */
{
    static uint32_t t_last, sum, n;
    uint32_t d = fm1_ms - t_last;
    t_last = fm1_ms;
    if (d > 2000u || d < 150u) { sum = n = 0; return; }
    if (n == 4u) { sum -= sum / 4u; n = 3; }
    sum += d; n++;
    d = sum / n;
    clk_tempo = (60000u + d / 2u) / d;
    if (clk_tempo < 40u) clk_tempo = 40u;
    if (clk_tempo > 240u) clk_tempo = 240u;
}
