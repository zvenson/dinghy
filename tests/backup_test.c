/* SPDX-License-Identifier: GPL-3.0-only */
/* kit_backup.h's protocol (from OberVave's test) against a fake flash: HELLO, READ, ERASE, WRITE, DONE, the region guard, pack7 both ways.
 *   cc -O2 -Wall -Ifirmware/hal -Ifirmware/src -o build/host/backup_test tests/backup_test.c && build/host/backup_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DINGHY_HOST 1
#define KIT_BACKUP_TEST 1
#define BOARD_SMK37 0
#define APP_NAME "DINGHY"
#define APP_VER "0.6-test"
#include "store.h"

static uint8_t FL[0x100000];
static uint8_t flash_ok = 1;
static volatile uint32_t stopped, fm1_ms;
#define BACKUP_STOP() (stopped = 1)
static int erases, writes;
static int st_read(uint32_t off, void *d, uint32_t n) { memcpy(d, FL + off, n); return 0; }
static int fl_erase4k(uint32_t off, uint32_t *t) { (void)t; memset(FL + off, 0xFF, 0x1000); erases++; return 0; }
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) FL[off + i] &= s[i]; writes++; return 0; }
static void fm1_wdt_feed(void) {}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { (void)p; (void)n; return 0; }
static void ota_frame_done(void) {}
#define C_BLACK 0
#define C_WHITE 0xFFFF
#define RGB(r, g, b) 0
static int FONT_L, FONT_S;
static void lcd_fill(int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void draw_text_box(int x, int y, int w, const int *f, const char *s, int c, int a) { (void)x; (void)y; (void)w; (void)f; (void)s; (void)c; (void)a; }

#include "kit_backup.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t req(uint8_t *q, uint8_t cmd) { q[0] = 0x7D; q[1] = 'D'; q[2] = 'Y'; q[3] = cmd; return 4; }

int main(void)
{
    uint8_t q[600], data[256], back[256];
    uint32_t n, w, i;
    int rb = 0, k;
    for (i = 0; i < sizeof FL; i++) FL[i] = (uint8_t)(i * 7u + (i >> 9));

    /* HELLO */
    n = req(q, 0);
    w = bk_handle(q, n, &rb);
    CHECK(w > 10 && bk_out[0] == 0xF0 && bk_out[w - 1] == 0xF7 && bk_out[4] == 0 && bk_out[5] == 1 && bk_out[6] == 0 && bk_out[7] == 3);
    CHECK(bk_addr(bk_out + 8) == SETTINGS_AT && bk_out[11] == 1);
    CHECK(bk_addr(bk_out + 12) == BLOBS_AT && bk_out[15] == NBLOB);
    CHECK(bk_addr(bk_out + 16) == SLOTS_AT && bk_out[19] == NSLOT * SLOT_SECT);
    CHECK(!strcmp((const char *)bk_out + 20, "DINGHY 0.6-test"));
    for (i = 1; i + 1 < w; i++) CHECK(bk_out[i] < 0x80);

    /* READ 256 from a slot, unpacked = the flash */
    n = req(q, 1); n += bk_put_addr(q + n, SLOTS_AT + 0x100); q[n++] = 0; q[n++] = 2;   /* 256 */
    w = bk_handle(q, n, &rb);
    CHECK(bk_out[4] == 1 && bk_addr(bk_out + 5) == SLOTS_AT + 0x100 && bk_out[8] == 0);
    k = bk_unpack7(back, sizeof back, bk_out + 9, w - 10);
    CHECK(k == 256 && !memcmp(back, FL + SLOTS_AT + 0x100, 256));
    for (i = 1; i + 1 < w; i++) CHECK(bk_out[i] < 0x80);
    /* READ outside the regions (the firmware, the calibration) or across a region's end: refused */
    n = req(q, 1); n += bk_put_addr(q + n, 0x10000); q[n++] = 16; q[n++] = 0;
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1);
    n = req(q, 1); n += bk_put_addr(q + n, SETTINGS_AT + 0xF80); q[n++] = 0; q[n++] = 2;
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1);
    n = req(q, 1); n += bk_put_addr(q + n, SETTINGS_AT); q[n++] = 0; q[n++] = 3;     /* 384 > 256 */
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1);

    /* WRITE before any ERASE: refused (only a restore writes) */
    for (i = 0; i < 256; i++) data[i] = (uint8_t)(255 - i);
    n = req(q, 3); n += bk_put_addr(q + n, BLOBS_AT); n += bk_pack7(q + n, data, 256);
    CHECK(n <= 640);                             /* (fits the platform's SysEx frame) */
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1 && writes == 0);

    /* ERASE: aligned, in a region; it locks and stops the instrument */
    n = req(q, 2); n += bk_put_addr(q + n, BLOBS_AT + 0x10);
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1 && erases == 0 && !bk_lock);
    n = req(q, 2); n += bk_put_addr(q + n, 0x90000);
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1 && erases == 0);
    n = req(q, 2); n += bk_put_addr(q + n, BLOBS_AT);
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 0 && erases == 1 && bk_lock && stopped);
    CHECK(FL[BLOBS_AT] == 0xFF && FL[BLOBS_AT + 0xFFF] == 0xFF);

    /* WRITE: lands, reads back */
    n = req(q, 3); n += bk_put_addr(q + n, BLOBS_AT); n += bk_pack7(q + n, data, 256);
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 0 && writes == 1 && !memcmp(FL + BLOBS_AT, data, 256));
    n = req(q, 3); n += bk_put_addr(q + n, BLOBS_AT + 0x100); n += bk_pack7(q + n, data, 5);   /* a short tail */
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 0 && !memcmp(FL + BLOBS_AT + 0x100, data, 5) && FL[BLOBS_AT + 0x105] == 0xFF);
    n = req(q, 3); n += bk_put_addr(q + n, 0x20000); n += bk_pack7(q + n, data, 16);         /* outside: refused */
    bk_handle(q, n, &rb); CHECK(bk_out[8] == 1 && writes == 2);

    /* DONE: asks for the restart */
    n = req(q, 4);
    w = bk_handle(q, n, &rb); CHECK(w == 7 && bk_out[5] == 0 && rb == 1);

    /* not ours: another header, an unknown command, a short frame */
    q[0] = 0x7D; q[1] = 'F'; q[2] = 'S'; q[3] = 1; CHECK(bk_handle(q, 4, &rb) == 0);
    n = req(q, 9); CHECK(bk_handle(q, n, &rb) == 0);
    CHECK(bk_handle(q, 3, &rb) == 0);

    /* pack7 both ways, every length 0..256 */
    for (k = 0; k <= 256; k++) {
        uint8_t pk[300], un[256];
        for (i = 0; i < (uint32_t)k; i++) data[i] = (uint8_t)(rand());
        n = bk_pack7(pk, data, (uint32_t)k);
        CHECK(n == (uint32_t)k + ((uint32_t)k + 6u) / 7u);
        CHECK(bk_unpack7(un, sizeof un, pk, n) == k && !memcmp(un, data, (size_t)k));
    }
    printf(fails ? "backup_test: %d failed\n" : "backup_test: all passed\n", fails);
    return fails != 0;
}
