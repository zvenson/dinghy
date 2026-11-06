#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# How much of the FM-1's audio interrupt the sound takes, measured on Joakim Eriksson's FM-1 emulator
# (github.com/joakimeriksson/fm1-emu) in its strict mode: every instruction costs its cycles as measured on an
# FM-1 (and the flash cache's misses), at the real 240 MHz. Plays a few scenes and prints, from 1.5 s on, the
# highest and the mean load (rdpno.c: the render's share of a 5.8 ms block) and the most parts sounding; then
# where the cycles go as six keys go down together (tools/emu_prof.py).
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
MS="${EMU_MS:-4500}"   # (the keys from 1.9 s on: the load counts from 1.5 s, after the boot)
CHORD8="1900:PLAY,2000:m903c64,2020:m904064,2040:m904364,2060:m904864,2080:m904c64,2100:m905064,2120:m905364,2140:m905864"
CHORD16="$CHORD8,2200:m902b64,2220:m902f64,2240:m903264,2260:m903764,2280:m903b64,2300:m903e64,2320:m904564,2340:m904a64"
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
scene one_key "2000:C4,4400:C4^"
scene chord4 "1900:PLAY,2000:C4,2020:E4,2040:G4,2060:C5"
scene keys5 "2000:C3,2000:G3,2000:C4,2000:E4,2000:G4,4000:C3^,4000:G3^,4000:C4^,4000:E4^,4000:G4^"
scene keys6 "2000:C3,2000:G3,2000:C4,2000:E4,2000:G4,2000:C5,4000:C3^,4000:G3^,4000:C4^,4000:E4^,4000:G4^,4000:C5^"
scene chord8 "$CHORD8"
REPEAT3="1900:PLAY,2000:C4,2020:E4,2040:G4,2200:C4^,2220:E4^,2240:G4^,2300:C4,2320:E4,2340:G4,2500:C4^,2520:E4^,2540:G4^,2600:C4,2620:E4,2640:G4,2800:C4^,2820:E4^,2840:G4^,2900:C4,2920:E4,2940:G4,3100:C4^,3120:E4^,3140:G4^,3200:C4,3220:E4,3240:G4,3400:C4^,3420:E4^,3440:G4^,3500:C4,3520:E4,3540:G4"
scene repeat3 "$REPEAT3"
scene chord16 "$CHORD16"
FM1_STRICT=1 FM1_BENCH_KEYS="2000:C3,2000:G3,2000:C4,2000:E4,2000:G4,2000:C5" FM1_TPROF=build/emu_prof.txt,2000,2060 "$LIVE" --fwsc build/rdpno.fwsc --mhz 240 \
    --shot 2200 > build/emu_prof.log 2>&1 || true
[ -f build/emu_prof.txt ] && python3 tools/emu_prof.py build/emu_prof.txt build/rdpno.syms build/rdpno.dis sa_half || true
