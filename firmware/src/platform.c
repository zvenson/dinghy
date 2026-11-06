/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy's platform: everything an FM-1 (or SMK-37) firmware needs before it can be fun, taken unchanged from
 * SLOOP / Felucca / sloopDX / OberVave and wired up here. You should not need to touch this file.
 *
 *   boot + boot guard   a firmware that crashes at boot falls back to the USB rescue, then to the ROM
 *   USB rescue          OCT- held at power-on: no audio, only the updater (the web installer still works)
 *   updater             the M-UPGRADE SysEx path: the web installer / the hub can always reach the FM-1
 *   MIDI                USB in and out, the FM-1's TRS jack in (midi_get: 1 USB, 2 TRS; midi_send); clock and
 *                       transport (F8 FA FB FC) reach the app too
 *   USB audio           the output as a class-compliant 16-bit stereo input, 44.1 or 48 kHz
 *   keys, knobs, LEDs   fm1_in.notes / buttons, fm1_input_edges, fm1_enc_take, fm1_led_key, fm1_led_key_glow
 *                       (hal/fm1_input.h; SMK-37: hal/smk_input.h, the same calls and velocity, pads, faders, wheels)
 *   screen              240 x 240, lcd_fill, draw_text_box with FONT_S (16 px) / FONT_L (32 px) (gfx.c); LCD_IDLE
 *   storage             settings, NBLOB blobs of one sector, NSLOT slots of SLOT_SECT sectors, each with a CRC (store.h)
 *   crash screen        the PC of a fault, then a reboot
 *
 * The app (dinghy.c) includes this file first and supplies:
 *   static void app_audio(int32_t *out, uint32_t n)   audio ISR: n stereo frames, L R L R, Q15 (+-32767), out is zero
 *   static void app_main(void)                        never returns; calls platform_poll() often (every few ms)
 * and may define, before the #include:
 *   LCD_IDLE()        called while the screen's DMA sends a strip (the keys, MIDI: nothing waits for a redraw)
 *   FELUCCA_UAC 0     no USB audio (MIDI only)
 */
/* the panel by name: buttons (fm1_in.buttons, fm1_input_edges, fm1_led_key: bit / id), the encoders
 * (fm1_enc_take); the 27 note keys are fm1_in.notes bit 0 (F3) .. 26 (G5), LED id 14 + key */
#ifndef BOARD_SMK37
#define BOARD_SMK37 0                           /* 1: the M-Vave SMK-37 (tools/build.py --board smk37; hal/fm1_cc.h) */
#endif
#if BOARD_SMK37
/* the M-Vave SMK-37 (docs/SMK37.md): the FM-1's buttons on the SMK buttons that mean the same (the fm1-to-smk37 kit's
 * printed-name map: PATCH = HOME, PARA = EDIT, CHORD = ENV, NOTE REPEAT = LFO, SCALE = SEL, GLOBE = GLO, BT = SAVE),
 * the four extras; K1 is the MASTER pot (hal/smk_input.h), K2 SELECT, K3 ALGORITHM, K4 PRESETS, K5..K8 KNOB 1..4.
 * The note keys have no LEDs here (the pads have colours: padled_set): KEY_LED ids are past the button LEDs and do nothing */
enum { BTN_KNOB_BANK = 0, BTN_FADER_BANK = 1, BTN_LEFT = 2, BTN_RIGHT = 3, BTN_OCT_DOWN = 4, BTN_OCT_UP = 5,
       BTN_ARP = 6, BTN_LFO = 7, BTN_SEL = 8, BTN_ENV = 9, BTN_GLO = 10, BTN_SAVE = 11, BTN_HOME = 12, BTN_EDIT = 13,
       BTN_FX = 14, BTN_SEQ = 15, BTN_PLAY = 16, BTN_REC = 17 };
enum { ENC_MASTER = 0, ENC_SELECT = 1, ENC_ALGORITHM = 2, ENC_PRESETS = 3, ENC_KNOB1 = 4, ENC_KNOB2 = 5, ENC_KNOB3 = 6,
       ENC_KNOB4 = 7 };
#define KEY_LED(k) (32u + (k))
#else
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))
#endif

