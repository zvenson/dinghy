/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy: the smallest FM-1 firmware to build your own on. A sine wave on the keys, and that is all.
 * This is the file to change. Everything below it (platform.c) is done: updates, the USB rescue,
 * MIDI (USB, the TRS jack), USB audio, keys, knobs, LEDs, the screen, flash storage; the FM-1 and the SMK-37.
 *
 *   keys       play sines, 4 voices; the notes go out over USB-MIDI too, on the MIDI channel
 *   OCT- OCT+  the octave (kept in flash)
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16 (kept in flash)
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack play the sines, with their velocity; a MIDI clock shows its tempo
 *              and blinks PLAY on the beat, START / STOP light it
 *   SMK-37     the keys with velocity, the outer keys too, the pads (white keys F3..G5, lit while they sound), the
 *              pitch wheel bends +-2 semitones
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
#define NV 2u                                   /* Polivoks duophonic voices */
#define MAX_HELD 10
static uint8_t held_notes[MAX_HELD];
static uint32_t num_held = 0;static struct {
    volatile uint32_t inc; 
    uint32_t ph, age;
    uint8_t note;
    
    // New ADSR Trackers
    volatile uint8_t state;  // Uses env_state enum
    int32_t env_val;         // Current volume level (0 to 32767)
} voice[NV];

static volatile uint32_t bend_q16 = 65536;      /* the pitch wheel: the steps times this / 65536 */

// Global Paraphonic Filter State
static int32_t f_lp = 0; // Low-pass output
static int32_t f_bp = 0; // Band-pass output

// Filter controls (We will map these to knobs later)
// f_cutoff: 0 to 16000 (Filter frequency)
// f_reso: 32767 down to 0 (Damping. Lower value = Higher resonance)
static int32_t f_cutoff = 5000; 
static int32_t f_reso = 4000;   

