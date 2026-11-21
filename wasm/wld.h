/* wld.h : the in-tree WebAssembly linker (skj-ld-wasm).

   It reads the relocatable objects skj-cc-wasm emits (standard WASM objects,
   tool-conventions Linking.md), resolves symbols across them, renumbers the
   type, function, and code sections, applies the relocations, and writes a
   complete runnable module.  It shares the wasm/enc.c binary primitives with the
   backend.  This covers the function, stack-pointer, data, and indirect-table
   links, and synthesizes the WASI runtime (_start plus the write/read/exit
   seam) for an entry link. */

#ifndef WASM_WLD_H
#define WASM_WLD_H

#include <stddef.h>
#include <stdint.h>

#include "enc.h"

#define WLD_MAXP 64

/* a function type (valtypes kept as their raw binary bytes for easy dedup) */
struct wtype {
    int           np;
    unsigned char p[WLD_MAXP];
    int           nr;               /* 0 or 1 results */
    unsigned char r;
};

/* a linking symbol-table entry */
struct wsymbol {
    int   kind;                     /* 0 function, 1 data, 2 global */
    int   flags;
    char *name;                     /* always set (imports take the import field) */
    int   index;                    /* function or global index within the object */
    int   seg, off, size;           /* data symbols */
};

/* a relocation in reloc.CODE or reloc.DATA */
struct wreloc {
    unsigned char type;
    size_t        off;              /* relative to the target section's contents */
    int           index;            /* symbol index, or a type index for TYPE_INDEX */
    long          addend;
};

/* one parsed relocatable object */
struct wobj {
    const char    *path;
    unsigned char *raw;
    size_t         rawlen;

    struct wtype  *types;
    int            ntypes;

    int            nfuncimports;    /* imported functions occupy the low indices */
    char         **funcimport_name;
    char         **funcimport_module;  /* "env", or a WASI module for a shim import */
    int           *funcimport_type;    /* object-local type index of each import */
    int            nglobimports;    /* imported globals (for the symbol names) */
    char         **globimport_name;

    int           *func_type;       /* [ndeffns] object-local type index */
    int            ndeffns;

    unsigned char *code;            /* the Code section payload (mutable copy) */
    size_t         codelen;
    size_t         code_entries_off;/* where the function entries begin (past count) */
    int            code_count;

    unsigned char *data;            /* the one data segment's bytes (mutable copy) */
    size_t         datalen;
    int            has_data;
    int            data_p2align;    /* from the segment-info subsection */
    size_t         data_bytes_off;  /* where the bytes begin within the Data contents */

    struct wsymbol *syms;
    int             nsyms;
    struct wreloc  *crelocs;        /* reloc.CODE */
    int             ncrelocs;
    struct wreloc  *drelocs;        /* reloc.DATA */
    int             ndrelocs;

    int             data_base;      /* assigned by the linker: this object's data base */
};

/* WASM linking relocation types (the subset the linker applies) */
#define R_WASM_FUNCTION_INDEX_LEB 0
#define R_WASM_TABLE_INDEX_SLEB   1
#define R_WASM_TABLE_INDEX_I32    2
#define R_WASM_MEMORY_ADDR_LEB    3
#define R_WASM_MEMORY_ADDR_SLEB   4
#define R_WASM_MEMORY_ADDR_I32    5
#define R_WASM_TYPE_INDEX_LEB     6
#define R_WASM_GLOBAL_INDEX_LEB   7

/* symbol flags */
#define WASM_SYM_UNDEFINED     0x10
#define WASM_SYM_EXPLICIT_NAME 0x40

/* symbol kinds */
#define WASM_SYMTAB_FUNCTION 0
#define WASM_SYMTAB_DATA     1
#define WASM_SYMTAB_GLOBAL   2

/* the shadow stack, matching the backend's layout */
#define WLD_DATA_BASE   1024
#define WLD_STACK_BYTES (1 << 20)

struct wobj *wobj_read(const char *path);

/* link the objects into a complete module written to `outpath`; exports lists
   the extra function names to export.  When `entry` is set the linker
   synthesizes the WASI runtime (the write/read/_exit/exit seam and a _start
   that calls main), imports the three preview1 functions, and exports _start;
   with entry clear it links a plain reactor module with no runtime. */
void wlink(struct wobj **objs, int nobj, const char **exports, int nexports,
           int entry, const char *outpath);

#endif /* WASM_WLD_H */
