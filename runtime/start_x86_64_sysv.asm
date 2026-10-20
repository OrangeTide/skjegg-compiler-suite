; start_x86_64_sysv.asm - x86-64 runtime for cc under the SysV AMD64 ABI.
;
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
global open
global close
global lseek
global statx
global mkdirat
global unlinkat
global renameat2
global fchmodat
global getdents64
global exit:weak                ; libc's exit (flush + atexit) overrides this
global _exit
extern main
extern __libc_fini:weak         ; provided by libc; 0 when freestanding

_start:
    mov     esp, stack_top     ; 16-aligned low-memory stack
    xor     edi, edi           ; argc = 0
    xor     esi, esi           ; argv = NULL
    call    main
    mov     ebx, eax           ; preserve main's return across the finalizer
    lea     rax, [rel __libc_fini]  ; weak: 0 when no libc is linked
    test    rax, rax
    jz      .nofini
    call    rax                ; run atexit handlers and flush stdio
.nofini:
    mov     edi, ebx           ; status
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

; int open(const char *path, int flags, int mode) - args in rdi/rsi/rdx
open:
    mov     eax, 2             ; __NR_open
    syscall
    ret

; int close(int fd)
close:
    mov     eax, 3             ; __NR_close
    syscall
    ret

; off_t lseek(int fd, off_t offset, int whence) - a simple 3-arg lseek
lseek:
    mov     eax, 8             ; __NR_lseek
    syscall
    ret

; int statx(int dirfd, const char *path, int flags, unsigned mask, void *buf)
; args in rdi/rsi/rdx/rcx/r8; the syscall wants the 4th arg in r10, not rcx
statx:
    mov     eax, 332           ; __NR_statx
    mov     r10, rcx
    syscall
    ret

; The directory/path file operations the libc wraps with AT_FDCWD
; (libc/fileops.c): mkdirat, unlinkat (unlink and rmdir), renameat2.  The *at
; forms keep the one libc path shared with the no-bare-syscall arches.
mkdirat:
    mov     eax, 258           ; __NR_mkdirat
    syscall
    ret

unlinkat:
    mov     eax, 263           ; __NR_unlinkat
    syscall
    ret

; renameat2(olddirfd, oldpath, newdirfd, newpath, flags): the 5th arg comes in
; r8; the syscall wants its 4th arg (newpath) in r10, not rcx.
renameat2:
    mov     eax, 316           ; __NR_renameat2
    mov     r10, rcx
    syscall
    ret

; fchmodat(dirfd, path, mode, flags): the libc chmod (libc/fileops.c) over
; AT_FDCWD; the syscall wants its 4th arg (flags) in r10, not rcx.
fchmodat:
    mov     eax, 268           ; __NR_fchmodat
    mov     r10, rcx
    syscall
    ret

; int getdents64(int fd, void *buf, int count); directory reads for the libc
; opendir/readdir (libc/dirent.c).
getdents64:
    mov     eax, 217           ; __NR_getdents64
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
