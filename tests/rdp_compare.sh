#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The C port (firmware/src/rdp.c) against rdpiano's own librdpiano, sample for sample (tests/rdp_script.h: notes,
# the pedal, patch changes, 18 notes on 16 voices, a 32 kHz patch).
#   tests/rdp_compare.sh [RDPIANO]     RDPIANO: a checkout of github.com/giulioz/rdpiano (default ../rdpiano)
# rdp_exact 0 (librdpiano's timing) must give the very same samples; rdp_exact 1 (the real 2 MHz, the firmware's)
# is printed next to it. Uses firmware/gen/rdp_rom.h (tools/pack_roms.py makes it from RDPIANO/roms if missing).
set -e
cd "$(dirname "$0")/.."
R="${1:-${RDPIANO:-../rdpiano}}"
[ -d "$R/librdpiano/src" ] || { echo "rdp_compare: no rdpiano checkout at $R (git clone https://github.com/giulioz/rdpiano)"; exit 1; }
[ -f firmware/gen/rdp_rom.h ] || python3 tools/pack_roms.py --roms "$R/roms"
mkdir -p build/host
c++ -O2 -std=c++17 -w -I"$R/librdpiano/include" -Ifirmware/gen -Itests -o build/host/rdp_ref tests/rdp_ref.cpp "$R"/librdpiano/src/*.cpp
cc -O2 -std=c99 -Wall -Wno-unused-function -Ifirmware/src -Ifirmware/gen -Itests -o build/host/rdp_test tests/rdp_test.c
build/host/rdp_ref "$R/roms" build/host/rdp_ref.raw 2>/dev/null
build/host/rdp_test build/host/rdp_port0.raw 0
if cmp -s build/host/rdp_ref.raw build/host/rdp_port0.raw; then
    echo "rdp_compare: rdp_exact 0 = librdpiano, sample for sample ($(($(wc -c < build/host/rdp_ref.raw) / 4)) samples)"
else
    echo "rdp_compare: FAILED: the port differs from librdpiano"; exit 1
fi
build/host/rdp_test build/host/rdp_port1.raw 1
