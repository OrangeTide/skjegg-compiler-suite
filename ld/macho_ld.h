/* macho_ld.h : the in-tree Mach-O static linker for the Darwin arm64 target
 * (skj-ld-aarch64).  It reads the MH_OBJECT relocatable objects skj-as-aarch64
 * writes, resolves symbols (defined across the inputs, undefined against
 * libSystem), lays out a PIE MH_EXECUTE, applies the arm64 relocations, binds
 * the dylib imports through arm64 stubs and a __got via dyld chained fixups,
 * and self-signs with an ad-hoc code signature (arm64 macOS refuses to run an
 * unsigned binary).  Linking stays hosted on libSystem; there is no
 * freestanding arm64 macOS tier.
 */

#ifndef MACHO_LD_H
#define MACHO_LD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "util.h"

/****************************************************************
 * Mach-O constants
 ****************************************************************/

#define MH_MAGIC_64        0xFEEDFACFu
#define CPU_TYPE_ARM64     0x0100000Cu
#define CPU_SUBTYPE_ARM64_ALL 0u
#define MH_OBJECT          1u
#define MH_EXECUTE         2u

#define MH_NOUNDEFS        0x1u
#define MH_DYLDLINK        0x4u
#define MH_TWOLEVEL        0x80u
#define MH_PIE             0x200000u

#define LC_REQ_DYLD        0x80000000u
#define LC_SYMTAB          0x02u
#define LC_DYSYMTAB        0x0Bu
#define LC_LOAD_DYLIB      0x0Cu
#define LC_LOAD_DYLINKER   0x0Eu
#define LC_UUID            0x1Bu
#define LC_SEGMENT_64      0x19u
#define LC_DYLD_INFO_ONLY  (0x22u | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS (0x34u | LC_REQ_DYLD)
#define LC_MAIN            (0x28u | LC_REQ_DYLD)
#define LC_BUILD_VERSION   0x32u
#define LC_CODE_SIGNATURE  0x1Du

#define PLATFORM_MACOS     1u

#define S_REGULAR               0x0u
#define S_ZEROFILL              0x1u
#define S_NON_LAZY_SYMBOL_POINTERS 0x6u
#define S_SYMBOL_STUBS          0x8u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define S_ATTR_DEBUG            0x02000000u

#define SG_READ_ONLY       0x10u   /* segment made read-only after dyld fixups */

#define VM_PROT_READ       0x1u
#define VM_PROT_WRITE      0x2u
#define VM_PROT_EXECUTE    0x4u

#define N_STAB             0xe0u
#define N_TYPE             0x0eu
#define N_EXT              0x01u
#define N_UNDF             0x00u
#define N_SECT             0x0eu

/* ARM64 relocation kinds */
#define ARM64_RELOC_UNSIGNED   0
#define ARM64_RELOC_BRANCH26   2
#define ARM64_RELOC_PAGE21     3
#define ARM64_RELOC_PAGEOFF12  4
#define ARM64_RELOC_ADDEND     10

/* classic dyld rebase/bind opcodes */
#define REBASE_TYPE_POINTER                     1
#define REBASE_OPCODE_DONE                      0x00
#define REBASE_OPCODE_SET_TYPE_IMM              0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES      0x60

#define BIND_TYPE_POINTER                       1
#define BIND_SPECIAL_DYLIB_FLAT_LOOKUP          (-2)
#define BIND_OPCODE_DONE                        0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM       0x10
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM                0x50
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OPCODE_DO_BIND                     0x90

/* dyld chained fixups (LC_DYLD_CHAINED_FIXUPS) */
#define DYLD_CHAINED_PTR_64_OFFSET  6       /* target is an image-relative offset */
#define DYLD_CHAINED_IMPORT         1       /* imports_format: dyld_chained_import */
#define DYLD_CHAINED_PTR_START_NONE 0xFFFFu /* no fixups on this page */
#define CHAINED_PAGE_SIZE           0x4000u /* 16 KiB arm64 page */

/* code signing */
#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0u
#define CSMAGIC_CODEDIRECTORY      0xfade0c02u
#define CSMAGIC_REQUIREMENTS       0xfade0c01u
#define CSSLOT_CODEDIRECTORY       0u
#define CSSLOT_REQUIREMENTS        2u
#define CS_ADHOC                   0x0000002u
#define CS_HASHTYPE_SHA256         2u
#define CS_EXECSEG_MAIN_BINARY     0x1u

/****************************************************************
 * SHA-256 (macho_sha256.c)
 ****************************************************************/

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    int nbuf;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[32]);

/****************************************************************
 * In-memory model of an input object
 ****************************************************************/

/* The placed section kinds.  TEXT/CONST/DATA/BSS are loaded; the three __DWARF
   kinds are non-loaded debug sections carried through to the executable (an
   object compiled with -g) so llvm-dwarfdump and lldb can read it.  A debug
   kind is identified by DEBUG_FIRST..; see msec_is_debug(). */
enum {
    MSEC_TEXT, MSEC_CONST, MSEC_DATA, MSEC_BSS,
    MSEC_DEBUG_LINE, MSEC_DEBUG_INFO, MSEC_DEBUG_ABBREV,
    MSEC_KIND_COUNT,
};

#define MSEC_DEBUG_FIRST MSEC_DEBUG_LINE
static inline int msec_is_debug(int k) { return k >= MSEC_DEBUG_FIRST; }

/* A relocation read from an input object, resolved against that object's
   symbols; target_sym is an index into the global symbol table. */
struct min_reloc {
    uint32_t offset;        /* within the object section */
    int kind;               /* ARM64_RELOC_* */
    int length;             /* 2 or 3 */
    int pcrel;
    int target_sym;         /* global symbol index */
    int64_t addend;
};

struct min_sec {
    int kind;               /* MSEC_*; -1 if not one we place */
    const uint8_t *data;    /* file bytes (NULL for bss) */
    uint64_t size;
    int align;              /* byte alignment */
    uint64_t out_addr;      /* assigned vm address after layout */
    struct min_reloc *relocs;
    int nrelocs;
};

struct min_obj {
    const char *path;
    uint8_t *file;
    long filelen;
    struct min_sec secs[MSEC_KIND_COUNT];
    /* object-local symbol index -> global symbol index */
    int *symmap;
    int nsyms;
};

/* A global, resolved symbol. */
struct min_gsym {
    const char *name;
    int defined;
    int is_import;          /* undefined -> bound from libSystem */
    int obj;                /* defining object index (if defined) */
    int seckind;            /* MSEC_* of its definition */
    uint64_t value;         /* final vm address (defined) */
    /* import bookkeeping */
    int stub_index;         /* -1 if none */
    int got_index;          /* -1 if none */
};

struct macho_link {
    struct arena arena;
    struct min_obj *objs;
    int nobj;
    struct min_gsym *gsyms;
    int ngsym;
    int gsym_cap;
    /* imports, in first-seen order */
    int *imports;           /* global symbol indices */
    int nimport;
};

/* macho_read.c */
void macho_read_obj(struct macho_link *ld, const char *path);
int  macho_gsym(struct macho_link *ld, const char *name);

/* macho_link.c : resolve + lay out + relocate, then macho_write.c emits. */
void macho_link_and_write(struct macho_link *ld, const char *out,
                          const char *entry);

#endif /* MACHO_LD_H */
