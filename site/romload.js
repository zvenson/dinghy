// SPDX-License-Identifier: GPL-3.0-only
// RDPNO's ROM pack and loader in the browser: the load page (site/load/) and the installer's own step (site/install/).
// The pack is made here from the player's own ROM files, exactly as tools/pack_roms.py makes it (tests/pack_js.sh
// compares the bytes), and sent to the FM-1 as tools/fm1_romload.py does (firmware/src/romload.c: the protocol).
// Nothing leaves the computer except to the FM-1. Descrambling after rdpiano (giulioz/rdpiano, GPL-3.0).
"use strict";
const RomLoad = (() => {
  const SETS = {
    mk80: {label: "MK-80", id: 2, wave: ["MK80_IC5.bin", "MK80_IC6.bin", "MK80_IC7.bin"], params: "MK80_IC18.bin",
      patches: [["CLASSIC", 0x000020, 20, 1], ["SPECIAL", 0x008000, 20, 2], ["BLEND", 0x010000, 20, 3], ["CONTEMPORARY", 0x018000, 32, 4], ["A.PIANO 1", 0x002C00, 20, 5], ["A.PIANO 2", 0x00B1F0, 20, 6], ["CLAVI", 0x012910, 32, 7], ["VIBRAPHONE", 0x0199F0, 20, 8]],
      order: [0, 1, 8, 9, 12, 13, 104, 14, 15, 20, 21, 5, 4, 6, 7, 18, 19, 28, 2, 29, 3, 22, 23, 26, 27, 80, 81, 82, 83, 124, 32, 10, 11, 30, 33, 56, 127, 126, 31, 57, 96, 97, 53, 52, 17, 16, 24, 42, 46, 43, 25, 105, 47, 55, 54, 34, 51, 50, 125, 35, 70, 58, 116, 108, 71, 72, 36, 109, 94, 40, 60, 120, 49, 48, 73, 100, 41, 59, 101, 37, 61, 121, 62, 68, 117, 63, 118, 69, 66, 95, 64, 79, 78, 119, 99, 65, 102, 98, 67, 44, 74, 45, 122, 75, 103, 123, 76, 77, 115, 114, 110, 84, 85, 93, 92, 87, 86, 112, 89, 88, 113, 91, 90, 111, 38, 39, 106, 107]},
    mks20a: {label: "MKS-20 A", id: 0, wave: ["mks20_15179738.BIN", "mks20_15179737.BIN", "mks20_15179736.BIN"], params: "mks20_15179757.BIN",
      patches: [["PIANO 1", 0x000000, 20, 1], ["PIANO 2", 0x008000, 20, 2], ["PIANO 3", 0x010000, 20, 3]],
      order: [0, 1, 5, 4, 15, 14, 13, 12, 9, 8, 60, 19, 18, 3, 2, 61, 32, 7, 6, 33, 57, 56, 34, 36, 22, 23, 35, 37, 17, 16, 92, 50, 82, 86, 93, 83, 87, 51, 59, 88, 21, 20, 58, 90, 91, 89, 48, 101, 100, 49, 38, 39, 46, 47, 10, 11, 44, 40, 42, 43, 45, 55, 54, 41, 74, 62, 75, 76, 84, 85, 77, 64, 63, 96, 97, 65, 26, 27, 94, 95, 72, 66, 67, 73, 103, 68, 70, 102, 69, 71, 78, 79, 99, 98, 80, 81, 30, 31, 24, 25, 52, 53, 28, 29, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127]},
    mks20b: {label: "MKS-20 B", id: 1, wave: ["mks20_15179741.BIN", "mks20_15179740.BIN", "mks20_15179739.BIN"], params: "mks20_15179757.BIN",
      patches: [["HARPSICHORD", 0x018000, 32, 4], ["CLAVI", 0x003C20, 32, 5], ["VIBRAPHONE", 0x00AB50, 20, 6], ["E-PIANO 1", 0x014260, 20, 7], ["E-PIANO 2", 0x01BEF0, 32, 8]],
      order: [0, 1, 8, 9, 20, 21, 46, 15, 14, 18, 19, 3, 2, 66, 12, 13, 67, 17, 16, 6, 7, 54, 55, 11, 5, 10, 4, 126, 127, 23, 22, 50, 76, 112, 51, 58, 113, 59, 27, 64, 26, 65, 52, 24, 60, 25, 61, 53, 32, 33, 56, 57, 48, 49, 62, 63, 77, 34, 35, 68, 70, 72, 69, 73, 71, 74, 75, 47, 36, 37, 44, 45, 39, 38, 41, 40, 43, 42, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 30, 31, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 28, 29]},
  };
  const PROG = "RD200_B.bin";
  /* the right dumps, by their SHA-256 (any file name), for the file picker; rdpiano's names also work */
  const KNOWN = {
    "30dde93ab8a237cc920b5f8963f9fc741df8fb5235f129f893e987f01e6c617a": "MK80_IC5.bin",
    "f7198cc2ebef5f9967418f4e385f6f2257affd10d593a8aadc2c607df78487dc": "MK80_IC6.bin",
    "1d44d63945fc58dcc8c01618c2d0f320822e52d33c55c0ba1dda01a5924bdf21": "MK80_IC7.bin",
    "91e997f2befa15b29ee21ae456cbacbd879463f4df881c4c680e2bdf06ab1f61": "MK80_IC18.bin",
    "9fca7296355940dca0ed1846e66a74ff448b35649fa34565f1d7f3d2e686dcde": "RD200_B.bin",
    "ab9a873b98434e076d224baf880804c5b8e8760b36c23f4e38e420030fb6362f": "mks20_15179736.BIN",
    "65c6ec1d48d1b0890e4c5186d55264b9a4c58dc08e9a5b9126fa0d0bea4c6988": "mks20_15179737.BIN",
    "81a4a9f0bfded527ec30bd5e28f2692d7589e2cc6b54d92e4006d9985587d0fd": "mks20_15179738.BIN",
    "ff4657eef24f9b197f40ee2733141e1b7949726a0f964bc4089b0bfd8d818b6a": "mks20_15179739.BIN",
    "b36daaf9946dc0235a9920b805b37b7c771f1fb80ab99395194bae6064be5f6b": "mks20_15179740.BIN",
    "6b37fec4142f8f2e03869a2fe743cdb7063c259a8f5fd1acbd286ffd828a06f6": "mks20_15179741.BIN",
    "6c4b4f34a931b300d29716db67ac25b1905ccb6ce09ad2d84b433891f125586f": "mks20_15179757.BIN",
  };
  const SIZE = {"RD200_B.bin": 0x2000};         // the others: 128 KiB

  /* ---------------------------------------------------------------- the pack (tools/pack_roms.py) */
  const bit = (v, n) => (v >> n) & 1;
  function bitswap(v, bits) { let r = 0; for (const b of bits) r = (r << 1) | bit(v, b); return r; }
  function unscrambleAddrWave(i) {
    return (bit(i, 16) << 16 | bit(i, 15) << 15 | bit(i, 14) << 14 | bit(i, 1) << 13 | bit(i, 4) << 12 |
            bit(i, 9) << 11 | bit(i, 5) << 10 | bit(i, 10) << 9 | bit(i, 3) << 8 | bit(i, 0) << 7 |
            bit(i, 6) << 6 | bit(i, 11) << 5 | bit(i, 7) << 4 | bit(i, 2) << 3 | bit(i, 12) << 2 |
            bit(i, 8) << 1 | bit(i, 13));
  }
  function wavePack(r5, r6, r7) {               // 3 bytes per address (pack_roms.py wave_pack)
    const ic = [r5, r6, r7].map((r) => { const o = new Uint8Array(0x20000); for (let s = 0; s < 0x20000; s++) o[s] = r[unscrambleAddrWave(s)]; return o; });
    const [ic5, ic6, ic7] = ic, out = new Uint8Array(0x20000 * 3), flip = 0b1100101010;
    for (let i = 0; i < 0x20000; i++) {
      const d = i ^ flip, a = ic5[d], b = ic6[d], c = ic7[d];
      const e = (bit(a, 0) << 13 | bit(b, 4) << 12 | bit(c, 4) << 11 | (bit(b, 0) ^ 1) << 10 | bit(c, 7) << 9 |
                 bit(a, 7) << 8 | (bit(a, 5) ^ 1) << 7 | bit(b, 2) << 6 | bit(c, 2) << 5 | bit(c, 1) << 4 |
                 (bit(a, 1) ^ 1) << 3 | bit(a, 3) << 2 | bit(b, 5) << 1 | (bit(b, 7) ^ 1));
      const es = bit(c, 3) ^ 1;
      const dl = ((bit(c, 6) ^ 1) << 8 | bit(a, 4) << 7 | bit(c, 0) << 6 | (bit(b, 3) ^ 1) << 5 | bit(a, 2) << 4 |
                  (bit(a, 6) ^ 1) << 3 | bit(b, 6) << 2 | bit(c, 5) << 1 | (bit(b, 7) ^ 1));
      const ds = bit(b, 1);
      out[3 * i] = e & 0xFF;
      out[3 * i + 1] = e >> 8 | es << 6 | ds << 7;
      out[3 * i + 2] = dl >> 1;
    }
    return out;
  }
  const PARAM_A = [16, 15, 13, 12, 14, 11, 8, 9, 10, 7, 6, 5, 4, 3, 2, 1, 0], PROG_A = [13, 12, 11, 8, 9, 10, 7, 6, 5, 4, 3, 2, 1, 0];
  const DATA_B = [7, 0, 6, 1, 5, 2, 4, 3];
  function unscramble(r, n, abits) { const o = new Uint8Array(n); for (let s = 0; s < n; s++) o[s] = bitswap(r[bitswap(s, abits)], DATA_B); return o; }
  function lenBytes(out, n) { while (n >= 255) { out.push(255); n -= 255; } out.push(n); }
  function lz4(src) {                           // tools/lz4blk.py compress, byte for byte
    const n = src.length, out = [], table = new Map(), limit = n - 12;
    let anchor = 0, i = 0;
    while (i < limit) {
      const key = src[i] | src[i + 1] << 8 | src[i + 2] << 16 | src[i + 3] * 16777216;
      const cand = table.get(key);
      table.set(key, i);
      if (cand === undefined || i - cand > 65535) { i++; continue; }
      let mlen = 4;
      while (i + mlen < n - 5 && src[cand + mlen] === src[i + mlen]) mlen++;
      const ll = i - anchor;
      out.push((Math.min(ll, 15) << 4) | Math.min(mlen - 4, 15));
      if (ll >= 15) lenBytes(out, ll - 15);
      for (let k = anchor; k < i; k++) out.push(src[k]);
      const off = i - cand;
      out.push(off & 0xFF, off >> 8);
      if (mlen - 4 >= 15) lenBytes(out, mlen - 4 - 15);
      i += mlen;
      anchor = i;
    }
    const ll = n - anchor;
    out.push(Math.min(ll, 15) << 4);
    if (ll >= 15) lenBytes(out, ll - 15);
    for (let k = anchor; k < n; k++) out.push(src[k]);
    return Uint8Array.from(out);
  }
  const CRC_T = (() => { const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t; })();
  function crc32(d) { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) c = CRC_T[(c ^ d[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

  /* the pack of set k from files {name: Uint8Array} (rdpiano's names) */
  function makePack(k, files) {
    const s = SETS[k], HDR = 512;
    const wave = wavePack(...s.wave.map((n) => files[n]));
    const sorted = new Uint8Array(wave.length);
    s.order.forEach((pg, j) => sorted.set(wave.subarray(pg * 3072, (pg + 1) * 3072), j * 3072));
    const pagePos = s.order.map((_, pg) => s.order.indexOf(pg));
    const params = unscramble(files[s.params], 0x20000, PARAM_A), prog = unscramble(files[PROG], 0x2000, PROG_A);
    const wins = [...new Set(s.patches.map((p) => p[1] >> 15))].sort((a, b) => a - b);
    const lz = wins.map((w) => lz4(params.subarray(w << 15, (w + 1) << 15)));
    const total = HDR + sorted.length + prog.length + lz.reduce((a, c) => a + c.length, 0);
    const pk = new Uint8Array(total), dv = new DataView(pk.buffer);
    pk.set([..."RDPNOPK1"].map((c) => c.charCodeAt(0)), 0);
    pk.set([...s.label].map((c) => c.charCodeAt(0)), 16);
    pk.set([s.id, s.patches.length, lz.length, 1], 32);
    dv.setUint32(36, HDR, true); dv.setUint32(40, sorted.length, true);
    dv.setUint32(44, HDR + sorted.length, true); dv.setUint32(48, prog.length, true);
    let at = HDR;
    pk.set(sorted, at); at += sorted.length;
    pk.set(prog, at); at += prog.length;
    lz.forEach((c, j) => { dv.setUint32(52 + 8 * j, at, true); dv.setUint32(56 + 8 * j, c.length, true); pk.set(c, at); at += c.length; });
    s.patches.forEach(([name, off, khz, num], j) => {
      const o = 96 + 32 * j;
      pk.set([...name].map((c) => c.charCodeAt(0)), o);
      pk.set([wins.indexOf(off >> 15), khz, num, 0], o + 16);
      dv.setUint32(o + 20, off & 0x7FFF, true); dv.setUint32(o + 24, off, true);
    });
    pk.set(pagePos, 352);
    dv.setUint32(8, total, true);
    dv.setUint32(12, crc32(pk.subarray(16)), true);
    return pk;
  }

  /* the player's files -> {name: bytes} by rdpiano's names (by content, else by name), and the sets they make */
  async function sha256hex(b) {
    const h = await crypto.subtle.digest("SHA-256", b);
    return [...new Uint8Array(h)].map((x) => x.toString(16).padStart(2, "0")).join("");
  }
  async function readFiles(list) {
    const files = {}, unknown = [];
    const byLower = {};
    const stem = (n) => n.toLowerCase().replace(/\.[a-z0-9]+$/, "");   // (RD200_B.rom = RD200_B.bin)
    for (const s of Object.values(SETS)) for (const n of [...s.wave, s.params]) byLower[stem(n)] = n;
    byLower[stem(PROG)] = PROG;
    for (const f of list) {
      const b = new Uint8Array(await f.arrayBuffer());
      const name = KNOWN[await sha256hex(b)] || byLower[stem(f.name)];
      if (name && b.length === (SIZE[name] || 0x20000)) files[name] = b;
      else if (b.length === 0x20000 || b.length === 0x2000) unknown.push(f.name);
    }
    return {files, unknown};
  }
  const setsIn = (files) => Object.keys(SETS).filter((k) => [...SETS[k].wave, SETS[k].params, PROG].every((n) => files[n]));
  const missing = (k, files) => [...SETS[k].wave, SETS[k].params, PROG].filter((n) => !files[n]);
  /* the set the chosen files point to: a complete one, else the one with most of its own files (not just RD200_B) */
  function bestSet(files, now) {
    const own = (k) => [...SETS[k].wave, SETS[k].params].filter((n) => files[n]).length;
    const full = setsIn(files);
    if (full.length) return full.includes(now) ? now : full[0];
    const k = Object.keys(SETS).reduce((a, b) => (own(b) > own(a) ? b : a), now);
    return own(k) > own(now) ? k : now;
  }
  /* what set k needs, file by file, as HTML: each with what it is and whether it is here (files: readFiles') */
  function checklist(k, files = {}) {
    const s = SETS[k], esc = (t) => t.replace(/[&<>]/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;"})[c]);
    const rows = [...s.wave.map((n) => [n, "sound ROM (the wave samples)"]), [s.params, "parameter ROM (the voices)"],
                  [PROG, "program ROM of the CPU-B board: every set needs it"]];
    const have = rows.filter(([n]) => files[n]).length;
    return `<p class="small">${esc(s.label)} needs these ${rows.length} files (${have} of them chosen):</p><ul class="need">` +
      rows.map(([n, what]) => `<li class="${files[n] ? "ok" : "bad"}">${files[n] ? "✓" : "✗"} <code>${esc(n)}</code> ${esc(what)}</li>`).join("") +
      "</ul>";
  }

  /* ---------------------------------------------------------------- the FM-1 (romload.c) */
  const HEAD = [0x7D, 0x52, 0x44], CHUNK = 448;
  const STATUS = {1: "a frame arrived out of place", 2: "the FM-1's flash refused a write", 3: "the check after loading failed",
                  4: "the FM-1 lost the load", 5: "a frame sent again differs from the flash", 6: "the FM-1 did not take it as a ROM pack",
                  7: "frames kept arriving damaged"};
  const u7 = (v, n) => { const a = []; for (let i = 0; i < n; i++) a.push(Math.floor(v / 2 ** (7 * i)) & 0x7F); return a; };
  const n7 = (a) => a.reduce((s, x, i) => s + (x & 0x7F) * 2 ** (7 * i), 0);
  function pack7(d) {
    const o = [];
    for (let i = 0; i < d.length; i += 7) {
      const c = d.subarray(i, i + 7);
      let hi = 0;
      c.forEach((b, k) => { hi |= ((b >> 7) & 1) << k; });
      o.push(hi);
      c.forEach((b) => o.push(b & 0x7F));
    }
    return o;
  }
  function frame(cmd, body = []) {
    let b = [cmd, ...body];
    if (cmd >= 0x10 && cmd <= 0x12) { const s = b.reduce((a, x) => a + x, 0); b = [...b, s & 0x7F, (s >> 7) & 0x7F]; }
    return [0xF0, ...HEAD, ...b, 0xF7];
  }
  function loadFrames(pk) {
    const fr = [frame(0x10, u7(pk.length, 4))];
    for (let off = 0; off < pk.length; off += CHUNK) fr.push(frame(0x11, [...u7(off, 4), ...pack7(pk.subarray(off, off + CHUNK))]));
    fr.push(frame(0x12, u7(crc32(pk), 5)));
    return fr;
  }
  const SETNAME = {0: "MKS-20 A", 1: "MKS-20 B", 2: "MK-80"};
  function parseHello(d) {
    if (d.length < 39) return null;
    return {pack: !!d[10], set: d[10] ? SETNAME[d[11]] : null, patches: d[12], area: n7(d.slice(13, 17)),
            crc: n7(d.slice(17, 22)) >>> 0, name: String.fromCharCode(...d.slice(22, 38)).replace(/\0.*$/, "")};
  }
  const isOurs = (d, cmd) => d.length >= 7 && d[0] === 0xF0 && d[1] === HEAD[0] && d[2] === HEAD[1] && d[3] === HEAD[2] &&
                             d[4] === 0x7F && d[5] === cmd;

  /* a link to the FM-1: the port pair that answers the hello. find(midi) -> Link or null */
  class Link {
    constructor(i, o) { this.i = i; this.o = o; this.waiting = null; this.resent = 0;
      i.onmidimessage = (e) => this.message(e.data); }
    message(d) {
      const w = this.waiting;
      if (!w || !isOurs(d, w.cmd)) return;
      if (w.echo && !(d[7] === w.echo[0] && d[8] === w.echo[1]) && d[6] !== 7) return;   // a late answer to another frame
      this.waiting = null; clearTimeout(w.timer); w.resolve(Array.from(d));
    }
    once(f, ms) {
      return new Promise((resolve, reject) => {
        const cmd = f[4], echo = cmd >= 0x10 && cmd <= 0x12 ? [f[f.length - 3], f[f.length - 2]] : null;
        this.waiting = {cmd, echo, resolve, timer: setTimeout(() => { this.waiting = null; reject(new Error("no answer from the FM-1")); }, ms)};
        try { this.o.send(f); } catch (err) { clearTimeout(this.waiting.timer); this.waiting = null; reject(err); }
      });
    }
    async hello(ms = 800) { try { return parseHello(await this.once(frame(0x01), ms)); } catch (_) { return null; } }
    async checked(f, ms) {
      let last;
      for (let t = 0; t < 6; t++) {
        let a;
        try { a = await this.once(f, t ? Math.max(ms, 1500) : ms); }
        catch (err) { last = err; this.resent++; await new Promise((r) => setTimeout(r, 200)); continue; }
        if (a[6] === 7) { last = new Error(STATUS[7]); this.resent++; continue; }
        if (a[6] !== 0) {
          const v = [n7(a.slice(9, 13)), n7(a.slice(13, 17)), n7(a.slice(17, 21))];
          throw new Error(`${STATUS[a[6]] || "error " + a[6]} (${v.join(", ")})`);
        }
        return a;
      }
      throw last;
    }
    close() { this.i.onmidimessage = null; }
    /* the pack, with progress(fraction); resolves when the FM-1 has checked it (it then restarts) */
    async load(pk, progress = () => {}) {
      if (this.info && this.info.area && pk.length > this.info.area)
        throw new Error(`the pack (${pk.length} bytes) is larger than the FM-1's room for it (${this.info.area} bytes)`);
      const fr = loadFrames(pk);
      await this.checked(fr[0], 8000);
      for (let k = 1; k < fr.length - 1; k++) { await this.checked(fr[k], 4000); progress(k / (fr.length - 1)); }
      await this.checked(fr[fr.length - 1], 20000);
      progress(1);
    }
  }
  const live = (m) => [...m.values()].filter((p) => p.state !== "disconnected" && !/bluetooth/i.test(p.name || ""));
  function outputFor(midi, input) {
    const outs = live(midi.outputs), bare = (s) => (s || "").replace(/\s+\d+$/, "");
    const same = outs.filter((o) => bare(o.name) === bare(input.name));
    return outs.find((o) => o.name === input.name) || (same.length === 1 ? same[0] : null) || (outs.length === 1 ? outs[0] : null);
  }
  async function find(midi) {
    for (let pass = 0; pass < 2; pass++)
      for (const i of live(midi.inputs)) {
        const o = outputFor(midi, i);
        if (!o) continue;
        try { await i.open(); await o.open(); } catch (_) { continue; }
        const l = new Link(i, o), h = await l.hello();
        if (h) { l.info = h; return l; }
        l.close();
      }
    return null;
  }
  /* after an install or a load: wait until RDPNO answers again (it restarts), up to ms */
  async function waitFor(midi, ms = 30000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const l = await find(midi);
      if (l) return l;
      await new Promise((r) => setTimeout(r, 1000));
    }
    return null;
  }
  return {SETS, makePack, readFiles, setsIn, missing, bestSet, checklist, crc32, loadFrames, find, waitFor, lz4};
})();
if (typeof module !== "undefined") module.exports = RomLoad;
