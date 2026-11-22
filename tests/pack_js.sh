#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The browser's ROM pack and frames (site/romload.js) = tools/pack_roms.py's and tools/fm1_romload.py's, byte for byte:
# made from random "ROMs" (no Roland data), every set; then from the real ROMs too, if RDP_ROMS (or ../rdpiano/roms) has them.
set -e
cd "$(dirname "$0")/.."
command -v node >/dev/null || { echo "pack_js: skipped (no node)"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 - "$T/roms" <<'PY'
import random, sys
from pathlib import Path
sys.path.insert(0, "tools")
import pack_roms as p
d = Path(sys.argv[1]); d.mkdir()
r = random.Random(1)
names = {n for s in p.SETS.values() for n in (*s["wave"], s["params"])}
for n in sorted(names):
    (d / n).write_bytes(bytes(r.getrandbits(8) for _ in range(0x20000)))
(d / p.PROG).write_bytes(bytes(r.getrandbits(8) for _ in range(0x2000)))
PY
check() {   # $1 the roms folder
    for s in mks20a mks20b mk80; do
        RDP_NO_ROMS=0 python3 tools/pack_roms.py --set $s --roms "$1" --out "$T/h.h" --pack "$T/$s.py.pack" >/dev/null
        python3 tools/fm1_romload.py frames "$T/$s.py.pack" "$T/$s.py.frames"
        node -e '
const fs = require("fs"), path = require("path"), R = require("./site/romload.js");
const [dir, set, out, fout] = process.argv.slice(1), s = R.SETS[set], files = {};
for (const n of [...s.wave, s.params, "RD200_B.bin"]) files[n] = new Uint8Array(fs.readFileSync(path.join(dir, n)));
const pk = R.makePack(set, files);
fs.writeFileSync(out, pk);
fs.writeFileSync(fout, R.loadFrames(pk).map((f) => Buffer.from(f).toString("hex") + "\n").join(""));' "$1" $s "$T/$s.js.pack" "$T/$s.js.frames"
        cmp "$T/$s.py.pack" "$T/$s.js.pack" && cmp "$T/$s.py.frames" "$T/$s.js.frames" && echo "  $s: pack and frames the same ($(wc -c < "$T/$s.js.pack") B)"
    done
}
echo "random ROMs:"; check "$T/roms"
R="${RDP_ROMS:-../rdpiano/roms}"
if [ -f "$R/RD200_B.bin" ]; then echo "your ROMs ($R):"; check "$R"; fi
