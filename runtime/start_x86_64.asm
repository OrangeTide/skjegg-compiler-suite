; start_x86_64.asm - minimal x86-64 runtime for Linux user-mode (NASM syntax).
; Made by a machine. PUBLIC DOMAIN (CC0-1.0)
;
; Provides _start (ELF entry point), syscall wrappers for read/write/exit,
; and the bump arena.  No 64-bit arithmetic helpers: the x86-64 backend does
; i64 natively.
;
; Linux x86-64 syscall convention:
;   rax = syscall number   (read=0, write=1, exit=60)
;   rdi, rsi, rdx, r10, r8, r9 = args 1..6
;   syscall to invoke
;   return value in rax
;
; The compiler's own calling convention passes all args on the stack, so the
; wrappers load from the stack into the syscall argument registers.  The
; backend uses an ILP32-style address model: every address is a 32-bit value,
; so _start switches to a low-memory stack (a 32-bit write to esp zeroes the
; upper half of rsp) and the arena lives low in .bss.  A non-PIE executable
; links text/data/bss below 4GB.

section .text

global _start
global read
global write
global exit
global _exit
global __moo_arena_alloc
global __moo_arena_reset
global __cont_capture
global __cont_resume
global __cont_mark_sp

_start:
    mov esp, stack_top         ; low-memory stack (zeroes the high half of rsp)
    call main
    mov edi, eax               ; status
    mov eax, 60                ; __NR_exit
    syscall
.hang:
    jmp .hang

; int read(int fd, char *buf, int n); args on stack
read:
    mov edi, [rsp+8]           ; fd
    mov esi, [rsp+16]          ; buf (32-bit addr, zero-extended)
    mov edx, [rsp+24]          ; n
    mov eax, 0                 ; __NR_read
    syscall
    ret

; int write(int fd, const char *buf, int n); args on stack
write:
    mov edi, [rsp+8]           ; fd
    mov esi, [rsp+16]          ; buf
    mov edx, [rsp+24]          ; n
    mov eax, 1                 ; __NR_write
    syscall
    ret

; void exit(int status); arg on stack
exit:
_exit:
    mov edi, [rsp+8]           ; status
    mov eax, 60                ; __NR_exit
    syscall
.hang:
    jmp .hang

; void *__moo_arena_alloc(int size);  bump allocator, aligns to 8; ptr in eax
__moo_arena_alloc:
    mov eax, [rsp+8]           ; size
    add eax, 7
    and eax, -8
    mov ecx, [__moo_arena_ptr] ; old pointer (32-bit)
    add eax, ecx               ; new pointer
    mov [__moo_arena_ptr], eax
    mov eax, ecx               ; return old pointer
    ret

; void __moo_arena_reset(void);
__moo_arena_reset:
    mov dword [__moo_arena_ptr], __moo_arena
    ret

; __cont_capture()
;   Called by IR_CAPTURE inline code after pushing the callee-saved regs.
;   Copies the stack segment [capture_sp, mark_sp) into an arena buffer,
;   restores the mark's fp/sp and jumps to the mark's re-entry PC with
;   rax = buffer address (nonzero).  The mark metadata and buffer header are
;   32-bit (ILP32); rep movsd copies the segment in dwords.
__cont_capture:
    mov rdx, rsp               ; capture_sp
    mov rbx, rbp               ; capture_fp
    mov eax, [__cont_mark_sp]  ; mark slot pointer (32-bit)
    mov esi, [rax+4]           ; mark's saved_sp
    ; size = mark_sp - capture_sp
    mov eax, esi
    sub eax, edx               ; eax = size
    mov r8d, [__cont_arena_ptr]; r8 = buf
    mov [r8], edx              ; buf[0] = capture_sp
    mov [r8+4], ebx            ; buf[4] = capture_fp
    mov [r8+8], eax            ; buf[8] = size
    lea r9d, [r8+rax+12]       ; advance arena pointer
    mov [__cont_arena_ptr], r9d
    ; copy segment: capture_sp -> buf+12
    mov ecx, eax
    shr ecx, 2                 ; dword count
    mov rsi, rdx               ; src
    lea rdi, [r8+12]           ; dst
    rep movsd
    ; return buf, restore mark's fp/sp, jump to re-entry
    mov eax, r8d               ; rax = buf (nonzero)
    mov ecx, [__cont_mark_sp]
    mov edx, [rcx+8]           ; edx = re-entry PC (zero-extended)
    mov ebp, [rcx]             ; restore fp
    mov esp, [rcx+4]           ; restore sp
    jmp rdx

; __cont_resume(buf, value)
;   Restores the captured stack segment and returns into it.  The captured
;   return address (from the CAPTURE call) is popped by ret, landing at the
;   IR_CAPTURE resume point with rax = value.
;   Args on stack: [rsp+8] = buf, [rsp+16] = value.
__cont_resume:
    mov ecx, [rsp+8]           ; buf
    mov edx, [rsp+16]          ; value
    mov eax, [rcx+8]           ; size
    mov ebp, [rcx+4]           ; restore fp
    lea rsi, [rcx+12]          ; src = buf+12
    mov esp, [rcx]             ; restore sp = capture_sp
    mov edi, esp               ; dst = capture_sp
    mov ecx, eax
    shr ecx, 2                 ; dword count
    rep movsd
    mov eax, edx               ; return value
    ret

extern main

section .data
    align 8
__moo_arena_ptr:
    dd __moo_arena
__cont_mark_sp:
    dd 0
__cont_arena_ptr:
    dd __cont_arena

section .bss
    align 16
__moo_arena:
    resb 131072
__cont_arena:
    resb 131072
    resb 131072
stack_top:
