/* SPDX-License-Identifier: GPL-3.0-only */
/* A fake FM-1 (or SMK-37) on a computer: the app (firmware/src/rdpno.c) runs unchanged on it. platform.c includes
 * this file instead of the hardware when DINGHY_HOST is set. A script presses keys over time; out come
 * the screen (PPM, at the times SHOT asks) and the sound (WAV, as the FM-1 sends it over USB).
 *
 *   mkdir -p build/host
 *   cc -O2 -Wall -DDINGHY_HOST -Ifirmware/hal -Ifirmware/src -Ifirmware/gen -o build/host/rdpno firmware/src/rdpno.c
 *   build/host/rdpno build/host/rdpno        (-> build/host/rdpno.wav, build/host/rdpno-<ms>.ppm)
 *   build/host/rdpno OUT script.txt [FPS]    (your own script; with FPS: OUT-NNNNN.ppm, a frame every 1/FPS s:
 *                                             a film of your firmware, e.g. ffmpeg -r 30 -i OUT-%05d.ppm ...)
 *   tests/run_host.sh [script] does it all (BOARD=smk37: the SMK-37 build); DINGHY_PERF=1: instructions per sample
 *
 * Change SCRIPT below to try your own app. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#endif

#ifndef APP_NAME
#define APP_NAME "DINGHY"
#endif
#ifndef APP_VER
#define APP_VER "0.0"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

static volatile uint32_t fm1_ms;
#if BOARD_SMK37
/* the SMK-37 (hal/smk_input.h's API): keys with velocity, pads as events, 4 faders, 2 wheels, the outer keys, the
 * pad LEDs; 8 encoders (K1 = MASTER) */
#define FM1_NKEY 37u
#define SMK_KEY_F3 5u
#define HOST_NENC 8
static struct {
    uint32_t notes, buttons, frames, adc_ok, adc_miss;
    uint64_t keys;
    uint8_t key_vel[FM1_NKEY], fader[4];
    uint16_t wheel[2], batt, pad_raw[16], pad_base[16];
} fm1_in = {.wheel = {535, 620}};               /* (the wheels at rest, as a unit reads them) */
static uint8_t smk_store_cleared, smk_trim_ok;
static uint32_t smk_jedec;
static uint8_t smk_pad_events;
static int32_t host_q[2][64];                   /* 0: pad events, 1: outer keys (key << 8 | velocity, 0 = release) */
static uint32_t host_qw[2], host_qr[2];
static void host_qput(int i, int32_t v) { host_q[i][host_qw[i]++ % 64u] = v; }
static int32_t host_qtake(int i) { return host_qr[i] == host_qw[i] ? -1 : host_q[i][host_qr[i]++ % 64u]; }
static int32_t smk_pad_take(void) { return host_qtake(0); }
static int32_t smk_extkey_take(void) { return host_qtake(1); }
static uint32_t padled_rgb[16];                 /* as lcd.c: g << 16 | r << 8 | b */
static void padled_set(uint32_t pad, uint8_t r, uint8_t g, uint8_t b) { if (pad < 16u) padled_rgb[pad] = (uint32_t)g << 16 | (uint32_t)r << 8 | b; }
static void padled_flush(void) {}
#else
#define HOST_NENC 7
static struct { uint32_t notes, buttons; } fm1_in;
#endif
static uint32_t host_edges, host_rel, host_rel_next;   /* press edges, release edges (a tap's release: the call after) */
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t b = host_edges;
    host_edges = 0;
    *released = host_rel;
    host_rel = host_rel_next;
    host_rel_next = 0;
    return b;
}
static uint32_t fm1_input_note_edges(void) { return 0; }   /* (the script sets fm1_in.notes) */
static int32_t host_enc[HOST_NENC], host_enc_pos[HOST_NENC];   /* detents owed to the app; the position so far (the film's log) */
static int32_t fm1_enc_take(uint32_t e) { int32_t v = host_enc[e % HOST_NENC]; host_enc[e % HOST_NENC] = 0; return v; }
static uint64_t host_leds;                      /* bit id: LED id lit (0..13 buttons, 14..40 keys) */
static void fm1_irq_off(void) {}                 /* (the host's audio runs between the main loop's passes) */
static void fm1_irq_on(void) {}
static void fm1_led_key(uint32_t id, int on) { if (on) host_leds |= (uint64_t)1 << id; else host_leds &= ~((uint64_t)1 << id); }
static uint64_t host_glow;                      /* the dim layer (not in the film log) */
static void fm1_led_key_glow(uint32_t id, int on) { if (on) host_glow |= (uint64_t)1 << id; else host_glow &= ~((uint64_t)1 << id); }

