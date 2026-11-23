/* SPDX-License-Identifier: GPL-3.0-only */
/* RDPNO: Roland's SA pianos (the MKS-20 by default; the Rhodes MK-80) on the M-VAVE FM-1, after rdpiano by
 * Giulio Zausa. rdp.c emulates the board (its CPU and the SA chip, from the ROMs: tools/pack_roms.py), fx.c does
 * what the MKS-20 does after it (voice level, equalizer, chorus, tremolo); this file is the panel, laid out after
 * the MKS-20's front. Built on Dinghy (zvenson/dinghy): platform.c does the rest.
 *
 *   PRESETS    the voice (PIANO 1, ...)                     on every page
 *   ALGORITHM  the MIDI receive channel: OMNI or 1..16      on every page
 *   SELECT     the page: HOME -> FX -> ENV; or HOME, FX, ENV themselves
 *   HOME       KNOB 1 voice level, 2 treble level, 3 bass level, 4 the keys' velocity (the FM-1 feels none)
 *   FX         KNOB 1 tremolo rate, 2 tremolo depth, 3 chorus rate, 4 chorus depth (depth 0 = off);
 *              FX again: the chorus type, SPACE-D (rdpiano_juce's Space-D, spaced.c) or BBD (fx.c's own)
 *   ENV        KNOB 1 middle frequency, 2 middle level, 3 band width (the middle equalizer), 4 voices: how many
 *              the board plays (4..10; its own voice stealing among them; fewer = less load: rdp.c rdp_voices)
 *   OCT- OCT+  the octave of the keys
 *   PLAY       the sustain pedal, latched (lit while it is down); CC 64 from MIDI works too
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes, CC 64, program change (the voice), on USB and the TRS jack; the keys go out on USB too
 *   2 CORES    the odd voices of the board's sound chip run on the second core (platform.c, from Clipper); a unit
 *              whose second core fails its test, or a job, runs on one (the screen says 1 CORE)
 * Everything but the pedal is kept in flash (2 s after the last change, once silent).
 *
 * The board runs at 20 kHz (some voices 32 kHz) inside the audio interrupt; a linear interpolation takes it to
 * 44.1 kHz, then fx.c. Integers only (docs/RULES.md). */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#ifndef APP_NAME
#define APP_NAME "RDPNO"
#endif
#include "platform.c"
#define RDP_SPLIT_RUN(fn, arg) cpu1_run((fn), (arg))   /* the odd voices on the second core (platform.c) */
#define RDP_SPLIT_WAIT() cpu1_wait()
#include "rdp.c"
#include "fx.c"

/* ---- the sound: the board, resampled, then the effects (audio interrupt) */
static volatile uint32_t rdp_ready;            /* rdp_init is done: render */
static volatile uint32_t load_now, load_peak;   /* the render's share of the 5.8 ms a block, in 1/1000 */
volatile uint32_t rdp_load_max, rdp_parts_max;  /* the highest since boot (tools/emu_load.sh reads them) */
volatile uint32_t rdp_quiet_min = 16;           /* the brake's deepest since boot (tools/emu_load.sh) */
volatile uint32_t rdp_load_sum, rdp_load_n;     /* and the sum over the blocks from 1.5 s on, for a mean */
/* The overload exit: a render over its block for 0.2 s on end has the main loop starved, and the note-offs that
 * would end it wait there (once the voices hang, it stays: up to switching off). Then ~60 ms of silence: the main
 * loop releases every note and the pedal (rdp_panic_req), the board takes them after. */
#define PANIC_BLOCKS 35u                        /* 35 blocks over 98 %: 0.2 s */
#define PANIC_SILENT 10u                        /* 10 blocks of silence: 58 ms */
static uint32_t panic_n;
#ifndef DINGHY_HOST
static uint32_t over_n;
#endif
static volatile uint32_t rdp_panic_req;
volatile uint32_t rdp_panics;                   /* since boot (tools/emu_load.sh) */
static int32_t rs_prev, rs_cur, rs_prevr, rs_curr;   /* the board's last two samples (after fx_board: two sides) */
static uint32_t rs_frac;                        /* where between them, Q16 */

