// SPDX-License-Identifier: GPL-3.0-only
// tests/site_test.py: a fake FM-1 running RDPNO behind Web MIDI (romload.c's protocol, every 97th frame lost: the
// page sends it again); the pages see it as navigator.requestMIDIAccess

(() => {
  const T = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; T[n] = c >>> 0; }
  const crc = (d) => { let c = 0xFFFFFFFF; for (const b of d) c = T[(c ^ b) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  const st = { flash: new Uint8Array(0x79000).fill(0xFF), len: 0, next: 0, active: false, pack: null, frames: 0, dropEvery: 97 };
  window.fakeFM1 = st;
  const n7 = (a) => a.reduce((s, x, i) => s + x * 2 ** (7 * i), 0), u7 = (v, n) => [...Array(n)].map((_, i) => Math.floor(v / 2 ** (7 * i)) & 0x7F);
  const input = { name: "FM-1", state: "connected", onmidimessage: null, open: async () => {} };
  const reply = (d) => setTimeout(() => input.onmidimessage && input.onmidimessage({ data: Uint8Array.from(d) }), 1);
  const output = { name: "FM-1", state: "connected", open: async () => {}, send(d) {
    d = Array.from(d);
    if (d[1] !== 0x7D || d[2] !== 0x52 || d[3] !== 0x44) return;
    const cmd = d[4];
    if (cmd === 1) {
      const name = st.pack ? st.pack.name : "";
      reply([0xF0, 0x7D, 0x52, 0x44, 0x7F, 1, 0, 0, 0, 1, st.pack ? 1 : 0, st.pack ? st.pack.id : 0, st.pack ? st.pack.np : 0,
             ...u7(0x79000, 4), ...u7(st.pack ? st.pack.crc : 0, 5), ...[...name.padEnd(16, "\0")].map((c) => c.charCodeAt(0)), 0xF7]);
      return;
    }
    st.frames++;
    if (st.frames % st.dropEvery === 0) return;           // a lost frame: the page sends it again
    const echo = d.slice(-3, -1), body = d.slice(5, -3), ans = (s, a = 0, b = 0, c = 0) =>
      reply([0xF0, 0x7D, 0x52, 0x44, 0x7F, cmd, s, ...echo, ...u7(a, 4), ...u7(b, 4), ...u7(c, 4), 0xF7]);
    if (cmd === 0x10) { st.len = n7(body.slice(0, 4)); st.next = 0; st.active = true; st.pack = null; ans(0); return; }
    if (cmd === 0x11) {
      const off = n7(body.slice(0, 4)), b = [];
      for (let i = 4; i < body.length; i += 8) for (let k = 1; k < 8 && i + k < body.length; k++) b.push(body[i + k] | (((body[i] >> (k - 1)) & 1) << 7));
      if (off + b.length <= st.next) { ans(0); return; }
      if (off !== st.next) { ans(1, off, st.next); return; }
      st.flash.set(b, off); st.next += b.length; ans(0); return;
    }
    if (cmd === 0x12) {
      const want = n7(body.slice(0, 5)), got = crc(st.flash.subarray(0, st.len));
      if (want !== got) { ans(3); return; }
      const f = st.flash;
      st.pack = { id: f[32], np: f[33], crc: (f[12] | f[13] << 8 | f[14] << 16 | f[15] << 24) >>> 0,
                  name: String.fromCharCode(...f.subarray(16, 32)).replace(/\0.*$/, "") };
      st.active = false; ans(0); st.done = true;
    }
  } };
  const map = (p) => new Map([["1", p]]);
  navigator.requestMIDIAccess = async () => ({ inputs: map(input), outputs: map(output), onstatechange: null });
})();