#ifdef DINGHY_HOST
#include "../../tests/host.c"                  /* on a computer: a fake FM-1 (tests/host.c) */
#else
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_cpu1.h"                           /* the second core (from Clipper: X0X's, after Melodee's) */
#if BOARD_SMK37
#include "smk_input.h"                          /* keys with velocity, buttons, 8 encoders, LEDs, pads, faders, wheels */
static void fm1_led_key_glow(uint32_t id, int on) { (void)id; (void)on; }   /* (no key LEDs: the pads glow) */
#else
#define FM1_SETTLE_US 100u                      /* the boot scan (OCT- for the rescue): 10 us read no key on an FM-1, the tick waits 100 too */
#include "fm1_input.h"
static void fm1_led_key_glow(uint32_t id, int on)   /* as fm1_led_key, the dim layer (~1/24): "there is something here" */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on) fm1_led_dim[p] |= (uint8_t)(1u << r);
                else fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}
#endif
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_audio.h"
#if !BOARD_SMK37
#include "fm1_adc.h"                            /* (SMK: smk_input.h stands in, MASTER = K1) */
#endif
#include "bootguard.h"

#ifndef APP_NAME
#define APP_NAME "DINGHY"
#endif
#ifndef FELUCCA_ID
#if BOARD_SMK37
#define FELUCCA_ID SMK_MODEL "_990"             /* SMK-37 Pro_99N (Elite, MKE-P37: SMK_MODEL), tools/build.py */
#else
#define FELUCCA_ID "FM-1_990"                   /* package identity: 99N = Dinghy build N (tools/build.py) */
#endif
#endif
#ifndef APP_VER
#define APP_VER "0.0"
#endif

bootguard_t bootguard __attribute__((section(".noinit")));
static volatile uint32_t fm1_ms;                /* milliseconds since power-on (TIMER5) */

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "libc.c"
#include "lcd.c"
#ifndef CV_MAX
#define CV_MAX (240u * 64u)                     /* the biggest text box drawn at once (canvas in RAM: 2 B a pixel) */
#endif
#include "gfx.c"

#define FS 44100u                               /* the sample rate */
#define HALF_FRAMES 256u                        /* I2S half buffer: 5.8 ms (as sloopDX: the USB audio band is set for it) */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1                           /* USB audio to the computer (0: MIDI only, as zp12) */
#endif
#define FELUCCA_UART (!BOARD_SMK37)             /* the FM-1's TRS MIDI IN (SMK-37: none, PH8 is a key-matrix row) */
#if BOARD_SMK37 && FELUCCA_UART
#error "BOARD_SMK37: no UART MIDI IN (RX on PH8, a key-matrix row on the SMK)"
#endif
#include "usb.c"
#if FELUCCA_UART
#include "midi_uart.c"                          /* (sloopDX's, after Felucca Salt: the ring read by content; cable 1) */
#endif
#if FELUCCA_UAC
#define UAC_ONLY(x) x
#else
#define UAC_ONLY(x) ((void)0)
#endif

/* ---- flash: read anywhere; erase / program only the update's staging area and the app's store (store.h) */
#include "store.h"
#include "fm1_flash.h"
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}

static uint8_t recovery_active;
#if BOARD_SMK37
#define OTA_IDENTITY (recovery_active ? SMK_MODEL "_000" : FELUCCA_ID)
#else
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#endif
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active) recovery_poll();
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
#if BOARD_SMK37   /* step 5 stays on screen while the loader writes the flash: say so */
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "UPDATING..."};
#else
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
#endif
    if (recovery_active) return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u)
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_WHITE, 1);
    else
        draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_WHITE, 1);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "recovery.c"

/* ---- storage: [magic][length][crc32] then the bytes. A save erases (~50 ms a sector with the interrupts off:
 * the sound drops out), so save when nothing plays, not on every knob step. Each sector wears out after ~100,000 */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
