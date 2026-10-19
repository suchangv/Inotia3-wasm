/*
 * elf_loader.c -- load libgame.so into the guest address space and resolve it.
 *
 * This is the wasm-side reimplementation of what lib/so_util/so_util.c does on
 * the Vita, minus everything that only exists because the Vita has a kernel
 * (so_memalign, kubridge module mapping, cache flushing). Here the guest
 * address space is just a buffer, so mapping a segment is a memcpy and an
 * R_ARM_RELATIVE is an add.
 */

#include "elf.h"
#include "guest.h"
#include "imports.h"
#include "compat.h"

/* --- little-endian readers, valid on wasm32 and native alike --------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *elf_sym_name(const guest_image *img, const elf_sym *s)
{
    if (!img->strtab || s->name_off >= img->strtab_sz)
        return "";
    return img->strtab + s->name_off;
}

uint32_t elf_find_symbol(const guest_image *img, const char *name)
{
    for (int i = 0; i < img->nsyms; i++) {
        if (img->syms[i].shndx == 0 || !img->syms[i].value)
            continue;
        const char *n = elf_sym_name(img, &img->syms[i]);
        if (n && strcmp(n, name) == 0) {
            uint32_t v = img->syms[i].value;
            return (v & ~1u) + img->base + (v & 1u);
        }
    }
    return 0;
}

/* ---- address -> symbol, O(log n) ---------------------------------------- */

/* Index of the last entry whose start is <= `addr`, or -1. */
static int sym_floor(const guest_image *img, uint32_t addr)
{
    uint32_t lo = 0, hi = (uint32_t)img->nsorted;
    uint32_t want = addr & ~1u;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (img->sorted[mid].value <= want)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (int)lo - 1;
}

const char *elf_nearest_sym(const guest_image *img, uint32_t guest_addr,
                            uint32_t *out_addr)
{
    int i = img->nsorted ? sym_floor(img, guest_addr) : -1;
    if (i < 0)
        return NULL;
    const sym_entry *e = &img->sorted[i];
    if (out_addr)
        *out_addr = e->value;
    return elf_sym_name(img, &img->syms[e->sym_idx]);
}

const char *elf_func_name(const guest_image *img, uint32_t guest_addr)
{
    int i = img->nsorted ? sym_floor(img, guest_addr) : -1;
    if (i < 0)
        return NULL;
    const sym_entry *e = &img->sorted[i];
    /* Reject a PC that is not genuinely inside a function: literal pools and
     * jump tables live in the gaps between functions, and attributing them to
     * the preceding function would silently inflate its profile share. */
    if (e->size && (guest_addr & ~1u) >= e->value + e->size)
        return NULL;
    const char *n = elf_sym_name(img, &img->syms[e->sym_idx]);
    return *n ? n : NULL;
}

/* ---- the load ------------------------------------------------------------ */

