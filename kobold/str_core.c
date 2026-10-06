/* str_core.c : the guest-owned string object, shared by the hosted VM and the
   freestanding sandbox through rt_glue.h. Only the object model and the byte-level
   operations live here; number formatting and parsing (Str, Val) are tier-specific
   (str_fmt.c hosted, rt_sandbox.c freestanding), since they need libc on the host
   and a hand-rolled implementation with no libc in the sandbox.

   A string is a guest-heap object: a {refcount, kind} header, then the byte length
   and the bytes inline (len bytes plus a trailing NUL). The value the front end
   holds is the object's guest address; nil (0) is the empty string. Strings are
   copy-on-write: the mutators take a borrowed value and return an independent owned
   reference, which the caller stores before releasing the previous value. */

#include "str.h"
#include "rt_glue.h"

struct kp_str {
    uint32_t refcount;
    uint32_t kind;              /* KP_OBJ_STRING */
    int32_t  len;
    char     bytes[1];         /* len bytes then a trailing NUL */
};

#define STRHDR  ((uint32_t)(3 * sizeof(uint32_t)))   /* 12: refcount, kind, len */

/* The string object at a guest address, or NULL for nil. */
static struct kp_str *
str_ptr(KP_VM_P uint32_t s)
{
    return s ? (struct kp_str *)RT_PTR(s) : (struct kp_str *)0;
}

/* The KP_OBJ_STRING reclaimer: the bytes are inline, so the whole object is one block. */
static void
str_finalize(KP_VM_P uint32_t s)
{
    RT_FREE(s);
}

/* Create a string of len bytes at refcount 1, returning its guest address and, through
   out, its object pointer. An empty string is nil (address 0). The bytes are
   uninitialized apart from the trailing NUL; the caller fills them. */
static uint32_t
str_make(KP_VM_P int32_t len, struct kp_str **out)
{
    uint32_t s;
    struct kp_str *d;

    RT_REGISTER(KP_OBJ_STRING, str_finalize);   /* idempotent */
    if (len <= 0) {
        *out = 0;
        return 0;
    }
    s = RT_ALLOC(STRHDR + (uint32_t)len + 1u);
    d = RT_PTR(s);
    d->refcount = 1;
    d->kind = KP_OBJ_STRING;
    d->len = len;
    d->bytes[len] = '\0';
    *out = d;
    return s;
}

uint32_t
kp_str_new(KP_VM_P const char *bytes, int len)
{
    struct kp_str *d;
    uint32_t s;

    if (len < 0)
        len = 0;
    s = str_make(KP_VM_A len, &d);
    if (len > 0) {
        if (bytes)
            memcpy(d->bytes, bytes, (unsigned long)len);
        else
            memset(d->bytes, 0, (unsigned long)len);
    }
    return s;
}

/* The guest-facing constructor: a literal or constant passes the guest address of its
   byte blob, translated to a real pointer for the copy. */
uint32_t
kp_str_new_guest(KP_VM_P uint32_t addr, int len)
{
    const char *bytes = (len > 0) ? (const char *)RT_PTR(addr) : (const char *)0;

    return kp_str_new(KP_VM_A bytes, len);
}

uint32_t
kp_str_concat(KP_VM_P uint32_t a, uint32_t b)
{
    struct kp_str *sa = str_ptr(KP_VM_A a);
    struct kp_str *sb = str_ptr(KP_VM_A b);
    int32_t la = sa ? sa->len : 0;
    int32_t lb = sb ? sb->len : 0;
    struct kp_str *d;
    uint32_t s;

    /* The guest heap never moves an existing block, so sa and sb stay valid across the
       allocation below. */
    s = str_make(KP_VM_A la + lb, &d);
    if (la)
        memcpy(d->bytes, sa->bytes, (unsigned long)la);
    if (lb)
        memcpy(d->bytes + la, sb->bytes, (unsigned long)lb);
    return s;
}

int32_t
kp_str_len(KP_VM_P uint32_t s)
{
    struct kp_str *d = str_ptr(KP_VM_A s);

    return d ? d->len : 0;
}

const char *
kp_str_bytes(KP_VM_P uint32_t s)
{
    struct kp_str *d = str_ptr(KP_VM_A s);

    return (d && d->len) ? d->bytes : "";
}

uint32_t
kp_str_base(KP_VM_P uint32_t s)
{
    KP_VM_UNUSED();              /* the value is the object address; bytes follow the header */
    return s ? s + STRHDR : 0;
}

uint32_t
kp_str_from_char(KP_VM_P int32_t code)
{
    char c = (char)(code & 0xFF);

    return kp_str_new(KP_VM_A &c, 1);
}

int32_t
kp_str_cmp(KP_VM_P uint32_t a, uint32_t b)
{
    int32_t la = kp_str_len(KP_VM_A a);
    int32_t lb = kp_str_len(KP_VM_A b);
    int32_t n = la < lb ? la : lb;
    int r = RT_MEMCMP(kp_str_bytes(KP_VM_A a), kp_str_bytes(KP_VM_A b), (unsigned long)n);

    if (r != 0)
        return r < 0 ? -1 : 1;
    if (la != lb)
        return la < lb ? -1 : 1;
    return 0;
}

int32_t
kp_str_pos(KP_VM_P uint32_t sub, uint32_t s)
{
    struct kp_str *ds = str_ptr(KP_VM_A sub);
    struct kp_str *dd = str_ptr(KP_VM_A s);
    int32_t ls = ds ? ds->len : 0;
    int32_t ld = dd ? dd->len : 0;
    int32_t i;

    if (ls == 0 || ls > ld)             /* an empty needle never matches (as FPC) */
        return 0;
    for (i = 0; i + ls <= ld; i++)
        if (RT_MEMCMP(dd->bytes + i, ds->bytes, (unsigned long)ls) == 0)
            return i + 1;               /* 1-based position */
    return 0;
}

uint32_t
kp_str_copy(KP_VM_P uint32_t s, int32_t index, int32_t count)
{
    struct kp_str *d = str_ptr(KP_VM_A s);
    int32_t len = d ? d->len : 0;
    int32_t avail;
    struct kp_str *ns;
    uint32_t id;

    if (index < 1)
        index = 1;
    if (count < 0 || index > len)
        count = 0;
    else {
        avail = len - (index - 1);
        if (count > avail)
            count = avail;
    }
    id = str_make(KP_VM_A count, &ns);
    if (count)
        memcpy(ns->bytes, d->bytes + (index - 1), (unsigned long)count);
    return id;
}
