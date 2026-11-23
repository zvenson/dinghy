#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The site in a browser (tools/make_site.py's output; python3 -m pip install playwright, playwright install chromium):
the front page and the screens load without errors; the load page finds a fake FM-1 (tests/fake_midi.js), takes the
ROM files, loads MKS-20 B (frames lost on the way sent again) and the fake FM-1 holds it; the installer takes the
package, asks for the ROMs in its own step (the pack kept from the load page offered first), makes the MK-80's pack and
keeps it, then finds no FM-1 to update (the fake one has no updater).

  tests/site_test.py SITE_DIR [ROMS_DIR]     (without ROMS_DIR: random "ROMs" under rdpiano's names)"""
import functools
import http.server
import random
import sys
import tempfile
import threading
from pathlib import Path

from playwright.sync_api import sync_playwright

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import pack_roms  # noqa: E402


def fake_roms(d):
    """random wave ROMs, parameter ROMs that pack small (as the real ones do: the pack must fit the FM-1's area)"""
    r = random.Random(2)
    for s in pack_roms.SETS.values():
        for n in s["wave"]:
            (d / n).write_bytes(r.randbytes(0x20000))
        (d / s["params"]).write_bytes(bytes(i // 64 & 0xFF for i in range(0x20000)))
    (d / pack_roms.PROG).write_bytes(r.randbytes(0x2000))


def main(site, roms=None):
    tmp = tempfile.TemporaryDirectory()
    if not roms:
        roms = Path(tmp.name)
        fake_roms(roms)
    files = sorted(str(p) for p in Path(roms).iterdir())
    h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=site)
    h.log_message = lambda *a: None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
    url = f"http://127.0.0.1:{srv.server_address[1]}"
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    bad = []
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("console", lambda m: m.type == "error" and errs.append(m.text))
        pg.add_init_script(path=str(HERE / "fake_midi.js"))
        for path in ("/", "/screens/"):
            pg.goto(url + path)
        pg.goto(url + "/load/")
        pg.click("#connect")
        pg.wait_for_function("document.getElementById('cstat').textContent.startsWith('Connected')")
        pg.set_input_files("#files", files)
        pg.wait_for_function("document.getElementById('fstat').textContent.includes('All files')")
        pg.select_option("#set", "mks20b")
        pg.click("#load")
        pg.wait_for_function("/^(Done|Stopped|Loaded\\.)/.test(document.getElementById('lstat').textContent)", timeout=600000)
        print("load page:", pg.text_content("#lstat"))
        held = pg.evaluate("fakeFM1.pack && fakeFM1.pack.name")
        if held != "MKS-20 B":
            bad.append(f"the fake FM-1 holds {held!r}")
        pg.goto(url + "/install/")
        pg.wait_for_function("!document.getElementById('go').disabled || document.getElementById('alert').textContent")
        print("installer:", pg.text_content("#status"), pg.text_content("#alert"))
        if pg.is_disabled("#go"):
            bad.append("the installer did not take the package")
        else:
            pg.click("#go")
            pg.click("#confirmgo")
            pg.wait_for_selector("#appask[open]")
            if "MKS-20 B" not in pg.text_content("#appaskt"):
                bad.append("the kept pack was not offered")
            pg.click("#appaskrow button[value=other]")
            pg.wait_for_selector("#rlfiles")
            pg.set_input_files("#rlfiles", files)
            pg.wait_for_function("document.getElementById('rlnote').textContent.includes('All files')")
            pg.select_option("#rlset", "mk80")
            pg.click("#appaskrow button[value=go]")
            pg.wait_for_function("document.getElementById('alert').textContent", timeout=120000)
            print("install:", pg.text_content("#alert")[:90])
            kept = pg.evaluate("""new Promise(r => { const q = indexedDB.open('rdpno-setup', 1); q.onsuccess = () => {
                const g = q.result.transaction('files').objectStore('files').get('pack'); g.onsuccess = () => r(g.result.set); }; })""")
            if kept != "MK-80":
                bad.append(f"kept {kept!r}")
        if errs:
            bad.append(f"errors: {errs}")
        b.close()
    srv.shutdown()
    print("site:", "FAIL " + "; ".join(bad) if bad else "OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
