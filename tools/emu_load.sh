#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# How much of the FM-1's audio interrupt the sound takes, measured on Joakim Eriksson's FM-1 emulator
# (github.com/joakimeriksson/fm1-emu) in its strict mode: every instruction costs its cycles as measured on an
# FM-1 (and the flash cache's misses), at the real 240 MHz. Plays a few scenes and prints, from 1.5 s on, the
# highest and the mean load (rdpno.c: the render's share of a 5.8 ms block) and the most parts sounding; then
# where the cycles go in a burst of the repeated keys (tools/emu_prof.py).
#   tools/emu_load.sh [FM1EMU_DIR]          after ./build.sh; FM1EMU_DIR: a checkout (default ../fm1-emu)
# In CI each result also becomes a notice (::notice), readable in the check run.
set -e
cd "$(dirname "$0")/.."
E="${1:-../fm1-emu}"
[ -d "$E/crates/fm1-live" ] || git clone -q --depth 1 https://github.com/joakimeriksson/fm1-emu "$E"
(cd "$E" && cargo build -q --release -p fm1-live)
LIVE="$E/target/release/fm1-live"
OBJDUMP="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}/common/bin/objdump"
"$OBJDUMP" -t build/rdpno.elf > build/rdpno.syms
addr() { awk -v s="$1" '$NF == s { print $1 }' build/rdpno.syms; }
A="$(addr rdp_load_max) $(addr rdp_parts_max) $(addr rdp_load_sum) $(addr rdp_load_n)"
MS="${EMU_MS:-3000}"
CHORD8="400:PLAY,500:m903c64,520:m904064,540:m904364,560:m904864,580:m904c64,600:m905064,620:m905364,640:m905864"
CHORD16="$CHORD8,700:m902b64,720:m902f64,740:m903264,760:m903764,780:m903b64,800:m903e64,820:m904564,840:m904a64"
scene() {   # name, FM1_BENCH_KEYS
    FM1_STRICT=1 FM1_DUMPRAM=build/emu_ram.bin FM1_BENCH_KEYS="$2" "$LIVE" --fwsc build/rdpno.fwsc \
        --mhz 240 --bench "$MS" > build/emu_"$1".log 2>&1 || { echo "emu_load: $1 failed"; tail -5 build/emu_"$1".log; return 1; }
    python3 - "$1" $A <<'PY'
import struct, sys
ram = open("build/emu_ram.bin", "rb").read()
def word(a): return struct.unpack_from("<I", ram, int(a, 16) - 0x01C00000)[0]
mx, parts, sm, n = (word(a) for a in sys.argv[2:6])
line = f"{sys.argv[1]}: CPU mean {sm / max(n, 1) / 10:.1f} %, peak {mx / 10:.1f} %, parts up to {parts}"
print(line)
print(f"::notice title=fm1-emu load (strict, 240 MHz)::{line}")
PY
}
scene idle ""
scene one_key "500:C4,2900:C4^"
scene chord4 "400:PLAY,500:C4,520:E4,540:G4,560:C5"
scene chord8 "$CHORD8"
REPEAT3="400:PLAY,500:C4,520:E4,540:G4,700:C4^,720:E4^,740:G4^,800:C4,820:E4,840:G4,1000:C4^,1020:E4^,1040:G4^,1100:C4,1120:E4,1140:G4,1300:C4^,1320:E4^,1340:G4^,1400:C4,1420:E4,1440:G4,1600:C4^,1620:E4^,1640:G4^,1700:C4,1720:E4,1740:G4,1900:C4^,1920:E4^,1940:G4^,2000:C4,2020:E4,2040:G4"
scene repeat3 "$REPEAT3"
scene chord16 "$CHORD16"
FM1_STRICT=1 FM1_BENCH_KEYS="$REPEAT3" FM1_TPROF=build/emu_prof.txt,1380,1480 "$LIVE" --fwsc build/rdpno.fwsc --mhz 240 \
    --shot 2100 > build/emu_prof.log 2>&1 || true
[ -f build/emu_prof.txt ] && python3 tools/emu_prof.py build/emu_prof.txt build/rdpno.syms build/rdpno.dis sa_half || true
