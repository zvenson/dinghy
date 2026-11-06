/* SPDX-License-Identifier: GPL-3.0-only */
/* The one script tests/rdp_test.c (the C port) and tests/rdp_ref.cpp (rdpiano's librdpiano) both play, sample for
 * sample: notes, the sustain pedal, patch changes (to a 32 kHz patch too). t is in samples of the patch's rate; a patch number is taken modulo the set's count. */
#pragma once
typedef struct { unsigned t; int patch; unsigned char st, d1, d2; } rdp_ev_t;   /* patch >= 0: a patch change */
static const rdp_ev_t RDP_SCRIPT[] = {
    {0, -1, 0x90, 48, 100}, {2000, -1, 0x90, 55, 90}, {4000, -1, 0x90, 64, 110}, {6000, -1, 0x90, 67, 70},
    {8000, -1, 0x90, 72, 127}, {10000, -1, 0x90, 76, 60}, {12000, -1, 0x90, 79, 100}, {14000, -1, 0x90, 84, 120},
    {20000, -1, 0x80, 48, 0}, {20000, -1, 0x80, 55, 0}, {22000, -1, 0x90, 64, 0}, {24000, -1, 0xB0, 64, 127},
    {24500, -1, 0x80, 67, 0}, {25000, -1, 0x80, 72, 0}, {26000, -1, 0x90, 36, 127}, {27000, -1, 0x80, 36, 0},
    {34000, -1, 0xB0, 64, 0},
    {40000, 4, 0, 0, 0},                                        /* A.PIANO 1 */
    {41000, -1, 0x90, 60, 100}, {41000, -1, 0x90, 64, 100}, {41000, -1, 0x90, 67, 100}, {41000, -1, 0x90, 71, 100},
    {41000, -1, 0x90, 74, 100}, {41000, -1, 0x90, 77, 100}, {41000, -1, 0x90, 81, 100}, {41000, -1, 0x90, 84, 100},
    {41000, -1, 0x90, 40, 100}, {41000, -1, 0x90, 43, 100}, {41000, -1, 0x90, 47, 100}, {41000, -1, 0x90, 50, 100},
    {41000, -1, 0x90, 52, 100}, {41000, -1, 0x90, 55, 100}, {41000, -1, 0x90, 59, 100}, {41000, -1, 0x90, 62, 100},
    {41000, -1, 0x90, 88, 100}, {41000, -1, 0x90, 91, 100},     /* 18 notes on 16 voices: the board steals two */
    {60000, -1, 0x80, 60, 0}, {60000, -1, 0x80, 64, 0}, {60000, -1, 0x80, 67, 0}, {60000, -1, 0x80, 71, 0},
    {70000, 3, 0, 0, 0},                                        /* CONTEMPORARY, 32 kHz */
    {71000, -1, 0x90, 57, 110}, {75000, -1, 0x90, 69, 90}, {79000, -1, 0x80, 57, 0}, {83000, -1, 0x80, 69, 0},
    {90000, 7, 0, 0, 0},                                        /* VIBRAPHONE */
    {91000, -1, 0x90, 72, 100}, {93000, -1, 0x90, 76, 100}, {95000, -1, 0x80, 72, 0}, {97000, -1, 0x80, 76, 0},
};
#define RDP_SCRIPT_N (sizeof RDP_SCRIPT / sizeof RDP_SCRIPT[0])
#define RDP_SCRIPT_END 110000u
