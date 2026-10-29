/* guest.h : sparse guest memory and the user-mode machine shared by the
 * ColdFire and RV32 emulators */

#ifndef GUEST_H
#define GUEST_H

#include <stdint.h>

/****************************************************************
 * Sparse guest memory
 *
 * The guest address space is 32 bits wide but a program touches very
 * little of it, so pages are committed on first access inside a mapped
 * region.  A read or write outside every region is a fault, which is
 * what a host kernel would answer with SIGSEGV.
 ****************************************************************/

#define GM_PAGE_SHIFT   12
#define GM_PAGE_SIZE    (1u << GM_PAGE_SHIFT)
#define GM_L1_ENTRIES   1024            /* addr >> 22 */
#define GM_L2_ENTRIES   1024            /* (addr >> 12) & 1023 */
#define GM_MAX_REGIONS  256             /* image segments plus dynamic mappings */

enum {
    GM_R = 1,
    GM_W = 2,
    GM_X = 4,
};

struct gm_region {
    uint32_t base;
    uint32_t size;
    int prot;
    const char *name;
};

/* An aggregate commit budget shared by several guests (the scheduler's
 * processes). Each guest still has its own per-guest limit; the pool caps the
 * total committed across all of them, so many children cannot together exhaust
 * host memory. limit 0 means no aggregate cap. */
struct gm_pool {
    uint64_t used;
    uint64_t limit;
};

typedef struct guest_mem {
    uint8_t **dir[GM_L1_ENTRIES];       /* lazily allocated leaf tables */
    uint8_t **wdir[GM_L1_ENTRIES];      /* write fast path: mirrors dir, set only
                                           for committed pages in a writable region
                                           (steady state), so a store skips the
                                           per-access region protection re-check */
    struct gm_region regions[GM_MAX_REGIONS];
    int nregions;
    struct gm_region *last;             /* one-entry lookup cache */
    uint32_t mmap_next;                 /* next free address for gm_mmap, 0 until first use */
    uint64_t committed;                 /* bytes of host memory handed out */
    uint64_t limit;                     /* cap on this guest's committed */
    struct gm_pool *pool;               /* shared aggregate budget, or NULL */
    int loading;                        /* loader writes ignore GM_W */
    uint8_t *extern_base;               /* a contiguous host buffer backing
                                           [0, extern_size), used by the
                                           hardware-virtualization guest whose RAM
                                           is one buffer mapped into its vcpu;
                                           NULL for a sparse (page-committed) guest */
    uint32_t extern_size;
    int fault;                          /* set on the first bad access */
    uint32_t fault_addr;
    int fault_write;
    const char *fault_why;
    int fault_guard;                    /* the fault hit a guard page (a stack overflow) */
} guest_mem;

void gm_init(guest_mem *m, uint64_t limit);
void gm_free(guest_mem *m);

/* Back [0, size) with a single contiguous host buffer rather than committed
 * pages, and map it read/write/execute.  The buffer is the guest's whole RAM,
 * shared with a vcpu (the hardware-virtualization cores); its lifetime is the
 * caller's.  Every gm_* access in range then reads or writes this buffer, so
 * the shared syscall layer reaches guest memory unchanged. */
void gm_set_extern(guest_mem *m, uint8_t *base, uint32_t size);

/* Declare an accessible range.  Returns 0 on success, -1 if the region
 * table is full. */
int gm_map(guest_mem *m, uint32_t base, uint32_t size, int prot,
           const char *name);

/* Page containing addr, committing it if it lies in a mapped region.
 * Returns NULL and records a fault otherwise. */
uint8_t *gm_page(guest_mem *m, uint32_t addr, int want_write);

/* Map a fresh region of `size` bytes (rounded up to pages) at an
 * emulator-chosen address, readable and writable, and return its base (0 on
 * failure). With GM_MAP_GUARD_LO an unmapped guard page sits just below it, so a
 * downward-growing stack that overruns faults on the guard rather than
 * corrupting other memory. This is the one mapping primitive the sandbox tier
 * uses for guarded coroutine stacks (and, later, host-shared buffers). */
