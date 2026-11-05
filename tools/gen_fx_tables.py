#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""firmware/src/fx_tables.h: the equalizer's and the effects' constants (the FM-1 has no float).
  python3 tools/gen_fx_tables.py"""
from pathlib import Path
import math
FS=44100.0
Q24=1<<24
LEVELS=[-12+1.5*i for i in range(17)]          # dB, index 8 = 0
MIDF=[200,280,400,560,800,1000,1400,2000,2800,4000,5600,8000]
WIDTH_Q=[0.35,0.5,0.7,1.0,1.4,2.0,2.8,4.0]      # width 1..8: higher = narrower
def q(x): return int(round(x*Q24))
def shelf(fc,db,high):
    A=10**(db/40); k=math.sqrt(2)
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
"#define FX_NLEVEL 17u                 /* -12..+12 dB in 1.5 dB steps; 8 = 0 dB */",
"#define FX_NMIDF 12u","#define FX_NWIDTH 8u",
"/* shelves (Simper's SVF, Q 0.707), Q24: a1 a2 a3 m0 m1 m2 per level; bass 100 Hz, treble 10 kHz, at 44.1 kHz */",
arr("FX_BASS","int32_t",[c for d in LEVELS for c in shelf(100,d,False)]),
arr("FX_TREBLE","int32_t",[c for d in LEVELS for c in shelf(10000,d,True)]),
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