static RDP_FAST void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, step;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), t;
#endif
    if (!rdp_ready) return;
    rdp_take();                                 /* a voice change: the render pauses until the main loop has done it */
    if (rdp_paused) return;
    if (panic_n) { panic_n--; return; }         /* the overload exit: silence while the main loop releases */
    step = rdp_khz32 ? 47554u : 29721u;         /* 32000 or 20000 * 65536 / 44100 */
    for (i = 0; i < n; i++) {
        int32_t s, sr, l, r;
        while (rs_frac >= 65536u) {             /* the board's next sample */
            rs_frac -= 65536u;
            rs_prev = rs_cur;
            rs_prevr = rs_curr;
            fx_board(rdp_render(), &rs_cur, &rs_curr);   /* Q15; Space-D here, at the board's rate */
        }
        s = rs_prev + (int32_t)(((int64_t)(rs_cur - rs_prev) * (int32_t)rs_frac) >> 16);
        sr = rs_curr == rs_cur && rs_prevr == rs_prev ? s :
             rs_prevr + (int32_t)(((int64_t)(rs_curr - rs_prevr) * (int32_t)rs_frac) >> 16);
        rs_frac += step;
        fx_process(s, sr, &l, &r);
        out[2u * i] += l;
        out[2u * i + 1u] += r;
    }
#ifndef DINGHY_HOST
    t = (fm1_ticks() - t0) / FM1_TICKS_PER_US;  /* us of the 5805 a block has */
    load_now = t * 1000u / 5805u;
    if (load_now > load_peak) load_peak = load_now;
    if (load_now > rdp_load_max && fm1_ms > 1500u) rdp_load_max = load_now;
    if (fm1_ms > 1500u) { rdp_load_sum += load_now; rdp_load_n++; }
    if (rdp_active > rdp_parts_max) rdp_parts_max = rdp_active;
    rdp_brake(load_now);                        /* too much: the faintest parts drop out for a while */
    if (load_now < 980u) over_n = 0;
    else if (++over_n >= PANIC_BLOCKS) {
        over_n = 0;
        panic_n = PANIC_SILENT;
        rdp_panic_req = 1;
        rdp_panics++;
    }
    if (rdp_quiet < rdp_quiet_min && fm1_ms > 1500u) rdp_quiet_min = rdp_quiet;
#endif
}

/* ---- what is kept: the voice, MIDI, the keys, and the MKS-20's edit parameters */
static struct {
    uint8_t patch, ch, vel, oct, page, voices;
    fx_par_t fx;
} set;
static void set_defaults(void)
{
    set.patch = 0; set.ch = 0; set.vel = 100; set.oct = 0; set.page = 0; set.voices = 8;
    set.fx.vol = 15; set.fx.bass = FX_LEVEL0; set.fx.treble = FX_LEVEL0; set.fx.mid_level = FX_LEVEL0; set.fx.mid_freq = 3;
    set.fx.width = 3; set.fx.cho_rate = 8; set.fx.cho_depth = 0; set.fx.trm_rate = 8; set.fx.trm_depth = 0;
    set.fx.cho_type = FX_SPACED;
}
static int set_ok(void)
{
    const fx_par_t *f = &set.fx;
    return set.patch < RDP_NPATCH && set.ch <= 16u && set.vel && set.vel <= 127u && (int8_t)set.oct >= -2 &&
           (int8_t)set.oct <= 2 && set.page < 3u && set.voices >= 4u && set.voices <= 16u && f->vol <= 15u && f->bass < FX_NLEVEL && f->treble < FX_NLEVEL &&
           f->mid_level < FX_NLEVEL && f->mid_freq < FX_NMIDF && f->width < FX_NWIDTH && f->cho_rate &&
           f->cho_rate <= 15u && f->cho_depth <= 15u && f->trm_rate && f->trm_rate <= 15u && f->trm_depth <= 15u &&
           f->cho_type <= FX_SPACED;
}

