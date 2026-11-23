#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RDPNO's ROM loader protocol (firmware/src/romload.c) in Python: the frames the load page (docs/load/) sends, for
the tests and for loading from a terminal.

  tools/fm1_romload.py load PACK            send a ROM pack (tools/pack_roms.py --pack) to an FM-1 running RDPNO
                                            (python3 -m pip install mido python-rtmidi)
  tools/fm1_romload.py load --set mks20a    the same, the pack made here from RDP_ROMS (rdpiano's roms/ folder)
  tools/fm1_romload.py hello                what the FM-1 holds now
  tools/fm1_romload.py frames PACK OUT      the frames of a load, one hex line each (tests/romload_test.c reads them)
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
HEAD = [0x7D, 0x52, 0x44]
CHUNK = 448                                     # bytes of the pack in one data frame (romload.c RL_CHUNK)
STATUS = {1: "a frame out of place", 2: "the FM-1's flash refused a write", 3: "the CRC after loading differs",
          4: "no load running on the FM-1", 5: "a frame sent again differs from the flash", 6: "not a ROM pack",
          7: "frames kept arriving damaged"}
SETS = {0: "MKS-20 A", 1: "MKS-20 B", 2: "MK-80"}


def u7(v, n):
    return [(v >> (7 * i)) & 0x7F for i in range(n)]


def n7(b):
    return sum((x & 0x7F) << (7 * i) for i, x in enumerate(b))


def pack7(data):
    """each 7 bytes as 8, the first carrying their high bits (bit k for byte k)"""
    out = []
    for i in range(0, len(data), 7):
        c = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(c)))
        out += [b & 0x7F for b in c]
    return out


def frame(cmd, body=()):
    b = [cmd, *body]
    if 0x10 <= cmd <= 0x12:                     # the check: the sum from cmd on, 2 x 7 bits
        s = sum(b)
        b += [s & 0x7F, (s >> 7) & 0x7F]
    return bytes([0xF0, *HEAD, *b, 0xF7])


def load_frames(pack):
    fr = [frame(0x10, u7(len(pack), 4))]
    for off in range(0, len(pack), CHUNK):
        fr.append(frame(0x11, u7(off, 4) + pack7(pack[off:off + CHUNK])))
    fr.append(frame(0x12, u7(zlib.crc32(pack) & 0xFFFFFFFF, 5)))
    return fr


def parse_hello(m):
    """the answer to 01: {"pack": bool, "set": name, "patches": n, "area": bytes, "crc": crc, "name": str}"""
    if len(m) < 39 or list(m[:6]) != [0xF0, *HEAD, 0x7F, 0x01]:
        return None
    return {"protocol": m[9], "pack": bool(m[10]), "set": SETS.get(m[11], "?") if m[10] else None, "patches": m[12],
            "area": n7(m[13:17]), "crc": n7(m[17:22]), "name": bytes(m[22:38]).split(b"\0")[0].decode("ascii", "replace")}


class Link:
    """the FM-1 over mido: the port pair that answers the hello"""

    def __init__(self):
        import mido
        self.mido = mido
        self.out = self.inp = None
        for name in mido.get_input_names():
            outs = [o for o in mido.get_output_names() if o.split(":")[0] == name.split(":")[0]] or \
                   [o for o in mido.get_output_names() if o == name]
            if not outs:
                continue
            try:
                i, o = mido.open_input(name), mido.open_output(outs[0])
            except OSError:
                continue
            self.inp, self.out = i, o
            if self.hello() is not None:
                return
            i.close()
            o.close()
        raise SystemExit("no FM-1 running RDPNO answered (connected by USB, not in use by another program?)")

    def send(self, f):
        self.out.send(self.mido.Message("sysex", data=list(f[1:-1])))

    def answer(self, cmd, ms):
        end = time.time() + ms / 1000
        while time.time() < end:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and list(m.data[:5]) == [*HEAD, 0x7F, cmd]:
                    return bytes([0xF0, *m.data, 0xF7])
            time.sleep(0.0005)
        return None

    def hello(self):
        self.send(frame(0x01))
        a = self.answer(0x01, 800)
        return parse_hello(a) if a else None

    def checked(self, f, ms):
        cmd = f[4]
        for _ in range(6):                      # a frame lost or damaged on the way goes again
            self.send(f)
            a = self.answer(cmd, ms)
            if a is None or a[6] == 7 or (len(f) > 7 and (a[7], a[8]) != (f[-3], f[-2])):
                continue
            if a[6]:
                raise SystemExit(f"stopped: {STATUS.get(a[6], a[6])} ({n7(a[9:13])}, {n7(a[13:17])}, {n7(a[17:21])})")
            return a
        raise SystemExit("stopped: no answer from the FM-1")


def make_pack(set_name):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "pack.bin"
        subprocess.run([sys.executable, str(SRC / "tools" / "pack_roms.py"), "--set", set_name, "--out",
                        str(Path(d) / "rdp_rom.h"), "--pack", str(p)], check=True, stdout=subprocess.DEVNULL)
        return p.read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    lo = sub.add_parser("load")
    lo.add_argument("pack", nargs="?")
    lo.add_argument("--set", choices=("mks20a", "mks20b", "mk80"))
    sub.add_parser("hello")
    fr = sub.add_parser("frames")
    fr.add_argument("pack")
    fr.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "frames":
        Path(a.out).write_text("".join(f.hex() + "\n" for f in load_frames(Path(a.pack).read_bytes())))
        return
    link = Link()
    if a.cmd == "hello":
        print(link.hello())
        return
    if not a.pack and not a.set:
        raise SystemExit("load: a pack file or --set")
    pack = Path(a.pack).read_bytes() if a.pack else make_pack(a.set)
    if pack[:8] != b"RDPNOPK1":
        raise SystemExit("that is not a ROM pack (tools/pack_roms.py --pack)")
    frames = load_frames(pack)
    t0 = time.time()
    link.checked(frames[0], 8000)
    for k, f in enumerate(frames[1:-1]):
        link.checked(f, 4000)
        if k % 50 == 0:
            print(f"\r{100 * k // (len(frames) - 2)} %", end="", flush=True)
    link.checked(frames[-1], 15000)
    print(f"\rloaded, {len(pack)} B in {time.time() - t0:.0f} s; the FM-1 restarts with it")


if __name__ == "__main__":
    main()
