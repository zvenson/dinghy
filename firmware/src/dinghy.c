/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy: the smallest FM-1 firmware to build your own on. A sine wave on the keys, and the basics every instrument
 * needs around it. This is the file to change. Below it, platform.c is done (updates, the USB rescue, MIDI over USB
 * and the TRS jack, USB audio, keys, knobs, LEDs, the screen, flash storage; the FM-1 and the SMK-37), and the kit_*.h
 * files bring what OberVave has grown: take the ones you want.
 *
 *   keys       play sines, 4 voices; the notes go out over USB-MIDI too, on the MIDI channel
 *   OCT- OCT+  the octave (kept in flash)
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16
 *   PLAY       starts / stops the transport (our own tempo; with a MIDI clock coming in, the master leads)
 *   GLO        the GLOBAL page: KNOB 1 MIDI channel, KNOB 2 SYNC AUTO / INTERNAL, KNOB 3 tempo, KNOB 4 the click,
 *              REC tapped = tap tempo, SELECT pressed = VIBE LINK OFF > ON > CONTROL (FM-1); GLO or HOME = back
 *   MASTER     the volume (platform.c); MIDI CC 7 too
 *   MIDI in    notes from USB or the TRS jack, with velocity; pitch bend (+-2), the mod wheel (vibrato), the sustain
 *              pedal, all notes off; a MIDI clock (24 a quarter, START / STOP) runs the transport, locked to it
 *   USB        audio into any DAW or phone (16-bit stereo, 44.1 / 48 kHz), FM-1 Mirror, the backup page
 *   SMK-37     the keys with velocity, the outer keys too, the pads (white keys F3..G5, lit while they sound), the
 *              pitch wheel bends +-2 semitones
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c and the kit offer: docs/API.md. The chip's rules: docs/RULES.md. */
#include <stdint.h>
static void app_idle(void);
static void clock_rt(uint32_t st);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#define MIDI_RT_HOOK(st) clock_rt(st)           /* clock and transport as they arrive (usb.c, midi_uart.c): kit_clock.h */
#define LCD_MIRROR 1                            /* what is painted also goes to FM-1 Mirror: kit_mirror.h */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
#define NV 4u                                   /* voices */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 (unbent) */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    int32_t amp;                                /* the level now: glides to gate, no clicks */
    uint32_t ph, age;
    uint8_t note;
} voice[NV];
static volatile int32_t wheel_q12;              /* the SMK-37's pitch wheel, 1/4096 semitone (MIDI's bend: kit_midi.h) */
static volatile uint32_t click_on;              /* the click on the beat (the GLOBAL page) */
static uint32_t click_left, click_wait, click_inc, click_ph, vib_ph;

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

/* 24 times a quarter while the transport runs (kit_clock.h, in the ISR): a beat clicks, the bar's first one higher */
static void app_tick(uint32_t tick, uint32_t ofs)
{
    if (tick % 24u || !click_on) return;
    click_wait = ofs;
    click_left = 265;                           /* 6 ms */
    click_ph = 0;
    click_inc = tick % 96u ? 155826907u : 233740360u;   /* 1.6 kHz, 2.4 kHz */
}
#define CLOCK_TICK(tick, ofs) app_tick(tick, ofs)
#include "kit_clock.h"

