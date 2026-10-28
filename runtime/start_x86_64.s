.intel_syntax noprefix
# start_x86_64.s - minimal x86-64 runtime for Linux user-mode (GNU intel syntax).
#
#
# Provides _start (ELF entry point), syscall wrappers for read/write/exit,
# and the bump arena.  No 64-bit arithmetic helpers: the x86-64 backend does
# i64 natively.
#
# Linux x86-64 syscall convention:
#   rax = syscall number   (read=0, write=1, exit=60)
#   rdi, rsi, rdx, r10, r8, r9 = args 1..6
#   syscall to invoke
#   return value in rax
#
# The compiler's own calling convention passes all args on the stack, so the
# wrappers load from the stack into the syscall argument registers.  The
# backend uses an LP64 address model: a pointer is a full 64-bit value.  To keep
# the risk low this runtime still places the stack, arena, and .bss low in
# memory (a non-PIE executable links text/data/bss below 4GB, and _start sets a
# low stack), so every live address happens to fit in 32 bits even though it is
# carried as a 64-bit value.

.text

.globl _start
.globl read
.globl write
.globl exit
.globl _exit
.globl __moo_arena_alloc
.globl __moo_arena_reset
.globl __cont_capture
.globl __cont_resume
.globl __cont_mark_sp
.globl __cont_arena_ptr

_start:
    mov esp, offset stack_top  # low-memory stack (zeroes the high half of rsp)
    call main
    mov edi, eax               # status
    mov eax, 60                # __NR_exit
    syscall
1:  jmp 1b

# int read(int fd, char *buf, int n); args on stack (one 8-byte word each)
read:
    mov edi, [rsp+8]           # fd
    mov rsi, [rsp+16]          # buf (64-bit pointer)
    mov edx, [rsp+24]          # n
    mov eax, 0                 # __NR_read
    syscall
    ret

# int write(int fd, const char *buf, int n); args on stack
write:
    mov edi, [rsp+8]           # fd
    mov rsi, [rsp+16]          # buf (64-bit pointer)
    mov edx, [rsp+24]          # n
    mov eax, 1                 # __NR_write
    syscall
    ret

# void exit(int status); arg on stack
exit:
_exit:
    mov edi, [rsp+8]           # status
    mov eax, 60                # __NR_exit
    syscall
1:  jmp 1b

# void *__moo_arena_alloc(int size);  bump allocator, aligns to 8; ptr in rax
__moo_arena_alloc:
    mov eax, [rsp+8]           # size (zero-extended into rax)
    add eax, 7
    and eax, -8
    mov rcx, [__moo_arena_ptr] # old pointer (64-bit)
    add rax, rcx               # new pointer
    mov [__moo_arena_ptr], rax
    mov rax, rcx               # return old pointer
    ret

# void __moo_arena_reset(void);
__moo_arena_reset:
    mov qword ptr [__moo_arena_ptr], offset __moo_arena
    ret

# __cont_capture()
#   Called by IR_CAPTURE inline code after pushing the callee-saved regs.
#   Copies the stack segment [capture_sp, mark_sp) into an arena buffer,
#   restores the mark's fp/sp and jumps to the mark's re-entry PC with
#   rax = buffer address (nonzero).  The mark metadata and buffer header are
#   64-bit (LP64); rep movsq copies the segment in qwords.
__cont_capture:
    mov rdx, rsp               # capture_sp
    mov rbx, rbp               # capture_fp
    mov rax, [__cont_mark_sp]  # mark slot pointer (64-bit)
    mov rsi, [rax+8]           # mark's saved_sp
    # size = mark_sp - capture_sp
    mov rax, rsi
    sub rax, rdx               # rax = size (bytes, a multiple of 8)
    mov r8, [__cont_arena_ptr] # r8 = buf
    mov [r8], rdx              # buf[0]  = capture_sp
    mov [r8+8], rbx            # buf[8]  = capture_fp
    mov [r8+16], rax           # buf[16] = size
    lea r9, [r8+rax+24]        # advance arena pointer
    mov [__cont_arena_ptr], r9
    # copy segment: capture_sp -> buf+24
    mov rcx, rax
    shr rcx, 3                 # qword count
    mov rsi, rdx               # src
    lea rdi, [r8+24]           # dst
    rep movsq
    # return buf, restore mark's fp/sp, jump to re-entry
    mov rax, r8                # rax = buf (nonzero)
    mov rcx, [__cont_mark_sp]
    mov rdx, [rcx+16]          # rdx = re-entry PC
    mov rbp, [rcx]             # restore fp
    mov rsp, [rcx+8]           # restore sp
    jmp rdx

# __cont_resume(buf, value)
#   Restores the captured stack segment and returns into it.  The captured
#   return address (from the CAPTURE call) is popped by ret, landing at the
#   IR_CAPTURE resume point with rax = value.
#   Args on stack: [rsp+8] = buf, [rsp+16] = value.
__cont_resume:
    mov rcx, [rsp+8]           # buf
    mov rdx, [rsp+16]          # value
    mov rax, [rcx+16]          # size
    mov rbp, [rcx+8]           # restore fp
    lea rsi, [rcx+24]          # src = buf+24
    mov rsp, [rcx]             # restore sp = capture_sp
    mov rdi, rsp               # dst = capture_sp
    mov rcx, rax
    shr rcx, 3                 # qword count
    rep movsq
    mov rax, rdx               # return value
    ret

.data
    .align 8
__moo_arena_ptr:
    .quad __moo_arena
__cont_mark_sp:
    .quad 0
__cont_arena_ptr:
    .quad __cont_arena

.bss
    .align 16
__moo_arena:
    .zero 131072
__cont_arena:
    .zero 131072
    .zero 131072
stack_top:

# Mark the stack non-executable; without this note the linker assumes an
# executable stack and warns.
.section .note.GNU-stack,"",@progbits
