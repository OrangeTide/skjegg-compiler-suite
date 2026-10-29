/* elf64.h : ELF64 loader for the hardware-virtualization guests */

#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>
#include "guest.h"
#include "elf32.h"       /* elf32_info, reused: HV guests live under 4 GiB */

#define EM_X86_64   62
#define EM_AARCH64  183

/* Read the header only, filling info (entry/machine/endianness).  Returns 0 on
 * success, -1 with a message on stderr otherwise. */
int elf64_probe(const char *path, elf32_info *info);

/* Load every PT_LOAD segment into g's memory by p_vaddr.  The guest image lies
 * under 4 GiB, so the 32-bit fields of elf32_info address it.  Returns 0 on
 * success, -1 on failure. */
int elf64_load(guest *g, const char *path, elf32_info *info);

/* The same against an in-memory image the caller owns. */
int elf64_probe_mem(const uint8_t *buf, long size, elf32_info *info);
int elf64_load_mem(guest *g, const uint8_t *buf, long size, elf32_info *info,
                   const char *what);

#endif /* ELF64_H */
