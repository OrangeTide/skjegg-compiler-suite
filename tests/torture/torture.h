/* torture.h : tiny deterministic output for the cross-backend differential
   corpus (V.E).

   Only write() from the crt is used (the JIT binds it to the host write and
   skj-run emulates the syscall), so one source links and runs unchanged on
   every backend, every JIT, and skj-run. Output is text, and a value is
   printed in hex through shifts and masks, never division: that keeps the
   result endianness-independent (it is the abstract integer, not its bytes)
   and needs no 64-bit divide helper. A byte-identical stdout across backends
   is the differential oracle, so nothing here may depend on the target. */

#ifndef TORTURE_H
#define TORTURE_H

extern long write(int fd, const void *buf, unsigned long n);

static void
t_emit(const char *s, unsigned long n)
{
    write(1, s, n);
}

static void
t_str(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    t_emit(s, n);
}

/* <label>=<16 hex digits>\n, label first so a diff names the failing value. */
static void
t_hex(const char *label, unsigned long long v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[20];
    int i;

    t_str(label);
    t_str("=");
    for (i = 0; i < 16; i++)
        buf[i] = hexd[(int)((v >> ((15 - i) * 4)) & 0xf)];
    buf[16] = '\n';
    t_emit(buf, 17);
}

/* A signed value prints by its bit pattern, so -1 reads as all-ones on every
   backend regardless of int width in the source. */
static void
t_sint(const char *label, long long v)
{
    t_hex(label, (unsigned long long)v);
}

#endif /* TORTURE_H */
