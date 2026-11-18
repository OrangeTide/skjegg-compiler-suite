/* co_free.c : the freestanding coroutine runtime for a Kobold guest.

   The hosted tier keeps coroutines in a host handle table with mmap stacks, a
   guard page, and a SIGSEGV handler that turns an overflow into a recoverable
   trap (co.c + co_shim.c).  This tier has none of that: a coroutine is an
   ordinary gheap object (kind KP_OBJ_CORO), counted by rt_free.c like any other
   managed value, and its private stack is a gheap block rather than a guarded
   mapping (the plan's accepted trade: no mmap, so no guard page).  Every trap on
   this tier is fatal, so there is no landing pad; that collapses the resume path
   to a bare save-switch-restore of the running-coroutine pointer.

   The context switch itself is guest code (co_switch, the per-target
   co_switch_*.S), whose saved register block this file seeds; the block size and
   the return-address position are target-specific (see kp_co_new_g).  The entry
   and value ABI match what the backend emits: kp_co_new(entry, argsp, nargs) with
   8-byte argument cells, a 32-bit resume/yield value, and the body entry taking
   (vm, argsp, nargs).  The guest reaches these through the _g names the backend's
   remap gives the coroutine operators. */

#include "obj.h"

#include <stdint.h>
#include <stddef.h>

typedef int32_t  i32;
typedef uint32_t u32;

/* From rt_free.c (the guest heap, the finalizer registry, the fatal trap) and
   io_free.c (the trap kinds share the exit path). */
u32  gheap_alloc(u32 nbytes);
u32  gheap_realloc(u32 addr, u32 nbytes);
void gheap_free(u32 addr);
void rt_register_final(u32 kind, void (*fn)(u32 addr));
void rt_trap(u32 kind);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

/* One saved stack pointer, the layout co_switch reads at offset 0. */
struct co_ctx {
    u32 sp;
};

/* void co_switch(struct co_ctx *from, struct co_ctx *to): save the callee-saved
   registers, stash sp into *from, load sp from *to, restore its registers, and
   return onto the switched stack (co_switch_rv.S). */
extern void co_switch(struct co_ctx *from, struct co_ctx *to);

/* The coroutine body ABI skj-kpc-rv emits: entry(vm, argsp, nargs).  The image
   passes no VM, so a0 is ignored; a1 is the guest address of the 8-byte-cell seed
   buffer, a2 the count.  The completion value returns in a0. */
typedef i32 (*co_entry)(u32 vm, u32 argsp, i32 nargs);

enum co_state {
    CO_SUSPENDED,
    CO_RUNNING,
    CO_DONE,
};

struct coro {
    u32           refcount;
    u32           kind;             /* KP_OBJ_CORO */
    struct co_ctx ctx;              /* saved sp while suspended */
    struct co_ctx *resumer;         /* context to switch back to on yield/completion */
    u32           stack;            /* the private stack block, owned (gheap) */
    u32           stack_size;
    u32           entry;            /* the body routine's code address */
    u32           argbuf;           /* guest address of the seed-argument copy, or 0 */
    i32           nargs;
    i32           xfer;             /* value carried across a switch, both directions */
    u32           state;
    u32           started;          /* resumed at least once, so it has live frames */
    u32           cancelling;       /* being torn down: yields do not suspend */
    u32           dead;             /* last reference went while running */
    u32           weakslot;         /* 1-based index into the weak registry, or 0
                                       if no weak reference has been taken (see the
                                       weak-reference registry below) */
};

#define CO_STACK_SIZE  (64u * 1024u)

/* The running coroutine, or 0 on the main stack.  Single VM, single thread. */
static u32 current;

static void co_final(u32 co);
static void co_reclaim(u32 co);
static void co_weak_invalidate(struct coro *co);

static struct coro *
coro_ptr(u32 co)
{
    return (struct coro *)(uintptr_t)co;
}

/* The first thing a fresh coroutine runs: call the body, record its result,
   mark done, and switch back to the resumer.  It never returns. */
static void
co_trampoline(void)
{
    struct coro *co = coro_ptr(current);

    co->xfer = ((co_entry)(uintptr_t)co->entry)(0, co->argbuf, co->nargs);
    co->state = CO_DONE;
    co_switch(&co->ctx, co->resumer);
}

/* Switch into a coroutine and back, saving the running-coroutine pointer across
   the switch so nested resumes on one stack still work. */
static void
co_run(u32 co, struct co_ctx *resumer)
{
    u32 prev = current;

    current = co;
    co_switch(resumer, &coro_ptr(co)->ctx);   /* returns on yield or completion */
    current = prev;
}

