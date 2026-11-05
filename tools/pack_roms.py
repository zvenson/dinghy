#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Descrambling after rdpiano (giulioz/rdpiano, GPL-3.0), librdpiano/src/{mcu,sound_chip}.cpp.
"""Pack the ROMs of one SA piano into firmware/gen/rdp_rom.h, the form the FM-1 reads straight from flash.

  tools/pack_roms.py [--set mk80|mks20a|mks20b] [--roms DIR] [--patches 0,4,1,5]

--patches (or RDP_PATCHES) keeps only those patches of the set, in that order: every parameter window left out
saves 20..28 KB of flash, if a build does not fit (two patches share a window: MK-80 0+4, 1+5, 2+6, 3+7).

DIR (or RDP_ROMS) holds the ROM dumps as rdpiano names them (its roms/ folder). The ROMs are Roland's: they are
not part of this repository, and a build that contains them is yours to keep, not to hand out.

What goes into the header (all const, in flash):
  RDP_WAVE      the three wave ROMs, descrambled, 3 bytes per address (exp sample, signs, delta), 384 KiB
  RDP_WIN_*     the 32 KiB windows of the parameter ROM the set's patches use, descrambled, LZ4 blocks
  RDP_PROG      the CPU-B program (RD200_B, the one rdpiano's MIDI handshake is written for), 8 KiB
  RDP_PATCH[]   name, window, sample rate (20 or 32 kHz), offset in the window, address in the ROM
"""
import argparse
import os
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import lz4blk  # noqa: E402

SETS = {   # rdpiano_juce PluginProcessor.cpp: patchToRomSet, patchToOffset, sampleRates
    "mk80": dict(wave=("MK80_IC5.bin", "MK80_IC6.bin", "MK80_IC7.bin"), params="MK80_IC18.bin", patches=[
        ("CLASSIC", 0x000020, 20), ("SPECIAL", 0x008000, 20), ("BLEND", 0x010000, 20),
        ("CONTEMPORARY", 0x018000, 32), ("A.PIANO 1", 0x002C00, 20), ("A.PIANO 2", 0x00B1F0, 20),
        ("CLAVI", 0x012910, 32), ("VIBRAPHONE", 0x0199F0, 20)]),
    "mks20a": dict(wave=("mks20_15179738.BIN", "mks20_15179737.BIN", "mks20_15179736.BIN"),
                   params="mks20_15179757.BIN", patches=[
        ("PIANO 1", 0x000000, 20), ("PIANO 2", 0x008000, 20), ("PIANO 3", 0x010000, 20)]),
    "mks20b": dict(wave=("mks20_15179741.BIN", "mks20_15179740.BIN", "mks20_15179739.BIN"),
                   params="mks20_15179757.BIN", patches=[
        ("HARPSICHORD", 0x018000, 32), ("CLAVI", 0x003C20, 32), ("VIBRAPHONE", 0x00AB50, 20),
        ("E-PIANO 1", 0x014260, 20), ("E-PIANO 2", 0x01BEF0, 32)]),
}
PROG = "RD200_B.bin"


def bit(v, n):
    return (v >> n) & 1


def bitswap(v, *bits):
    r = 0
    for b in bits:
        r = (r << 1) | bit(v, b)
    return r


def unscramble_addr_wave(i):
    return (bit(i, 16) << 16 | bit(i, 15) << 15 | bit(i, 14) << 14 | bit(i, 1) << 13 | bit(i, 4) << 12 |
            bit(i, 9) << 11 | bit(i, 5) << 10 | bit(i, 10) << 9 | bit(i, 3) << 8 | bit(i, 0) << 7 |
            bit(i, 6) << 6 | bit(i, 11) << 5 | bit(i, 7) << 4 | bit(i, 2) << 3 | bit(i, 12) << 2 |
            bit(i, 8) << 1 | bit(i, 13))


def wave_pack(r5, r6, r7):
    """-> 3 bytes per address: exp[7:0] | exp[13:8], exp sign << 6, delta sign << 7 | delta[8:1]
    (delta bit 0 is exp bit 0: both are ~ic6 bit 7)"""
    ic = [bytearray(0x20000) for _ in range(3)]
    for k, r in enumerate((r5, r6, r7)):
        for s in range(0x20000):
            ic[k][s] = r[unscramble_addr_wave(s)]          # (the data unscramble is the identity)
    ic5, ic6, ic7 = ic
    out = bytearray(0x20000 * 3)
    flip = 0b1100101010                                   # address bits 1, 3, 5, 8, 9 inverted
    for i in range(0x20000):
        d = i ^ flip
        a, b, c = ic5[d], ic6[d], ic7[d]
        e = (bit(a, 0) << 13 | bit(b, 4) << 12 | bit(c, 4) << 11 | (bit(b, 0) ^ 1) << 10 | bit(c, 7) << 9 |
             bit(a, 7) << 8 | (bit(a, 5) ^ 1) << 7 | bit(b, 2) << 6 | bit(c, 2) << 5 | bit(c, 1) << 4 |
             (bit(a, 1) ^ 1) << 3 | bit(a, 3) << 2 | bit(b, 5) << 1 | (bit(b, 7) ^ 1))
        es = bit(c, 3) ^ 1
        dl = ((bit(c, 6) ^ 1) << 8 | bit(a, 4) << 7 | bit(c, 0) << 6 | (bit(b, 3) ^ 1) << 5 | bit(a, 2) << 4 |
              (bit(a, 6) ^ 1) << 3 | bit(b, 6) << 2 | bit(c, 5) << 1 | (bit(b, 7) ^ 1))
        ds = bit(b, 1)
        assert (dl & 1) == (e & 1)
        out[3 * i] = e & 0xFF
        out[3 * i + 1] = e >> 8 | es << 6 | ds << 7
        out[3 * i + 2] = dl >> 1
    return out


