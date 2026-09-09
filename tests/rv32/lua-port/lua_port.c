/* lua_port.c : runs a Lua interpreter on the RV32 emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Lua is a far harder test than a benchmark. It allocates constantly, it
 * uses setjmp and longjmp for error handling, it does string formatting
 * and floating-point arithmetic, and its own test script can check its own
 * answers. If it runs, most of the C library and most of the instruction
 * set are working together.
 *
 * This file is the whole of the platform layer: the handful of calls
 * picolibc expects from a board, a heap that grows into the space the
 * linker script left, and a main that runs an embedded script. Nothing is
 * read from a filesystem, because there is not one. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/****************************************************************
 * The two calls that reach the host
 ****************************************************************/

#define SYS_WRITE   64
#define SYS_EXIT    93

static long
sys_write(int fd, const void *buf, unsigned long len)
{
    register long a0 __asm__("a0") = fd;
    register const void *a1 __asm__("a1") = buf;
    register unsigned long a2 __asm__("a2") = len;
    register long a7 __asm__("a7") = SYS_WRITE;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
}

static void
sys_exit(int code)
{
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = SYS_EXIT;

    __asm__ volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    for (;;)
        ;
}

/****************************************************************
 * The board interface picolibc expects
 *
 * The heap is whatever the linker script left between the end of the
 * image and the top of memory. There is no operating system to ask for
 * more, so sbrk simply hands out the region until it runs out.
 ****************************************************************/

extern char __heap_start[];
extern char __heap_end[];

static char *heap_cursor;

void *
sbrk(ptrdiff_t increment)
{
    char *previous;

    if (heap_cursor == 0)
        heap_cursor = __heap_start;
    if (heap_cursor + increment > __heap_end)
        return (void *)-1;
    previous = heap_cursor;
    heap_cursor += increment;
    return previous;
}

int
write(int fd, const void *buf, size_t len)
{
    return (int)sys_write(fd, buf, len);
}

int
read(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return 0;                   /* end of file: there is no input */
}

int
close(int fd)
{
    (void)fd;
    return 0;
}

off_t
lseek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    return 0;
}

int
isatty(int fd)
{
    (void)fd;
    return 1;
}

__attribute__((noreturn)) void
_exit(int code)
{
    sys_exit(code);
    __builtin_unreachable();
}

/****************************************************************
 * Calls Lua's os and io libraries reach for
 *
 * There is no filesystem and no clock behind them. Refusing cleanly is
 * better than omitting the libraries: a script that calls io.open gets a
 * Lua error it can catch, rather than the interpreter failing to build.
 ****************************************************************/

int
open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return -1;
}

int
unlink(const char *path)
{
    (void)path;
    return -1;
}

int
rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return -1;
}

clock_t
times(struct tms *buf)
{
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return 0;
}

int
gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}

/****************************************************************
 * The standard streams
 *
 * picolibc's small stdio leaves these to the board, because a board is
 * not obliged to have a console at all. All three point at the same
 * stream here: output goes to the write syscall a character at a time,
 * and input is always at end of file since the guest has no console to
 * read from.
 ****************************************************************/

static int
console_put(char c, FILE *stream)
{
    (void)stream;
    sys_write(1, &c, 1);
    return (unsigned char)c;
}

static int
console_get(FILE *stream)
{
    (void)stream;
    return EOF;
}

static FILE console = FDEV_SETUP_STREAM(console_put, console_get, NULL,
                                        _FDEV_SETUP_RW);

FILE *const stdin = &console;
FILE *const stdout = &console;
FILE *const stderr = &console;

/****************************************************************
 * The script
 *
 * Embedded rather than loaded, and self-checking: it reports its own
 * failures so that a wrong answer is visible without a reference run.
 ****************************************************************/