u32
kp_co_new_g(u32 entry, u32 argsp, i32 nargs)
{
    u32 a = gheap_alloc((u32)sizeof(struct coro));
    struct coro *co = coro_ptr(a);
    uint8_t *top;
    u32 *base;
    u32 reserve, ra_off;

    rt_register_final(KP_OBJ_CORO, co_final);   /* idempotent */
    memset(co, 0, sizeof *co);
    co->refcount = 1;
    co->kind = KP_OBJ_CORO;
    co->entry = entry;
    co->nargs = nargs;
    co->state = CO_SUSPENDED;

    /* The private stack is a gheap block, not a guarded mapping: this tier has no
       mmap, so a body that overruns its stack corrupts the heap rather than
       faulting on a guard.  64 KiB is generous for the coroutine bodies a verb
       runs. */
    co->stack = gheap_alloc(CO_STACK_SIZE);
    co->stack_size = CO_STACK_SIZE;
    if (co->stack == 0)
        rt_trap(0);                 /* no room for a stack: fatal */

    /* The stack grows down from a 16-aligned top. */
    top = (uint8_t *)(uintptr_t)(co->stack + CO_STACK_SIZE);
    top = (uint8_t *)((uintptr_t)top & ~(uintptr_t)15);

    /* Copy the seed arguments to the top of the private stack so they outlive the
       caller's frame (the body runs lazily).  8-byte cells, as skj-kpc-rv stores
       them; a 16-byte multiple keeps sp aligned. */
    if (nargs > 0) {
        u32 resv = ((u32)nargs * 8u + 15u) & ~15u;

        top -= resv;
        memcpy(top, (const void *)(uintptr_t)argsp, (u32)nargs * 8u);
        co->argbuf = (u32)(uintptr_t)top;
    }

    /* Seed the switch block so the first co_switch restores a zeroed callee-saved
       set and lands in the trampoline, leaving sp back at the 16-aligned entry
       top.  The block size and where the return address sits differ per target,
       matching that target's co_switch (co_switch_*.S):
         RV32  a 160-byte block, ra restored from word 0 into the ra register;
         MIPS  a 96-byte block, ra restored from offset 36 into $ra;
         CF    a 96-byte block, ra sitting just above it at offset 96, popped by
               the rts after the block (so the reserve is 100).
       The one shared 4-aligned access is reading this zeroed block on the first
       switch into a fresh coroutine; its restored fp slots are dead (the body
       sets them before use), and sp is 16-aligned for every switch thereafter. */
#if defined(__mcoldfire__) || defined(__skj_m68k__)
    reserve = 100; ra_off = 96;
#elif defined(__mips__) || defined(__skj_mips__)
    reserve = 96;  ra_off = 36;
#else /* __riscv */
    reserve = 160; ra_off = 0;
#endif
    top -= reserve;
    base = (u32 *)top;
    memset(base, 0, reserve);
    base[ra_off / 4] = (u32)(uintptr_t)&co_trampoline;
    co->ctx.sp = (u32)(uintptr_t)base;

    return a;
}

i32
kp_co_resume_g(u32 co_addr, i32 value)
{
    struct coro *co = co_addr ? coro_ptr(co_addr) : 0;
    struct co_ctx resumer;
    i32 result;

    if (!co || co->kind != KP_OBJ_CORO || co->state != CO_SUSPENDED)
        rt_trap(0);                 /* resumed a running, completed, or nil coroutine */

    co->xfer = value;
    co->resumer = &resumer;
    co->state = CO_RUNNING;
    co->started = 1;

    co_run(co_addr, &resumer);      /* yield or completion */

    result = co->xfer;
    if (co->dead)
        co_reclaim(co_addr);
    return result;
}

i32
kp_co_yield_g(i32 value)
{
    struct coro *co = current ? coro_ptr(current) : 0;

    if (!co)
        rt_trap(0);                 /* yield with no coroutine current */

    if (co->cancelling)
        return 0;

    co->xfer = value;
    co->state = CO_SUSPENDED;
    co_switch(&co->ctx, co->resumer);   /* back to the resumer */

    co->state = CO_RUNNING;             /* resumed again; xfer holds the new value */
    return co->xfer;
}

u32
kp_co_self_g(void)
{
    return current;
}

i32
kp_co_cancelling_g(void)
{
    return current ? (i32)coro_ptr(current)->cancelling : 0;
}

/* Drive a suspended coroutine through its scope-exit releases and off the end,
   like resume but in cancel mode: yields return 0, the body runs its cleanup to
   completion, the carried value discarded. */
static void
co_cancel(u32 co_addr)
{
    struct coro *co = coro_ptr(co_addr);
    struct co_ctx resumer;

    co->cancelling = 1;
    co->resumer = &resumer;
    co->state = CO_RUNNING;
    co_run(co_addr, &resumer);
}