/* the kit's MIDI in (kit_midi.h) plays the same voices: the pedal and the channel are already applied */
static void play(uint32_t note, uint32_t vel);
static void stop(uint32_t note);
static void all_off(void);
#define MIDI_NOTE(on, note, vel) ((on) ? play(note, vel) : stop(note))
#define MIDI_PANIC() all_off()
#include "kit_midi.h"

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t v, i, scale;
    int32_t pitch, vol;
    clock_audio(n);                             /* the transport first: its ticks for this block */
    pitch = bend_q12() + wheel_q12;
    vib_ph += 137130000u;                       /* the mod wheel's vibrato: 5.5 Hz (2^32 * 5.5 * 256 / 44100 a block) */
    pitch += (sine(vib_ph) * (int32_t)midi_mod) >> 11;   /* up to +-1/2 semitone (32767 * 127 >> 11 = 2031) */
    scale = pitch_q16(pitch);
    vol = midi_vol_q15();
    for (v = 0; v < NV; v++) {
        uint32_t ph = voice[v].ph, step = (uint32_t)(((uint64_t)voice[v].inc * scale) >> 16);   /* (int64 multiply: fine) */
        int32_t amp = voice[v].amp, g = voice[v].gate;
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        for (i = 0; i < n; i++) {
            int32_t s;
            if (amp < g) { amp += 48; if (amp > g) amp = g; }        /* attack ~5 ms */
            else if (amp > g) { amp -= 6; if (amp < g) amp = g; }    /* release ~45 ms */
            ph += step;
            s = (((sine(ph) * amp) >> 15) * vol) >> 15;
            out[2u * i] += s;                   /* L */
            out[2u * i + 1u] += s;              /* R */
        }
        voice[v].ph = ph;
        voice[v].amp = amp;
    }
    for (i = 0; i < n && click_left; i++) {     /* the click */
        int32_t s;
        if (click_wait) { click_wait--; continue; }
        click_ph += click_inc;
        s = sine(click_ph) * (int32_t)click_left / 1060;   /* (fades out) */
        out[2u * i] += s;
        out[2u * i + 1u] += s;
        click_left--;
    }
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t ages, last_note = 255;

static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    if (note > 127u || !vel) return;
    for (v = 0; v < NV; v++) {                  /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == note && voice[v].gate ? 0u
                       : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].inc = INC_C9[note % 12u] >> (10u - note / 12u);
    voice[pick].gate = (int32_t)(vel * 90u);    /* 127 -> 11430: four voices stay under full scale */
    last_note = note;
}
static void stop(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].note == note)
            voice[v].gate = 0;
}
static void all_off(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        voice[v].gate = 0;
}
static int quiet(void)                          /* nothing sounds, not even a release */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].amp) return 0;
    return 1;
}

#include "kit_link.h"                           /* the FM-1 as the VIBE FM's controller */
#include "kit_mirror.h"                         /* FM-1 Mirror: the screen on a computer */
#define BACKUP_STOP() (all_off(), clock_stop())
#include "kit_backup.h"                         /* the backup page: settings, blobs, slots to a file and back */

/* ---- what survives a power-off (add yours, up to SECTOR_MAX bytes) */
#define SET_VER 2u
static struct { uint8_t oct, ch, ver, sync_int, tempo, click, link, pad; } set = {0, 0, SET_VER, 0, 120, 0, 0, 0};

static void set_check(void)
{
    if ((int8_t)set.oct < -2 || (int8_t)set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    set.sync_int &= 1u; set.click &= 1u;
    if (set.link > 1u) set.link = 0;            /* (CONTROL is never where a power-on starts) */
}
static void set_load(void)
{
    if (settings_load(&set, sizeof set) || set.ver != SET_VER) {   /* 0.5 kept the octave and the channel only */
        struct { uint8_t oct, ch; } old;
        uint8_t oct = 0, ch = 0;
        if (!settings_load(&old, sizeof old)) oct = old.oct, ch = old.ch;
        set.oct = oct; set.ch = ch; set.ver = SET_VER; set.sync_int = 0; set.tempo = 120; set.click = 0; set.link = 0;
    }
    set_check();
}

/* ---- the screen: the name, the last note big, the keyboard with what sounds, the octave; or the GLOBAL page */
#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define C_TXT RGB(200, 200, 200)
#define KEY0 53u                                /* key 0 = F3 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static char *put(char *p, const char *a) { while (*a) *p++ = *a++; *p = 0; return p; }
static char *put_u(char *p, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = b[--n];
    *p = 0;
    return p;
}
static char *put_bpm(char *p, uint32_t bpm10)
{
    p = put_u(p, bpm10 / 10u);
    *p++ = '.';
    *p++ = (char)('0' + bpm10 % 10u);
    return put(p, " BPM");
}

static void draw_head(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, C_ACC, 1);
    draw_text_box(0, 52, 240, &FONT_S, "a sine to build on  v" APP_VER, C_OFF, 1);
}

