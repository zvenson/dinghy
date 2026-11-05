#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# How much of the FM-1's audio interrupt the sound takes, measured on Joakim Eriksson's FM-1 emulator
# (github.com/joakimeriksson/fm1-emu) in its strict mode: every instruction costs its cycles as measured on an
# FM-1 (and the flash cache's misses), at the real 240 MHz. Plays a few scenes and prints the highest load
# (rdpno.c's rdp_load_max: the render's share of a 5.8 ms block) and the most parts sounding.
#   tools/emu_load.sh [FM1EMU_DIR]          after ./build.sh; FM1EMU_DIR: a checkout (default ../fm1-emu)
# In CI each line also becomes a notice (::notice), readable in the check run.
set -e
cd "$(dirname "$0")/.."
E="${1:-../fm1-emu}"
[ -d "$E/crates/fm1-live" ] || git clone -q --depth 1 https://github.com/joakimeriksson/fm1-emu "$E"
(cd "$E" && cargo build -q --release -p fm1-live)
LIVE="$E/target/release/fm1-live"
OBJDUMP="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}/common/bin/objdump"
addr() { "$OBJDUMP" -t build/rdpno.elf | awk -v s="$1" '$NF == s { print $1 }'; }
A_LOAD=$(addr rdp_load_max); A_PARTS=$(addr rdp_parts_max)
[ -n "$A_LOAD" ] && [ -n "$A_PARTS" ] || { echo "emu_load: no rdp_load_max in build/rdpno.elf"; exit 1; }
MS="${EMU_MS:-3000}"
scene() {   # name, FM1_BENCH_KEYS
    FM1_STRICT=1 FM1_DUMPRAM=build/emu_ram.bin FM1_BENCH_KEYS="$2" "$LIVE" --fwsc build/rdpno.fwsc --mhz 240 \
        --bench "$MS" > build/emu_"$1".log 2>&1 || { echo "emu_load: $1 failed"; tail -5 build/emu_"$1".log; return 1; }
    python3 - "$1" "$A_LOAD" "$A_PARTS" <<'PY'
import struct, sys
ram = open("build/emu_ram.bin", "rb").read()
def word(a): o = int(a, 16) - 0x01C00000; return struct.unpack_from("<I", ram, o)[0]
load, parts = word(sys.argv[2]), word(sys.argv[3])
line = f"{sys.argv[1]}: CPU peak {load / 10:.1f} %, parts up to {parts} (strict, 240 MHz)"
print(line)
print(f"::notice title=fm1-emu load::{line}")
PY
}
scene idle ""
scene one_key "500-2900:n7"
scene chord4 "400:PLAY,500-2900:n7,550-2900:n11,600-2900:n14,650-2900:n19"
scene chord8 "400:PLAY,500:m903c64,520:m904064,540:m904364,560:m904864,580:m904c64,600:m905064,620:m905364,640:m905864"
