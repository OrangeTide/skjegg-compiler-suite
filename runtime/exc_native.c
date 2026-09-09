/* exc_native.c : the native host binding for libexc (host-abi.md
 * layer 2, the test-host / native-tier implementation).
 *
 * Implements the __exh_* services over plain calls (write, exit) and
 * owns the entry driver: main() spawns the bootstrap actor and sends
 * the entry verb, handing it the console, a host-native player object
 * whose tell(msg) writes the string plus a newline to stdout
 * (output.md). One address space, no persistence, no scheduler; the
 * VM-host bindings (smolmoo, boris) replace this file, not libexc.
 *
 * Cross-compiled with the m68k toolchain (freestanding, no libc) and
 * linked with start.S, which calls this main().
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "libexc.h"

extern int write(int fd, const char *buf, int n);
extern void exit(int code);

/* Channel 0 is the author channel (trace and fault text, stderr);
 * channel 1 is the player console (stdout). */
void
__exh_emit(long chan, const char *buf, long len)
{
    write(chan == 0 ? 2 : 1, buf, (int)len);
}

/* libexc has already emitted the fault report on channel 0; the native
 * binding's routing is the classic exit 70 (runtime-errors.md). */
void
__exh_fault(const struct exc_trapdesc *why, long a, long b)
{
    (void)why;
    (void)a;
    (void)b;
    exit(70);
}

/* The rest of the bound D5 surface: the native tier has no persistence,
 * no other VMs, and no scheduler, so these are honest no-ops. */
long
__exh_prop_get(long obj, const char *key, char *buf, long bufsz)
{
    (void)obj; (void)key; (void)buf; (void)bufsz;
    return -1;
}

long
__exh_prop_put(long obj, const char *key, const char *val)
{
    (void)obj; (void)key; (void)val;
    return -1;
}

long
__exh_post(long target, const char *buf, long len)
{
    (void)target; (void)buf; (void)len;
    return -1;
}

void
__exh_yield(void)
{
}

void
__exh_sleep(long ms)
{
    (void)ms;
}

/* The console: dispatched through the ordinary __exc_send path via a
 * native descriptor, so output tests exercise the real send machinery
 * end to end. Native, so it has no fields: nwords 0, no image. */
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

int
main(void)
{
    word argv[1];
    long argc = 0;
    long tell = exc_sel_by_name("tell");
    struct exc_obj *bootstrap;

    if (tell >= 0) {                /* the module knows `tell` */
        console_desc.nverbs = 1;
        console_desc.verbs[0] = tell;
        console_desc.verbs[1] = (long)console_tell;
    }
    /* the entry actor is spawned like any other, so it gets its own field
     * segment seeded from its class image */
    bootstrap = __exc_spawn(__exc_entry_class);
    __exc_self = bootstrap;
    if (__exc_entry_argc >= 1) {
        argv[0] = (word)&console_obj;
        argc = 1;
    }
    return (int)__exc_send(bootstrap, __exc_entry_selector, argc, argv);
}
