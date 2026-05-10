; start_x86.asm - minimal i686 runtime for Linux user-mode (NASM syntax).
; Made by a machine. PUBLIC DOMAIN (CC0-1.0)
;
; Provides _start (ELF entry point), syscall wrappers for read/write/exit,
; and 64-bit arithmetic helpers (__muldi3, __ashldi3, __ashrdi3, __lshrdi3).
;
; Linux i386 syscall convention:
;   eax = syscall number
;   ebx, ecx, edx, esi, edi = args 1..5
;   int 0x80 to invoke
;   return value in eax
;
; All exported functions use cdecl: args on stack, return in eax (or eax:edx
; for 64-bit).  Callee-save: ebx, esi, edi, ebp.

section .text

global _start
global read
global write
global exit
global _exit
global __muldi3
global __ashldi3
global __ashrdi3
global __lshrdi3
global __moo_arena_alloc
global __moo_arena_reset
global __cont_capture
global __cont_resume
global __cont_mark_sp

_start:
    mov esp, stack_top
    sub esp, 16
    call main
    mov ebx, eax
    mov eax, 1              ; __NR_exit
    int 0x80
.hang:
    jmp .hang

; int read(int fd, char *buf, int n);
read:
    push ebx
    mov ebx, [esp+8]       ; fd
    mov ecx, [esp+12]      ; buf
    mov edx, [esp+16]      ; n
    mov eax, 3              ; __NR_read
    int 0x80
    pop ebx
    ret

; int write(int fd, const char *buf, int n);
write:
    push ebx
    mov ebx, [esp+8]       ; fd
    mov ecx, [esp+12]      ; buf
    mov edx, [esp+16]      ; n
    mov eax, 4              ; __NR_write
    int 0x80
    pop ebx
    ret

; void exit(int status);
exit:
_exit:
    mov ebx, [esp+4]       ; status
    mov eax, 1              ; __NR_exit
    int 0x80
.hang:
    jmp .hang

; __muldi3(a_hi, a_lo, b_hi, b_lo) -> eax=hi, edx=lo
;   64-bit multiply (low 64 bits of result).
;   Stack: [esp+4]=a_hi, [esp+8]=a_lo, [esp+12]=b_hi, [esp+16]=b_lo
;
;   Uses the x86 32x32->64 mul instruction:
;     result_lo = lo32(a_lo * b_lo)
;     result_hi = hi32(a_lo * b_lo) + lo32(a_hi * b_lo) + lo32(a_lo * b_hi)
__muldi3:
    push ebx
    mov eax, [esp+12]      ; eax = a_lo
    mul dword [esp+20]     ; edx:eax = a_lo * b_lo (unsigned full 64-bit)
    mov ebx, eax           ; ebx = result_lo
    mov ecx, edx           ; ecx = hi32(a_lo * b_lo)
    mov eax, [esp+8]       ; eax = a_hi
    imul eax, [esp+20]     ; eax = lo32(a_hi * b_lo)
    add ecx, eax
    mov eax, [esp+12]      ; eax = a_lo
    imul eax, [esp+16]     ; eax = lo32(a_lo * b_hi)
    add ecx, eax
    mov eax, ecx           ; eax = result_hi
    mov edx, ebx           ; edx = result_lo
    pop ebx
    ret

; __ashldi3(a_hi, a_lo, shift) -> eax=hi, edx=lo
;   64-bit left shift.
;   Stack: [esp+4]=a_hi, [esp+8]=a_lo, [esp+12]=shift
__ashldi3:
    mov ecx, [esp+12]      ; ecx = shift count
    mov eax, [esp+4]       ; eax = a_hi
    mov edx, [esp+8]       ; edx = a_lo
    cmp ecx, 32
    jae .ashl_ge32
    test ecx, ecx
    jz .ashl_done
    shld eax, edx, cl      ; hi = (hi << n) | (lo >> (32-n))
    shl edx, cl            ; lo = lo << n
    jmp .ashl_done
.ashl_ge32:
    cmp ecx, 64
    jae .ashl_zero
    sub ecx, 32
    mov eax, edx           ; hi = lo << (n-32)
    shl eax, cl
    xor edx, edx           ; lo = 0
    jmp .ashl_done
.ashl_zero:
    xor eax, eax
    xor edx, edx
.ashl_done:
    ret

; __ashrdi3(a_hi, a_lo, shift) -> eax=hi, edx=lo
;   64-bit arithmetic right shift.
;   Stack: [esp+4]=a_hi, [esp+8]=a_lo, [esp+12]=shift
__ashrdi3:
    mov ecx, [esp+12]      ; ecx = shift count
    mov eax, [esp+4]       ; eax = a_hi
    mov edx, [esp+8]       ; edx = a_lo
    cmp ecx, 32
    jae .ashr_ge32
    test ecx, ecx
    jz .ashr_done
    shrd edx, eax, cl      ; lo = (lo >> n) | (hi << (32-n))
    sar eax, cl            ; hi = hi >> n (arithmetic)
    jmp .ashr_done
.ashr_ge32:
    cmp ecx, 64
    jae .ashr_sign
    sub ecx, 32
    mov edx, eax           ; lo = hi >> (n-32) (arithmetic)
    sar edx, cl
    sar eax, 31            ; hi = sign extension
    jmp .ashr_done
.ashr_sign:
    sar eax, 31
    mov edx, eax
