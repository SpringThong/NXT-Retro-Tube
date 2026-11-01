/* nxt-iconpatch — give NXT Retro Tube (and NXT Retro Music) their Tools-menu icons in the
 * NX Redux launcher of ANY version, by patching the launcher the device already has.
 *
 * The launcher maps tool names to icon files with a fixed table in Tiles_toolIcon():
 *   {"RetroAchievements", "menu_icon_achievements.png"}, ... {"Cheat Database", "menu_icon_cheats.png"}, ...
 * This tool finds that table (pairs of pointers to strings), writes a copy with our entries
 * appended into a new read-only segment (the unused PT_NOTE program header becomes a PT_LOAD),
 * and points the code at the copy: every "adrp xN, table; add xN, xN, :lo12:table" and the
 * loop's "cmp xM, #entries" next to it. Anything it does not recognise exactly: no change.
 *
 *   nxt-iconpatch --check  launcher       prints: patched | patchable | unsupported: <why>
 *   nxt-iconpatch launcher out            writes the patched launcher to out (exit 0 = ok)
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <elf.h>

static unsigned char *F; static size_t FN;
static Elf64_Ehdr *EH; static Elf64_Phdr *PH;

static void die(const char *why) { printf("unsupported: %s\n", why); exit(2); }
static long off_of(uint64_t va, size_t need) {          /* file offset of a mapped address */
    for (int i = 0; i < EH->e_phnum; i++) {
        Elf64_Phdr *p = &PH[i];
        if (p->p_type == PT_LOAD && va >= p->p_vaddr && va + need <= p->p_vaddr + p->p_filesz) return (long)(p->p_offset + (va - p->p_vaddr));
    }
    return -1;
}
static const char *str_at(uint64_t va) {
    long o = off_of(va, 1); if (o < 0) return NULL;
    size_t i = (size_t)o; while (i < FN && F[i] && i - (size_t)o < 64) i++;
    if (i >= FN || F[i]) return NULL;
    return (const char *)F + o;
}
static uint64_t rd64(long o) { uint64_t v; memcpy(&v, F + o, 8); return v; }
static uint32_t rd32(long o) { uint32_t v; memcpy(&v, F + o, 4); return v; }
static void wr32(long o, uint32_t v) { memcpy(F + o, &v, 4); }
static int loose_eq(const char *a, const char *b) {     /* the launcher's looseMatch */
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        if (!*a) return 1;
        a++; b++;
    }
}
static int is_icon(const char *s) { size_t l = s ? strlen(s) : 0; return l > 14 && !strncmp(s, "menu_icon_", 10) && !strcmp(s + l - 4, ".png"); }
static int is_name(const char *s) { return s && *s && strlen(s) < 40; }

/* ---- AArch64 bits ---- */
static int64_t sext(uint64_t v, int bits) { uint64_t m = 1ULL << (bits - 1); return (int64_t)((v ^ m) - m); }
static int is_adrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }
static uint64_t adrp_target(uint32_t i, uint64_t pc) { uint64_t imm = ((i >> 29) & 3) | (((i >> 5) & 0x7FFFF) << 2); return (pc & ~0xFFFULL) + (uint64_t)(sext(imm, 21) << 12); }
static uint32_t adrp_make(uint32_t i, uint64_t pc, uint64_t target) {
    int64_t d = (int64_t)((target & ~0xFFFULL) - (pc & ~0xFFFULL)) >> 12;
    uint32_t imm = (uint32_t)d & 0x1FFFFF;
    return (i & 0x9F00001Fu) | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5);
}
static int is_add64imm(uint32_t i) { return (i & 0xFF800000u) == 0x91000000u; }     /* add xd, xn, #imm (sh=0) */
static int is_adr(uint32_t i) { return (i & 0x9F000000u) == 0x10000000u; }
static int is_ret(uint32_t i) { return i == 0xD65F03C0u; }
static int is_cmp_imm(uint32_t i, unsigned n) {          /* cmp xN|wN, #n (subs zr, rn, #n, sh=0) */
    return ((i & 0xFFC0001Fu) == 0xF100001Fu || (i & 0xFFC0001Fu) == 0x7100001Fu) && ((i >> 10) & 0xFFF) == n;
}