static int area_load(uint32_t at, uint32_t max, uint32_t magic, void *p, uint32_t n)   /* reads straight into p */
{
    uint32_t h[3];
    if (!flash_ok || n > max || st_read(at, h, sizeof h) || h[0] != magic || h[1] != n || st_read(at + sizeof h, p, n))
        return -1;
    return crc32(p, n) == h[2] ? 0 : -1;
}
static int area_save(uint32_t at, uint32_t sectors, uint32_t magic, const void *p, uint32_t n)   /* p in RAM */
{
    uint32_t h[3] = {magic, n, 0}, k, took;
    int rc;
    if (!flash_ok || n > sectors * 0x1000u - sizeof h)
        return -1;
    h[2] = crc32(p, n);
    for (k = 0; k < sectors; k++) {
        fm1_wdt_feed();                         /* (~50 ms an erase) */
        if ((rc = fl_erase4k(at + k * 0x1000u, &took)) != 0)
            return rc;
    }
    if ((rc = fl_write(at + sizeof h, p, n)) != 0)
        return rc;
    return fl_write(at, (const uint8_t *)h, sizeof h);   /* the header last: a cut save stays invalid */
}
/* one sector each, up to SECTOR_MAX bytes; 0: done. A load that fails leaves p as it was (a copy first) */
static int sector_load(uint32_t at, void *p, uint32_t n)
{
    static uint8_t buf[SECTOR_MAX];
    if (n > sizeof buf || area_load(at, SECTOR_MAX, STORE_MAGIC, buf, n))
        return -1;
    memcpy(p, buf, n);
    return 0;
}
static int settings_load(void *p, uint32_t n) { return sector_load(SETTINGS_AT, p, n); }
static int settings_save(const void *p, uint32_t n) { return area_save(SETTINGS_AT, 1u, STORE_MAGIC, p, n); }
static int blob_load(uint32_t i, void *p, uint32_t n) { return i < NBLOB ? sector_load(BLOBS_AT + i * 0x1000u, p, n) : -1; }
static int blob_save(uint32_t i, const void *p, uint32_t n) { return i < NBLOB ? area_save(BLOBS_AT + i * 0x1000u, 1u, STORE_MAGIC, p, n) : -1; }
/* slot i: SLOT_SECT sectors, up to SLOT_MAX bytes. slot_load reads straight into p (no buffer that big): on -1 p holds
 * rubbish, start afresh */
#define SLOT_MAGIC 0x544F4C53u                  /* "SLOT" */
static int slot_load(uint32_t i, void *p, uint32_t n) { return i < NSLOT ? area_load(SLOTS_AT + i * SLOT_SECT * 0x1000u, SLOT_MAX, SLOT_MAGIC ^ STORE_MAGIC, p, n) : -1; }
static int slot_save(uint32_t i, const void *p, uint32_t n) { return i < NSLOT ? area_save(SLOTS_AT + i * SLOT_SECT * 0x1000u, SLOT_SECT, SLOT_MAGIC ^ STORE_MAGIC, p, n) : -1; }
#if BOARD_SMK37
/* the store's owner (the kit's convention, store.h): our tag, load as ever; anything else (the stock settings,
 * another port's data) is cleared once and the tag written, before the audio starts */
static uint8_t smk_store_cleared, smk_trim_ok;   /* (the panel check shows them) */
static uint32_t smk_jedec;
static void smk_owner_check(void)
{
    uint8_t tag[16], want[16];
    uint32_t i, ours = 1, took;
    if (!flash_ok)
        return;
    smk_owner_tag(want);
    if (st_read(OWNER_AT, tag, 16))
        return;
    for (i = 0; i < 16u; i++)
        ours &= tag[i] == want[i];
    if (ours)
        return;
    fl_erase4k(SETTINGS_AT, &took);
    for (i = 0; i < NBLOB; i++) {
        fm1_wdt_feed();                         /* (~50 ms an erase) */
        fl_erase4k(BLOBS_AT + i * 0x1000u, &took);
    }
    for (i = 0; i < NSLOT; i++) {               /* (each slot's head; another firmware's samples go) */
        fm1_wdt_feed();
        fl_erase4k(SLOTS_AT + i * SLOT_SECT * 0x1000u, &took);
    }
    fl_erase4k(OWNER_AT, &took);
    fl_write(OWNER_AT, want, 16);
    smk_store_cleared = 1;
}
/* the factory key / pad velocity curves (USRTRIM 0xC1000, read only; smk_input.h checks them, else its defaults) */
static void smk_trim_read(void)
{
    static uint8_t rec[0x22Cu];
    if (flash_ok && !st_read(SMK_TRIM_FLASH, rec, sizeof rec))
        smk_trim_ok = (uint8_t)smk_trim_load(rec);
}
#endif

