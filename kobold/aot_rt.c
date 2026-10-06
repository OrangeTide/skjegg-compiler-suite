/* aot_rt.c : the C start file and link-time runtime surface for skj-kp output.

   An AOT-compiled Kobold program (the assembly skj-kp emits) is a set of guest
   functions that call the Kobold runtime by symbol: the string, array, map,
   coroutine, and ARC routines (str_core.c, dynarr.c, map.c, co.c, co_shim.c,
   rt_host.c) and the console I/O (kp_host.c), all linked in directly rather than
   bound through a table as in the JIT.  This file supplies the rest the link needs:
   the process entry, the trap handlers and the kprun-compatible debug surface under
   the exact names the guest calls, since those are the ones the JIT's binding table
   renamed rather than provided verbatim.

   The entry sets up a low-memory execution stack (a guest address is a 32-bit
   value, so the stack must live below 4GB, matching the JIT's execution stack and
   the low-memory guest heap in rt_host.c) and enters the program through
   kp_call_guest, exactly as the JIT driver does.  The program is linked -no-pie so
   its code and data also sit below 4GB. */

#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

/* The renamed guest entry (skj-kp emits the program's entry under this name). */
extern void kp_guest_entry(void);

/* From jit/call_guest_x86.S and kobold/co_shim.c. */
uint64_t kp_call_guest(void *entry, void *stack_top,
                       const int64_t *iargs, const double *fargs, double *fret);
void kp_handle_sweep(void);

/* From kobold/kp_host.c. */
char *kp_read_file(const char *path, size_t *lenp);
int kp_write_file(const char *path, const char *data, size_t len);
void kp_io_set_args(int argc, char **argv);

/****************************************************************
 * Traps (the guest calls these on a failed runtime check)
 ****************************************************************/

static void aot_trap(const char *what)
{
    fprintf(stderr, "skj-kp: runtime fault: %s\n", what);
    exit(70);
}
void kp_trap_div_zero(void) { aot_trap("divide by zero"); }
void kp_trap_overflow(void) { aot_trap("integer overflow"); }
void kp_trap_index(void)    { aot_trap("array index out of bounds"); }

/****************************************************************
 * The kprun-compatible debug/test surface (mirrors jit_main.c)
 ****************************************************************/

void printInt(int n) { printf("%d\n", n); }
void emit(int n)     { printf("%d\n", n); }
void emit64(int64_t n) { printf("%lld\n", (long long)n); }

/* emitf takes a real.  On rv32 the backend's soft-float call ABI hands it over as
   the double's bits in an integer register pair, not in a float register, so it
   arrives as a uint64_t (mirrors jit_main.c:h_emitf). */
#if defined(__riscv) && __riscv_xlen == 32
void emitf(uint64_t xb) { double x; memcpy(&x, &xb, sizeof x); printf("%g\n", x); }
#else
void emitf(double x) { printf("%g\n", x); }
#endif

uint32_t ReadFileStr(uint32_t pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    char *buf;
    size_t n = 0;
    uint32_t s;

    if (plen < 0 || plen >= (int)sizeof(path))
        return kp_str_new("", 0);
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    buf = kp_read_file(path, &n);
    s = kp_str_new(buf ? buf : "", (int)n);
    free(buf);
    return s;
}

/* WriteFileStr(path, data): write a guest string to a host file named by another
   guest string, returning 1 on success and 0 on failure.  The front end's `-o`
   uses it to write the KPM1 module without a shell redirect. */
int WriteFileStr(uint32_t pathstr, uint32_t datastr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);

    if (plen < 0 || plen >= (int)sizeof(path))
        return 0;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return kp_write_file(path, kp_str_bytes(datastr), (size_t)kp_str_len(datastr));
}

/****************************************************************
 * Process entry
 ****************************************************************/

#define KP_STACK_BYTES ((size_t)8u << 20)   /* 8 MiB guest execution stack */

/* Map the execution stack below 4 GB, so an address-of-local fits the 32-bit guest
   address model.  Mirrors rt_host.c:rt_map_low (x86-64 MAP_32BIT; a 32-bit host is
   low already; arm64 probes fixed low hints), with its own hint base so it does not
   fight the guest heap's probe region. */
static void *aot_map_low(size_t len)
{
#if defined(MAP_32BIT)
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#elif UINTPTR_MAX <= 0xffffffffULL
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    static uintptr_t hint = 0x30000000;
    while (hint + len <= 0x100000000ULL) {
        uintptr_t at = hint;
        void *p = mmap((void *)at, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        hint += (len + 0xffffffUL) & ~(uintptr_t)0xffffff;   /* advance 16MB */
        if (p != MAP_FAILED && (uintptr_t)p == at)
            return p;
        if (p != MAP_FAILED)
            munmap(p, len);
    }
    return MAP_FAILED;
#endif
}

/* ISO C does not define a function-pointer-to-void* cast; route it through a union
   to stay warning-clean under -Wpedantic, as the JIT drivers do. */
static void *entry_addr(void (*fn)(void))
{
    union { void (*f)(void); void *o; } u;
    u.f = fn;
    return u.o;
}

int main(int argc, char **argv)
{
    void *stack;
    uintptr_t top;
    int64_t iargs[6] = { 0 };
    double fargs[8] = { 0 };
    double fret = 0;

    /* Record the command line for ParamCount / ParamStr.  kp_io_set_args drops its
       first element, which in jit_main.c is the JIT launcher's own name (so the
       module name it is given leads the guest's view).  A standalone AOT program is
       different: its argv[0] is the program's own name, which Pascal's ParamStr(0)
       must keep.  So prepend a placeholder for the setter to drop, leaving argv[0]
       as ParamStr(0) and argv[1..] as ParamStr(1..ParamCount). */
    {
        char **gargv = malloc((size_t)(argc + 1) * sizeof *gargv);
        if (!gargv) {
            fprintf(stderr, "skj-kp: cannot allocate argument vector\n");
            return 70;
        }
        gargv[0] = argv[0];                 /* placeholder; dropped by the setter */
        for (int i = 0; i < argc; i++)
            gargv[i + 1] = argv[i];
        kp_io_set_args(argc + 1, gargv);
    }

    /* A low-memory execution stack so an address-of-local fits the 32-bit guest
       address model, as the JIT's execution stack does. */
    stack = aot_map_low(KP_STACK_BYTES);
    if (stack == MAP_FAILED) {
        fprintf(stderr, "skj-kp: cannot map execution stack\n");
        return 70;
    }
    top = ((uintptr_t)stack + KP_STACK_BYTES) & ~(uintptr_t)15;

    kp_call_guest(entry_addr(kp_guest_entry), (void *)top, iargs, fargs, &fret);

    kp_handle_sweep();   /* finalize any live handle at teardown */
    return 0;
}