int main(int argc, char **argv) {
    int check = argc == 3 && !strcmp(argv[1], "--check");
    if (!check && argc != 3) { fprintf(stderr, "usage: nxt-iconpatch --check launcher | nxt-iconpatch launcher out\n"); return 1; }
    const char *in = check ? argv[2] : argv[1];
    FILE *f = fopen(in, "rb"); if (!f) die("cannot read the launcher");
    fseek(f, 0, SEEK_END); FN = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    F = malloc(FN + 0x30000); if (!F || fread(F, 1, FN, f) != FN) die("cannot read the launcher"); fclose(f);
    EH = (Elf64_Ehdr *)F;
    if (FN < sizeof(*EH) || memcmp(EH->e_ident, ELFMAG, 4) || EH->e_ident[EI_CLASS] != ELFCLASS64 || EH->e_machine != EM_AARCH64) die("not an aarch64 ELF");
    if (EH->e_type != ET_EXEC) die("not a fixed-address executable");
    PH = (Elf64_Phdr *)(F + EH->e_phoff);

    /* 1. the table: pairs of pointers {name, "menu_icon_*.png"} containing the Cheat Database entry */
    long tab = -1; uint64_t tabva = 0; int n = 0, tables = 0, any_tube = 0;
    for (int s = 0; s < EH->e_phnum; s++) {
        Elf64_Phdr *p = &PH[s];
        if (p->p_type != PT_LOAD) continue;
        for (uint64_t o = p->p_offset & ~7ULL; o + 16 <= p->p_offset + p->p_filesz; o += 8) {
            const char *nm = str_at(rd64((long)o)), *ic;
            if (!nm || strcmp(nm, "Cheat Database")) continue;
            if (!(ic = str_at(rd64((long)o + 8))) || strcmp(ic, "menu_icon_cheats.png")) continue;
            long a = (long)o;                                            /* walk back to the first entry */
            while (a - 16 >= (long)p->p_offset && is_name(str_at(rd64(a - 16))) && is_icon(str_at(rd64(a - 8)))) a -= 16;
            int cnt = 0;
            while ((uint64_t)a + 16 * (cnt + 1) <= p->p_offset + p->p_filesz && is_name(str_at(rd64(a + 16 * cnt))) && is_icon(str_at(rd64(a + 16 * cnt + 8)))) cnt++;
            for (int i = 0; i < cnt; i++) if (loose_eq(str_at(rd64(a + 16 * i)), "NXT Retro Tube")) any_tube = 1;
            tables++;
            /* the launcher's own table is the one in its first (code) segment */
            if (tab < 0) { tab = a; n = cnt; tabva = p->p_vaddr + ((uint64_t)a - p->p_offset); }
            o = (uint64_t)a + 16 * (uint64_t)cnt;
        }
    }
    if (any_tube) { printf(check ? "patched\n" : "already patched\n"); return check ? 0 : 3; }
    if (tab < 0 || n < 3 || n > 4000) die("tool icon table not found");
    if (getenv("NXT_ICONPATCH_DEBUG")) fprintf(stderr, "table at 0x%llx, %d entries\n", (unsigned long long)tabva, n);
    int has_tube = 0, has_music = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = str_at(rd64(tab + 16 * i));
        if (loose_eq(nm, "NXT Retro Tube")) has_tube = 1;
        if (loose_eq(nm, "NXT Retro Music")) has_music = 1;
    }
    if (has_tube) { printf(check ? "patched\n" : "already patched\n"); return check ? 0 : 3; }
    int add = 1 + !has_music;

    /* 2. the code that uses it: adrp+add pairs to the table (or its end) and the entry count */
    struct { long adrp, add; uint64_t pc_adrp, pc_add, target; } refs[32]; int nr = 0;
    long cmps[64]; int nc = 0;
    for (int s = 0; s < EH->e_phnum; s++) {
        Elf64_Phdr *p = &PH[s];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        for (uint64_t o = (p->p_offset + 3) & ~3ULL; o + 4 <= p->p_offset + p->p_filesz; o += 4) {
            uint32_t ins = rd32((long)o); uint64_t pc = p->p_vaddr + (o - p->p_offset);
            if (is_adr(ins)) {
                uint64_t imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7FFFF) << 2), t = pc + (uint64_t)sext(imm, 21);
                if (t >= tabva && t <= tabva + 16 * (uint64_t)n) die("table reached with adr");
                continue;
            }
            if (!is_adrp(ins) || (adrp_target(ins, pc) != (tabva & ~0xFFFULL) && adrp_target(ins, pc) != ((tabva + 16 * (uint64_t)n) & ~0xFFFULL))) continue;
            unsigned rd = ins & 0x1F;
            for (int k = 1; k <= 16 && o + 4 * k + 4 <= p->p_offset + p->p_filesz; k++) {
                uint32_t a = rd32((long)o + 4 * k);
                if (!is_add64imm(a) || ((a >> 5) & 0x1F) != rd) continue;
                uint64_t t = adrp_target(ins, pc) + ((a >> 10) & 0xFFF);
                if (t != tabva && t != tabva + 16 * (uint64_t)n) break;          /* another variable in that page */
                if ((a & 0x1F) != rd) die("table address kept in two registers");
                if (nr >= 32) die("too many uses of the table");
                refs[nr].adrp = (long)o; refs[nr].add = (long)o + 4 * k; refs[nr].pc_adrp = pc; refs[nr].pc_add = pc + 4 * k; refs[nr].target = t; nr++;
                /* the loop bound: cmp #n between here and the function's return */
                if (t == tabva) {
                    int found = 0;
                    for (uint64_t q = o; q + 4 <= p->p_offset + p->p_filesz && q < o + 0x600; q += 4) {
                        uint32_t c = rd32((long)q);
                        if (is_cmp_imm(c, (unsigned)n)) { int dup = 0; for (int z = 0; z < nc; z++) dup |= cmps[z] == (long)q; if (!dup && nc < 64) cmps[nc++] = (long)q; found = 1; }
                        if (is_ret(c)) break;
                    }
                    if (!found) {
                        /* the compiler may use the table's address as an anchor for the data after it
                         * (add xN, xN, #off past the table): that is not a use of the table, leave it */
                        int other = 0;
                        for (int z = 1; z <= 24 && refs[nr - 1].add + 4 * z + 4 <= (long)(p->p_offset + p->p_filesz); z++) {
                            uint32_t c = rd32(refs[nr - 1].add + 4 * z);
                            unsigned rn = (c >> 5) & 0x1F, rm = (c >> 16) & 0x1F;
                            if (is_add64imm(c) && rn == rd && ((c >> 10) & 0xFFF) >= 16u * (unsigned)n) { other = 1; break; }
                            if ((c & 0x5F000000u) == 0x1E000000u) continue;                 /* floating point */
                            if ((c & 0xFF000010u) == 0x54000000u) continue;                 /* b.cond */
                            if ((c & 0x1F000000u) == 0x11000000u) { if (rn == rd) break; continue; }          /* add/sub imm */
                            if ((c & 0x1F200000u) == 0x0B000000u) { if (rn == rd || rm == rd) break; continue; } /* add/sub reg */
                            if ((c & 0x1F000000u) == 0x0A000000u) { if (rn == rd || rm == rd) break; continue; } /* logical reg */
                            if ((c & 0x3A000000u) == 0x38000000u) { if (rn == rd || (c & 0x1F) == rd) break; continue; }  /* load/store */
                            if ((c & 0xFF800000u) == 0xD2800000u || (c & 0xFF800000u) == 0x52800000u) { if ((c & 0x1F) == rd) break; continue; } /* movz */
                            break;                                                           /* anything else: not sure */
                        }
                        if (!other) die("loop bound not found");
                        if (getenv("NXT_ICONPATCH_DEBUG")) fprintf(stderr, "ref at 0x%llx: anchor for other data, kept\n", (unsigned long long)pc);
                        nr--;
                    }
                }
                break;
            }
        }
    }
    if (!nr) die("no code uses the table");
    int nstart = 0; for (int i = 0; i < nr; i++) nstart += refs[i].target == tabva;
    if (!nstart) die("table start not referenced");

    /* 3. a new read-only segment: the PT_NOTE program header becomes a PT_LOAD */
    Elf64_Phdr *note = NULL; uint64_t maxend = 0;
    for (int i = 0; i < EH->e_phnum; i++) {
        if (PH[i].p_type == PT_NOTE && !note) note = &PH[i];
        if (PH[i].p_type == PT_LOAD && PH[i].p_vaddr + PH[i].p_memsz > maxend) maxend = PH[i].p_vaddr + PH[i].p_memsz;
    }
    if (!note) die("no spare program header");
    for (int i = 0; i < EH->e_phnum; i++) if (PH[i].p_type == PT_LOAD && &PH[i] > note) die("program header order");
    if (check) { printf("patchable\n"); return 0; }

    const uint64_t AL = 0x10000;
    size_t noff = (FN + AL - 1) & ~(AL - 1);
    uint64_t nva = ((maxend + AL - 1) & ~(AL - 1)) + AL;
    nva += noff % AL;                                                       /* offset == vaddr (mod align) */
    unsigned char seg[0x8000]; size_t sl = 0; memset(seg, 0, sizeof(seg));
    size_t tsz = 16 * (size_t)(n + add);
    if (tsz + 256 > sizeof(seg)) die("table too large");
    memcpy(seg, F + tab, 16 * (size_t)n);
    sl = tsz;
    const char *names[2][2] = {{"NXT Retro Music", "menu_icon_nxt.png"}, {"NXT Retro Tube", "menu_icon_nxt_tube.png"}};
    int e = n;
    for (int k = has_music ? 1 : 0; k < 2; k++) {
        uint64_t pn = nva + sl; memcpy(seg + sl, names[k][0], strlen(names[k][0]) + 1); sl += strlen(names[k][0]) + 1;
        uint64_t pi = nva + sl; memcpy(seg + sl, names[k][1], strlen(names[k][1]) + 1); sl += strlen(names[k][1]) + 1;
        memcpy(seg + 16 * e, &pn, 8); memcpy(seg + 16 * e + 8, &pi, 8); e++;
    }
    sl = (sl + 7) & ~(size_t)7;
    memset(F + FN, 0, noff - FN);
    memcpy(F + noff, seg, sl);
    size_t newlen = noff + sl;
    note->p_type = PT_LOAD; note->p_flags = PF_R; note->p_offset = noff; note->p_vaddr = note->p_paddr = nva;
    note->p_filesz = note->p_memsz = sl; note->p_align = AL;

    /* 4. point the code at the new table */
    for (int i = 0; i < nr; i++) {
        uint64_t t = refs[i].target == tabva ? nva : nva + tsz;
        wr32(refs[i].adrp, adrp_make(rd32(refs[i].adrp), refs[i].pc_adrp, t));
        uint32_t a = rd32(refs[i].add);
        wr32(refs[i].add, (a & ~(0xFFFu << 10)) | (uint32_t)((t & 0xFFF) << 10));
    }
    for (int i = 0; i < nc; i++) { uint32_t c = rd32(cmps[i]); wr32(cmps[i], (c & ~(0xFFFu << 10)) | ((uint32_t)(n + add) << 10)); }

    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(F, 1, newlen, o) != newlen || fclose(o)) { printf("unsupported: cannot write\n"); return 1; }
    printf("patched %d entries -> %d (%d code uses, %d bounds)\n", n, n + add, nr, nc);
    return 0;
}
