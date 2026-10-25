/* insncount — a QEMU TCG plugin: guest instructions executed, attributed to each translation block's start
 * address, written to $INSN_OUT at exit ("<vaddr hex> <instructions>"). VibeSDR FT8 CPU work, 2026-10-25. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
typedef struct { uint64_t vaddr; uint64_t insns; } Ent;
#define CAP (1u << 21)
static Ent* tab;
static Ent* slot(uint64_t va) {
    uint64_t h = (va * 0x9E3779B97F4A7C15ull) >> 43;
    for (;;) { Ent* e = &tab[h & (CAP - 1)]; if (e->vaddr == va || e->vaddr == 0) { e->vaddr = va; return e; } ++h; }
}
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
    Ent* e = slot(qemu_plugin_tb_vaddr(tb) | 1);   /* |1: never 0 */
    qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &e->insns, qemu_plugin_tb_n_insns(tb));
}
static void at_exit(qemu_plugin_id_t id, void* p) {
    const char* path = getenv("INSN_OUT"); FILE* f = fopen(path ? path : "insn.txt", "w"); uint64_t tot = 0;
    for (uint32_t i = 0; i < CAP; ++i) if (tab[i].insns) { fprintf(f, "%llx %llu\n", (unsigned long long)(tab[i].vaddr & ~1ull), (unsigned long long)tab[i].insns); tot += tab[i].insns; }
    fprintf(f, "TOTAL %llu\n", (unsigned long long)tot); fclose(f);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
    tab = calloc(CAP, sizeof(Ent));
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