#define GM_MAP_GUARD_LO 1u
uint32_t gm_mmap(guest_mem *m, uint32_t size, unsigned flags);

/* Release a region gm_mmap returned, freeing its committed pages and its guard
 * page. base and size are the values gm_mmap returned and was asked for. */
void gm_munmap(guest_mem *m, uint32_t base, uint32_t size);

/* Byte access.  A fault reads as zero and swallows the write. */
uint8_t gm_read8(guest_mem *m, uint32_t addr);
void gm_write8(guest_mem *m, uint32_t addr, uint8_t val);

/* Sized access in the guest's byte order; size is 1, 2 or 4.  These are
 * what the CPU bus callbacks are built from. */
uint32_t gm_load(guest_mem *m, uint32_t addr, int size, int be);
void gm_store(guest_mem *m, uint32_t addr, int size, int be, uint32_t val);

/* Copy a host buffer in, committing pages as needed (loader path). */
void gm_write(guest_mem *m, uint32_t addr, const void *src, uint32_t len);

/* Zero a range without faulting on unmapped pages inside a region. */
void gm_zero(guest_mem *m, uint32_t addr, uint32_t len);

/* Copy a guest range out to a host buffer.  A byte in an unmapped page reads
 * as zero (gm_read8), so the caller checks the region is mapped when that
 * matters. */
void gm_read_out(guest_mem *m, uint32_t addr, void *dst, uint32_t len);

/****************************************************************
 * The user-mode machine
 *
 * One structure serves both CPUs.  The syscall layer is arch-neutral:
 * each front end decodes its own register convention and number space
 * into a call on guest_syscall().
 ****************************************************************/

enum guest_arch {
    GUEST_ARCH_NONE = 0,
    GUEST_ARCH_COLDFIRE,
    GUEST_ARCH_RV32,
    GUEST_ARCH_MIPS32,
    GUEST_ARCH_X86_64,          /* hardware-virtualization (KVM) */
    GUEST_ARCH_AARCH64,         /* hardware-virtualization (KVM) */
};

/* Arch-neutral syscall selectors.  The per-arch tables map Linux numbers
 * onto these, so a call added here reaches both targets at once. */
enum guest_sys {
    GSYS_UNKNOWN = 0,
    GSYS_READ,
    GSYS_WRITE,
    GSYS_WRITEV,
    GSYS_OPEN,
    GSYS_OPENAT,
    GSYS_CLOSE,
    GSYS_LSEEK,
    GSYS_LLSEEK,
    GSYS_BRK,
    GSYS_EXIT,
    GSYS_EXIT_GROUP,
    GSYS_STATX,
    GSYS_MKDIRAT,
    GSYS_UNLINKAT,
    GSYS_RENAMEAT2,
    GSYS_FCHMODAT,
    GSYS_GETDENTS64,
    GSYS_CLOCK_GETTIME,
    GSYS_GETRANDOM,
    GSYS_NANOSLEEP,
    GSYS_SCHED_YIELD,
};

/* The sandbox fd table.  A guest fd is an opaque INDEX into this table; the
 * host fd lives only in the entry and is never handed to the guest, so a guest
 * cannot name a host fd it was not given.  Each entry routes to a device
 * driver; its param is the host fd.  (Only console and hostfs exist today, both
 * backed by a host fd; the tag is there for ramfs / 9p / null drivers later.) */
#define GUEST_MAX_FDS 64

enum guest_dev {
    GDEV_NONE = 0,
    GDEV_CONSOLE,               /* 0/1/2 */
    GDEV_HOSTFS,                /* param is a host fd (file or directory) */
};

struct gfd {
    int used;
    int dev;                    /* enum guest_dev */
    int host_fd;                /* the driver's parameter */
    void *dirp;                 /* a cached host directory stream (hp_dir *) for
                                 * getdents64, opened on first use; NULL until then */
};