/* the screen: 240 x 240 RGB565, as the ST7789 gets it (gfx.c sends the bytes swapped) */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}
#ifndef CV_MAX
#define CV_MAX (240u * 64u)
#endif
#include "gfx.c"

/* storage in RAM (lost at exit), store.h's sizes */
#include "store.h"
static uint8_t host_set[SECTOR_MAX + 4], host_blob[NBLOB][SECTOR_MAX + 4], host_slot[NSLOT][SLOT_MAX + 4];
static int host_load(uint8_t *a, uint32_t max, void *p, uint32_t n)   /* a[0] used, a[1..3] the length */
{
    if (!a[0] || n > max || (uint32_t)(a[1] | a[2] << 8 | a[3] << 16) != n) return -1;
    memcpy(p, a + 4, n);
    return 0;
}
static int host_save(uint8_t *a, uint32_t max, const void *p, uint32_t n, const char *what, uint32_t i)
{
    if (n > max) return -1;
    a[0] = 1; a[1] = (uint8_t)n; a[2] = (uint8_t)(n >> 8); a[3] = (uint8_t)(n >> 16);
    memcpy(a + 4, p, n);
    printf("%6u ms  %s %u saved (%u B)\n", fm1_ms, what, i, n);
    return 0;
}
static int settings_load(void *p, uint32_t n) { return host_load(host_set, SECTOR_MAX, p, n); }
static int settings_save(const void *p, uint32_t n) { return host_save(host_set, SECTOR_MAX, p, n, "settings", 0); }
static int blob_load(uint32_t i, void *p, uint32_t n) { return i < NBLOB ? host_load(host_blob[i], SECTOR_MAX, p, n) : -1; }
static int blob_save(uint32_t i, const void *p, uint32_t n) { return i < NBLOB ? host_save(host_blob[i], SECTOR_MAX, p, n, "blob", i) : -1; }
static int slot_load(uint32_t i, void *p, uint32_t n) { return i < NSLOT ? host_load(host_slot[i], SLOT_MAX, p, n) : -1; }
static int slot_save(uint32_t i, const void *p, uint32_t n) { return i < NSLOT ? host_save(host_slot[i], SLOT_MAX, p, n, "slot", i) : -1; }

static uint8_t mq[64][4];                       /* status, data 1, data 2, source (1 USB, 2 TRS) */
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    int src;
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2]; src = mq[mq_r % 64u][3] ? mq[mq_r % 64u][3] : 1;
    mq_r++;
    return src;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { printf("%6u ms  MIDI out %02X %u %u\n", fm1_ms, st, d1, d2); }

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

/* the script: at t ms, keys down (bit k = key k, 0 = F3), a button edge, a MIDI message. A script file (the second
 * argument) replaces it, a line each: "T keys MASK" (hex or decimal), "T btn ID", "T midi ST D1 D2" (USB),
 * "T trs ST D1 D2" (the TRS jack), "T clock BPM N" (N MIDI clocks at BPM over USB, from T on), "T enc ID N" (encoder
 * ID turned N detents, + = clockwise), "T btndown ID" / "T btnup ID" (a button held, let go), "T end" */
typedef struct { uint32_t t, keys; int btn; uint8_t st, d1, d2; int8_t enc, encn; uint8_t smk, sa; uint16_t sb; uint8_t src; } host_ev_t;
/* SMK-37 builds (BOARD_SMK37) also: "T vel V" (the velocity of the keys pressed from now), "T pad P V" (hardware pad
 * P hit at velocity V, V 0 = let go), "T fader F V" (0..127), "T wheel W RAW" (0 pitch, 1 mod; raw ADC 0..1023),
 * "T xkey K V" (keybed key K 0..36 at V, 0 = let go: the outer keys 0..4, 32..36); any build: "T shot" (the screen
 * at T -> OUT-T.ppm) */