#define KEY0 53u                                /* key 0 = F3 */
/* At most 10 voices: with the pedal down every key struck keeps one, and 12 (~110 parts) are more than the FM-1
 * computes in time (distortion, and MIDI waits). 8 by default. */
#define RDP_VOICES_MAX 10u
static uint32_t sustain;
/* Everything for the board goes through a queue, in order, and a note-on leaves it no sooner than STAGGER_MS
 * after the one before: keys struck together start a few ms apart, so their loud attacks (the board's busiest
 * moments, and all the parts at full level) do not all land in one audio block. 6 keys: 15 ms from first to last. */
#define STAGGER_MS 3u
#define EVQ 128u
static struct { uint8_t st, d1, d2; } evq[EVQ];
static uint32_t evq_w, evq_r, ev_last_on, ev_any_on;
/* A note-off or the pedal is never lost: when the queue is full it waits as a bit (ev_rel, ev_ped) and goes
 * after the queue; a note-on that does not fit is dropped (and counted). ev_held: the notes on, as sent. */
static uint32_t ev_held[4], ev_rel[4], ev_ped, ev_drops;
static int ev_push(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (evq_w - evq_r >= EVQ) return 0;
    evq[evq_w % EVQ].st = (uint8_t)st;
    evq[evq_w % EVQ].d1 = (uint8_t)d1;
    evq[evq_w % EVQ].d2 = (uint8_t)d2;
    evq_w++;
    return 1;
}
static void ev_pump(void)                       /* main loop: what may go to the board now */
{
    uint32_t i;
    while (evq_r != evq_w) {
        uint32_t st = evq[evq_r % EVQ].st, d1 = evq[evq_r % EVQ].d1, d2 = evq[evq_r % EVQ].d2;
        uint32_t on = st == 0x90u && d2;
        if (on && ev_any_on && fm1_ms - ev_last_on < STAGGER_MS) return;
        if (!rdp_room(3)) return;               /* (the board's own queue is full: next time) */
        rdp_midi(st, d1, d2);
        if (on) { ev_last_on = fm1_ms; ev_any_on = 1; }
        evq_r++;
    }
    for (i = 0; i < 128u; i++)                  /* the note-offs that did not fit, after it */
        if (ev_rel[i >> 5] & 1u << (i & 31u)) {
            if (!rdp_room(3)) return;
            rdp_midi(0x80, i, 0);
            ev_rel[i >> 5] &= ~(1u << (i & 31u));
        }
    if (ev_ped) {
        if (!rdp_room(3)) return;
        rdp_midi(0xB0, 64, ev_ped == 2u ? 127u : 0u);
        ev_ped = 0;
    }
}
static void note_on(uint32_t note, uint32_t vel)
{
    note &= 127u;
    ev_rel[note >> 5] &= ~(1u << (note & 31u));   /* (a release still waiting is overtaken: the key is down again) */
    if (ev_push(0x90, note, vel)) ev_held[note >> 5] |= 1u << (note & 31u);
    else ev_drops++;
}
static void note_off(uint32_t note)
{
    note &= 127u;
    ev_held[note >> 5] &= ~(1u << (note & 31u));
    if (!ev_push(0x80, note, 0)) ev_rel[note >> 5] |= 1u << (note & 31u);
}
static void pedal(uint32_t on)
{
    sustain = on;
    if (!ev_push(0xB0, 64, on ? 127u : 0u)) ev_ped = on ? 2u : 1u;
}
static void release_all(void)                   /* CC 120 / 123, or MIDI in lost something: every note off */
{
    uint32_t i;
    for (i = 0; i < 128u; i++)
        if (ev_held[i >> 5] & 1u << (i & 31u)) note_off(i);
}
static uint32_t midi_lost(void)                 /* messages lost on the way in, since the start */
{
#if FELUCCA_UART
    return um.drops + um.resync + ev_drops;
#else
    return ev_drops;
#endif
}

/* ---- the pages: four knob rows each */
enum { PG_HOME, PG_FX, PG_ENV, PG_N };
enum { V_VOL, V_TREBLE, V_BASS, V_VEL, V_TRATE, V_TDEPTH, V_CRATE, V_CDEPTH, V_MFREQ, V_MLEVEL, V_WIDTH, V_VOICES,
       V_NONE };