.ashr_done:
    ret

; __lshrdi3(a_hi, a_lo, shift) -> eax=hi, edx=lo
;   64-bit logical right shift.
;   Stack: [esp+4]=a_hi, [esp+8]=a_lo, [esp+12]=shift
__lshrdi3:
    mov ecx, [esp+12]      ; ecx = shift count
    mov eax, [esp+4]       ; eax = a_hi
    mov edx, [esp+8]       ; edx = a_lo
    cmp ecx, 32
    jae .lshr_ge32
    test ecx, ecx
    jz .lshr_done
    shrd edx, eax, cl      ; lo = (lo >> n) | (hi << (32-n))
    shr eax, cl            ; hi = hi >> n (logical)
    jmp .lshr_done
.lshr_ge32:
    cmp ecx, 64
    jae .lshr_zero
    sub ecx, 32
    mov edx, eax           ; lo = hi >> (n-32)
    shr edx, cl
    xor eax, eax           ; hi = 0
    jmp .lshr_done
.lshr_zero:
    xor eax, eax
    xor edx, edx
.lshr_done:
    ret

; void *__moo_arena_alloc(int size);
;   Bump allocator. Aligns size to 4 bytes. Returns pointer in eax.
__moo_arena_alloc:
    mov eax, [esp+4]
    add eax, 3
    and eax, -4
    mov ecx, [__moo_arena_ptr]
    add eax, ecx
    mov [__moo_arena_ptr], eax
    mov eax, ecx           ; return old pointer
    ret

; void __moo_arena_reset(void);
__moo_arena_reset:
    mov dword [__moo_arena_ptr], __moo_arena
    ret

; __cont_capture()
;   Called by IR_CAPTURE inline code after pushing ebx, esi, edi.
;   The call return address on the stack becomes the resume entry point
;   (captured in the stack segment; __cont_resume's ret pops it).
;
;   Reads the 12-byte mark slot via __cont_mark_sp, bump-allocates a
;   buffer in __cont_arena, copies the stack segment from capture_sp
;   to mark_sp, then longjmps to the mark's re-entry label with
;   eax = buffer address (nonzero).
;
;   On entry:  ebx, esi, edi on stack (pushed by IR_CAPTURE inline code)
;              [esp] = return address to IR_CAPTURE resume point
;   Clobbers:  all regs (callee-saves are in the captured segment)
__cont_capture:
    mov edx, esp               ; edx = capture_sp
    mov ebx, ebp               ; ebx = capture_fp

    ; read 12-byte mark slot
    mov eax, [__cont_mark_sp]
    mov ecx, [eax]             ; ecx = mark's saved_fp
    mov esi, [eax+4]           ; esi = mark's saved_sp
    mov edi, [eax+8]           ; edi = mark's re-entry PC (temp)

    ; size = mark_sp - capture_sp (stack grows down)
    mov eax, esi
    sub eax, edx               ; eax = size

    ; bump-allocate: 12-byte header + size bytes
    mov ebp, [__cont_arena_ptr]; ebp = buf

    ; write buffer header [capture_sp, capture_fp, size]
    mov [ebp], edx             ; buf[0] = capture_sp
    mov [ebp+4], ebx           ; buf[4] = capture_fp
    mov [ebp+8], eax           ; buf[8] = size

    ; advance arena pointer past this buffer
    lea ebx, [ebp+eax+12]
    mov [__cont_arena_ptr], ebx

    ; copy stack segment: capture_sp -> buf+12 (uses rep movsd)
    mov ecx, eax
    shr ecx, 2                ; ecx = dword count
    mov esi, edx               ; esi = src (capture_sp)
    lea edi, [ebp+12]          ; edi = dst (buf + 12)
    rep movsd

    ; longjmp to mark: restore FP/SP, jump to re-entry label
    mov eax, ebp               ; eax = buf (nonzero return value)
    mov ecx, [__cont_mark_sp]  ; re-read mark slot (unchanged)
    mov ebp, [ecx]             ; restore mark's FP
    mov esp, [ecx+4]           ; restore mark's SP
    jmp dword [ecx+8]          ; jump to mark's re-entry label

; __cont_resume(buf, value)
;   Restores the captured stack segment and returns into it via ret.
;   The ret pops the call __cont_capture return address, landing at
;   the IR_CAPTURE inline code's resume point.
;
;   On entry:  [esp+4] = buf pointer, [esp+8] = resume value
__cont_resume:
    mov ecx, [esp+4]           ; ecx = buf
    mov edx, [esp+8]           ; edx = resume value

    ; read buffer header
    mov eax, [ecx+8]           ; eax = size
    mov ebp, [ecx+4]           ; restore FP
    mov esp, [ecx]             ; restore SP (stack is now captured state)

    ; copy stack data back: buf+12 -> capture_sp
    lea esi, [ecx+12]          ; esi = src (buf + 12)
    mov edi, esp               ; edi = dst (capture_sp)
    mov ecx, eax
    shr ecx, 2                ; ecx = dword count
    rep movsd

    mov eax, edx               ; eax = resume value
    ret

extern main

section .data
    align 4
__cont_mark_sp:
    dd 0
__cont_arena_ptr:
    dd __cont_arena
__moo_arena_ptr:
    dd __moo_arena

section .bss
    align 4
__cont_arena:
    resb 65536
__moo_arena:
    resb 65536
    resb 65536
stack_top:
