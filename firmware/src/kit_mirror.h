/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 Mirror: your firmware's screen, live and big on a computer or phone, for players who cannot read the 1.54-inch
 * display – Patrick F. Kellogg's FM-1 Mirror app (github.com/pfkellogg/fm1-mirror) and his screen_mirror module
 * (screen_mirror/, MIT). Nothing is sent unless the app asks (its hello about once a second); 3 s after the last one
 * it stops by itself.
 *
 * lcd.c hands every RAMWR's pixels here (LCD_MIRROR): they become rows for screen_mirror. The app's hello is taken
 * from the platform's SysEx frame before platform_poll would drop it (not an update), the rows go out through the
 * same SysEx queue as the installer's replies.
 *
 * Use: #define LCD_MIRROR 1 before the platform.c include, include this after it, and call mirror_poll() each
 * main-loop pass before platform_poll(): when it returns 1, draw your whole screen again (a new viewer). */
#include "screen_mirror/screen_mirror.c"

static uint32_t mr_x0, mr_x1, mr_x, mr_y;        /* the window lcd.c writes into, and where its next pixel goes */
static uint16_t mr_row[240];
static uint8_t mr_lost;                          /* rows did not fit: the next pass draws everything again */

#ifndef DINGHY_HOST
static int mr_send_wait(const uint8_t *m, unsigned n)   /* (blocking: the buffer is full in the middle of a picture) */
{
    return usb.config && !ota_wire_send(m, n) ? 0 : -1;
}
static int mr_send(const uint8_t *m, unsigned n)        /* (main loop: only what the USB queue takes now) */
{
    if (!usb.config)
        return -1;
    if (SXQ - (so_w - so_r) < (n + 2u) / 3u + 1u)
        return -2;
    return ota_wire_send(m, n) ? -1 : 0;
}
#else                                            /* the host has no USB: nothing is ever asked for or sent */
static int mr_send_wait(const uint8_t *m, unsigned n) { (void)m; (void)n; return -1; }
static int mr_send(const uint8_t *m, unsigned n) { (void)m; (void)n; return -1; }
#ifndef KIT_HOST_FRAMES
#define KIT_HOST_FRAMES
static int ota_frame_get(const uint8_t **p, uint32_t *n) { (void)p; (void)n; return 0; }
static void ota_frame_done(void) {}
#endif
#endif

static void lcd_mirror_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    (void)y1;
    mr_x0 = x0, mr_x1 = x1, mr_x = x0, mr_y = y0;
}

static void lcd_mirror_px(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t px = n / 2u;
    if (!sm_on())
        return;
    while (px && mr_y < 240u) {
        uint32_t w = mr_x1 + 1u - mr_x, k = px < w ? px : w, i;
        for (i = 0; i < k; i++)
            mr_row[i] = (uint16_t)((uint32_t)b[2u * i] << 8 | b[2u * i + 1u]);
        if (!sm_rows(mr_row, mr_x, mr_y, k, 1)) {
            sm_poll(fm1_ms, mr_send_wait);       /* full: send what waits, then this row */
            if (!sm_rows(mr_row, mr_x, mr_y, k, 1))
                mr_lost = 1;
        }
        b += 2u * k, px -= k, mr_x += k;
        if (mr_x > mr_x1)
            mr_x = mr_x0, mr_y++;
    }
}

/* every main-loop pass, before platform_poll: the app's hello, then what is queued; 1 = draw the whole screen again */
static int mirror_poll(void)
{
    const uint8_t *p;
    uint32_t n;
    if (ota_frame_get(&p, &n)) {
        if (n == 4u && p[0] == 0x7Du && p[1] == 'F' && p[2] == 'S' && p[3] <= 1u) {
            const uint8_t m[6] = {0xF0, 0x7D, 'F', 'S', p[3], 0xF7};
            ota_frame_done();
            sm_sysex(m, 6);
        }
    }
    sm_poll(fm1_ms, mr_send);
    if (sm_take_full_request() || (mr_lost && sm_on())) {
        mr_lost = 0;
        return 1;
    }
    return 0;
}
