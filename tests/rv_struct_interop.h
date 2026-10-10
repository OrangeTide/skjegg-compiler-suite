/* Shared declarations for the skj-cc-rv <-> gcc by-value struct interop test.
   Both halves must see identical struct layouts, so they live here.  pt is a
   2-word struct (passed in a0/a1, returned in a0/a1); big is a 4-word struct
   (passed by reference, returned through a hidden pointer) under the RISC-V
   ilp32 psABI. */

#ifndef RV_STRUCT_INTEROP_H
#define RV_STRUCT_INTEROP_H

struct pt { int x, y; };
struct big { int a, b, c, d; };

/* gcc-built: struct argument (<=8 and >8 bytes) and struct return */
int gcc_sum_pt(struct pt p);
struct pt gcc_make_pt(int a, int b);
int gcc_sum_big(struct big b);
struct big gcc_make_big(int base);

/* gcc calls back into this skj-built function with a by-value struct */
int skj_cb(struct pt p);
int gcc_calls_skj(void);

#endif
