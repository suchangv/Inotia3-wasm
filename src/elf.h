/*
 * elf.h -- minimal ELF32/ARM reader for libgame.so.
 *
 * Deliberately does NOT #include <elf.h>: the guest is a 32-bit little-endian
 * ARM object and the host may be a wasm32 freestanding build with no system
 * headers at all. All parsing is explicit byte arithmetic.
 *
 * Only what a *static* loader needs. libgame.so is BIND_NOW + SYMBOLIC with no
 * mprotect import and no self-modifying code, so relocations are all resolvable
 * up front and the guest never needs a runtime dynamic linker.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

/* --- e_ident --- */
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS32  1
#define ELFDATA2LSB 1

/* --- e_type / e_machine --- */
#define ET_DYN      3
#define EM_ARM      40

/* --- p_type --- */
#define PT_LOAD     1
#define PT_DYNAMIC  2

/* --- sh_type --- */
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_DYNSYM  11

/* --- st_info helpers --- */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_ST_BIND(i) ((i) >> 4)

/* --- r_info helpers --- */
#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)

/* --- ARM relocation types we actually meet (verified against the real .so:
 *     2954 R_ARM_RELATIVE + 178 R_ARM_JUMP_SLOT + 5 R_ARM_GLOB_DAT + 2 ABS32) --- */
#define R_ARM_ABS32     2
#define R_ARM_GLOB_DAT  21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE  23

/* --- dynamic tags --- */
#define DT_NULL         0
#define DT_PLTRELSZ     2
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_STRSZ        10
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH     0x6ffffef5

/* A dynamic symbol, resolved out of the guest image. */
typedef struct {
    uint32_t name_off;   /* offset into the string table          */
    uint32_t value;      /* st_value; bit0 set => Thumb function  */
    uint32_t size;       /* st_size                               */
    uint8_t  info;       /* st_info: bind<<4 | type               */
    uint16_t shndx;      /* st_shndx; 0 => undefined (an import)  */
} elf_sym;

/* One entry of the address-ordered symbol index. Carries `size` so the
 * profiler can reject a PC that is not genuinely inside the function (literal
 * pools and jump tables live between functions and would otherwise be
 * attributed to whichever function precedes them). */
typedef struct {
    uint32_t value;      /* start address, Thumb bit masked off  */
    uint32_t size;       /* 0 when the producer emitted no size   */
    uint32_t sym_idx;    /* back into guest_image.syms            */
} sym_entry;

typedef struct {
    uint32_t  base;        /* guest address the image was loaded at      */
    uint32_t  entry;       /* e_entry, rebased                           */
    uint32_t  init_array;  /* guest addr of DT_INIT_ARRAY, or 0          */
    uint32_t  init_array_n;/* element count                              */
    uint32_t  text_lo;     /* first executable guest address             */
    uint32_t  text_hi;     /* one past the last executable guest address */

    /* Dynamic symbol table, kept for two reasons: resolving relocations, and
     * attributing profile samples to function names. 6141 FUNC symbols in the
     * real .so, which is what makes an AOT translation tractable later. */
    elf_sym  *syms;
    int       nsyms;
    char     *strtab;      /* host-allocated copy of .dynstr             */
    uint32_t  strtab_sz;

    sym_entry *sorted;     /* by `value`, for O(log n) lookup            */
    int        nsorted;
} guest_image;

/* Load `len` bytes of `file` into the guest address space. Returns 0 on
 * success. On failure returns non-zero and logs why. */
int  elf_load(const uint8_t *file, size_t len, guest_image *out);

/* Resolve an import by name: 1 if the symbol is undefined in the image. */
const char *elf_sym_name(const guest_image *img, const elf_sym *s);

/* Name of the function containing `guest_addr`, or NULL. Used by the profiler;
 * returns a pointer into the image's string table (stable for the process). */
const char *elf_func_name(const guest_image *img, uint32_t guest_addr);

/* Look a defined symbol up by name. Returns its guest address with the Thumb
 * bit preserved (st_value's bit 0), or 0 if not found. The Thumb bit matters:
 * it is what tells cpu_run which instruction set to decode, and what a patch
 * uses to choose between the Thumb and ARM encodings. */
uint32_t elf_find_symbol(const guest_image *img, const char *name);

/* Nearest symbol at or below `guest_addr`, even if it is a data symbol. */
const char *elf_nearest_sym(const guest_image *img, uint32_t guest_addr,
                            uint32_t *out_addr);

#endif /* ELF_H */
