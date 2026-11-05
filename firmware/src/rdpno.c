/* SPDX-License-Identifier: GPL-3.0-only */
/* RDPNO: Roland's SA pianos (the MK-80 by default; MKS-20) on the M-VAVE FM-1, after rdpiano by Giulio Zausa.
 * rdp.c emulates the board (its CPU and the SA chip, from the ROMs: tools/pack_roms.py); this file plays it.
 * Built on Dinghy (zvenson/dinghy): platform.c does the rest (updates, USB rescue, MIDI, USB audio, the panel).
 *
 *   keys       play; the notes go out over USB-MIDI too, on the MIDI channel
 *   PRESETS    the patch (CLASSIC, SPECIAL, ...: the set pack_roms.py packed)   (kept in flash)
 *   SELECT     the MIDI channel: OMNI (in: all, out: 1) or 1..16                (kept in flash)
 *   KNOB 1     the velocity the keys play with (the FM-1's keys feel none)       (kept in flash)
 *   OCT- OCT+  the octave                                                        (kept in flash)
 *   PLAY       the sustain pedal, latched (lit while held down); CC 64 from MIDI works too
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes, CC 64 (sustain), program change (the patch)
 *   screen     the patch, the CPU load of the sound (now and its peak), the parts sounding (of 160)
 *
 * The board runs at 20 kHz (two MK-80 patches at 32 kHz) inside the audio interrupt; a linear interpolation takes
 * it to 44.1 kHz. Integers only (docs/RULES.md). */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#ifndef APP_NAME
#define APP_NAME "RDPNO"
#endif
#include "platform.c"
#include "rdp.c"

/* ---- the sound: the board, resampled (audio interrupt) */
static volatile uint32_t rdp_ready;            /* rdp_init is done: render */
static volatile uint32_t load_now, load_peak;   /* the render's share of the 5.8 ms a block, in 1/1000 */
static int32_t rs_prev, rs_cur;                 /* the board's last two samples */
static uint32_t rs_frac;                        /* where between them, Q16 */

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, step;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), t;
#endif
    if (!rdp_ready) return;
    rdp_take();                                 /* a patch change the main loop has unpacked */
    step = rdp_khz32 ? 47554u : 29721u;         /* 32000 or 20000 * 65536 / 44100 */
    for (i = 0; i < n; i++) {
        int32_t s;
        while (rs_frac >= 65536u) {             /* the board's next sample */
            rs_frac -= 65536u;
            rs_prev = rs_cur;
            rs_cur = rdp_render() >> 3;         /* +-2^17 or so -> Q15 with room for a full chord */
        }
        s = rs_prev + (int32_t)(((int64_t)(rs_cur - rs_prev) * (int32_t)rs_frac) >> 16);
        rs_frac += step;
        out[2u * i] += s;
        out[2u * i + 1u] += s;
    }
#ifndef DINGHY_HOST
    t = (fm1_ticks() - t0) / FM1_TICKS_PER_US;  /* us of the 5805 a block has */
    load_now = t * 1000u / 5805u;
    if (load_now > load_peak) load_peak = load_now;
#endif
}

/* ---- the settings */
static struct { uint8_t patch, ch, vel, oct; } set = {0, 0, 100, 0};
#define KEY0 53u                                /* key 0 = F3 */
static uint32_t sustain, last_note = 255, held_notes;   /* held_notes: bit k, key k sounds (the screen) */

static void note_on(uint32_t note, uint32_t vel) { rdp_midi(0x90, note, vel); last_note = note; }
static void note_off(uint32_t note) { rdp_midi(0x80, note, 0); }
static void pedal(uint32_t on)
{
    sustain = on;
    rdp_midi(0xB0, 64, on ? 127u : 0u);
}

