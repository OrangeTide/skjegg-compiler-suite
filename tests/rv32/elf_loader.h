/* elf_loader.h : the loader interface the rv32 harnesses expect,
 * answered by the tree's own loader.
 *
 * Upstream ships an elf_loader.c beside these harnesses. Skjegg already
 * has that walk in emu/elf32.c, where skj-run uses it, so this maps one
 * onto the other rather than carrying a second ELF reader. Keeping the
 * shape identical is what lets every file in this directory stay byte
 * for byte what the articles describe, so a later re-sync is a copy.
 *
 * made by a machine. PUBLIC DOMAIN
 */

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "elf32.h"

typedef elf32_poke_fn elf_write_fn;

static inline uint32_t
elf_load(const char *path, elf32_poke_fn write, void *ctx)
{
    elf32_info info;

    return elf32_load_raw(path, write, ctx, &info);
}

static inline uint32_t
elf_symbol(const char *path, const char *name)
{
    return elf32_symbol(path, name);
}

#endif /* ELF_LOADER_H */