def params_unscramble(r):
    return bytes(bitswap(r[bitswap(s, 16, 15, 13, 12, 14, 11, 8, 9, 10, 7, 6, 5, 4, 3, 2, 1, 0)], 7, 0, 6, 1, 5, 2, 4, 3)
                 for s in range(0x20000))


def prog_unscramble(r):
    return bytes(bitswap(r[bitswap(s, 13, 12, 11, 8, 9, 10, 7, 6, 5, 4, 3, 2, 1, 0)], 7, 0, 6, 1, 5, 2, 4, 3)
                 for s in range(0x2000))


def carr(name, data, per_line=24):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        lines.append(",".join(str(x) for x in data[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default=os.environ.get("RDP_SET", "mk80"), choices=SETS)
    ap.add_argument("--roms", default=os.environ.get("RDP_ROMS", str(SRC.parent / "rdpiano" / "roms")))
    ap.add_argument("--out", default=str(SRC / "firmware" / "gen" / "rdp_rom.h"))
    ap.add_argument("--patches", default=os.environ.get("RDP_PATCHES", ""), help="e.g. 0,4,1,5 (default: all)")
    ap.add_argument("--raw", help="also write the unpacked wave/params/prog here (tests/rdp_compare.sh)")
    a = ap.parse_args()
    s, roms = dict(SETS[a.set]), Path(a.roms)
    if a.patches:
        try:
            s["patches"] = [s["patches"][int(i)] for i in a.patches.split(",")]
        except (ValueError, IndexError):
            raise SystemExit(f"pack_roms: --patches {a.patches}: 0..{len(s['patches']) - 1}, comma separated")

    def rd(n, size):
        p = roms / n
        if not p.exists():
            raise SystemExit(f"pack_roms: {p} is missing (give --roms or RDP_ROMS: rdpiano's roms/ folder)")
        d = p.read_bytes()
        if len(d) != size:
            raise SystemExit(f"pack_roms: {p} is {len(d)} B, not {size}")
        return d

    wave = wave_pack(*(rd(n, 0x20000) for n in s["wave"]))
    params = params_unscramble(rd(s["params"], 0x20000))
    prog = prog_unscramble(rd(PROG, 0x2000))
    wins = sorted({off >> 15 for _, off, _ in s["patches"]})
    parts = [f"/* generated by tools/pack_roms.py --set {a.set}: do not edit, do not commit (Roland's ROMs) */",
             "#pragma once", "#include <stdint.h>", f'#define RDP_SET_NAME "{a.set.upper()}"',
             f"#define RDP_NPATCH {len(s['patches'])}u", f"#define RDP_NWIN {len(wins)}u",
             *(f'#define RDP_REF_{k} "{n}"' for k, n in zip(("IC5", "IC6", "IC7", "PARAMS"), (*s["wave"], s["params"]))),
             carr("RDP_WAVE", wave), carr("RDP_PROG", prog)]
    total = len(wave) + len(prog)
    for k, w in enumerate(wins):
        raw = params[w << 15:(w + 1) << 15]
        c = lz4blk.compress(raw)
        if lz4blk.decompress(c) != raw:
            raise SystemExit("pack_roms: LZ4 round trip failed")
        parts.append(carr(f"RDP_WIN{k}", c))
        total += len(c)
        print(f"pack_roms: parameter window {w}: {len(c)} B")
    parts.append("static const uint8_t *const RDP_WIN[RDP_NWIN] = {" + ",".join(f"RDP_WIN{k}" for k in range(len(wins))) + "};")
    parts.append("static const uint32_t RDP_WIN_LEN[RDP_NWIN] = {" +
                 ",".join(f"sizeof RDP_WIN{k}" for k in range(len(wins))) + "};")
    parts.append("static const struct { const char *name; uint8_t win; uint8_t khz; uint16_t off; uint32_t addr; } RDP_PATCH[RDP_NPATCH] = {")
    for n, off, khz in s["patches"]:
        parts.append(f'    {{"{n}", {wins.index(off >> 15)}, {khz}, {off & 0x7FFF:#06x}, {off:#07x}}},')
    parts.append("};")
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(parts) + "\n")
    print(f"pack_roms: {a.set}, {len(s['patches'])} patches, {len(wins)} parameter windows, {total} B of flash -> {out}")
    if a.raw:
        r = Path(a.raw)
        r.mkdir(parents=True, exist_ok=True)
        (r / "wave.bin").write_bytes(wave)
        (r / "params.bin").write_bytes(params)
        (r / "prog.bin").write_bytes(prog)


if __name__ == "__main__":
    main()