// The 5 phases of a synthesizer note
enum env_state { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// Global ADSR Parameters (Rates are amount added/subtracted per audio sample)
static int32_t p_attack_rate = 48;     // Higher number = faster attack
static int32_t p_decay_rate = 12;      // Higher number = faster decay
static int32_t p_sustain_level = 16384; // Q15 integer: 16384 is 50% volume
static int32_t p_release_rate = 6;      // Higher number = faster release

// How much the ADSR envelope opens the filter (0 to 16000)
static int32_t f_env_depth = 10000; 

// Global LFO Parameters
static uint32_t lfo_ph = 0;

// lfo_inc determines the speed (Rate). 
// 50000 is a slow, dramatic sweep. 500000 is a fast wobble.
static uint32_t lfo_inc = 50000; 

// How widely the LFO sweeps the filter (0 to 16000)
static int32_t lfo_depth = 6000; 

// Oscillator 1: Band-Limited Sawtooth
static int32_t saw(uint32_t ph, uint32_t inc) 
{
    // 1. Generate the naive Sawtooth (-32767 to +32767)
    int32_t val = 32767 - (int32_t)(ph >> 16); 
    
    // 2. PolyBLEP Correction (Integer approximation)
    // If we are right at the wrap-around point (ph < inc)
    if (ph < inc) {
        // Calculate t (0 to 1) represented as 0 to 32767
        int32_t t = (int32_t)((ph << 15) / inc);
        // Apply smoothing curve: val -= (1 - t)^2
        int32_t correction = 32767 - (2 * t) + ((t * t) >> 15);
        val -= correction;
    } 
    // If we are right before the wrap-around point
    else if (ph > (0xFFFFFFFFu - inc)) {
        int32_t t = (int32_t)(((ph - (0xFFFFFFFFu - inc)) << 15) / inc);
        int32_t correction = (t * t) >> 15;
        val += correction;
    }
    
    return val;
}

// Oscillator 2: Band-Limited Square
static int32_t square(uint32_t ph, uint32_t inc) 
{
    // 1. Naive Square wave
    int32_t val = (ph < 0x80000000u) ? 32767 : -32767;
    
    // 2. PolyBLEP Correction at Phase 0 (Rising Edge)
    if (ph < inc) {
        int32_t t = (int32_t)((ph << 15) / inc);
        val -= (32767 - (2 * t) + ((t * t) >> 15));
    } else if (ph > (0xFFFFFFFFu - inc)) {
        int32_t t = (int32_t)(((ph - (0xFFFFFFFFu - inc)) << 15) / inc);
        val += ((t * t) >> 15);
    }
    
    // 3. PolyBLEP Correction at Phase 180 (Falling Edge)
    uint32_t ph_half = ph + 0x80000000u; // Shift by 180 degrees
    if (ph_half < inc) {
        int32_t t = (int32_t)((ph_half << 15) / inc);
        val += (32767 - (2 * t) + ((t * t) >> 15));
    } else if (ph_half > (0xFFFFFFFFu - inc)) {
        int32_t t = (int32_t)(((ph_half - (0xFFFFFFFFu - inc)) << 15) / inc);
        val -= ((t * t) >> 15);
    }
    
    return val;
}

static int32_t polivoks_filter(int32_t input, int32_t current_cutoff) 
{
    // Run the filter twice per sample for integer stability
    for (int i = 0; i < 2; i++) {
        
        // 1. Calculate the High-Pass node
        // Note: We intentionally do NOT boost the input based on resonance.
        // This causes the classic Polivoks "bass drop" at high resonance.
        int32_t hp = input - f_lp - ((f_reso * f_bp) >> 15);
        
        // 2. Calculate the Band-Pass node (and apply Soviet Op-Amp clipping)
        // CHANGE: We now use current_cutoff instead of f_cutoff
        f_bp += (current_cutoff * hp) >> 15;
        
        // The Polivoks secret: Hard-clipping the bandpass node forces 
        // the aggressive, screaming self-oscillation.
        if (f_bp > 32767) f_bp = 32767;
        else if (f_bp < -32767) f_bp = -32767;
        
        // 3. Calculate the Low-Pass node
        // CHANGE: We now use current_cutoff instead of f_cutoff
        f_lp += (current_cutoff * f_bp) >> 15;
    }
    
    // The Polivoks is primarily a low-pass synth
    return f_lp; 
}

    
    // The Polivoks is primarily a low-pass synth
    return f_lp; 
}

static int32_t process_adsr(uint32_t v) 
{
    switch (voice[v].state) {
        case ENV_ATTACK:
            voice[v].env_val += p_attack_rate;
            // Once volume hits max (32767), automatically switch to Decay
            if (voice[v].env_val >= 32767) {
                voice[v].env_val = 32767;
                voice[v].state = ENV_DECAY;
            }
            break;
            
        case ENV_DECAY:
            voice[v].env_val -= p_decay_rate;
            // Once volume drops to the sustain knob level, hold it there
            if (voice[v].env_val <= p_sustain_level) {
                voice[v].env_val = p_sustain_level;
                voice[v].state = ENV_SUSTAIN;
            }
            break;
            
        case ENV_SUSTAIN:
            // Do nothing to the volume. Wait for the key to be released.
            voice[v].env_val = p_sustain_level;
            break;
            
        case ENV_RELEASE:
            voice[v].env_val -= p_release_rate;
            // Once volume hits 0, the note is completely dead
            if (voice[v].env_val <= 0) {
                voice[v].env_val = 0;
                voice[v].state = ENV_IDLE;
            }
            break;
            
        case ENV_IDLE:
        default:
            voice[v].env_val = 0;
            break;
    }
    
    return voice[v].env_val;
}