static void draw_note(void)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    if (last_note < 128u) {
        char *p = put(s, NAME[last_note % 12u]);
        if (last_note >= 12u) *p++ = (char)('0' + last_note / 12u - 1u), *p = 0;
    }
    draw_text_box(0, 82, 240, &FONT_L, s, C_WHITE, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 56, C_BLACK);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 56, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 34, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, 140);
}

static void draw_status(int32_t oct, uint32_t ch)
{
    char s[24], *p = put(s, "OCT ");
    *p++ = oct < 0 ? '-' : '+';
    *p++ = (char)('0' + (oct < 0 ? -oct : oct));
    p = put(p, "  MIDI ");
    if (ch) { p = put(p, "CH "); put_u(p, ch); } else put(p, "OMNI");
    draw_text_box(0, 212, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_midi(uint32_t src, uint32_t bpm10, uint32_t ext, uint32_t run)   /* where notes came from, the tempo */
{
    char s[40], *p = s;
    *p = 0;
    if (src) p = put(p, src == 2u ? "TRS IN  " : "USB IN  ");
    p = put(p, ext ? "SYNC " : run ? "PLAY " : "");
    put_bpm(p, ext ? (bpm10 + 5u) / 10u * 10u : bpm10);   /* (a master's tempo is measured to ~0.3 %: whole BPM) */
    draw_text_box(0, 120, 240, &FONT_S, s, run ? C_TXT : C_OFF, 1);
}

static void draw_global(void)                   /* the GLOBAL page, all of it */
{
    char s[40], *p;
    lcd_fill(0, 82, 240, 130, C_BLACK);
    draw_text_box(0, 82, 240, &FONT_S, "GLOBAL", C_ACC, 1);
    p = put(s, "K1 MIDI CH   ");
    if (set.ch) put_u(p, set.ch); else put(p, "OMNI");
    draw_text_box(0, 106, 240, &FONT_S, s, C_TXT, 1);
    draw_text_box(0, 124, 240, &FONT_S, set.sync_int ? "K2 SYNC  INTERNAL" : "K2 SYNC  AUTO", C_TXT, 1);
    p = put(s, "K3 TEMPO  ");
    put_u(p, set.tempo);
    draw_text_box(0, 142, 240, &FONT_S, s, C_TXT, 1);
    draw_text_box(0, 160, 240, &FONT_S, set.click ? "K4 CLICK  ON" : "K4 CLICK  OFF", C_TXT, 1);
#if !BOARD_SMK37
    p = put(s, "SEL VIBE LINK  ");
    put(p, LINK_NAME[link_mode % 3u]);
    draw_text_box(0, 178, 240, &FONT_S, s, C_TXT, 1);
#endif
    draw_text_box(0, 196, 240, &FONT_S, "REC = TAP   GLO = BACK", C_OFF, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static uint32_t idle_ok;
static void app_idle(void)
{
    if (idle_ok) midi_poll(1);
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u;
    uint32_t midi_drawn = ~0u, page = 0, page_drawn = ~0u;   /* page: 0 the keyboard, 1 GLOBAL */
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    set_load();
    midi_ch = set.ch;
    clk_tempo = set.tempo;
    clock_set_sync(set.sync_int);
    click_on = set.click;
    link_set(set.link);

    draw_head();

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, st;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        if (mirror_poll()) {                    /* FM-1 Mirror opened (or rows were lost): everything drawn again */
            draw_head();
            lit_drawn = note_drawn = status_drawn = midi_drawn = page_drawn = ~0u;
        }
        if (backup_poll()) { platform_poll(); continue; }   /* a restore from the backup page: nothing else runs */
        platform_poll();
        clock_poll();

        if (link_mode == 2u) {                  /* VIBE LINK CONTROL: the panel is the VIBE's */
            int32_t o = oct;
            if (link_control(&o)) { draw_head(); lit_drawn = note_drawn = status_drawn = midi_drawn = page_drawn = ~0u; prev = fm1_in.notes; }
            if (o != oct) { set.oct = (uint8_t)o; dirty = 1; t_change = fm1_ms; }
            set.link = link_mode;
            midi_poll(0);                       /* (MIDI in still plays nothing here: the voices were stopped) */
            all_off();
            continue;
        }

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); key_note(1, held[k], vel); }
            if (up >> k & 1u) key_note(0, held[k], 0);
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); key_note(1, ext_note[key], vel); }
            else if (ext_note[key]) { key_note(0, ext_note[key], 0); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); key_note(1, pad_note[pad], vel); }
            else if (pad_note[pad]) { key_note(0, pad_note[pad], 0); pad_note[pad] = 0; }
        }
        wheel_q12 = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* 1/4096 semitone */
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        link_buttons(b, rel);                   /* (VIBE LINK ON: they go out as well) */
        link_master();
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_PLAY)) clock_toggle();
        if (b & (1u << BTN_GLO)) page ^= 1u;
        if (b & (1u << BTN_HOME)) page = 0;
        if ((e = link_enc_take(ENC_PRESETS)) != 0) {   /* detents turned since the last call */
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms; page_drawn = ~0u;
        }
        if (page == 1u) {                       /* GLOBAL */
            int32_t c;
            uint32_t chg = 0;
            if ((e = link_enc_take(ENC_KNOB1)) != 0) { c = (int32_t)set.ch + e; set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c); chg = 1; }
            if ((e = link_enc_take(ENC_KNOB2)) != 0) { set.sync_int = e > 0; clock_set_sync(set.sync_int); chg = 1; }
            if ((e = link_enc_take(ENC_KNOB3)) != 0) { c = (int32_t)set.tempo + e; set.tempo = (uint8_t)(c < 40 ? 40 : c > 240 ? 240 : c); clk_tempo = set.tempo; chg = 1; }
            if ((e = link_enc_take(ENC_KNOB4)) != 0) { set.click = e > 0; click_on = set.click; chg = 1; }
            if (b & (1u << BTN_REC)) { clock_tap(); set.tempo = (uint8_t)clk_tempo; chg = 1; }
            if (b & (1u << BTN_SEL)) { link_set(link_mode + 1u); set.link = link_mode; chg = 1; }
            if (chg) { dirty = 1; t_change = fm1_ms; page_drawn = ~0u; }
            if (link_mode == 2u) { all_off(); continue; }
        }
        midi_ch = set.ch;

        midi_poll(0);

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_GLO, page == 1u);
        {
            static uint32_t beat_seen, beat_t;   /* PLAY: lit while it runs, out for a moment on each beat */
            if (clk_ui_beat != beat_seen) { beat_seen = clk_ui_beat; beat_t = fm1_ms; }
            fm1_led_key(BTN_PLAY, clk_run && fm1_ms - beat_t > 90u);
        }
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (page != (page_drawn & 0xFFu) || page_drawn == ~0u) {
            page_drawn = page;
            if (page == 1u) draw_global();
            else { lcd_fill(0, 82, 240, 130, C_BLACK); lit_drawn = note_drawn = midi_drawn = ~0u; }
        }
        if (page == 0u) {                       /* redraw only what changed */
            if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }
            if (last_note != note_drawn) { note_drawn = last_note; draw_note(); }
            if ((st = midi_src | clock_bpm10() << 2 | (uint32_t)clk_ext() << 14 | clk_run << 15) != midi_drawn) {
                midi_drawn = st; draw_midi(midi_src, clock_bpm10(), (uint32_t)clk_ext(), clk_run);
            }
        }
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4) != status_drawn) { status_drawn = st; draw_status(oct, set.ch); }
        idle_ok = 0;

        if (dirty && quiet() && !clk_run && fm1_ms - t_change > 2000u) {   /* 2 s after the change, silent and stopped (an
                                                 * erase stalls the sound and the clock) */
            set_check();
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
