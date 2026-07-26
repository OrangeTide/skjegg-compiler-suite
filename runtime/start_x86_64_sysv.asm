; start_x86_64_sysv.asm - x86-64 runtime for cc under the SysV AMD64 ABI.
; Made by a machine. PUBLIC DOMAIN (CC0-1.0)
;
; cc-x86-64 (built -DCC_PSABI) emits System V AMD64 calls: args in
; rdi,rsi,rdx,rcx,r8,r9 / xmm0..7, return in rax/xmm0.  So the syscall wrappers
; here just move the args (already in the SysV registers) to the syscall
; registers - which for read/write/exit are the same registers - and issue the
; syscall.  main receives argc in rdi, argv in rsi (the tests take neither).
;
; The low-memory stack keeps every address sub-4GB, matching the LP64 backend's
; runtime (a 32-bit write to esp zeroes the high half of rsp).

section .text

global _start
global read
global write
global exit
global _exit
extern main

_start:
    mov     esp, stack_top     ; 16-aligned low-memory stack
    xor     edi, edi           ; argc = 0
    xor     esi, esi           ; argv = NULL
    call    main
    mov     edi, eax           ; status
    mov     eax, 60            ; __NR_exit
    syscall
.hang:
    jmp     .hang

; ssize_t read(int fd, void *buf, size_t n) - SysV args already in rdi/rsi/rdx
read:
    mov     eax, 0             ; __NR_read
    syscall
    ret

; ssize_t write(int fd, const void *buf, size_t n)
write:
    mov     eax, 1             ; __NR_write
    syscall
    ret

; void exit(int status) - status already in rdi
exit:
_exit:
    mov     eax, 60            ; __NR_exit
    syscall
.hang:
    jmp     .hang

section .bss
    align 16
    resb 65536
stack_top:
