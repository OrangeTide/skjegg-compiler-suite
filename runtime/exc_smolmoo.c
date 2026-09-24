/* exc_smolmoo.c : the smolmoo host binding for libexc (host-abi.md
 * layer 2, the first VM-host binding).
 *
 * smolmoo (https://github.com/OrangeTide/smolmoo) is a RISC-V RV32
 * machine (rv32.c, vm_rv.ld). It runs each verb invocation in a fresh,
 * zeroed 128KB VM: code loads at 0x400, the host writes the parsed verb
 * arguments to a fixed struct at 0x380 (the struct vm_args layout
 * below), seeds sp at the top of guest RAM (0x20000) so C runs
 * immediately, enters _start, and routes fd 1 to the invoking player
 * and fd 2 to the server log.
 *
 * Hypercalls are RISC-V `ecall` under the ILP32 psABI: the syscall
 * number in a7, arguments in a0..a5, the result in a0. The sys_* stubs
 * below mirror the smolmoo SDK's own (verb_rt_rv.S) so the two stay
 * recognizably one ABI. The C caller has placed the arguments in
 * a0..a5; each stub only adds the number in a7.
 *
 * This binding implements the __exh_* services over those hypercalls,
 * provides the arena in C (there is no start.S here; the source
 * coroutine helpers come from smolmoo_rt_rv.S), and owns the entry
 * driver: _start spawns the bootstrap actor, resolves the invoked
 * verb's name against __exc_selnames so one Excelsior program can
 * carry several public verbs, thaws its fields from the host object,
 * sends it, freezes back, and exits.
 *
 * Everything above the sys_* stubs is arch-neutral C. The prior version
 * of this file targeted ColdFire (LINE_A hypercalls); the retarget to
 * RV32 ecall touched only the six stubs, the coroutine shim, and the
 * linker script, per doc/smolmoo-v1.md.
 *
 * Built with the smolmoo SDK convention: RV32 psABI, -ffreestanding,
 * linked against runtime/smolmoo_rv.ld by skj-ld-rv (make smolmoo-demo).
 * Link-only in this tree (the ELF runs under the smolmoo server, whose
 * ecall hypercalls have no Linux meaning, not qemu).
 */

#include "libexc.h"

/* ---- the smolmoo guest ABI ---- */

#define SYS_EXIT     0
#define SYS_WRITE    4
#define SYS_BROADCAST 6
#define SYS_GETPROP  7
#define SYS_SETPROP  8
#define SYS_SUSPEND 11

/* The parsed verb arguments the host writes before entry; pointers are
 * VM addresses into the host's string area. */
struct vm_args {
    int player;
    int room;
    char *argstr;
    int arglen;
    int this_obj;
    int dobj;
    int iobj;
    char *dobjstr;
    char *iobjstr;
    char *prepstr;
    char *verb;
};

/* The struct sits at fixed address 0x380; launder the constant through
 * an asm barrier so gcc does not treat it as a null-page access. */
static volatile struct vm_args *
vm_args(void)
{
    unsigned long a = 0x380;
    __asm__("" : "+r"(a));
    return (volatile struct vm_args *)a;
}

static __attribute__((noreturn)) void
sys_exit(int status)
{
    register int a0 __asm__("a0") = status;
    register int a7 __asm__("a7") = SYS_EXIT;
    __asm__ volatile("ecall" :: "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}

static int
sys_write(int fd, const void *buf, int len)
{
    register int a0 __asm__("a0") = fd;
    register const void *a1 __asm__("a1") = buf;
    register int a2 __asm__("a2") = len;
    register int a7 __asm__("a7") = SYS_WRITE;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a7)
        : "memory");
    return a0;
}

static int
sys_broadcast(int room, const char *msg)
{
    register int a0 __asm__("a0") = room;
    register const char *a1 __asm__("a1") = msg;
    register int a7 __asm__("a7") = SYS_BROADCAST;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a7)
        : "memory");
    return a0;
}

static int
sys_getprop(int obj, const char *name, char *buf, int bufsz)
{
    register int a0 __asm__("a0") = obj;
    register const char *a1 __asm__("a1") = name;
    register char *a2 __asm__("a2") = buf;
    register int a3 __asm__("a3") = bufsz;
    register int a7 __asm__("a7") = SYS_GETPROP;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
        : "memory");
    return a0;
}

static int
sys_setprop(int obj, const char *name, const char *val)
{
    register int a0 __asm__("a0") = obj;
    register const char *a1 __asm__("a1") = name;
    register const char *a2 __asm__("a2") = val;
    register int a7 __asm__("a7") = SYS_SETPROP;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a7)
        : "memory");
    return a0;
}

static int
sys_suspend(int delay_ms)
{
    register int a0 __asm__("a0") = delay_ms;
    register int a7 __asm__("a7") = SYS_SUSPEND;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a7)
        : "memory");
    return a0;
}

/* ---- the arena (libexc.h's __moo_arena_alloc, in C: no start.S) ---- */

/* Sized against the 128KB VM: program text and data plus this arena
 * plus stack headroom below 0x20000 must fit. Bounds-checked: an
 * exhausted arena returns 0, which libexc's spawn treats as nil (the
 * quota fault is memory.md's). */
#define EXC_SMOLMOO_ARENA (64 * 1024)

static char arena[EXC_SMOLMOO_ARENA] __attribute__((aligned(4)));
static unsigned arena_used;

void *
__moo_arena_alloc(int size)
{
    unsigned n = ((unsigned)size + 3u) & ~3u;
    void *p;

    if (arena_used + n > sizeof(arena))
        return 0;
    p = arena + arena_used;
    arena_used += n;
    return p;
}

void
__moo_arena_reset(void)
{
    arena_used = 0;
}

/* ---- the __exh_* binding surface (host-abi.md D5) ---- */

/* Channel 0, the author channel, is the server log (fd 2: smolmoo does
 * not route it to the player); channel 1 is the invoking player (fd 1). */
void
__exh_emit(long chan, const char *buf, long len)
{
    sys_write(chan == 0 ? 2 : 1, buf, (int)len);
}

/* libexc has already emitted the report on channel 0; exit 70 ends the
 * task (the fresh-VM model means there is no turn to roll back). */
void
__exh_fault(const struct exc_trapdesc *why, long a, long b)
{
    (void)why;
    (void)a;
    (void)b;
    sys_exit(70);
}

long
__exh_prop_get(long obj, const char *key, char *buf, long bufsz)
{
    return sys_getprop((int)obj, key, buf, (int)bufsz);
}

long
__exh_prop_put(long obj, const char *key, const char *val)
{
    return sys_setprop((int)obj, key, val);
}

long
__exh_post(long target, const char *buf, long len)
{
    (void)len;                      /* smolmoo broadcasts NUL-terminated */
    return sys_broadcast((int)target, buf);
}

void
__exh_yield(void)
{
    sys_suspend(0);
}

void
__exh_sleep(long ms)
{
    sys_suspend((int)ms);
}

/* ---- the console and the entry driver ---- */

/* The invoking player, as an in-VM native object: tell(msg) writes the
 * string plus a newline to fd 1, which smolmoo routes to that player.
 * Dispatched through the ordinary __exc_send path, like the native
 * binding's console. */
static word
console_tell(struct exc_str *msg)
{
    if (msg && msg->len)
        __exh_emit(1, msg->data, msg->len);
    __exh_emit(1, "\n", 1);
    return 0;
}

static struct {
    struct class_desc *parent;
    long nverbs;
    long nwords;
    const word *image;
    long verbs[2];
} console_desc = { 0, 0, 0, 0, { 0, 0 } };

static struct exc_obj console_obj = { (struct class_desc *)&console_desc };

void __attribute__((section(".text.entry"), noreturn))
_start(void)
{
    word argv[1];
    long argc = 0;
    long tell = exc_sel_by_name("tell");
    long sel = -1;
    struct exc_obj *bootstrap;

    if (tell >= 0) {
        console_desc.nverbs = 1;
        console_desc.verbs[0] = tell;
        console_desc.verbs[1] = (long)console_tell;
    }
    /* the invoked verb picks the selector, so one program serves several
     * verb slots; an unknown or absent name falls back to the entry verb */
    if (vm_args()->verb)
        sel = exc_sel_by_name((const char *)vm_args()->verb);
    if (sel < 0)
        sel = __exc_entry_selector;

    /* thaw-run-freeze (host-abi.md D6/D7): the actor's fields live in
     * the verb's owning host object between invocations, so the fresh
     * VM is a stateless executor and the smolmoo object is the actor */
    bootstrap = __exc_spawn(__exc_entry_class);
    __exc_self = bootstrap;
    exc_thaw(bootstrap, vm_args()->this_obj);
    if (__exc_entry_argc >= 1) {
        argv[0] = (word)&console_obj;
        argc = 1;
    }
    {
        int status = (int)__exc_send(bootstrap, sel, argc, argv);
        exc_freeze(bootstrap, vm_args()->this_obj);
        sys_exit(status);
    }
}