/* A 9p channel the launcher bound: a doorbell names its channel's guest address,
 * which this maps to the host-side p9 server serving it. */
#define GUEST_MAX_P9 16
struct guest_p9bind {
    uint32_t chan;              /* channel region's guest address */
    void    *srv;              /* struct p9_server * serving it */
};

typedef struct guest {
    guest_mem mem;
    int arch;
    int exited;
    int exit_code;
    int quiet;                  /* suppress guest stdout/stderr */
    uint64_t syscalls;
    uint32_t brk;               /* current program break */
    uint32_t brk_base;          /* start of the heap region */
    uint32_t brk_limit;         /* end of the heap region */
    uint32_t stack_top;
    int big_endian;
    int fds_inited;
    struct gfd fdtab[GUEST_MAX_FDS];
    int np9;                    /* bound 9p channels */
    struct guest_p9bind p9[GUEST_MAX_P9];
} guest;

/* Service a 9p doorbell: find the server bound to channel `chan`, read the
 * T-message out of guest memory, dispatch it, and write the R-message back.
 * Returns 0, or -1 if no server is bound to that channel. */
int guest_p9_doorbell(guest *g, uint32_t chan);

/* Release the 9p servers bound to this guest (called at process teardown). */
void guest_p9_free(guest *g);

/* Service one syscall.  args are already in host order; the return value
 * is the Linux return convention (negative errno on failure). */
int32_t guest_syscall(guest *g, int sys, uint32_t a0, uint32_t a1,
                      uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

/****************************************************************
 * Run options, filled by the driver and read by the arch front ends
 ****************************************************************/

#define RUN_MAX_MOUNTS 16       /* matches fs/fs.h FS_MAX_MOUNTS */

typedef struct run_opts {
    const char *path;
    char **argv;                /* guest argv, argv[0] is the program */
    int argc;
    const char *mounts[RUN_MAX_MOUNTS]; /* --mount name:hostdir grants */
    int nmounts;
    const char *devs[RUN_MAX_MOUNTS];   /* --dev name grants (devfs)        */
    int ndevs;
    const char *roms[RUN_MAX_MOUNTS];   /* --rom name:image.img grants      */
    int nroms;
    const char *p9s[RUN_MAX_MOUNTS];    /* --9p name:hostdir grants         */
    int np9s;
    uint64_t mem_limit;
    uint32_t stack_size;
    uint32_t heap_size;
    uint64_t max_insns;         /* 0 = unlimited */
    uint64_t total_mem;         /* aggregate commit cap across all processes, 0 = unlimited */
    uint64_t child_max_insns;   /* per-spawned-child instruction budget, 0 = unlimited */
    int trace;                  /* dump the CPU trace ring on a fault */
    int stats;                  /* instruction count to stderr on exit */
    int quiet;
    int gdb_port;               /* >0: serve the GDB remote protocol on this port (RV32) */
} run_opts;

/* The launcher: open each --mount host directory as a hostfs capability in the
 * guest fd table and write the fs/ mount table (the fs_mounts / fs_mount_count /
 * fs_cur_mount symbols) into guest memory.  A no-op if the guest links no fs/
 * table or no mounts were granted. */
void guest_install_mounts(guest *g, const char *elf_path, const run_opts *o);

/* Each front end loads the ELF, builds the machine, runs it, and returns
 * the process exit status the driver should adopt. */
int run_coldfire(const run_opts *o, int *status);
int run_rv32(const run_opts *o, int *status);
int run_mips32(const run_opts *o, int *status);
int run_x86_64hv(const run_opts *o, int *status);
int run_arm64hv(const run_opts *o, int *status);

/* Shared stack image: writes argc/argv/envp/auxv and returns the stack
 * pointer the guest should start with.  The _64 variant uses 8-byte slots for
 * the LP64 hardware-virtualization guests. */
uint32_t guest_build_stack(guest *g, const run_opts *o);
uint32_t guest_build_stack64(guest *g, const run_opts *o);

#endif /* GUEST_H */
