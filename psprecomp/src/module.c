/* psprecomp -- loading and starting a recompiled module. See module.h. */

#include "psprecomp/module.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"
#include "psprecomp/ctors.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int psp_module_load(const char *path, uint32_t span, psp_module *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "psprecomp: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *b = (n > 52) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b);
        fprintf(stderr, "psprecomp: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    /* A decrypted PRX is an ELF with one PT_LOAD. Its p_paddr is not a
     * physical address: it holds the file offset of the module info. */
    const uint32_t phoff = rd32(b + 28);
    if (memcmp(b, "\x7F" "ELF", 4) != 0 || (size_t)phoff + 32 > (size_t)n) {
        free(b);
        fprintf(stderr, "psprecomp: %s is not a decrypted ELF module\n", path);
        return -1;
    }
    const uint8_t *ph = b + phoff;
    const uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), paddr = rd32(ph + 12);
    const uint32_t filesz = rd32(ph + 16);
    if ((size_t)off + filesz > (size_t)n || filesz > span ||
        psp_mem_map_module(vaddr, span) != 0 ||
        psp_mem_write_block(vaddr, b + off, filesz) != 0) {
        free(b);
        fprintf(stderr, "psprecomp: cannot place %s at 0x%08X\n", path, vaddr);
        return -1;
    }

    m->base = vaddr;
    m->image_end = vaddr + filesz;
    m->entry = rd32(b + 24);
    /* SceModuleInfo: u16 attr, u16 version, char name[28], then gp. */
    const uint32_t info = (paddr & 0x7FFFFFFFu) - off + vaddr;
    m->gp = psp_read32(info + 32);

    /* Later PT_LOADs. Some titles (Asura-engine games) put .ctors, .dtors and
     * the tail of .data in a second segment; skipping it leaves the real
     * constructor table unloaded and the heuristic below finds a vtable. */
    const uint16_t phnum = (uint16_t)(b[44] | (b[45] << 8));
    for (uint16_t i = 1; i < phnum && (size_t)phoff + 32u * (i + 1) <= (size_t)n; i++) {
        const uint8_t *p = b + phoff + 32u * i;
        if (rd32(p) != 1) continue;                       /* PT_LOAD */
        const uint32_t o = rd32(p + 4), va = rd32(p + 8), fs = rd32(p + 16);
        if (fs && (size_t)o + fs <= (size_t)n && va + fs <= vaddr + span)
            psp_mem_write_block(va, b + o, fs);
    }

    /* Prefer the .ctors section named in the section headers. GCC's table is
     * -1, entries..., 0; psp_ctors_run walks from the first entry to the 0. */
    int have_ctors = 0;
    const uint32_t shoff = rd32(b + 32);
    const uint16_t shent = (uint16_t)(b[46] | (b[47] << 8));
    const uint16_t shnum = (uint16_t)(b[48] | (b[49] << 8));
    const uint16_t shstr = (uint16_t)(b[50] | (b[51] << 8));
    if (shoff && shent >= 40 && shstr < shnum && (size_t)shoff + (size_t)shent * shnum <= (size_t)n) {
        const uint8_t *ss = b + shoff + (size_t)shent * shstr;
        const uint32_t stroff = rd32(ss + 16);
        for (uint16_t i = 0; i < shnum; i++) {
            const uint8_t *s = b + shoff + (size_t)shent * i;
            const uint32_t nm = rd32(s);
            if ((size_t)stroff + nm + 7 > (size_t)n) continue;
            if (strcmp((const char *)b + stroff + nm, ".ctors") != 0) continue;
            uint32_t addr = rd32(s + 12), size = rd32(s + 20);
            if (size >= 4 && psp_read32(addr) == 0xFFFFFFFFu) { addr += 4; size -= 4; }
            m->ctors = addr;
            m->nctors = (int)(size / 4) - 1;
            have_ctors = 1;
            break;
        }
    }
    free(b);
    if (have_ctors) return 0;

    /* The constructor table and the constructors sit together at the end of
     * the image; a vtable elsewhere has the same shape, so only that region
     * is searched, and the entries must point into it too.
     * ponytail: right for WTF's modules (8 constructors just below .bss);
     * widen the window if another title keeps them elsewhere. */
    const uint32_t lo = m->image_end > vaddr + 0x10000u ? m->image_end - 0x10000u : vaddr;
    uint32_t t;
    int count;
    if (psp_ctors_find(lo, m->image_end, lo, m->image_end, &t, &count) == 0) {
        m->ctors = t;
        m->nctors = count;
    }
    return 0;
}

/* The loader's leftovers. $k0 is the thread pointer: libc reaches its
 * per-thread reent, and through it the heap, at *(k0 + 4). The stack is only
 * module_start's, which returns once it has started the game's threads.
 * ponytail: fixed addresses, proven on WTF; take them from sysmem if a title
 * minds. */
#define START_STACK 0x08F00000u
#define TCB_ADDR    0x08800000u
#define REENT_ADDR  0x08800100u

uint32_t psp_module_start(const psp_module *m) {
    psp_cpu.r[PSP_REG_GP] = m->gp;
    psp_cpu.r[PSP_REG_SP] = START_STACK;
    psp_cpu.r[PSP_REG_A0] = 0;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_cpu.r[26] = TCB_ADDR;                   /* $k0 */
    psp_write32(TCB_ADDR + 4, REENT_ADDR);
    /* Elite Squadron's libc treats *(k0+4) as a thread record and looks its
     * +0x37C key up in a per-thread reent table; key 0 matches an empty slot
     * and yields garbage. A key no table holds falls back to _impure_ptr. */
    psp_write32(REENT_ADDR + 0x37C, 0x7FFFFFFFu);
    if (m->ctors) psp_ctors_run(m->ctors);
    psp_call_from_host(m->entry);
    return psp_cpu.r[PSP_REG_V0];
}
