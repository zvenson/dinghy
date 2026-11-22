/* SPDX-License-Identifier: GPL-3.0-only
 * RDPNO's own step in the installer (installer.html APP; after sq80-fm1's app/install_app.js): an update clears the
 * ROMs from the FM-1's flash, so the installer loads them again right after it. before() asks for the player's ROM
 * files (or offers the pack this browser kept from last time: kept in this browser only, never sent anywhere but the
 * FM-1) and makes the pack (romload.js); after() waits for RDPNO to start, then loads it, unless the FM-1 still
 * holds the same one. */
({
  KEEP_DB: "rdpno-setup",
  sleep: (ms) => new Promise((r) => setTimeout(r, ms)),
  db() {
    return new Promise((resolve, reject) => {
      const r = indexedDB.open(this.KEEP_DB, 1);
      r.onupgradeneeded = () => r.result.createObjectStore("files");
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  },
  async kept() {
    try {
      const d = await this.db();
      return await new Promise((resolve) => {
        const q = d.transaction("files").objectStore("files").get("pack");
        q.onsuccess = () => resolve(q.result || null);
        q.onerror = () => resolve(null);
      });
    } catch (_) { return null; }
  },
  async keep(v) {
    try {
      const d = await this.db();
      await new Promise((resolve) => {
        const tx = d.transaction("files", "readwrite");
        tx.objectStore("files").put(v, "pack");
        tx.oncomplete = tx.onerror = () => resolve();
      });
    } catch (_) { /* not kept: asked again next time */ }
  },

  /* the files asked for in the installer's dialog: a pack, or null (no ROMs), or false (cancel) */
  async ask(ui, note) {
    const sets = Object.entries(RomLoad.SETS).map(([k, s]) => `<option value="${k}">${s.label}: ${s.patches.map((p) => p[0]).join(", ")}</option>`);
    const p = ui.ask("Your ROMs",
      `<p>RDPNO plays Roland's own sound ROMs, and they are not part of it: load your own dumps. Installing clears
       them from the FM-1, so they are loaded again right after the firmware.</p>
       <p>Choose the ROM files (all of them at once is fine: the page takes the ones the set needs, by their
       content), as <a href="https://github.com/giulioz/rdpiano" target="_blank" rel="noopener">rdpiano</a> names them.
       They stay on this computer; only the FM-1 gets them.</p>
       <p><input type="file" id="rlfiles" multiple></p>
       <p><label>Set <select id="rlset">${sets.join("")}</select></label></p>
       <p class="small" id="rlnote">${note || ""}</p>`,
      [["go", "Install with these ROMs"], ["none", "Install without ROMs"], ["cancel", "Cancel"]]);
    let got = {files: {}, unknown: []};
    const show = () => {
      const have = RomLoad.setsIn(got.files), k = document.getElementById("rlset").value;
      if (have.length && !have.includes(k)) document.getElementById("rlset").value = have[0];
      const miss = RomLoad.missing(document.getElementById("rlset").value, got.files);
      document.getElementById("rlnote").textContent = miss.length ? `Missing for this set: ${miss.join(", ")}` +
        (got.unknown.length ? `. Not recognised: ${got.unknown.join(", ")}` : "") : "All files of this set are here.";
    };
    document.getElementById("rlfiles").addEventListener("change", async (e) => { got = await RomLoad.readFiles(e.target.files); show(); });
    document.getElementById("rlset").addEventListener("change", show);
    const v = await p;
    if (v === "cancel") return false;
    if (v === "none") return null;
    const k = document.getElementById("rlset").value;
    const miss = RomLoad.missing(k, got.files);
    if (miss.length) return this.ask(ui, `Missing for ${RomLoad.SETS[k].label}: ${miss.join(", ")}.`);
    ui.say("Making the ROM pack…");
    await this.sleep(30);
    const pack = RomLoad.makePack(k, got.files);
    await this.keep({set: RomLoad.SETS[k].label, pack});
    return {set: RomLoad.SETS[k].label, pack};
  },

  async before(midi, ui) {
    const k = await this.kept();
    if (k && k.pack) {
      const v = await ui.ask("Your ROMs",
        `<p>Installing clears the ROMs from the FM-1. This browser kept the pack you loaded last time:
         <b>${k.set}</b>. Load it again right after the firmware?</p>`,
        [["kept", `Yes, ${k.set}`], ["other", "Other ROMs…"], ["none", "Without ROMs"]]);
      if (v === "kept") return {pack: k.pack, set: k.set};
      if (v === "none") return {pack: null};
    }
    const r = await this.ask(ui);
    return r === false ? false : {pack: r && r.pack, set: r && r.set};
  },

  async after(midi, ui, st) {
    if (!st || !st.pack) { ui.say("Done. RDPNO runs without ROMs: load them on the load page."); return; }
    ui.say("Waiting for RDPNO to start…");
    await this.sleep(2000);
    const link = await RomLoad.waitFor(midi, 40000);
    if (!link) throw new Error("RDPNO did not answer after the install: load the ROMs on the load page.");
    if (link.info.pack && link.info.crc === RomLoad.crc32(st.pack.subarray(16)) >>> 0) {
      link.close();
      ui.say(`Done. The FM-1 still holds ${st.set}.`);
      return;
    }
    ui.say(`Loading ${st.set}…`);
    try {
      await link.load(st.pack, (f) => { ui.progress(f * 100); ui.say(`Loading ${st.set}: ${Math.round(f * 100)} %`); });
    } finally { link.close(); }
    ui.say(`Done. RDPNO restarts with ${st.set}.`);
  },
})