static const char script[] =
"print(string.format('lua %s  integer max %s  1e300 -> %s',\n"
"      _VERSION, tostring(math.maxinteger), tostring(1e300)))\n"
"local fail = 0\n"
"local function check(name, got, want)\n"
"  if got ~= want then\n"
"    fail = fail + 1\n"
"    print(string.format('FAIL %-22s got %s want %s', name,\n"
"                        tostring(got), tostring(want)))\n"
"  else\n"
"    print(string.format('ok   %-22s %s', name, tostring(got)))\n"
"  end\n"
"end\n"
"\n"
"-- integers and the integer/float divide that 5.3 introduced\n"
"check('integer arithmetic', 7 // 2, 3)\n"
"check('float division', 7 / 2, 3.5)\n"
"check('modulo of negatives', -7 % 3, 2)\n"
"check('integer overflow wraps', math.maxinteger + 1, math.mininteger)\n"
"check('float to integer', math.tointeger(3.0), 3)\n"
"\n"
"-- closures and upvalues\n"
"local function counter()\n"
"  local n = 0\n"
"  return function() n = n + 1; return n end\n"
"end\n"
"local c = counter()\n"
"c(); c()\n"
"check('closure upvalue', c(), 3)\n"
"\n"
"-- tables, sorting and string building\n"
"local t = {}\n"
"for i = 1, 50 do t[i] = (i * 37) % 101 end\n"
"table.sort(t)\n"
"check('sorted first', t[1], 2)\n"
"check('sorted last', t[50], 100)\n"
"local parts = {}\n"
"for i = 1, 10 do parts[#parts + 1] = tostring(t[i]) end\n"
"check('concat', table.concat(parts, ','), '2,3,6,9,10,12,13,16,19,20')\n"
"\n"
"-- string library, including patterns and formatting\n"
"check('gsub', ('hello world'):gsub('o', '0'), 'hell0 w0rld')\n"
"check('match', ('key=value'):match('(%w+)=(%w+)'), 'key')\n"
"check('rep and len', ('ab'):rep(4):len(), 8)\n"
"check('format', string.format('%5.2f|%d|%s', 1.5, 42, 'x'), ' 1.50|42|x')\n"
"check('byte and char', string.char(82, 86, 51, 50), 'RV32')\n"
"\n"
"-- metatables\n"
"local V = {}\n"
"V.__index = V\n"
"V.__add = function(a, b) return setmetatable({a[1]+b[1], a[2]+b[2]}, V) end\n"
"V.__eq = function(a, b) return a[1]==b[1] and a[2]==b[2] end\n"
"local a = setmetatable({1, 2}, V)\n"
"local b = setmetatable({10, 20}, V)\n"
"local sum = a + b\n"
"check('metatable __add', sum[1] .. ',' .. sum[2], '11,22')\n"
"check('metatable __eq', a + b == setmetatable({11,22}, V), true)\n"
"\n"
"-- coroutines\n"
"local co = coroutine.create(function(x)\n"
"  for i = 1, 3 do x = coroutine.yield(x * 2) end\n"
"  return 'done'\n"
"end)\n"
"local _, r1 = coroutine.resume(co, 5)\n"
"local _, r2 = coroutine.resume(co, 7)\n"
"check('coroutine yield', r1, 10)\n"
"check('coroutine resume', r2, 14)\n"
"\n"
"-- error handling, which is setjmp and longjmp underneath\n"
"local ok, err = pcall(function() error('boom') end)\n"
"check('pcall caught', ok, false)\n"
"check('error message', err:match('boom') ~= nil, true)\n"
"local ok2 = pcall(function() local x = nil; return x.y end)\n"
"check('index nil caught', ok2, false)\n"
"\n"
"-- floating point, which runs on the F extension or in software\n"
"check('sqrt', math.sqrt(2) > 1.414 and math.sqrt(2) < 1.415, true)\n"
"check('floor and ceil', math.floor(-1.5) .. ',' .. math.ceil(-1.5),\n"
"      '-2,-1')\n"
"check('huge', math.huge > 1e300, true)\n"
"\n"
"-- garbage collection, which exercises the allocator hard\n"
"collectgarbage('collect')\n"
"local before = collectgarbage('count')\n"
"for i = 1, 2000 do local s = {} ; s[1] = ('x'):rep(20) end\n"
"collectgarbage('collect')\n"
"check('gc reclaims', collectgarbage('count') < before + 40, true)\n"
"\n"
"-- a recursive algorithm, for the call and return path\n"
"local function fib(n) if n < 2 then return n end\n"
"  return fib(n-1) + fib(n-2) end\n"
"check('fib(20)', fib(20), 6765)\n"
"\n"
"print(fail == 0 and 'all lua checks passed' or (fail .. ' checks FAILED'))\n"
"return fail\n";

/****************************************************************
 * Entry
 ****************************************************************/

int
main(void)
{
    lua_State *L;
    int rc;

    L = luaL_newstate();
    if (L == NULL) {
        sys_write(1, "cannot create lua state\n", 24);
        return 1;
    }
    luaL_openlibs(L);

    if (luaL_dostring(L, script) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        unsigned long n = 0;

        sys_write(1, "lua error: ", 11);
        while (msg && msg[n])
            n++;
        sys_write(1, msg, n);
        sys_write(1, "\n", 1);
        lua_close(L);
        return 1;
    }

    rc = (int)lua_tointeger(L, -1);
    lua_close(L);
    return rc;
}