static int32_t triangle_lfo(uint32_t ph) 
{
    // The first half of the cycle (Rising edge)
    if (ph < 0x80000000u) {
        // Shift the 31-bit number down to 16-bit, and offset it
        // so it ramps from -32767 up to +32767
        return (int32_t)(ph >> 15) - 32767;
    } 
    // The second half of the cycle (Falling edge)
    else {
        // Ramps from +32767 back down to -32767
        return 32767 - (int32_t)((ph - 0x80000000u) >> 15);
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int32_t raw_osc1 = 0;
        int32_t raw_osc2 = 0;
        
        // 1. Advance the LFO State
        lfo_ph += lfo_inc;
        int32_t current_lfo = triangle_lfo(lfo_ph);
        
        // 2. Advance the ADSR state machine
        int32_t env0 = process_adsr(0);
        int32_t env1 = process_adsr(1);
        
        // 3. Render Oscillators
        if (voice[0].state != ENV_IDLE) {
            uint32_t step0 = (uint32_t)(((uint64_t)voice[0].inc * bend_q16) >> 16);
            voice[0].ph += step0;
            raw_osc1 = (saw(voice[0].ph, step0) * env0) >> 15;
        }

        if (voice[1].state != ENV_IDLE) {
            uint32_t step1 = (uint32_t)(((uint64_t)voice[1].inc * bend_q16) >> 16);
            voice[1].ph += step1;
            raw_osc2 = (square(voice[1].ph, step1) * env1) >> 15;
        }

        // 4. Calculate dynamic filter cutoff (Base + ADSR + LFO)
        // We scale the LFO by lfo_depth so we can control how extreme the wobble is
        int32_t dynamic_cutoff = f_cutoff 
                               + ((env0 * f_env_depth) >> 15)
                               + ((current_lfo * lfo_depth) >> 15);
        
        // Critical: Clamp the cutoff so the LFO doesn't push it into negative math
        if (dynamic_cutoff < 0) dynamic_cutoff = 0;
        if (dynamic_cutoff > 16000) dynamic_cutoff = 16000;

        // 5. Sum the oscillators and route through the modulated filter
        int32_t mixed_raw = (raw_osc1 + raw_osc2) / 2;
        int32_t filtered_out = polivoks_filter(mixed_raw, dynamic_cutoff);

        // 6. Write to the hardware DAC
        out[2u * i]     += filtered_out;
        out[2u * i + 1u] += filtered_out;
    }
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

static uint32_t last_note = 255;

// 1. The Polivoks Brain
static void update_voice_routing(uint32_t vel)
{
    if (num_held == 0) {
        // Note Off! Send both voices into the Release phase
        if (voice[0].state != ENV_IDLE) voice[0].state = ENV_RELEASE;
        if (voice[1].state != ENV_IDLE) voice[1].state = ENV_RELEASE;
        return;
    }

    // ... (Keep the existing lowest/highest note scanning logic here) ...
    // voice[0].inc = ...
    // voice[1].inc = ...

    if (vel) {
        // New Key Pressed! Restart the envelope at the Attack phase
        voice[0].state = ENV_ATTACK;
        voice[1].state = ENV_ATTACK;
        
        // Optional: If you want hard analog note-stealing (clicks included),
        // you can force voice[v].env_val = 0; here to snap the envelope down.
    }
}

// Also update quiet() to check the new variables:
static int quiet(void) {
    return (voice[0].state == ENV_IDLE && voice[1].state == ENV_IDLE);
}

    uint8_t note1, note2;
    
    if (num_held == 1) {
        // Monophonic: Both oscillators play the same fat note
        note1 = held_notes[0];
        note2 = held_notes[0];
    } else {
        // Duophonic: Scan for lowest and highest keys
        uint8_t lowest = 127, highest = 0;
        for (uint32_t i = 0; i < num_held; i++) {
            if (held_notes[i] < lowest) lowest = held_notes[i];
            if (held_notes[i] > highest) highest = held_notes[i];
        }
        note1 = lowest;
        note2 = highest;
    }

    // Assign notes and look up the integer phase increment for the pitch
    voice[0].note = note1;
    voice[1].note = note2;
    voice[0].inc = INC_C9[note1 % 12u] >> (10u - note1 / 12u);
    voice[1].inc = INC_C9[note2 % 12u] >> (10u - note2 / 12u);

    // Only trigger the attack if a new key was actually pressed
    if (vel) {
        int32_t target_gate = (int32_t)(vel * 90u);
        voice[0].gate = target_gate;
        voice[1].gate = target_gate;
    }
}