static const char *const PAGE_NAME[PG_N] = {"HOME", "FX: TREMOLO / CHORUS", "ENV: MIDDLE EQ"};
static const char *const CHO_NAME[2] = {"FX: TREMOLO / BBD CHORUS", "FX: TREMOLO / SPACE-D"};   /* (FX again) */
static const uint8_t PAGE_ROW[PG_N][4] = {
    {V_VOL, V_TREBLE, V_BASS, V_VEL}, {V_TRATE, V_TDEPTH, V_CRATE, V_CDEPTH}, {V_MFREQ, V_MLEVEL, V_WIDTH, V_VOICES}};
static const char *const VAL_NAME[V_NONE] = {"VOICE LEVEL", "TREBLE LEVEL", "BASS LEVEL", "KEY VELOCITY",
    "TREMOLO RATE", "TREMOLO DEPTH", "CHORUS RATE", "CHORUS DEPTH", "MIDDLE FREQ", "MIDDLE LEVEL", "BAND WIDTH",
    "VOICES"};

static uint8_t *val_ptr(uint32_t v)
{
    switch (v) {
    case V_VOL: return &set.fx.vol;
    case V_TREBLE: return &set.fx.treble;
    case V_BASS: return &set.fx.bass;
    case V_VEL: return &set.vel;
    case V_TRATE: return &set.fx.trm_rate;
    case V_TDEPTH: return &set.fx.trm_depth;
    case V_CRATE: return &set.fx.cho_rate;
    case V_CDEPTH: return &set.fx.cho_depth;
    case V_MFREQ: return &set.fx.mid_freq;
    case V_MLEVEL: return &set.fx.mid_level;
    case V_VOICES: return &set.voices;
    default: return &set.fx.width;
    }
}
static void val_turn(uint32_t v, int32_t e)     /* a knob turned e detents */
{
    static const uint8_t LO[V_NONE] = {0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 4};
    static const uint8_t HI[V_NONE] = {15, FX_NLEVEL - 1u, FX_NLEVEL - 1u, 127, 15, 15, 15, 15, FX_NMIDF - 1u, FX_NLEVEL - 1u,
                                       FX_NWIDTH - 1u, RDP_VOICES_MAX};
    uint8_t *p = val_ptr(v);
    int32_t x = (int32_t)*p + (v == V_VEL ? 4 * e : e);
    *p = (uint8_t)(x < LO[v] ? LO[v] : x > HI[v] ? HI[v] : x);
}

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_db(char *p, uint32_t lvl)      /* 0..14 -> "-10.5" .. "+10.5" in 1.5 dB steps */
{
    int32_t t = ((int32_t)lvl - (int32_t)FX_LEVEL0) * 15;   /* tenths of a dB */
    uint32_t a = (uint32_t)(t < 0 ? -t : t);
    *p++ = t < 0 ? '-' : t > 0 ? '+' : ' ';
    p = put_u(p, a / 10u);
    *p++ = '.';
    *p++ = (char)('0' + a % 10u);
    return put(p, " dB");
}
static void val_text(uint32_t v, char *p)
{
    uint32_t x = *val_ptr(v);
    if (v == V_TREBLE || v == V_BASS || v == V_MLEVEL) p = put_db(p, x);
    else if (v == V_MFREQ) { p = put_u(p, FX_MID_HZ[x]); p = put(p, " Hz"); }
    else if (v == V_WIDTH) p = put_u(p, x + 1u);
    else if ((v == V_TDEPTH || v == V_CDEPTH) && !x) p = put(p, "OFF");
    else p = put_u(p, x);
    *p = 0;
}

/* ---- the screen, as the MKS-20's: the voice on top, the page and its four knobs, then MIDI and the load */
#define C_ACC RGB(90, 200, 230)                 /* the MKS-20's LCD blue */
#define C_LED RGB(255, 70, 50)                  /* its indicator red */
#define C_OFF RGB(110, 110, 110)
#define ROW_Y(i) (82u + 24u * (i))