enum { HS_NONE, HS_VEL, HS_PAD, HS_FADER, HS_WHEEL, HS_XKEY, HS_SHOT };
static const host_ev_t SCRIPT0[] = {
    {200, 1u << 7, -1, 0, 0, 0, -1, 0},                         /* C4 */
    {700, 1u << 7 | 1u << 11 | 1u << 14, -1, 0, 0, 0, -1, 0},   /* + E4 G4: a chord */
    {1500, 0, -1, 0, 0, 0, -1, 0},
    {1700, 0, -1, 0x90, 81, 120, -1, 0, 0, 0, 0, 1},             /* MIDI in (USB): A5 */
    {2300, 0, -1, 0x80, 81, 0, -1, 0, 0, 0, 0, 1},
    {2500, 0, BTN_OCT_UP, 0, 0, 0, -1, 0},
    {2700, 1u << 2 | 1u << 9, -1, 0, 0, 0, -1, 0},              /* G4 + D5, an octave up */
    {3400, 0, -1, 0, 0, 0, -1, 0},
};
static const host_ev_t *script = SCRIPT0;
static uint32_t script_n = sizeof SCRIPT0 / sizeof SCRIPT0[0], end_ms = 6500, frame_fps;
static const uint32_t SHOT[] = {1000, 3000};

static void read_script(const char *path)
{
    static host_ev_t ev[65536];
    unsigned bpm = 0, nclk = 0;
    char line[256], what[16];
    FILE *f = fopen(path, "r");
    uint32_t n = 0, i;
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && n < 65536u) {
        host_ev_t e = {0, 0, -1, 0, 0, 0, -1, 0, 0, 0, 0, 0};
        unsigned a = 0, b = 0, c = 0;
        int ea = 0, eb = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s", &e.t, what) != 2) continue;
        if (!strcmp(what, "end")) { end_ms = e.t; continue; }
        if (!strcmp(what, "keys")) { sscanf(line, "%*u %*s %i", (int *)&e.keys); }
        else if (!strcmp(what, "btn")) { sscanf(line, "%*u %*s %d", &e.btn); }          /* a tap */
        else if (!strcmp(what, "btndown")) { sscanf(line, "%*u %*s %d", &e.btn); e.btn += 100; }   /* held ... */
        else if (!strcmp(what, "btnup")) { sscanf(line, "%*u %*s %d", &e.btn); e.btn += 200; }     /* ... let go */
        else if (!strcmp(what, "enc")) { sscanf(line, "%*u %*s %d %d", &ea, &eb); e.enc = (int8_t)ea; e.encn = (int8_t)eb; }
        else if (!strcmp(what, "vel")) { sscanf(line, "%*u %*s %u", &a); e.smk = HS_VEL; e.sb = (uint16_t)a; }
        else if (!strcmp(what, "pad")) { sscanf(line, "%*u %*s %u %u", &a, &b); e.smk = HS_PAD; e.sa = (uint8_t)a; e.sb = (uint16_t)b; }
        else if (!strcmp(what, "fader")) { sscanf(line, "%*u %*s %u %u", &a, &b); e.smk = HS_FADER; e.sa = (uint8_t)a; e.sb = (uint16_t)b; }
        else if (!strcmp(what, "wheel")) { sscanf(line, "%*u %*s %u %u", &a, &b); e.smk = HS_WHEEL; e.sa = (uint8_t)a; e.sb = (uint16_t)b; }
        else if (!strcmp(what, "xkey")) { sscanf(line, "%*u %*s %u %u", &a, &b); e.smk = HS_XKEY; e.sa = (uint8_t)a; e.sb = (uint16_t)b; }
        else if (!strcmp(what, "shot")) e.smk = HS_SHOT;   /* (any board: the screen now -> OUT-<ms>.ppm) */
        else if (!strcmp(what, "midi") || !strcmp(what, "trs")) {
            sscanf(line, "%*u %*s %i %u %u", (int *)&a, &b, &c);
            e.st = (uint8_t)a; e.d1 = (uint8_t)b; e.d2 = (uint8_t)c; e.src = what[0] == 't' ? 2 : 1;
        } else if (!strcmp(what, "clock")) {    /* 24 a quarter: one every 2500 / BPM ms */
            uint32_t k;
            sscanf(line, "%*u %*s %u %u", &bpm, &nclk);
            for (k = 0; k < nclk && n < 65536u && bpm; k++) {
                host_ev_t c0 = {e.t + k * 2500u / bpm, 0, -1, 0xF8, 0, 0, -1, 0, 0, 0, 0, 1};
                ev[n++] = c0;
            }
            continue;
        }
        else continue;
        ev[n++] = e;
    }
    if (!feof(f)) fprintf(stderr, "%s: more than 65536 events, the rest (and the end line) ignored\n", path);
    fclose(f);
    for (i = 1; i < n; i++) {                   /* in time order, stable (a clock line adds its own events later on) */
        host_ev_t x = ev[i];
        uint32_t j = i;
        while (j && ev[j - 1u].t > x.t) { ev[j] = ev[j - 1u]; j--; }
        ev[j] = x;
    }
    script = ev;
    script_n = n;
}