/* Reclaim a coroutine object.  A suspended, started body still owns the handles
   in its frames, so cancel-unwind runs its releases first.  Frees the private
   stack and the object. */
static void
co_reclaim(u32 co_addr)
{
    struct coro *co = coro_ptr(co_addr);

    if (co->state == CO_SUSPENDED && co->started)
        co_cancel(co_addr);
    co_weak_invalidate(co);     /* a weak reference to this coroutine now reads nil */
    if (co->stack)
        gheap_free(co->stack);
    gheap_free(co_addr);
}

/* The KP_OBJ_CORO reclaimer kp_release runs at refcount zero.  A coroutine that
   released its own last reference from inside its running body cannot free the
   stack it is on, so mark it dead and let kp_co_resume_g reclaim it once it
   unwinds back; otherwise reclaim now. */
static void
co_final(u32 co)
{
    if (coro_ptr(co)->state == CO_RUNNING)
        coro_ptr(co)->dead = 1;
    else
        co_reclaim(co);
}

/****************************************************************
 * Weak-reference registry
 *
 * A weak field holds a process without counting the reference, so it must read
 * back as nil once the coroutine is reclaimed.  The hosted tier gets this from the
 * handle table: a process there is a table-id slot carrying a generation, and the
 * slot outlives the object.  This tier has no handle table; a process is a gheap
 * object whose address is recycled on free, so a stored address cannot tell a live
 * coroutine from a freed-and-reused block.
 *
 * The registry supplies the missing persistent slot.  Each coroutine that is ever
 * weakly referenced is assigned a slot, lazily, holding its live address and a
 * generation.  The slot array is never freed back to the heap, so reading a slot is
 * always safe; reclaiming a coroutine clears its slot's address and bumps the
 * generation, so a weak value captured against it no longer matches.  A coroutine
 * that is never weakly referenced gets no slot and costs nothing.  rt_free.c packs
 * the (slot, generation) pair into the weak value and routes the two calls here.
 ****************************************************************/

struct co_slot {
    u32 addr;           /* the live coroutine's gheap address, 0 when free */
    u32 generation;     /* bumped each time the slot is freed; carried across
                           occupants, so a stale weak value never re-matches */
};

static u32 wk_slots;    /* gheap address of the slot array, or 0 before first use */
static u32 wk_cap;      /* number of slots allocated */

static struct co_slot *
wk_ptr(void)
{
    return (struct co_slot *)(uintptr_t)wk_slots;
}

/* Clear a coroutine's slot at reclaim.  Called for every coroutine; a no-op for
   one that was never weakly referenced. */
static void
co_weak_invalidate(struct coro *co)
{
    struct co_slot *s;

    if (co->weakslot == 0)
        return;
    s = &wk_ptr()[co->weakslot - 1];
    s->addr = 0;
    s->generation++;
    co->weakslot = 0;
}

/* Assign a slot to a coroutine on its first weak reference, growing the array if
   full.  gheap_realloc traps fatally on exhaustion, so this never returns a bad
   slot. */
static u32
co_weak_slot_assign(struct coro *co, u32 co_addr)
{
    struct co_slot *s;
    u32 i;

    if (co->weakslot)
        return co->weakslot;
    s = wk_ptr();
    for (i = 0; i < wk_cap; i++)
        if (s[i].addr == 0)
            break;
    if (i == wk_cap) {
        u32 ncap = wk_cap ? wk_cap * 2u : 16u;

        wk_slots = gheap_realloc(wk_slots, ncap * (u32)sizeof(struct co_slot));
        s = wk_ptr();
        memset(&s[wk_cap], 0, (ncap - wk_cap) * (u32)sizeof(struct co_slot));
        wk_cap = ncap;
    }
    s[i].addr = co_addr;        /* generation carries over from the last occupant */
    co->weakslot = i + 1;
    return co->weakslot;
}

/* Ensure a slot for a process value and report its generation.  Returns the 1-based
   slot index, or 0 for a nil process (which packs as a nil weak value). */
u32
co_free_weak_slot(u32 co_addr, u32 *gen_out)
{
    u32 slot;

    if (co_addr == 0) {
        *gen_out = 0;
        return 0;
    }
    slot = co_weak_slot_assign(coro_ptr(co_addr), co_addr);
    *gen_out = wk_ptr()[slot - 1].generation;
    return slot;
}

/* Resolve a weak value's (slot, generation) to the live coroutine address, or 0 if
   the slot is free or now holds a different occupant. */
u32
co_free_weak_addr(u32 slot, u32 generation)
{
    struct co_slot *s;

    if (slot == 0 || slot > wk_cap)
        return 0;
    s = &wk_ptr()[slot - 1];
    if (s->addr == 0 || s->generation != generation)
        return 0;
    return s->addr;
}