static void draw_voice(void)
{
    char s[40], *p = s;
    draw_text_box(0, 10, 240, &FONT_S, RDP_NO_ROMS ? "NO ROMS: BUILD IT YOURSELF" : RDP_PATCH[set.patch].name,
                  RDP_NO_ROMS ? C_LED : C_ACC, 1);
    p = put(p, "VOICE "); p = put_u(p, RDP_PATCH[set.patch].num); p = put(p, "  "); p = put(p, RDP_SET_NAME);
    p = put(p, "  "); p = put_u(p, RDP_PATCH[set.patch].khz); p = put(p, "kHz"); *p = 0;
    draw_text_box(0, 32, 240, &FONT_S, s, C_OFF, 1);
}
static void draw_page(uint32_t page)
{
    draw_text_box(0, 56, 240, &FONT_S, page == PG_FX ? CHO_NAME[set.fx.cho_type & 1u] : PAGE_NAME[page], C_LED, 1);
}
static void draw_row(uint32_t i, uint32_t v, int hot)
{
    char s[24] = "", k[4] = "K1";
    k[1] = (char)('1' + i);
    if (v == V_NONE) {
        draw_text_box(0, ROW_Y(i), 240, &FONT_S, "", C_OFF, 0);
        return;
    }
    {
        char lab[24], *p = lab;
        p = put(p, k); p = put(p, " "); p = put(p, VAL_NAME[v]); *p = 0;
        draw_text_box(8, ROW_Y(i), 150, &FONT_S, lab, hot ? C_WHITE : C_OFF, 0);
    }
    val_text(v, s);
    draw_text_box(158, ROW_Y(i), 74, &FONT_S, s, hot ? C_WHITE : C_ACC, 2);
}
static void draw_midi(int32_t oct, uint32_t ch, uint32_t sus, uint32_t lost)
{
    char s[40], *p = s;
    p = put(p, "MIDI RX-CH "); if (ch) p = put_u(p, ch); else p = put(p, "OMNI");
    p = put(p, "  OCT "); *p++ = oct < 0 ? '-' : '+'; *p++ = (char)('0' + (oct < 0 ? -oct : oct));
    if (sus) p = put(p, "  SUS");
    if (lost) { p = put(p, "  E"); p = put_u(p, lost > 99u ? 99u : lost); }   /* MIDI in lost messages */
    else p = put(p, cpu1_on() ? "  2 CORES" : "  1 CORE");
    *p = 0;
    draw_text_box(0, 186, 240, &FONT_S, s, C_OFF, 1);
}
static void draw_load(uint32_t now, uint32_t peak, uint32_t parts)   /* the bar: now; the tick: the peak */
{
    char s[40], *p = s;
    uint32_t w = now > 1000u ? 1000u : now, pk = peak > 1000u ? 1000u : peak;
    cv_begin(240, 8, C_BLACK);
    cv_rect(20, 2, 200, 4, RGB(40, 40, 40));
    cv_rect(20, 2, (int32_t)(w * 200u / 1000u), 4, now > 800u ? C_LED : C_ACC);
    cv_rect(20 + (int32_t)(pk * 199u / 1000u), 0, 2, 8, C_WHITE);
    cv_blit(0, 208);
    p = put(p, "CPU "); p = put_u(p, now / 10u); p = put(p, "%  PEAK "); p = put_u(p, peak / 10u);
    p = put(p, "%  PARTS "); p = put_u(p, parts);
    if (rdp_quiet < 10u) p = put(p, " *");      /* the brake is on (rdp_brake) */
    *p = 0;
    draw_text_box(0, 220, 240, &FONT_S, s, C_OFF, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static uint32_t idle_ok, want_patch = 0xFFu;
static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2) != 0) {
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF0u) continue;              /* clock and transport: nothing for a piano */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;
        if (ty == 0x90u && d2) note_on(d1, d2);
        else if (ty == 0x80u || ty == 0x90u) note_off(d1);
        else if (ty == 0xB0u && d1 == 64u) pedal(d2 >= 64u);
        else if (ty == 0xB0u && (d1 == 120u || d1 == 123u)) release_all();   /* all sound / all notes off */
        else if (ty == 0xC0u) want_patch = d1 % RDP_NPATCH;
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

