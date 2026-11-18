// ebootdec: decrypts a PSP EBOOT.BIN to an ELF and applies its relocations at
// base 0, matching PPSSPP's "dump decrypted EBOOT". GPLv3, built from
// pspdecrypt (https://github.com/John-K/pspdecrypt) and PPSSPP's PrxDecrypter
// and libkirk.
#include <cstdio>
#include <cstring>
#include <vector>
#include "PrxDecrypter.h"

static u32 rd(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static int relocate(u8 *b, u32 n) {
    const u32 phoff = rd(b + 28), shoff = rd(b + 32);
    const u16 phnum = b[44] | b[45] << 8, shnum = b[48] | b[49] << 8;
    if (n < 52 || phnum > 16 || (u64)phoff + 32ull * phnum > n || (u64)shoff + 40ull * shnum > n) return -1;
    u32 seg_va[16] = {}, seg_off[16] = {};
    for (u32 i = 0; i < phnum && i < 16; i++) { seg_off[i] = rd(b + phoff + 32 * i + 4); seg_va[i] = rd(b + phoff + 32 * i + 8); }
    auto at = [&](u32 va) -> u8 * {             // vaddr to file pointer (segment 0 or 1)
        for (int i = phnum - 1; i >= 0; i--) if (va >= seg_va[i]) { u32 o = va - seg_va[i] + seg_off[i]; return o + 4 <= n ? b + o : nullptr; }
        return nullptr;
    };
    for (u32 s = 0; s < shnum; s++) {
        const u8 *sh = b + shoff + 40 * s;
        if (rd(sh + 4) != 0x700000A0u) continue;
        const u32 off = rd(sh + 16), cnt = rd(sh + 20) / 8;
        if ((u64)off + 8ull * cnt > n) return -1;
        for (u32 r = 0; r < cnt; r++) {
            const u32 ofs = rd(b + off + 8 * r), info = rd(b + off + 8 * r + 4);
            const u32 type = info & 0xFF, os = (info >> 8) & 0xFF, vs = (info >> 16) & 0xFF;
            if (os >= 16 || vs >= 16) return -1;
            u8 *p = at(seg_va[os] + ofs); if (!p) return -1;
            const u32 to = seg_va[vs], op = rd(p);
            switch (type) {
            case 0: case 7: break;
            case 2: wr(p, op + to); break;
            case 4: wr(p, (op & 0xFC000000u) | ((((op & 0x03FFFFFFu) << 2) + to) >> 2 & 0x03FFFFFFu)); break;
            case 5: {                                    // HI16: pair with the next LO16
                s32 lo = 0;
                for (u32 k = r + 1; k < cnt; k++) {
                    const u32 i2 = rd(b + off + 8 * k + 4);
                    if ((i2 & 0xFF) == 6) { u8 *q = at(seg_va[(i2 >> 8) & 0xFF] + rd(b + off + 8 * k)); if (q) lo = (s16)(rd(q) & 0xFFFF); break; }
                }
                const u32 full = (op << 16) + lo + to;
                wr(p, (op & 0xFFFF0000u) | (((full - (s16)(full & 0xFFFF)) >> 16) & 0xFFFF));
                break;
            }
            case 6: wr(p, (op & 0xFFFF0000u) | ((op + to) & 0xFFFF)); break;
            default: fprintf(stderr, "unknown relocation type %u\n", type); return -1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: ebootdec <EBOOT.BIN> <out.elf>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 4; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0x150) { fprintf(stderr, "cannot read %s\n", argv[1]); return 4; }
    std::vector<u8> in(n), out(n + 0x1000);
    if (fread(in.data(), 1, n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[1]); return 4; } fclose(f);
    int m = pspDecryptPRX(in.data(), out.data(), (u32)n);
    if (m <= 0) { fprintf(stderr, "decrypt failed\n"); return 1; }
    if (out[0] == 0x1F && out[1] == 0x8B) { fprintf(stderr, "compressed output not supported\n"); return 1; }
    if (memcmp(out.data(), "\x7F" "ELF", 4)) { fprintf(stderr, "output is not an ELF\n"); return 1; }
    if (relocate(out.data(), (u32)m)) { fprintf(stderr, "bad relocations\n"); return 1; }
    FILE *o = fopen(argv[2], "wb"); if (!o || fwrite(out.data(), 1, m, o) != (size_t)m || fclose(o)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 3; }
    return 0;
}
