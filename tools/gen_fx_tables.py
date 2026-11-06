#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""firmware/src/fx_tables.h: the equalizer's and the effects' constants (the FM-1 has no float).
  python3 tools/gen_fx_tables.py"""
from pathlib import Path
import math
FS=44100.0
Q24=1<<24
LEVELS=[-10.5+1.5*i for i in range(15)]        # dB, index 7 = 0: the MKS-20's -10.5..+10.5 in 1.5 dB steps
# The MKS-20's bass is a 100 Hz shelf, its treble a 10 kHz one. As gentle shelves (Q 0.5) centred at 200 Hz and
# 4 kHz they reach (nearly) their full level at 100 Hz and 10 kHz: a shelf centred at 10 kHz did next to nothing
# here, where the board's sound ends at 10 kHz (it samples at 20 kHz); one at 100 Hz little to a piano.
BASS_HZ=200
TREBLE_HZ=4000
SHELF_Q=0.5
MIDF=[400,560,770,1000,1400,2000,2800,4000]       # the MKS-20's eight
WIDTH_Q=[0.35,0.5,0.7,1.0,1.4,2.0,2.8,4.0]      # width 1..8: higher = narrower
def q(x): return int(round(x*Q24))
def shelf(fc,db,high):
    A=10**(db/40); k=1/SHELF_Q
    g=math.tan(math.pi*fc/FS)*(math.sqrt(A) if high else 1/math.sqrt(A))
    a1=1/(1+g*(g+k)); a2=g*a1; a3=g*a2
    if high: m0,m1,m2=A*A,k*(1-A)*A,1-A*A
    else: m0,m1,m2=1,k*(A-1),A*A-1
    return [q(a1),q(a2),q(a3),q(m0),q(m1),q(m2)]
def arr(name,typ,v,per=6):
    s=[f"static const {typ} {name}[{len(v)}] = {{"]
    for i in range(0,len(v),per): s.append("    "+", ".join(str(x) for x in v[i:i+per])+",")
    s.append("};"); return "\n".join(s)
out=["/* SPDX-License-Identifier: GPL-3.0-only */",
"/* The equalizer's and the effects' constants, computed on a computer (the FM-1 has no float): see the comments.",
" * Made by tools/gen_fx_tables.py. */",
"#pragma once","#include <stdint.h>","",
"#define FX_NLEVEL 15u                 /* -10.5..+10.5 dB in 1.5 dB steps; 7 = 0 dB (the MKS-20) */",
"#define FX_LEVEL0 7u",
"#define FX_NMIDF 8u","#define FX_NWIDTH 8u",
f"/* shelves (Simper's SVF, Q {SHELF_Q}), Q24: a1 a2 a3 m0 m1 m2 per level; centred at {BASS_HZ} and {TREBLE_HZ} Hz (full at 100 Hz, 10 kHz), at 44.1 kHz */",
arr("FX_BASS","int32_t",[c for d in LEVELS for c in shelf(BASS_HZ,d,False)]),
arr("FX_TREBLE","int32_t",[c for d in LEVELS for c in shelf(TREBLE_HZ,d,True)]),
"/* the middle band (a bell): g = tan(pi f / fs) per frequency, Q24 */",
arr("FX_MID_G","int32_t",[q(math.tan(math.pi*f/FS)) for f in MIDF]),
arr("FX_MID_HZ","uint16_t",MIDF),
"/* 1 / Q per width, Q24 */",
arr("FX_MID_K","int32_t",[q(1/x) for x in WIDTH_Q],8),
"/* per level: 1 / A and A^2 - 1 (A = 10^(dB/40)), Q24 */",
arr("FX_INV_A","int32_t",[q(1/10**(d/40)) for d in LEVELS]),
arr("FX_A2M1","int32_t",[q(10**(d/20)-1) for d in LEVELS]),
"/* voice level 0..15 -> gain Q12: 0 = off, 15 = 0 dB, 2 dB a step */",
arr("FX_VOL","uint16_t",[0]+[int(round(4096*10**((l-15)*2/20))) for l in range(1,16)],8),
"/* chorus rate 1..15: the LFO's phase step per 44.1 kHz sample (periods after rdpiano_juce's table, 2.7 s .. 175 ms) */",
arr("FX_CHO_INC","uint32_t",[int(round((1000.0/p)*2**32/FS)) for p in [2700,1380,910,680,540,450,385,335,300,265,245,220,205,190,175]],5),
"/* tremolo rate 1..15: 0.5 .. 7.5 Hz (as rdpiano_juce: rate x 0.5 Hz), phase step per sample */",
arr("FX_TRM_INC","uint32_t",[int(round((0.5*r)*2**32/FS)) for r in range(1,16)],5),
]
open(Path(__file__).resolve().parents[1] / 'firmware/src/fx_tables.h','w').write("\n".join(out)+"\n")
print("fx_tables.h written")