#if RDP_ROMTEST
#include "romtest.c"                            /* tools/build.py --romtest: the ROM area test, nothing else */
#endif

static void app_main(void)
{
    static const uint8_t KNOB[4] = {ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4};
    static const uint8_t PAGE_BTN[PG_N] = {BTN_HOME, BTN_FX, BTN_ENV};
    uint32_t prev = 0, k, t_change = 0, dirty = 0, t_load = 0, hold = 0, t_hold = 0, hot = V_NONE;
    uint32_t page_drawn = ~0u, patch_drawn = ~0u, midi_drawn = ~0u, load_drawn = ~0u, rows_drawn[4], leds_set = ~0u;
    uint8_t held[27] = {0};
    uint32_t lost_seen = 0;
#if BOARD_SMK37
    uint8_t ext_note[FM1_NKEY] = {0};
    smk_pad_events = 1;                         /* the pads as events: taken and dropped (no use for them yet) */
#endif
    for (k = 0; k < 4u; k++) rows_drawn[k] = ~0u;
    if (settings_load(&set, sizeof set) || !set_ok()) set_defaults();
    if (set.voices > RDP_VOICES_MAX) set.voices = RDP_VOICES_MAX;
    fx_set(&set.fx);
    rdp_voices = set.voices;
    rdp_side = fx_side;                         /* Space-D on the second core, beside the board's CPU */

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 100, 240, &FONT_S, APP_NAME "  v" APP_VER, C_ACC, 1);
#if RDP_ROMTEST
    romtest_main();                             /* (does not return) */
#endif
    if (rdp_init(set.patch)) {                  /* (a broken rdp_rom.h: say so, stay in the main loop) */
#if RDP_NO_ROMS
        draw_text_box(0, 130, 240, &FONT_S, "NO ROMS: BUILD IT YOURSELF", C_LED, 1);
#else
        draw_text_box(0, 130, 240, &FONT_S, "ROM DATA BROKEN", C_LED, 1);
#endif
    } else {
        rs_frac = 65536u;
        rdp_ready = 1;
    }
    lcd_fill(0, 0, 240, 240, C_BLACK);

    for (;;) {
        uint32_t keys, down, up, rel, b, out_ch = set.ch ? set.ch - 1u : 0u, st, fx_dirty = 0;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* (a key stops the note it started, whatever the octave now) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];
#else
            uint32_t vel = set.vel;
#endif
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); note_on(held[k], vel); midi_send(0x90u | out_ch, held[k], vel); }
            if (up >> k & 1u) { note_off(held[k]); midi_send(0x80u | out_ch, held[k], 0); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27 */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); note_on(ext_note[key], vel); midi_send(0x90u | out_ch, ext_note[key], vel); }
            else if (ext_note[key]) { note_off(ext_note[key]); midi_send(0x80u | out_ch, ext_note[key], 0); ext_note[key] = 0; }
        }
        while (smk_pad_take() >= 0) {}          /* (the pads: not used) */
