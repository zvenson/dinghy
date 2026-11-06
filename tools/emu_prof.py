#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Where the cycles go: fm1-live's FM1_TPROF profile (PC -> time) by function and by hottest instruction.
  tools/emu_prof.py PROF SYMS DIS      (tools/emu_load.sh: emu_prof.txt, objdump -t, build/rdpno.dis)"""
import bisect, re, sys

prof, syms, dis = sys.argv[1:4]
funcs = []
for ln in open(syms):
    p = ln.split()
    if len(p) >= 6 and "F" in p[2:4] and p[-2] != "00000000":
        funcs.append((int(p[0], 16), int(p[-2], 16), p[-1]))
funcs.sort()
starts = [f[0] for f in funcs]
text = {}
for ln in open(dis, errors="replace"):
    m = re.match(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*\t(.*)$", ln)
    if m:
        text[int(m.group(1), 16)] = m.group(2).strip()
by_f, by_pc, total, miss = {}, {}, 0, {}
by_f1, total1 = {}, 0
for ln in open(prof):
    p = ln.split()
    if p[0] == "0":
        pc, n = int(p[1], 16), int(p[2])
        total += n
        by_pc[pc] = by_pc.get(pc, 0) + n
        i = bisect.bisect_right(starts, pc) - 1
        name = funcs[i][2] if i >= 0 and pc < funcs[i][0] + funcs[i][1] else "?"
        by_f[name] = by_f.get(name, 0) + n
    elif p[0] == "1":
        pc, n = int(p[1], 16), int(p[2])
        total1 += n
        i = bisect.bisect_right(starts, pc) - 1
        name = funcs[i][2] if i >= 0 and pc < funcs[i][0] + funcs[i][1] else "?"
        by_f1[name] = by_f1.get(name, 0) + n
    elif p[0].startswith("m"):
        miss[int(p[1], 16)] = miss.get(int(p[1], 16), 0) + int(p[2])
out = ["core 0, by function:"]
for name, n in sorted(by_f.items(), key=lambda x: -x[1])[:14]:
    out.append(f"  {100 * n / total:5.1f} %  {name}")
if total1:
    out.append("core 1, by function:")
    for name, n in sorted(by_f1.items(), key=lambda x: -x[1])[:6]:
        out.append(f"  {100 * n / total1:5.1f} %  {name}")
out.append("hottest instructions:")
for pc, n in sorted(by_pc.items(), key=lambda x: -x[1])[:20]:
    out.append(f"  {100 * n / total:5.1f} %  {pc:08x}  {text.get(pc, '')}")
out.append(f"flash cache misses: {sum(miss.values())} on {len(miss)} lines; most:")
for a, n in sorted(miss.items(), key=lambda x: -x[1])[:8]:
    i = bisect.bisect_right(starts, a) - 1
    out.append(f"  {n:8d}  {a:08x}  {funcs[i][2] if i >= 0 else '?'}")
print("\n".join(out))
print("::notice title=fm1-emu profile (three keys struck again)::" + "%0A".join(out))

# the hot function's code, for reading (tools/emu_load.sh: in the check run)
if len(sys.argv) > 4:
    want = sys.argv[4]
    lo = [(a, n) for a, n, name in funcs if name == want]
    lines = []
    if lo:
        a0, n0 = lo[0]
        for pc in sorted(k for k in text if a0 <= k < a0 + n0):
            lines.append(f"{100 * by_pc.get(pc, 0) / max(total, 1):4.1f} {pc:08x} {text[pc]}")
    print(f"::notice title=code of {want}::" + ("%0A".join(lines) if lines else "(not found)"))