static const char *host_out = "build/host/rdpno";
static int16_t *wav;
static uint32_t wav_n;

static void write_ppm_as(const char *name)
{
    FILE *f;
    uint32_t i;
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fprintf(f, "P6 240 240 255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) * 255 / 31, f);
        fputc((c >> 5 & 63) * 255 / 63, f);
        fputc((c & 31) * 255 / 31, f);
    }
    fclose(f);
}
static void write_ppm(uint32_t t)
{
    char name[512];
    snprintf(name, sizeof name, "%s-%u.ppm", host_out, t);
    write_ppm_as(name);
    printf("%6u ms  screen -> %s\n", t, name);
}

static void write_wav(void)
{
    char name[512];
    FILE *f;
    uint32_t bytes = wav_n * 4u, v[] = {16, 0x00020001u, FS, FS * 4u, 0x00100004u};
    snprintf(name, sizeof name, "%s.wav", host_out);
    if (!(f = fopen(name, "wb"))) { perror(name); exit(1); }
    fwrite("RIFF", 1, 4, f); bytes += 36; fwrite(&bytes, 4, 1, f); bytes -= 36;
    fwrite("WAVEfmt ", 1, 8, f); fwrite(v, 4, 5, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(wav, 4, wav_n, f);
    fclose(f);
    printf("sound  -> %s (%u frames)\n", name, wav_n);
}

/* DINGHY_PERF=1: the instructions the renders take, per sample (Linux perf counter; the chip runs other code, but
 * sloopDX's engines sit at ~1000-1500 here and fit the ISR, so this is the yardstick) */
static long perf_fd = -1;
static uint64_t perf_insns, perf_frames;
static void perf_open(void)
{
#ifdef __linux__
    struct perf_event_attr a;
    if (!getenv("DINGHY_PERF")) return;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE; a.size = sizeof a; a.config = PERF_COUNT_HW_INSTRUCTIONS;
    a.exclude_kernel = 1; a.exclude_hv = 1; a.disabled = 1;
    perf_fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
#endif
}
static void perf_report(void)
{
    if (perf_fd >= 0 && perf_frames)
        printf("perf   %llu instructions / sample over %llu frames (sloopDX engines: ~1000-1500)\n",
               (unsigned long long)(perf_insns / perf_frames), (unsigned long long)perf_frames);
}

static void platform_poll(void)                 /* one call = one millisecond */
{
    static uint32_t step, shot, owed, frame;
    static int32_t blk[2u * HALF_FRAMES];
    fm1_ms++;
#if BOARD_SMK37
    fm1_in.frames++;                            /* (the SMK scans ~625 frames a second; enough for "settled") */
#endif
    while (step < script_n && script[step].t <= fm1_ms) {
        if (script[step].smk == HS_SHOT) {
            write_ppm(fm1_ms);
        } else if (script[step].smk) {
#if BOARD_SMK37
            static uint32_t vel = 100;
            const host_ev_t *e = &script[step];
            uint32_t k;
            switch (e->smk) {
            case HS_VEL: vel = e->sb ? e->sb : 1u; for (k = 0; k < FM1_NKEY; k++) fm1_in.key_vel[k] = (uint8_t)vel; break;
            case HS_PAD: host_qput(0, (int32_t)((e->sa & 15u) << 8 | (e->sb & 0x7Fu))); break;
            case HS_FADER: fm1_in.fader[e->sa & 3u] = (uint8_t)(e->sb > 127u ? 127u : e->sb); break;
            case HS_WHEEL: fm1_in.wheel[e->sa & 1u] = e->sb; break;
            case HS_XKEY: if (e->sb) fm1_in.key_vel[e->sa % FM1_NKEY] = (uint8_t)e->sb; host_qput(1, (int32_t)((e->sa % FM1_NKEY) << 8 | (e->sb & 0x7Fu))); break;
            }
#else
            fprintf(stderr, "script: an SMK-37 event in an FM-1 build (BOARD_SMK37=0), ignored\n");
#endif
        } else if (script[step].st) {
            mq[mq_w % 64u][0] = script[step].st; mq[mq_w % 64u][1] = script[step].d1; mq[mq_w % 64u][2] = script[step].d2;
            mq[mq_w % 64u][3] = script[step].src;
            mq_w++;
        } else if (script[step].btn >= 200) {
            host_rel |= 1u << (script[step].btn - 200);
            fm1_in.buttons &= ~(1u << (script[step].btn - 200));
        } else if (script[step].btn >= 100) {
            host_edges |= 1u << (script[step].btn - 100);
            fm1_in.buttons |= 1u << (script[step].btn - 100);
        } else if (script[step].btn >= 0) {
            host_edges |= 1u << script[step].btn;
            host_rel_next |= 1u << script[step].btn;
        }
        else if (script[step].enc >= 0)
            host_enc[script[step].enc % HOST_NENC] += script[step].encn, host_enc_pos[script[step].enc % HOST_NENC] += script[step].encn;
        else
            fm1_in.notes = script[step].keys;
        step++;
    }
    owed += FS;                                 /* the frames this millisecond is worth, in blocks as the ISR renders */
    while (owed >= 1000u * HALF_FRAMES) {
        uint32_t i;
        owed -= 1000u * HALF_FRAMES;
        memset(blk, 0, sizeof blk);
#ifdef __linux__
        if (perf_fd >= 0) { ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0); ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0); }
#endif
        app_audio(blk, HALF_FRAMES);
#ifdef __linux__
        if (perf_fd >= 0) {
            long long c = 0;
            ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
            if (read(perf_fd, &c, sizeof c) == sizeof c && fm1_ms > 1000u) { perf_insns += (uint64_t)c; perf_frames += HALF_FRAMES; }
        }
#endif
        wav = realloc(wav, (wav_n + HALF_FRAMES) * 4u);
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            int32_t v = blk[i];                 /* (USB audio at the MASTER knob all the way up) */
            wav[2u * wav_n + i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        }
        wav_n += HALF_FRAMES;
    }
    if (frame_fps) {                            /* a film: a frame every 1/fps s, as the screen is then, and a log line:
                                                   keys held, key LEDs, button LEDs, the 7 encoder positions */
        if ((uint64_t)frame * 1000u <= (uint64_t)fm1_ms * frame_fps) {
            static FILE *log;
            char name[512];
            uint32_t e;
            snprintf(name, sizeof name, "%s-%05u.ppm", host_out, frame++);
            write_ppm_as(name);
            if (!log) { snprintf(name, sizeof name, "%s-log.txt", host_out); log = fopen(name, "w"); }
            if (log) {
                fprintf(log, "%u %u %u %u", fm1_ms, fm1_in.notes, (uint32_t)(host_leds >> 14), (uint32_t)(host_leds & 0x3FFFu));
                for (e = 0; e < (uint32_t)HOST_NENC; e++) fprintf(log, " %d", host_enc_pos[e]);
                fprintf(log, "\n");
                fflush(log);
            }
        }
    } else if (shot < sizeof SHOT / sizeof SHOT[0] && fm1_ms == SHOT[shot])
        write_ppm(SHOT[shot++]);
    if (fm1_ms >= end_ms) {
        write_wav();
        perf_report();
        exit(0);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) host_out = argv[1];
    if (argc > 2) read_script(argv[2]);
    if (argc > 3) frame_fps = (uint32_t)atoi(argv[3]);
    perf_open();
    app_main();
    return 0;
}