int elf_load(const uint8_t *file, size_t len, guest_image *out)
{
    if (len < 52 || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' ||
        file[3] != 'F') {
        host_log("elf: not an ELF file\n");
        return 1;
    }
    if (file[EI_CLASS] != ELFCLASS32 || file[EI_DATA] != ELFDATA2LSB) {
        host_log("elf: expected ELF32 little-endian\n");
        return 2;
    }
    if (rd16(file + 18) != EM_ARM) {
        host_log("elf: expected EM_ARM (got %u)\n", rd16(file + 18));
        return 3;
    }
    if (rd16(file + 16) != ET_DYN) {
        host_log("elf: expected a shared object (ET_DYN)\n");
        return 4;
    }

    memset(out, 0, sizeof(*out));
    out->base  = GUEST_IMAGE_BASE;
    out->entry = rd32(file + 24) + GUEST_IMAGE_BASE;

    uint32_t phoff = rd32(file + 28);
    uint16_t phentsize = rd16(file + 42);
    uint16_t phnum = rd16(file + 44);
    if (phentsize < 32) {
        host_log("elf: bad phentsize %u\n", phentsize);
        return 5;
    }

    /* ---- PT_LOAD ---- */
    const uint8_t *dyn = NULL;
    uint32_t dyn_sz = 0;
    uint32_t img_hi = 0;
    uint32_t text_lo_v = 0, text_hi_v = 0;

    for (int i = 0; i < phnum; i++) {
        const uint8_t *p = file + phoff + (uint32_t)i * phentsize;
        uint32_t type   = rd32(p + 0);
        uint32_t offset = rd32(p + 4);
        uint32_t vaddr  = rd32(p + 8);
        uint32_t filesz = rd32(p + 16);
        uint32_t memsz  = rd32(p + 20);
        uint32_t flags  = rd32(p + 24);

        if (type == PT_DYNAMIC) {
            dyn = file + offset;
            dyn_sz = filesz;
            continue;
        }
        if (type != PT_LOAD)
            continue;

        if (offset + filesz > len) {
            host_log("elf: PT_LOAD %d runs past EOF\n", i);
            return 6;
        }
        uint32_t dst = GUEST_IMAGE_BASE + vaddr;
        if (vaddr + memsz > GUEST_IMAGE_SIZE) {
            host_log("elf: PT_LOAD %d overflows the image region "
                     "(vaddr=%08x memsz=%08x)\n", i, vaddr, memsz);
            return 7;
        }
        /* Zero first: .bss is the tail of the RW segment and must not inherit
         * whatever the buffer held. */
        memset(g_mem + dst, 0, memsz);
        memcpy(g_mem + dst, file + offset, filesz);

        if (flags & 1 /* PF_X */) {
            if (!text_lo_v || vaddr < text_lo_v)
                text_lo_v = vaddr;
            if (vaddr + memsz > text_hi_v)
                text_hi_v = vaddr + memsz;
        }
        if (vaddr + memsz > img_hi)
            img_hi = vaddr + memsz;
    }
    out->text_lo = GUEST_IMAGE_BASE + text_lo_v;
    out->text_hi = GUEST_IMAGE_BASE + text_hi_v;

    host_log("elf: loaded %u bytes of image at %08x (text %08x-%08x)\n",
             img_hi, out->base, out->text_lo, out->text_hi);

    if (!dyn) {
        host_log("elf: no PT_DYNAMIC; a static .so is not expected here\n");
        return 8;
    }

    /* ---- pull what we need out of PT_DYNAMIC ---- */
    uint32_t dt_rel = 0, dt_relsz = 0, dt_jmprel = 0, dt_pltrelsz = 0;
    uint32_t dt_symtab = 0, dt_strtab = 0, dt_strsz = 0;
    uint32_t dt_init_array = 0, dt_init_arraysz = 0;
    uint32_t dt_hash = 0, dt_gnu_hash = 0;

    for (const uint8_t *d = dyn; d < dyn + dyn_sz; d += 8) {
        uint32_t tag = rd32(d), val = rd32(d + 4);
        if (tag == DT_NULL)
            break;
        switch (tag) {
        case DT_REL:          dt_rel = val; break;
        case DT_RELSZ:        dt_relsz = val; break;
        case DT_JMPREL:       dt_jmprel = val; break;
        case DT_PLTRELSZ:     dt_pltrelsz = val; break;
        case DT_SYMTAB:       dt_symtab = val; break;
        case DT_STRTAB:       dt_strtab = val; break;
        case DT_STRSZ:        dt_strsz = val; break;
        case DT_INIT_ARRAY:   dt_init_array = val; break;
        case DT_INIT_ARRAYSZ: dt_init_arraysz = val; break;
        case DT_HASH:         dt_hash = val; break;
        case DT_GNU_HASH:     dt_gnu_hash = val; break;
        default: break;
        }
    }

    if (!dt_symtab || !dt_strtab) {
        host_log("elf: PT_DYNAMIC has no symbol/string table\n");
        return 9;
    }

    /* Symbol count. DT_HASH stores it plainly (nchain); DT_GNU_HASH does not,
     * so fall back to walking section headers for .dynsym's size. libgame.so
     * carries full section headers, so the fallback is what actually runs. */
    int nsyms = 0;
    if (dt_hash) {
        nsyms = (int)rd32(g_mem + GUEST_IMAGE_BASE + dt_hash + 4);
    } else {
        if (dt_gnu_hash)
            host_log("elf: only DT_GNU_HASH present (no DT_HASH); falling back "
                     "to section headers for the symbol count\n");
        uint32_t shoff = rd32(file + 32);
        uint16_t shentsize = rd16(file + 46);
        uint16_t shnum = rd16(file + 48);
        if (shoff && shentsize >= 40) {
            for (int i = 0; i < shnum; i++) {
                const uint8_t *s = file + shoff + (uint32_t)i * shentsize;
                if (rd32(s + 4) == SHT_DYNSYM) {
                    nsyms = (int)(rd32(s + 20) / rd32(s + 36));
                    break;
                }
            }
        }
        if (!nsyms) {
            host_log("elf: cannot determine .dynsym count (no DT_HASH, no "
                     "section headers)\n");
            return 10;
        }
    }
    if (nsyms > 40000) {
        host_log("elf: implausible .dynsym count %d\n", nsyms);
        return 11;
    }

    const uint8_t *symtab = g_mem + GUEST_IMAGE_BASE + dt_symtab;
    const char    *strtab = (const char *)(g_mem + GUEST_IMAGE_BASE + dt_strtab);

    out->syms = (elf_sym *)host_alloc(sizeof(elf_sym) * (size_t)nsyms);
    out->nsyms = nsyms;
    for (int i = 0; i < nsyms; i++) {
        const uint8_t *s = symtab + (uint32_t)i * 16;
        out->syms[i].name_off = rd32(s + 0);
        out->syms[i].value    = rd32(s + 4);
        out->syms[i].size     = rd32(s + 8);
        out->syms[i].info     = s[12];
        out->syms[i].shndx    = rd16(s + 14);
    }

    /* Keep our own copy of .dynstr: the guest could in principle scribble on
     * its own string table, and profile reports outlive the run. */
    out->strtab_sz = dt_strsz ? dt_strsz : (uint32_t)(nsyms * 16);
    out->strtab = (char *)host_alloc(out->strtab_sz);
    memcpy(out->strtab, strtab, out->strtab_sz);

    /* ---- register imports ---- */
    int nimport = 0;
    for (int i = 0; i < nsyms; i++) {
        if (out->syms[i].shndx != 0)
            continue;
        if (ELF32_ST_TYPE(out->syms[i].info) != STT_FUNC &&
            ELF32_ST_TYPE(out->syms[i].info) != STT_OBJECT &&
            ELF32_ST_TYPE(out->syms[i].info) != STT_NOTYPE)
            continue;
        const char *nm = elf_sym_name(out, &out->syms[i]);
        if (!*nm)
            continue;
        import_register(nm, ELF32_ST_TYPE(out->syms[i].info) == STT_OBJECT);
        nimport++;
    }
    host_log("elf: %d undefined imports registered\n", nimport);
    imports_build_stubs();

    /* ---- apply relocations ---- */
    struct { uint32_t off, sz; const char *what; } rl[2] = {
        { dt_rel,    dt_relsz,    ".rel.dyn" },
        { dt_jmprel, dt_pltrelsz, ".rel.plt" },
    };
    uint32_t n_rel = 0, n_rel_unknown = 0;

    for (int r = 0; r < 2; r++) {
        if (!rl[r].off || !rl[r].sz)
            continue;
        const uint8_t *rel = g_mem + GUEST_IMAGE_BASE + rl[r].off;
        for (uint32_t o = 0; o + 8 <= rl[r].sz; o += 8) {
            uint32_t r_offset = rd32(rel + o);
            uint32_t r_info   = rd32(rel + o + 4);
            uint32_t type     = ELF32_R_TYPE(r_info);
            uint32_t symidx   = ELF32_R_SYM(r_info);
            uint32_t *slot    = (uint32_t *)(g_mem + GUEST_IMAGE_BASE + r_offset);
            uint32_t addend   = *slot;

            switch (type) {
            case R_ARM_RELATIVE:
                *slot = addend + GUEST_IMAGE_BASE;
                break;

            case R_ARM_ABS32: {
                /* A base-relative add *only* when there is no symbol. With one
                 * the value is S + A, and S is the symbol's address: a stub
                 * address when the symbol is imported, base + st_value when it
                 * is defined here.
                 *
                 * Treating it as RELATIVE regardless wrote base + 0 for the two
                 * imported-function pointers this binary keeps in .data --
                 * jsonp_malloc and jsonp_free store &malloc and &free -- so
                 * `blx r3` jumped to the image base and executed the ELF
                 * header. Both sides of the differential agreed on the wrong
                 * value (it is in the dumped image), which is exactly why the
                 * trace shows it as "the interpreter walked into .rodata and
                 * the oracle reported an invalid instruction". */
                uint32_t S = GUEST_IMAGE_BASE;
                if (symidx && symidx < (uint32_t)nsyms) {
                    const elf_sym *sym = &out->syms[symidx];
                    if (sym->shndx == 0) {
                        const char *nm = elf_sym_name(out, sym);
                        int idx = import_register(nm, ELF32_ST_TYPE(sym->info) == STT_OBJECT);
                        S = g_imports[idx].is_object ? g_imports[idx].stub
                                                     : g_imports[idx].interwork;
                    } else {
                        S = GUEST_IMAGE_BASE + sym->value;
                    }
                }
                *slot = S + addend;
                break;
            }

            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                if (symidx >= (uint32_t)nsyms) {
                    host_log("elf: %s: symbol index %u out of range\n",
                             rl[r].what, symidx);
                    return 12;
                }
                const char *nm = elf_sym_name(out, &out->syms[symidx]);
                int idx = import_register(nm, ELF32_ST_TYPE(out->syms[symidx].info) == STT_OBJECT);
                /* `interwork` rather than `stub`: the ARM PLT reaches this
                 * through `ldr pc, [ip]`, and bit 0 of the loaded value is
                 * what selects Thumb state. */
                *slot = g_imports[idx].is_object ? g_imports[idx].stub
                                                 : g_imports[idx].interwork;
                /* A function reached through the PLT must get a stub, never a
                 * variable's address: the engine calls through this slot, and
                 * a data address there is a jump into the loader's own state.
                 * Logged rather than assumed -- a symbol classified as an
                 * object when it is a function is invisible until something
                 * calls it. */
                static int n_obj_reloc;
                if (g_imports[idx].is_object && n_obj_reloc < 12) {
                    n_obj_reloc++;
                    host_log("elf: reloc %s at %08x -> %08x (%s)\n",
                             rl[r].what, r_offset, *slot, nm);
                }
                break;
            }

            default:
                /* Anything else (TLS, GOT_PREL, ...) is not expected in this
                 * binary -- it has no TLS relocations and GCC emitted only the
                 * four kinds above. Fail loudly rather than misinterpret a
                 * relocation and debug the fallout later. */
                n_rel_unknown++;
                if (n_rel_unknown <= 8)
                    host_log("elf: %s: unhandled reloc type %u at %08x (%s)\n",
                             rl[r].what, type, r_offset,
                             symidx < (uint32_t)nsyms
                                 ? elf_sym_name(out, &out->syms[symidx]) : "?");
                break;
            }
            n_rel++;
        }
    }
    host_log("elf: %u relocations applied, %u unhandled\n", n_rel, n_rel_unknown);

    if (dt_init_array && dt_init_arraysz) {
        out->init_array   = GUEST_IMAGE_BASE + dt_init_array;
        out->init_array_n = dt_init_arraysz / 4;
    }

    /* ---- address-ordered index for the profiler ----
     * Values are rebased into the guest address space. st_value is an offset
     * from the image base, but the profiler is handed live PCs, so comparing
     * the two without rebasing makes every lookup land on whichever symbol has
     * the highest offset -- which is the linker's `_end`, and looks
     * alarmingly like "100% of the time is spent in _end". */
    out->sorted = (sym_entry *)host_alloc(sizeof(sym_entry) * (size_t)(nsyms ? nsyms : 1));
    out->nsorted = 0;
    for (int i = 0; i < nsyms; i++) {
        if (out->syms[i].shndx == 0 || !out->syms[i].value)
            continue;
        out->sorted[out->nsorted].value   = (out->syms[i].value & ~1u) + GUEST_IMAGE_BASE;
        out->sorted[out->nsorted].size    = out->syms[i].size;
        out->sorted[out->nsorted].sym_idx = (uint32_t)i;
        out->nsorted++;
    }
    sort_by_u32((uint8_t *)out->sorted, (size_t)out->nsorted,
                sizeof(sym_entry), offsetof(sym_entry, value));

    host_log("elf: %d symbols indexed for profiling\n", out->nsorted);
    return 0;
}
