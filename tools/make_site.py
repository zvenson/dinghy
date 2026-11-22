#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RDPNO's site (GitHub Pages) from site/ and a ROM-free release package (tools/build.py --romload --release X.Y):

  index.html          the front page (site/index.html, {{VERSION}} and {{PKG}} filled in)
  install/index.html  the web installer: fm1-base's (site/install/installer.html, MPL-2.0) with site/lib/fm1ports.js,
                      fm1pkg.js, fm1ota.js, site/romload.js and the package's metadata inlined, and RDPNO's own step
                      (site/install/install_app.js: the ROMs loaded again after the firmware)
  load/, screens/     the ROM load page, the three screens
  firmware/           the package, with the licences of what is in it (GPL-3.0; the JieLi SDK's files: Apache-2.0)

  tools/make_site.py build/rdpno-X.Y.fwsc X.Y OUT_DIR

The package must be a --romload release build: no Roland data in it (checked: it holds no ROM pack)."""
import json
import re
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
SITE = SRC / "site"
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    """an ES module's text as plain script: no export, no import (to inline it in a page)"""
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def fill(text, values, what):
    for k, v in values.items():
        text = text.replace("{{" + k + "}}", v)
    left = sorted(set(re.findall(r"\{\{[A-Z_]+\}\}", text)))
    if left:
        raise SystemExit(f"{what}: {' '.join(left)} not filled in")
    return text


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_95\d", product):
        raise SystemExit(f"{pkg}: identity {product!r}: make a release build (tools/build.py --romload --release X.Y)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no update loader in it")
    if b"RDPNOPK1" in raw:
        raise SystemExit(f"{pkg}: it holds a ROM pack: only a --romload build goes on the site")
    name = f"rdpno-{version}.fwsc"
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SITE, out, ignore=shutil.ignore_patterns("install", "lib"))
    (out / "install").mkdir()
    (out / "firmware").mkdir()
    shutil.copy(pkg, out / "firmware" / name)
    shutil.copy(SRC / "LICENSE", out / "firmware" / "LICENSE-GPL-3.0.txt")
    shutil.copy(SRC / "LICENSES" / "Apache-2.0.txt", out / "firmware" / "Apache-2.0.txt")   # (the JieLi SDK's files)
    (out / "index.html").write_text(fill((SITE / "index.html").read_text(encoding="utf-8"),
                                         {"VERSION": version, "PKG": name}, "site/index.html"), encoding="utf-8")
    html = (SITE / "install" / "installer.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((SITE / "lib" / f).read_text(encoding="utf-8"))
                    for f in ("fm1ports.js", "fm1pkg.js", "fm1ota.js")) + "\n" + (SITE / "romload.js").read_text(encoding="utf-8")
    meta = json.dumps({"version": version, "product": product, "pkg": "../firmware/" + name, "backup": False,
                       "res": None, "boot": None})
    for mark in ("/*LIB*/", "/*META*/", "/*APP*/"):
        if html.count(mark) != 1:
            raise SystemExit(f"site/install/installer.html must contain {mark} once")
    repo = "https://github.com/R-Jeb/rdpno-fm1"
    html = fill(html, {"RES": "the data", "RES_JA": "データ", "NAME": "RDPNO", "REPO": repo,
                       "REPO_SHORT": re.sub(r"^https?://", "", repo), "LICENSE": "GPL-3.0", "CREDITS": ""},
                "site/install/installer.html")
    app = (SITE / "install" / "install_app.js").read_text(encoding="utf-8")
    (out / "install" / "index.html").write_text(html.replace("/*LIB*/", lib).replace("/*META*/", meta)
                                                .replace("/*APP*/", app), encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"site: {out}: index.html, install/ ({product}), load/, screens/, firmware/{name} ({len(raw)} B)")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