/* ---- MIDI: USB and the TRS jack share one ring of 4-byte USB-MIDI packets (USB cable 0, the jack cable 1) */
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)   /* 1 (USB) or 2 (TRS): a message; 0: none */
{
    uint32_t pk;
    if (mi_r == mi_w)
        return 0;
    pk = midi_in_q[mi_r % MQ];
    mi_r++;
    *st = (uint8_t)(pk >> 8);
    *d1 = (uint8_t)(pk >> 16);
    *d2 = (uint8_t)(pk >> 24);
    return pk & 0xF0u ? 2 : 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2)   /* USB out, from the main loop only; F8 FA FB FC too */
{
    midi_out_event((st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24);
}

/* ---- the timer: 10 kHz key scan, milliseconds, USB at 2 kHz. It outranks the audio ISR; nested in a
 * render it only scans, counts and feeds the USB audio stream (as sloopDX main.c) */
static volatile uint8_t in_audio;
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed, last, acc;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (usb_due)
        owed |= 1u;
#if FELUCCA_UART
    if (sub % 5u == 2u)
        owed |= 2u;                             /* 2 kHz: <= ~7 bytes per call at 31250 baud */
#endif
    if (++sub == 10u)
        sub = 0;
#if !FELUCCA_UAC                                /* MIDI only: the USB poll keeps its 2 kHz, nested in a render too (as zp12,
                                                   whose MIDI out works on the device) */
    if (usb_due)
        usb_poll();
#if FELUCCA_UART
    if (owed & 2u)
        uart_midi_poll();
#endif
    owed = 0;
    return;
#endif
    if (in_audio) {
        if (usb_due)
            UAC_ONLY(uac_service());
        return;
    }
    if (owed & 1u)
        usb_poll();
#if FELUCCA_UART
    if (owed & 2u)
        uart_midi_poll();
#endif
    owed = 0;
}
extern void isr_timer5(void);

/* ---- the sound: ALNK0 I2S, two halves; the app renders, the MASTER knob scales, Q15 -> 24 bit (-6 dBFS ceiling) */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile int32_t master_q12 = 2048;
static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    in_audio = 1;
    p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
        uint32_t i, b;
        for (i = 0; i < HALF_WORDS; i++)
            o[i] = 0;
        UAC_ONLY(uac_render_start());
        app_audio(o, HALF_FRAMES);
        for (i = 0; i < HALF_WORDS; i++) {
            int32_t v = (o[i] * master_q12) >> 12;
            o[i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
        }
        for (b = 0; b < HALF_FRAMES; b += 32u)
            UAC_ONLY(uac_tap(o + 2u * b, 32u));   /* the computer records what the speaker plays */
        for (i = 0; i < HALF_WORDS; i++)
            o[i] <<= 7;
        fm1_audio_ack_half();
    }
    in_audio = 0;
}
extern void isr_alnk0(void);

/* ---- the main loop's duties: watchdog, USB, the installer's update request, the MASTER knob */
static void platform_poll(void)
{
    static int32_t knob_avg = 512 * 16;
    int32_t a;
    fm1_wdt_feed();
    usb_retry(fm1_ms);
    ota_service();
    if (usb.ota_req) {
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();                      /* returns only if nothing was committed */
    }
    if (usb.uboot_req) {
        fm1_audio_stop();
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 110, 240, &FONT_S, "UBOOT (USB)", C_WHITE, 1);
        usb_detach();
        fm1_delay_ms(30);
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    if (fm1_ms >= 30000u && bootguard.pending)
        bootguard_clear(&bootguard);            /* 30 s up: this firmware boots */
    if ((a = fm1_adc_read(FM1_ADC_MASTER)) >= 0) {
        uint32_t k10;
        knob_avg += (a * 16 - knob_avg) / 8;
        k10 = (uint32_t)(knob_avg / 16);
        master_q12 = (int32_t)((k10 * k10) >> 8);
    }
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t i, t0, v[3] = {c->vec, c->pc, c->rets};
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 20, 240, &FONT_S, APP_NAME " CRASH", C_WHITE, 1);
    for (i = 0; i < 3u; i++) {
        uint32_t k;
        for (k = 0; k < 8u; k++) b[k] = "0123456789ABCDEF"[(v[i] >> (28u - 4u * k)) & 15u];
        b[8] = 0;
        draw_text_box(10, 60 + i * 20, 220, &FONT_S, b, C_WHITE, 0);
    }
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 8000u * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    fm1_reboot();
}

