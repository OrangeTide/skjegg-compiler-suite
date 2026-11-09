.intel_syntax noprefix
# basic_sys_x86_64.S - the psABI symbols BASIC needs that the shared SysV start
# file (start_x86_64_sysv.s, written for cc + libc) does not provide: the moo
# bump arena, and the time/random syscalls behind RND, TIME, and WAIT.  It is
# linked alongside start_x86_64_sysv.o into every BASIC x86-64 binary, the way
# start_rv.S bundles the same seam for the rv32 build.
#
# Args arrive in the System V registers (rdi, rsi, rdx), which are also the
# Linux syscall argument registers for these calls, so each wrapper only sets
# the call number and issues the syscall.

.text

.globl __moo_arena_alloc
.globl __moo_arena_reset
.globl clock_gettime
.globl getrandom
.globl nanosleep

# void *__moo_arena_alloc(int size);  bump allocator, 8-byte aligned result.
__moo_arena_alloc:
    mov     eax, edi               # size, zero-extended into rax
    add     rax, 7
    and     rax, -8                # round the request up to 8 bytes
    mov     rcx, [rip+__moo_arena_ptr]
    lea     rdx, [rcx+rax]
    mov     [rip+__moo_arena_ptr], rdx
    mov     rax, rcx               # return the old pointer
    ret

# void __moo_arena_reset(void);
__moo_arena_reset:
    lea     rax, [rip+__moo_arena]
    mov     [rip+__moo_arena_ptr], rax
    ret

# int clock_gettime(int clk_id, void *ts) - args already in rdi/rsi
clock_gettime:
    mov     eax, 228               # __NR_clock_gettime
    syscall
    ret

# int getrandom(void *buf, int len, unsigned flags) - rdi/rsi/rdx
getrandom:
    mov     eax, 318               # __NR_getrandom
    syscall
    ret

# int nanosleep(const void *req, void *rem) - rdi/rsi
nanosleep:
    mov     eax, 35                # __NR_nanosleep
    syscall
    ret

.data
    .align 8
__moo_arena_ptr:
    .quad __moo_arena

.bss
    .align 8
__moo_arena:
    .zero 131072

# Mark the stack non-executable, matching the shared start file.
.section .note.GNU-stack,"",@progbits