// 2. The MIDI/Key Handlers
static void play(uint32_t note, uint32_t vel)
{
    if (note > 127u || !vel) return;
    
    // Prevent duplicate notes in the array
    for (uint32_t i = 0; i < num_held; i++) {
        if (held_notes[i] == note) return; 
    }
    
    if (num_held < MAX_HELD) {
        held_notes[num_held++] = (uint8_t)note;
        update_voice_routing(vel); // Trigger with velocity
    }
    last_note = note;
}

static void stop(uint32_t note)
{
    for (uint32_t i = 0; i < num_held; i++) {
        if (held_notes[i] == note) {
            // Shift array to remove the released note
            for (uint32_t j = i; j < num_held - 1; j++) {
                held_notes[j] = held_notes[j + 1];
            }
            num_held--;
            break;
        }
    }
    update_voice_routing(0); // Update pitch without re-triggering attack
}

static int quiet(void)
{
    return !(voice[0].gate || voice[0].amp || voice[1].gate || voice[1].amp);
}


/* ---- the screen: the name, the last note big, the keyboard with what sounds, the octave */
#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define KEY0 53u                                /* key 0 = F3 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static void draw_note(void)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    if (last_note < 128u) {
        const char *p = NAME[last_note % 12u];
        uint32_t i = 0;
        while (*p) s[i++] = *p++;
        if (last_note >= 12u) s[i++] = (char)('0' + last_note / 12u - 1u);
        s[i] = 0;
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
    char s[24] = "OCT +0  MIDI OMNI";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (ch) {
        s[13] = 'C'; s[14] = 'H'; s[15] = (char)(ch < 10u ? ' ' : '1'); s[16] = (char)('0' + ch % 10u);
    }
    draw_text_box(0, 212, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_midi(uint32_t src, uint32_t bpm, uint32_t running)   /* where notes came from last, the clock */
{
    char s[32] = "", *p = s;
    const char *a = src == 2u ? "TRS IN" : src == 1u ? "USB IN" : "";
    while (*a) *p++ = *a++;
    if (bpm) {
        if (p != s) { *p++ = ' '; *p++ = ' '; }
        a = running ? "CLOCK " : "clock ";
        while (*a) *p++ = *a++;
        if (bpm >= 100u) *p++ = (char)('0' + bpm / 100u);
        *p++ = (char)('0' + bpm / 10u % 10u);
        *p++ = (char)('0' + bpm % 10u);
        a = " BPM";
        while (*a) *p++ = *a++;
    }
    *p = 0;
    draw_text_box(0, 120, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_filter(void)
{
    // 1. Open a 240x40 pixel canvas in RAM, filled with black
    cv_begin(240, 40, C_BLACK);
    
    // 2. Calculate pixel widths (Screen is 240px wide)
    uint32_t cut_w = (f_cutoff * 240) / 16000;
    
    // Invert resonance so the bar gets wider as resonance gets higher (f_reso goes down)
    uint32_t res_w = ((32767 - f_reso) * 240) / 32767; 
    
    // 3. Draw Cutoff Bar (X, Y, Width, Height, Color)
    // We use C_ACC (Lilac) which is already defined in Dinghy
    cv_rect(0, 5, cut_w, 10, C_ACC); 
    
    // 4. Draw Resonance Bar
    // We use the RGB macro to make this bar screaming red
    cv_rect(0, 25, res_w, 10, RGB(255, 50, 50)); 
    
    // 5. Push this canvas to the physical screen at X=0, Y=95
    // This places it perfectly between the note name and the keyboard
    cv_blit(0, 95);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static struct { uint8_t oct, ch; } set = {0, 0};   /* what survives a power-off (add yours, up to SECTOR_MAX bytes) */
static uint32_t idle_ok, midi_src, clk_n, clk_t0, clk_bpm, clk_run, clk_beat_ms;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {                      /* clock: 24 a quarter note */
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    clk_bpm = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
                clk_beat_ms = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_run = 1; clk_n = 0; continue; }   /* START: the next clock is beat 1 */
        if (st == 0xFBu) { clk_run = 1; continue; }              /* CONTINUE */
        if (st == 0xFCu) { clk_run = 0; continue; }              /* STOP */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;      /* not our channel */
        if (ty == 0x90u && d2) { play(d1, d2); midi_src = (uint32_t)src; }
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

static void app_main(void)
{
uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u;
uint32_t filter_drawn = ~0u; // NEW
    uint32_t midi_drawn = ~0u;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    if (settings_load(&set, sizeof set) || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u)
        set.oct = 0, set.ch = 0;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, C_ACC, 1);
    draw_text_box(0, 52, 240, &FONT_S, "a sine to build on  v" APP_VER, C_OFF, 1);

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, out_ch = set.ch ? set.ch - 1u : 0u, st;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {                 /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); play(held[k], vel); midi_send(0x90u | out_ch, held[k], vel); }
            if (up >> k & 1u) { stop(held[k]); midi_send(0x80u | out_ch, held[k], 0); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); play(ext_note[key], vel); midi_send(0x90u | out_ch, ext_note[key], vel); }
            else if (ext_note[key]) { stop(ext_note[key]); midi_send(0x80u | out_ch, ext_note[key], 0); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); play(pad_note[pad], vel); midi_send(0x90u | out_ch, pad_note[pad], vel); }
            else if (pad_note[pad]) { stop(pad_note[pad]); midi_send(0x80u | out_ch, pad_note[pad], 0); pad_note[pad] = 0; }
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* 1/4096 semitone */
            bend_q16 = (uint32_t)(65536 + (bd * 3785 >> 12) + (((bd * bd) >> 12) * 109 >> 12));   /* ~2^(s/12) */
        }
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {   /* detents turned since the last call */
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1;
            t_change = fm1_ms;
        }
    // Knob 1: Filter Cutoff (0 to 16000)
    if ((e = fm1_enc_take(ENC_1)) != 0) {
        // Multiply the click by 500 for a fast, responsive sweep
        f_cutoff += e * 500; 
        
        // Clamp the values so it doesn't crash the integer math
        if (f_cutoff < 0) f_cutoff = 0;
        if (f_cutoff > 16000) f_cutoff = 16000;
        
        // Tell the UI thread that the screen needs updating
        dirty = 1; 
    }

    // Knob 2: Filter Resonance (32767 down to 0)
    if ((e = fm1_enc_take(ENC_2)) != 0) {
        // Note the MINUS sign: Turning the knob right (+e) 
        // lowers the damping variable, which INCREASES resonance.
        f_reso -= e * 1000;
        
        if (f_reso < 0) f_reso = 0; // Max screaming resonance
        if (f_reso > 32767) f_reso = 32767; // Fully damped
        
        dirty = 1;
    }

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));   /* lit: running; a blink a beat */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

    idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
    
    // Combine our two variables into one 32-bit state tracker
    uint32_t current_filter = (uint32_t)(f_cutoff) | ((uint32_t)(f_reso) << 16);
    if (current_filter != filter_drawn) { 
        filter_drawn = current_filter; 
        draw_filter(); 
    }

    if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }   /* redraw only what changed */
    if (last_note != note_drawn) { note_drawn = last_note; draw_note(); }
    if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4) != status_drawn) { status_drawn = st; draw_status(oct, set.ch); }
    if ((st = midi_src | clk_bpm << 2 | clk_run << 12) != midi_drawn) { midi_drawn = st; draw_midi(midi_src, clk_bpm, clk_run); }
    idle_ok = 0;


        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