/* ---- the second core (Clipper's cpu1.c, cut down): started at power-on and proved with a self-test job; the
 * app hands it work through cpu1_run / cpu1_wait. If it does not answer, or a job comes back wrong or late, the
 * FM-1 runs on one core (some units' core 1 reads the RAM wrong: fm1_cpu1.h) */
enum { CPU1_OFF, CPU1_NOSTART, CPU1_FAILED, CPU1_OK };
static uint8_t cpu1_state = CPU1_OFF;
static volatile uint32_t cpu1_test_out;
static uint32_t cpu1_lcg(uint32_t x)
{
    uint32_t i;
    for (i = 0; i < 64u; i++)
        x = x * 1664525u + 1013904223u;
    return x;
}
static void cpu1_selftest(uint32_t arg) { cpu1_test_out = cpu1_lcg(arg); }
static void cpu1_boot(void)
{
    fm1_guard_pc_open();                        /* (it starts in the chip's ROM) */
    if (fm1_cpu1_start() != 0) {
        fm1_guard_enable(FM1_GUARD_PC);
        cpu1_state = CPU1_NOSTART;
        return;
    }
    fm1_guard_enable(FM1_GUARD_PC);
    cpu1_test_out = 0;
    if (!fm1_cpu1_run(cpu1_selftest, 20261106u) || fm1_cpu1_wait() != 0 || cpu1_test_out != cpu1_lcg(20261106u)) {
        fm1_cpu1_hold();
        cpu1_state = CPU1_FAILED;
        return;
    }
    cpu1_state = CPU1_OK;
}
static int cpu1_on(void) { return cpu1_state == CPU1_OK && fm1_c1_on; }
static int cpu1_run(void (*fn)(uint32_t), uint32_t arg) { return cpu1_state == CPU1_OK ? fm1_cpu1_run(fn, arg) : 0; }
static int cpu1_wait(void)                      /* 0 = done; -1 = given up: one core from now on */
{
    if (fm1_cpu1_wait() == 0)
        return 0;
    cpu1_state = CPU1_FAILED;
    return -1;
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    extern uint32_t _c1_start[], _c1_end[], _c1_load[];
    uint32_t *s, *d, p3, boot_mode;
    fm1_cpu1_halt();                            /* a second core a reset left running would read the RAM cleared
                                                 * below (Clipper, fm1_sys.h: registers only) */
    fm1_time_init();
    fm1_supplies_stock();                       /* VDC14 1.60 V, SYSVDD 1.38 V, as M-VAVE's firmware: both cores read
                                                 * the flash and the SRAM right on every unit (Clipper, fm1_sys.h) */
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _pool_start; d < _pool_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    for (s = _c1_load, d = _c1_start; d < _c1_end; s++, d++) *d = *s;   /* the second core's loop */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    if (boot_mode == BOOT_RECOVERY || recovery_key())
        recovery_main();                        /* OCT- at power-on: the USB rescue */
#if BOARD_SMK37
    smk_jedec = FL_FAR(fl_jedec_ram)();         /* (the panel check shows it: units may carry another part) */
    flash_ok = FL_JEDEC_OK(smk_jedec);
#else
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());
#endif
    ota_boot_cleanup();
    lcd_init();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    fm1_input_init();
    fm1_adc_init();
#if BOARD_SMK37
    smk_trim_read();                            /* (after fm1_input_init: it sets the defaults first) */
    smk_owner_check();
#endif
    cpu1_boot();                                /* the second core: before the audio and the timers (Clipper) */
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);   /* (abuf is zero: .bss) */
    usb_start();
#if FELUCCA_UART
    uart_midi_init();                           /* before TIMER5: PORTH read-modify-write */
#endif
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    app_main();
    for (;;)
        platform_poll();
}
#endif