/* ---- the screen */
#define C_ACC RGB(255, 150, 60)                 /* a warm orange: an old Rhodes' panel light */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void draw_patch(void)
{
    char s[40], *p = s;
    p = put_u(p, set.patch + 1u); p = put(p, "/"); p = put_u(p, RDP_NPATCH); *p = 0;
    draw_text_box(0, 58, 240, &FONT_S, s, C_OFF, 1);
    draw_text_box(0, 80, 240, &FONT_S, RDP_PATCH[set.patch].name, C_WHITE, 1);
    p = s; p = put(p, RDP_SET_NAME); p = put(p, "  "); p = put_u(p, RDP_PATCH[set.patch].khz); p = put(p, " kHz"); *p = 0;
    draw_text_box(0, 102, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_load(uint32_t now, uint32_t peak, uint32_t parts)   /* the bar: now; the tick: the peak */
{
    char s[40], *p = s;
    uint32_t w = now > 1000u ? 1000u : now, pk = peak > 1000u ? 1000u : peak;
    cv_begin(240, 10, C_BLACK);
    cv_rect(20, 2, 200, 6, RGB(40, 40, 40));
    cv_rect(20, 2, (int32_t)(w * 200u / 1000u), 6, now > 800u ? RGB(255, 60, 60) : C_ACC);
    cv_rect(20 + (int32_t)(pk * 199u / 1000u), 0, 2, 10, C_WHITE);
    cv_blit(0, 132);
    p = put(p, "CPU "); p = put_u(p, now / 10u); p = put(p, "%  PEAK "); p = put_u(p, peak / 10u);
    p = put(p, "%  PARTS "); p = put_u(p, parts); *p = 0;
    draw_text_box(0, 146, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k is down */
{
    uint32_t k, w = 0;
    cv_begin(240, 40, C_BLACK);
    for (k = 0; k < 27u; k++)
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 40, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 24, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, 170);
}

static void draw_status(int32_t oct, uint32_t ch, uint32_t vel, uint32_t sus)
{
    char s[48], *p = s;
    p = put(p, "OCT "); *p++ = oct < 0 ? '-' : '+'; *p++ = (char)('0' + (oct < 0 ? -oct : oct));
    p = put(p, "  VEL "); p = put_u(p, vel);
    p = put(p, ch ? "  CH " : "  OMNI"); if (ch) p = put_u(p, ch);
    if (sus) p = put(p, "  SUS");
    *p = 0;
    draw_text_box(0, 218, 240, &FONT_S, s, C_OFF, 1);
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
        else if (ty == 0xC0u) want_patch = d1 % RDP_NPATCH;
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, t_load = 0, hold = 0, t_hold = 0;
    uint32_t keys_drawn = ~0u, status_drawn = ~0u, patch_drawn = ~0u, load_drawn = ~0u;
    uint8_t held[27] = {0};
#if BOARD_SMK37
    uint8_t ext_note[FM1_NKEY] = {0};
    smk_pad_events = 1;                         /* the pads as events: taken and dropped (no use for them yet) */
#endif
    if (settings_load(&set, sizeof set) || set.patch >= RDP_NPATCH || set.ch > 16u || !set.vel || set.vel > 127u ||
        (int8_t)set.oct < -2 || (int8_t)set.oct > 2)
        set.patch = 0, set.ch = 0, set.vel = 100, set.oct = 0;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 14, 240, &FONT_S, APP_NAME "  v" APP_VER, C_ACC, 1);
    draw_text_box(0, 34, 240, &FONT_S, "SA PIANO, AFTER RDPIANO", C_OFF, 1);
    if (rdp_init(set.patch)) {                  /* (a broken rdp_rom.h: say so, stay in the main loop) */
        draw_text_box(0, 100, 240, &FONT_S, "ROM DATA BROKEN", RGB(255, 60, 60), 1);
    } else {
        rs_frac = 65536u;
        rdp_ready = 1;
    }

    for (;;) {
        uint32_t keys, down, up, rel, b, out_ch = set.ch ? set.ch - 1u : 0u, st;
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
        held_notes = keys;
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
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t p = ((int32_t)set.patch + e) % (int32_t)RDP_NPATCH;
            want_patch = (uint32_t)(p < 0 ? p + (int32_t)RDP_NPATCH : p);
        }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) {
            int32_t v = (int32_t)set.vel + e * 4;
            set.vel = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
            dirty = 1; t_change = fm1_ms;
        }

        midi_in();
        if (want_patch != 0xFFu && rdp_ready && rdp_select(want_patch) == 0) {   /* the render takes it next block */
            if (want_patch != set.patch) { set.patch = (uint8_t)want_patch; dirty = 1; t_change = fm1_ms; }
            want_patch = 0xFFu;
        }
        rdp_poll();                             /* after a patch change: the board's reload commands */

        oct = (int8_t)set.oct;
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), held_notes >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, sustain != 0);

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (held_notes != keys_drawn) { keys_drawn = held_notes; draw_keys(held_notes); }
        if (set.patch != patch_drawn) { patch_drawn = set.patch; draw_patch(); }
        if ((st = (uint32_t)(oct + 2) | set.ch << 4 | (uint32_t)set.vel << 9 | sustain << 16) != status_drawn) {
            status_drawn = st;
            draw_status(oct, set.ch, set.vel, sustain);
        }
        if (fm1_ms - t_load >= 250u) {          /* the load four times a second, and the highest of the last 2 s */
            uint32_t now = load_now, pk = load_peak, parts = rdp_active;
            load_peak = 0;
            t_load = fm1_ms;
            if (pk >= hold || fm1_ms - t_hold > 2000u) { hold = pk; t_hold = fm1_ms; }
            st = now | hold << 11 | parts << 22;
            if (st != load_drawn) { load_drawn = st; draw_load(now, hold, parts); }
        }
        idle_ok = 0;

        if (dirty && !rdp_active && fm1_ms - t_change > 2000u) {   /* 2 s after a change, when silent */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
