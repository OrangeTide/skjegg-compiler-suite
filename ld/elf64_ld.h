/* elf64_ld.h : shared ELF64 little-endian static linker (x86-64 and AArch64).
 *
 * Both targets are ELFCLASS64 + ELFDATA2LSB and differ only in the machine id
 * and the relocation set, so they share the ELF64 reader (elf64_read.c), the
 * ELF64 writer (elf64_write.c) and the arch-neutral layout core from ld.h
 * (ld_layout / ld_find_output_sec / ld_build_debug / ld_check_undefined /
 * ld_write_map / ld_free) plus the script parser and mapfile reader.  Each
 * target supplies its own relocation application (x86_64_link.c /
 * aarch64_link.c) and its built-in default script.
 *
 * Executable virtual addresses are kept in the low 4 GiB, so the uint32_t
 * layout core is reused unchanged; only the on-disk encoding is 64-bit.  A
 * value that does not fit is a link error, not a silent truncation. */

#ifndef ELF64_LD_H
#define ELF64_LD_H

#include "ld.h"

#define ELFCLASS64      2
#define ELFDATA2LSB     1

#define EM_X86_64       62
#define EM_AARCH64      183

/* x86-64 relocations emitted by skj-as-x86-64 */
enum {
    R_X86_64_NONE  = 0,
    R_X86_64_64    = 1,     /* S + A, 8 bytes */
    R_X86_64_PC32  = 2,     /* S + A - P, 4 bytes */
    R_X86_64_32    = 10,    /* S + A, 4 bytes zero-extended */
    R_X86_64_32S   = 11,    /* S + A, 4 bytes sign-extended */
    R_X86_64_PLT32 = 4,     /* L + A - P; no PLT in a static link, so == PC32 */
};

/* AArch64 relocations emitted by skj-as-aarch64 */
enum {
    R_AARCH64_NONE            = 0,
    R_AARCH64_ABS64           = 257,   /* S + A, 8 bytes */
    R_AARCH64_ABS32           = 258,   /* S + A, 4 bytes */
    R_AARCH64_PREL32          = 261,   /* S + A - P, 4 bytes */
    R_AARCH64_ADR_PREL_LO21   = 274,   /* ADR: (S+A-P)[20:0] */
    R_AARCH64_ADR_PREL_PG_HI21 = 275,  /* ADRP: page(S+A) - page(P), [32:12] */
    R_AARCH64_ADD_ABS_LO12_NC = 277,   /* ADD: (S+A)[11:0] */
    R_AARCH64_CONDBR19        = 280,   /* B.cond/CBZ: (S+A-P)>>2, 19 bits */
    R_AARCH64_JUMP26          = 282,   /* B: (S+A-P)>>2, 26 bits */
    R_AARCH64_CALL26          = 283,   /* BL: (S+A-P)>>2, 26 bits */
};

/* ELF64 symbol/reloc field accessors */
#define ELF64_ST_BIND(i)    ((i) >> 4)
#define ELF64_ST_TYPE(i)    ((i) & 0xF)
#define ELF64_ST_INFO(b,t)  (((b) << 4) | ((t) & 0xF))
#define ELF64_R_SYM(i)      ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)     ((uint32_t)((i) & 0xffffffffULL))

/* elf64_read.c: parse an ELFCLASS64 LE relocatable object, validating `machine`
 * (EM_X86_64 / EM_AARCH64).  Shared by a mapped file and an archive member. */
int elf64_parse(struct arena *a, struct ld_object *obj, const uint8_t *buf,
                size_t fsize, const char *path, int machine);
int elf64_read_object(struct arena *a, struct ld_object *obj, const char *path,
                      int machine);

/* elf64_write.c: emit an ELFCLASS64 LE executable with machine id `machine`. */
int elf64_write_exec(struct linker *ld, const char *path, int machine);

/* elf64_archive.c: ar(1) archive support, shared across the ELF64 targets.  The
 * member reader validates each pulled member against `machine`, like the file
 * reader.  Members are pulled only to resolve a still-undefined symbol. */
int elf64_is_archive(const char *path);
int elf64_read_archive(struct linker *ld, const char *path, int machine);
void elf64_archive_reset(void);

/* x86_64_link.c */
int x86_64_ld_link(struct linker *ld);
void x86_64_default_script(struct arena *a, struct ld_script *script);

/* aarch64_link.c */
int aarch64_ld_link(struct linker *ld);
void aarch64_default_script(struct arena *a, struct ld_script *script);

#endif /* ELF64_LD_H */