#endif

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_PLAY)) { pedal(!sustain); midi_send(0xB0u | out_ch, 64, sustain ? 127u : 0u); }
        for (k = 0; k < PG_N; k++)
            if (b & (1u << PAGE_BTN[k]) && set.page != k) { set.page = (uint8_t)k; hot = V_NONE; dirty = 1; t_change = fm1_ms; }
            else if (b & (1u << PAGE_BTN[k]) && k == PG_FX) {   /* FX on the FX page: the other chorus */
                set.fx.cho_type ^= 1u;
                fx_dirty = 1; dirty = 1; t_change = fm1_ms;
            }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t pg = ((int32_t)set.page + e) % (int32_t)PG_N;
            set.page = (uint8_t)(pg < 0 ? pg + (int32_t)PG_N : pg);
            hot = V_NONE; dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t p = ((int32_t)set.patch + e) % (int32_t)RDP_NPATCH;
            want_patch = (uint32_t)(p < 0 ? p + (int32_t)RDP_NPATCH : p);
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++)
            if ((e = fm1_enc_take(KNOB[k])) != 0 && PAGE_ROW[set.page][k] != V_NONE) {
                hot = PAGE_ROW[set.page][k];
                val_turn(hot, e);
                if (hot == V_VOICES) rdp_voices = set.voices;
                else if (hot != V_VEL) fx_dirty = 1;
                dirty = 1; t_change = fm1_ms;
            }
        if (fx_dirty) fx_set(&set.fx);

        midi_in();
        if (want_patch != 0xFFu && rdp_ready && rdp_select(want_patch) == 0) {   /* the render takes it next block */
            if (want_patch != set.patch) { set.patch = (uint8_t)want_patch; dirty = 1; t_change = fm1_ms; }
            want_patch = 0xFFu;
        }
        rdp_poll();                             /* a voice change, once the render has paused */
        ev_pump();                              /* notes and the pedal to the board, the note-ons spaced */

        oct = (int8_t)set.oct;
        if ((st = keys | (uint32_t)set.page << 27 | (uint32_t)(oct + 2) << 29 | sustain << 31) != leds_set) {
            for (k = 0; k < 27u; k++)           /* the LEDs: only when something changed (each costs) */
                if (((st ^ leds_set) >> k & 1u) || leds_set == ~0u) fm1_led_key(KEY_LED(k), keys >> k & 1u);
            for (k = 0; k < PG_N; k++)
                fm1_led_key(PAGE_BTN[k], set.page == k);
            fm1_led_key(BTN_OCT_DOWN, oct < 0);
            fm1_led_key(BTN_OCT_UP, oct > 0);
            fm1_led_key(BTN_PLAY, sustain != 0);
            leds_set = st;
        }

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (set.patch != patch_drawn) { patch_drawn = set.patch; draw_voice(); }
        if ((set.page | (uint32_t)set.fx.cho_type << 4) != page_drawn) {
            page_drawn = set.page | (uint32_t)set.fx.cho_type << 4;
            draw_page(set.page);
            for (k = 0; k < 4u; k++) rows_drawn[k] = ~0u;
        }
        for (k = 0; k < 4u; k++) {              /* a row: its value and whether it was the last one turned */
            uint32_t v = PAGE_ROW[set.page][k];
            st = v == V_NONE ? 0xFFFFu : (uint32_t)*val_ptr(v) | v << 8 | (uint32_t)(v == hot) << 12;
            if (st != rows_drawn[k]) { rows_drawn[k] = st; draw_row(k, v, v == hot); }
        }
        if (rdp_panic_req) {                    /* the overload exit (app_audio): every note and the pedal off */
            rdp_panic_req = 0;
            release_all();
            pedal(0);
        }
        if (midi_lost() != lost_seen) {         /* a note-off may have been among them: everything off */
            uint32_t was = sustain;
            lost_seen = midi_lost();
            release_all();
            pedal(0);                           /* (the pedal's release may have been lost too: up, and down */
            if (was) pedal(1);                  /* again as it was last heard) */
        }
        if ((st = (uint32_t)(oct + 2) | set.ch << 4 | sustain << 9 | (uint32_t)cpu1_on() << 10 |
                  (lost_seen > 99u ? 99u : lost_seen) << 11) != midi_drawn) {
            midi_drawn = st;
            draw_midi(oct, set.ch, sustain, lost_seen);
        }
        if (fm1_ms - t_load >= 250u) {          /* the load four times a second, and the highest of the last 2 s */
            uint32_t now = load_now, pk = load_peak, parts = rdp_active;
            load_peak = 0;
            t_load = fm1_ms;
            if (pk >= hold || fm1_ms - t_hold > 2000u) { hold = pk; t_hold = fm1_ms; }
            st = (now | hold << 11 | parts << 22) ^ (uint32_t)(rdp_quiet < 10u) << 31;
            if (st != load_drawn) { load_drawn = st; draw_load(now, hold, parts); }
        }
        idle_ok = 0;

        if (dirty && !rdp_active && fm1_ms - t_change > 2000u) {   /* 2 s after a change, when silent */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
