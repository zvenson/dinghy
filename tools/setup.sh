#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# RDPNO: everything to build it yourself, on Linux x86-64 or Windows with WSL (Ubuntu), from this checkout:
#   sh tools/setup.sh            the tools, the ROMs, then the three firmwares
#   sh tools/setup.sh --no-build the tools and the ROMs only
# What it puts where:
#   apt packages         git, curl, xz-utils, python3, python3-pil, build-essential (sudo)
#   ~/.jieli/            the JieLi toolchain (tools/get_toolchain.sh)
#   ~/fw-AC79_AIoT_SDK   JieLi's AC79 SDK (three of its files go into every package)
#   ../rdpiano           giulioz/rdpiano: its roms/ folder has the ROM dumps tools/pack_roms.py reads
# The ROMs are Roland's: they stay on this computer, and so do the firmwares built from them (build/*.fwsc). Do not
# publish either. Results: build/rdpno.fwsc (MKS-20 A, the pianos), build/rdpno-b.fwsc (MKS-20 B: harpsichord,
# clavi, vibraphone, e-pianos), build/rdpno-mk80.fwsc (MK-80); under WSL also copied to Windows' Downloads.
set -e
cd "$(dirname "$0")/.."
HERE="$(pwd)"

echo "== packages"
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -qq
    sudo apt-get install -y -qq git curl xz-utils python3 python3-pil build-essential
else
    echo "(no apt-get: install git, curl, xz, python3 with Pillow and a C compiler yourself)"
fi

echo "== JieLi toolchain"
[ -x "$HOME/.jieli/toolchain/pi32v2/bin/clang" ] || sh tools/get_toolchain.sh

echo "== JieLi AC79 SDK"
if [ ! -f "$HOME/fw-AC79_AIoT_SDK/cpu/wl82/tools/uboot.boot" ]; then
    for i in 1 2 3; do                          # (gitee.com drops a clone now and then)
        git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
            https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git "$HOME/fw-AC79_AIoT_SDK" && break
        rm -rf "$HOME/fw-AC79_AIoT_SDK"
        sleep 20
    done
    [ -f "$HOME/fw-AC79_AIoT_SDK/cpu/wl82/tools/uboot.boot" ] || { echo "setup: the SDK clone failed, try again"; exit 1; }
fi

echo "== the ROM dumps (../rdpiano/roms: stay on this computer)"
[ -d "$HERE/../rdpiano/roms" ] || git clone --depth 1 https://github.com/giulioz/rdpiano "$HERE/../rdpiano"

[ "$1" = "--no-build" ] && { echo "== ready: ./build.sh [--set mks20b|mk80]"; exit 0; }

echo "== build"
./build.sh --set mk80
./build.sh --set mks20b
./build.sh
ls -l build/rdpno*.fwsc

if grep -qi microsoft /proc/version 2>/dev/null; then   # WSL: into Windows' Downloads, for the installers
    WIN="$(wslpath "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')")/Downloads"
    cp build/rdpno.fwsc build/rdpno-b.fwsc build/rdpno-mk80.fwsc "$WIN"/ && echo "== copied to $WIN"
fi
echo "== done: install with M-UPGRADE or the Firmware Hub (README.md)"
