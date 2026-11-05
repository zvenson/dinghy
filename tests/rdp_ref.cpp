// SPDX-License-Identifier: GPL-3.0-only
// The reference: rdpiano's own librdpiano (giulioz/rdpiano) plays tests/rdp_script.h, the way rdpiano_juce drives it
// (constructor, loadSounds, reset, the handshake; a patch change: loadSounds + 0x31 0x30). tests/rdp_compare.sh
// builds it against a checkout of rdpiano and compares its samples with the C port's (rdp_exact 0): they must match.
//   rdp_ref ROMDIR OUT.raw
#include <cstdio>
#include <cstdlib>
#include <string>
#include "mcu.h"
#include "rdp_rom.h"     // (only RDP_PATCH: which patch is where)
#include "rdp_script.h"

static u8 ic5[0x20000], ic6[0x20000], ic7[0x20000], prog[0x2000], params[0x20000];
static void load(const std::string &dir, const char *name, u8 *d, size_t n)
{
    FILE *f = fopen((dir + "/" + name).c_str(), "rb");
    if (!f || fread(d, 1, n, f) != n) { fprintf(stderr, "rdp_ref: cannot read %s\n", name); exit(1); }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: rdp_ref ROMDIR OUT.raw\n"); return 2; }
    std::string d = argv[1];
    load(d, RDP_REF_IC5, ic5, sizeof ic5); load(d, RDP_REF_IC6, ic6, sizeof ic6); load(d, RDP_REF_IC7, ic7, sizeof ic7);
    load(d, "RD200_B.bin", prog, sizeof prog); load(d, RDP_REF_PARAMS, params, sizeof params);
    Mcu *m = new Mcu(ic5, ic6, ic7, prog, params);
    unsigned cur = 0;
    m->loadSounds(ic5, ic6, ic7, params, RDP_PATCH[0].addr);
    m->reset();
    m->commands_queue.push(0x30); m->commands_queue.push(0xE0); m->commands_queue.push(0); m->commands_queue.push(0);
    for (int i = 0; i < 1024; i++) m->generate_next_sample(RDP_PATCH[0].khz == 32);   // (as rdp_init: at the patch's rate)
    m->commands_queue.push(0x31); m->commands_queue.push(0x30);
    FILE *f = fopen(argv[2], "wb");
    unsigned e = 0;
    for (unsigned t = 0; t < RDP_SCRIPT_END; t++) {
        for (; e < RDP_SCRIPT_N && RDP_SCRIPT[e].t == t; e++) {
            if (RDP_SCRIPT[e].patch >= 0) {
                cur = (unsigned)RDP_SCRIPT[e].patch % RDP_NPATCH;
                m->loadSounds(ic5, ic6, ic7, params, RDP_PATCH[cur].addr);
                m->commands_queue.push(0x31); m->commands_queue.push(0x30);
            } else {
                m->sendMidiCmd(RDP_SCRIPT[e].st, RDP_SCRIPT[e].d1, RDP_SCRIPT[e].d2);
            }
        }
        s32 s = m->generate_next_sample(RDP_PATCH[cur].khz == 32);
        fwrite(&s, 4, 1, f);
    }
    fclose(f);
    return 0;
}
