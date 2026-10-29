/* aarch64_encode.c : the AArch64 instruction encoder for skj-as-aarch64.
 *
 * One entry point, a64_encode, turns a parsed mnemonic plus operands into one
 * 32-bit little-endian word in the current section (a pseudo such as `ldr =v`
 * also queues a literal-pool entry).  The bitfield recipes mirror the JIT
 * encoder jit/emit_arm64.c, which is checked byte-for-byte against
 * aarch64-linux-gnu-as, so this assembler agrees with both.
 *
 * Instruction length never depends on operand values, so the two passes emit
 * identical words; only relocations and resolved branch/adr displacements are
 * withheld until pass 2.  A branch, cbz/cbnz or adr to a symbol defined in the
 * current section resolves to a direct displacement; any other target becomes a
 * relocation.
 */

#include <stdlib.h>
#include <string.h>

#include "aarch64.h"
#include "util.h"

/****************************************************************
 * Word emission and in-place field patching
 ****************************************************************/

static void
w(struct section *s, uint32_t insn)
{
    sec_emit32(s, insn);                /* section is little-endian */
}

static uint32_t
rd_word(struct section *s, uint32_t off)
{
    return (uint32_t)s->data[off] | ((uint32_t)s->data[off + 1] << 8) |
           ((uint32_t)s->data[off + 2] << 16) | ((uint32_t)s->data[off + 3] << 24);
}

static void
wr_word(struct section *s, uint32_t off, uint32_t v)
{
    s->data[off + 0] = (uint8_t)v;
    s->data[off + 1] = (uint8_t)(v >> 8);
    s->data[off + 2] = (uint8_t)(v >> 16);
    s->data[off + 3] = (uint8_t)(v >> 24);
}

static int gsf(const struct a64_operand *o) { return o->size == 8; }     /* gpr sf */
static int fsz(const struct a64_operand *o)                              /* fp size field */
{ return o->size == 8 ? 3 : o->size == 2 ? 1 : 2; }
static int ftype(int szf) { return szf == 3 ? 1 : szf == 1 ? 3 : 0; }

/****************************************************************
 * Data-processing recipes (from jit/emit_arm64.c)
 ****************************************************************/

static void
movw(struct section *s, int sf, int opc, int Rd, unsigned imm16, int shift)
{
    int hw = shift / 16;
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)opc << 29) | (0x25u << 23) |
         ((uint32_t)(hw & 3) << 21) | ((imm16 & 0xffff) << 5) | (uint32_t)(Rd & 31));
}

static int encode_bitmask(uint64_t imm, int sf, uint32_t *n_immr_imms);
static int logic_imm(struct section *s, int sf, int opc, int Rd, int Rn, uint64_t imm);

/* `mov Rd, #imm`: the GNU as alias that resolves to a single movz / movn / orr
   (bitmask) instruction, chosen by value in that preference order. */
static void
mov_imm(struct section *s, int sf, int Rd, long imm)
{
    uint64_t v = sf ? (uint64_t)imm : ((uint64_t)imm & 0xffffffffULL);
    uint64_t nv = (~v) & (sf ? ~0ULL : 0xffffffffULL);
    int lanes = sf ? 4 : 2, hw;

    for (hw = 0; hw < lanes; hw++)              /* movz: one nonzero lane */
        if ((v & ~(0xffffULL << (16 * hw))) == 0) {
            movw(s, sf, 2, Rd, (unsigned)((v >> (16 * hw)) & 0xffff), 16 * hw);
            return;
        }
    for (hw = 0; hw < lanes; hw++)              /* movn: ~v has one nonzero lane */
        if ((nv & ~(0xffffULL << (16 * hw))) == 0) {
            movw(s, sf, 0, Rd, (unsigned)((nv >> (16 * hw)) & 0xffff), 16 * hw);
            return;
        }
    if (logic_imm(s, sf, 1, Rd, A64_ZR, v) == 0) /* orr Rd, zr, #bitmask */
        return;
    die("aarch64: mov immediate is not a single instruction");
}

static void
addsub_imm(struct section *s, int sf, int op, int Rd, int Rn, unsigned imm12, int lsl12)
{
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)op << 30) | (0x11u << 24) |
         ((uint32_t)(lsl12 ? 1 : 0) << 22) | ((imm12 & 0xfff) << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
addsub_reg(struct section *s, int sf, int op, int S, int Rd, int Rn, int Rm)
{
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)op << 30) | ((uint32_t)S << 29) |
         (0x0Bu << 24) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

/* Add/subtract (extended register), option UXTX/UXTW, imm3=0.  Required when Rd
   or Rn is the stack pointer (reg 31 is SP here, not XZR). */
static void
addsub_ext(struct section *s, int sf, int op, int S, int Rd, int Rn, int Rm)
{
    int option = sf ? 3 : 2;            /* UXTX (64) / UXTW (32) */
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)op << 30) | ((uint32_t)S << 29) |
         (0x0Bu << 24) | (1u << 21) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)option << 13) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
logic_reg(struct section *s, int sf, int opc, int N, int Rd, int Rn, int Rm)
{
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)opc << 29) | (0x0Au << 24) |
         ((uint32_t)N << 21) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static int
is_shifted_mask(uint64_t v)
{
    return v != 0 && (((v + (v & (uint64_t)(-(int64_t)v))) & v) == 0);
}

/* Logical (immediate) bitmask encoding (N:immr:imms), the canonical algorithm
   (ARM ARM / LLVM AArch64_AM::processLogicalImmediate).  Returns -1 if v is not
   a representable bitmask immediate for the given size. */
static int
encode_bitmask(uint64_t imm, int sf, uint32_t *n_immr_imms)
{
    unsigned size = 64;
    uint64_t mask;
    int i, cto;

    if (sf == 0) {
        imm &= 0xffffffffULL;
        if (imm == 0 || imm == 0xffffffffULL)
            return -1;
        imm |= imm << 32;               /* replicate into 64 bits */
    } else if (imm == 0 || imm == ~0ULL) {
        return -1;
    }

    /* find the element size: the smallest width whose halves disagree */
    while (size > 2) {
        size /= 2;
        mask = (1ULL << size) - 1;
        if ((imm & mask) != ((imm >> size) & mask)) {
            size *= 2;
            break;
        }
    }
    mask = (size == 64) ? ~0ULL : (1ULL << size) - 1;
    imm &= mask;

    if (is_shifted_mask(imm)) {
        i = __builtin_ctzll(imm);
        cto = __builtin_ctzll(~(imm >> i));
    } else {
        imm |= ~mask;
        if (!is_shifted_mask(~imm))
            return -1;
        {
            int clo = __builtin_clzll(~imm);
            i = 64 - clo;
            cto = clo + __builtin_ctzll(~imm) - (64 - (int)size);
        }
    }

    {
        unsigned immr = (size - (unsigned)i) & (size - 1);
        unsigned nimms = (~(size - 1) << 1) & 0x7f;
        unsigned Nbit;
        nimms |= (unsigned)(cto - 1);
        Nbit = ((nimms >> 6) & 1) ^ 1;
        *n_immr_imms = ((uint32_t)Nbit << 12) | ((uint32_t)immr << 6) |
                       (uint32_t)(nimms & 0x3f);
    }
    return 0;
}

static int
logic_imm(struct section *s, int sf, int opc, int Rd, int Rn, uint64_t imm)
{
    uint32_t nri;
    if (encode_bitmask(imm, sf, &nri) < 0)
        return -1;
    w(s, ((uint32_t)(sf & 1) << 31) | ((uint32_t)opc << 29) | (0x24u << 23) |
         (nri << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
    return 0;
}

static void
madd(struct section *s, int sf, int o0, int Rd, int Rn, int Rm, int Ra)
{
    w(s, ((uint32_t)(sf & 1) << 31) | (0x1Bu << 24) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)o0 << 15) | ((uint32_t)(Ra & 31) << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
dp2(struct section *s, int sf, int opcode, int Rd, int Rn, int Rm)
{
    w(s, ((uint32_t)(sf & 1) << 31) | (0x0D6u << 21) | ((uint32_t)(Rm & 31) << 16) |
         ((uint32_t)opcode << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
a_sxtw(struct section *s, int Rd, int Rn)
{
    w(s, (1u << 31) | (0x26u << 23) | (1u << 22) | (31u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
a_cset(struct section *s, int sf, int Rd, int cc)
{
    int inv = cc ^ 1;
    w(s, ((uint32_t)(sf & 1) << 31) | (0xD4u << 21) | ((uint32_t)A64_ZR << 16) |
         ((uint32_t)(inv & 0xf) << 12) | (1u << 10) | ((uint32_t)A64_ZR << 5) |
         (uint32_t)(Rd & 31));
}

/* GPR load/store.  size 0=B,1=H,2=W,3=X; opc store=0, load=1, sign->64=2,
   sign->32=3.  force_unscaled picks the unscaled 9-bit form (ldur/stur). */
static void
ldst_gpr(struct section *s, int size, int opc, int Rt, int Rn, long off,
         int force_unscaled)
{
    int scale = size;
    if (!force_unscaled && off >= 0 && (off & ((1 << scale) - 1)) == 0 &&
        (off >> scale) <= 0xfff) {
        unsigned imm12 = (unsigned)(off >> scale);
        w(s, ((uint32_t)size << 30) | (0x39u << 24) | ((uint32_t)opc << 22) |
             (imm12 << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    } else {
        unsigned imm9 = (unsigned)(off & 0x1ff);
        w(s, ((uint32_t)size << 30) | (0x38u << 24) | ((uint32_t)opc << 22) |
             (imm9 << 12) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    }
}

static void
ldst_fp(struct section *s, int sz, int opc, int Rt, int Rn, long off,
        int force_unscaled)
{
    int scale = sz;
    if (!force_unscaled && off >= 0 && (off & ((1 << scale) - 1)) == 0 &&
        (off >> scale) <= 0xfff) {
        unsigned imm12 = (unsigned)(off >> scale);
        w(s, ((uint32_t)sz << 30) | (0x3Du << 24) | ((uint32_t)opc << 22) |
             (imm12 << 10) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    } else {
        unsigned imm9 = (unsigned)(off & 0x1ff);
        w(s, ((uint32_t)sz << 30) | (0x3Cu << 24) | ((uint32_t)opc << 22) |
             (imm9 << 12) | ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rt & 31));
    }
}

static void
fp1(struct section *s, int szf, int opcode, int Rd, int Rn)
{
    w(s, (0x1Eu << 24) | ((uint32_t)ftype(szf) << 22) | (1u << 21) |
         ((uint32_t)opcode << 15) | (0x10u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
fp2(struct section *s, int szf, int opcode, int Rd, int Rn, int Rm)
{
    w(s, (0x1Eu << 24) | ((uint32_t)ftype(szf) << 22) | (1u << 21) |
         ((uint32_t)(Rm & 31) << 16) | ((uint32_t)opcode << 12) | (0x2u << 10) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

static void
fp_fcmp(struct section *s, int szf, int Rn, int Rm)
{
    w(s, (0x1Eu << 24) | ((uint32_t)ftype(szf) << 22) | (1u << 21) |
         ((uint32_t)(Rm & 31) << 16) | (0x8u << 10) |
         ((uint32_t)(Rn & 31) << 5) | 0u);
}

static void
fcvt_int(struct section *s, int sf, int szf, int rmode, int opcode, int Rd, int Rn)
{
    w(s, ((uint32_t)(sf & 1) << 31) | (0x1Eu << 24) | ((uint32_t)ftype(szf) << 22) |
         (1u << 21) | ((uint32_t)rmode << 19) | ((uint32_t)opcode << 16) |
         ((uint32_t)(Rn & 31) << 5) | (uint32_t)(Rd & 31));
}

/****************************************************************
 * Branch / adr relocation or direct resolution
 ****************************************************************/

enum { FIX_IMM26, FIX_IMM19, FIX_ADR };

/* A branch / cbz / adr at insn_off referencing a symbol.  A local symbol
   defined in the current section resolves to a direct displacement patched into
   the word; any other target records a relocation (reloc_type) and leaves the
   field zero. */
static void
branch_fixup(struct a64_asm *a, struct section *s, uint32_t insn_off,
             int fix, int reloc_type, const char *name, int32_t add, int emit)
{
    int idx;

    if (!name)
        die("aarch64: branch/adr operand is not a symbol");
    if (!emit)
        return;

    idx = sym_lookup(&a->st, name);
    if (idx >= 0 && a->st.syms[idx].defined && !a->st.syms[idx].global &&
        !a->st.syms[idx].weak && a->st.syms[idx].section == a->cur_section) {
        long target = (long)a->st.syms[idx].value + add;
        long rel = target - (long)insn_off;
        uint32_t insn = rd_word(s, insn_off);
        if (fix == FIX_ADR) {
            uint32_t immlo = (uint32_t)rel & 0x3u;
            uint32_t immhi = ((uint32_t)rel >> 2) & 0x7FFFFu;
            insn = (insn & 0x9F00001Fu) | (immlo << 29) | (immhi << 5);
        } else if (fix == FIX_IMM26) {
            insn = (insn & 0xFC000000u) | (((uint32_t)(rel >> 2)) & 0x03FFFFFFu);
        } else {                        /* FIX_IMM19 */
            insn = (insn & 0xFF00001Fu) | ((((uint32_t)(rel >> 2)) & 0x7FFFFu) << 5);
        }
        wr_word(s, insn_off, insn);
        return;
    }

    if (idx < 0)
        idx = sym_add(&a->st, name);
    sec_add_reloc_t(s, insn_off, idx, add, reloc_type);
}

/****************************************************************
 * Literal pool
 ****************************************************************/

void
a64_ref_lit(struct a64_asm *a, int size, int is_sym, long value,
            const char *sym, int32_t add, uint32_t insn_off)
{
    int entry = -1, i;

    /* deduplicate: reuse an identical pending entry (GNU as merges literals). */
    for (i = 0; i < a->nlits; i++) {
        struct a64_litent *e = &a->lits[i];
        if (e->size != size || e->is_sym != is_sym)
            continue;
        if (is_sym ? (e->add == add && strcmp(e->sym, sym) == 0)
                   : (e->value == value)) {
            entry = i;
            break;
        }
    }
    if (entry < 0) {
        if (a->nlits >= a->lit_cap) {
            a->lit_cap = a->lit_cap ? a->lit_cap * 2 : 32;
            a->lits = realloc(a->lits, (size_t)a->lit_cap * sizeof(*a->lits));
            if (!a->lits)
                die("out of memory");
        }
        entry = a->nlits++;
        a->lits[entry].size = size;
        a->lits[entry].is_sym = is_sym;
        a->lits[entry].value = value;
        a->lits[entry].sym = sym;
        a->lits[entry].add = add;
    }

    if (a->nlitrefs >= a->litref_cap) {
        a->litref_cap = a->litref_cap ? a->litref_cap * 2 : 64;
        a->litrefs = realloc(a->litrefs, (size_t)a->litref_cap * sizeof(*a->litrefs));
        if (!a->litrefs)
            die("out of memory");
    }
    a->litrefs[a->nlitrefs].insn_off = insn_off;
    a->litrefs[a->nlitrefs].entry = entry;
    a->nlitrefs++;
}

void
a64_flush_litpool(struct a64_asm *a, int emit)
{
    struct section *s = &a->sections[a->cur_section];
    int i, grp;
    static const int group_size[2] = { 4, 8 };

    if (a->nlits == 0)
        return;

    /* Place entries grouped by size, 4-byte before 8-byte, each group aligned
       to its own size and in first-reference order (matching GNU as: no pool-
       wide padding, the 8-byte group aligned to 8). */
    for (grp = 0; grp < 2; grp++) {
        int gsz = group_size[grp];
        int any = 0;
        for (i = 0; i < a->nlits; i++)
            if (a->lits[i].size == gsz) { any = 1; break; }
        if (!any)
            continue;
        while (s->len & (gsz - 1))
            sec_emit8(s, 0);
        for (i = 0; i < a->nlits; i++) {
            struct a64_litent *e = &a->lits[i];
            if (e->size != gsz)
                continue;
            e->slot = (uint32_t)s->len;
            if (e->is_sym) {            /* 8-byte ABS64 to the symbol */
                if (emit) {
                    int idx = sym_lookup(&a->st, e->sym);
                    if (idx < 0) idx = sym_add(&a->st, e->sym);
                    sec_add_reloc_t(s, e->slot, idx, e->add, R_AARCH64_ABS64);
                }
                sec_emit32(s, 0);
                sec_emit32(s, 0);
            } else if (e->size == 8) {
                sec_emit32(s, (uint32_t)e->value);
                sec_emit32(s, (uint32_t)((uint64_t)e->value >> 32));
            } else {
                sec_emit32(s, (uint32_t)e->value);
            }
        }
    }

    /* fix each referencing LDR-literal: imm19 = (slot - insn_off) >> 2 */
    if (emit) {
        for (i = 0; i < a->nlitrefs; i++) {
            struct a64_litref *r = &a->litrefs[i];
            long rel = (long)a->lits[r->entry].slot - (long)r->insn_off;
            uint32_t insn = rd_word(s, r->insn_off);
            insn = (insn & 0xFF00001Fu) |
                   ((((uint32_t)(rel >> 2)) & 0x7FFFFu) << 5);
            wr_word(s, r->insn_off, insn);
        }
    }
    a->nlits = 0;
    a->nlitrefs = 0;
}

/****************************************************************
 * Dispatch
 ****************************************************************/

/* LDR-literal: opc 00=W,01=X; V=0 (gpr) or 1 (fp, opc 00=S,01=D,10=Q).  The
   imm19 is patched when the pool is flushed. */
static uint32_t
ldr_literal_word(int Rt, int fp, int size)
{
    uint32_t opc, V = fp ? 1u : 0u;
    if (fp)
        opc = (size == 8) ? 1u : (size == 4) ? 0u : 0u;
    else
        opc = (size == 8) ? 1u : 0u;
    return (opc << 30) | (0x18u << 24) | (V << 26) | (uint32_t)(Rt & 31);
}

void
a64_encode(struct a64_asm *a, const char *mnem,
           struct a64_operand *ops, int nops, int emit)
{
    struct section *s = &a->sections[a->cur_section];

    if (!strcmp(mnem, "nop"))  { w(s, 0xD503201Fu); return; }
    if (!strcmp(mnem, "ret"))  { w(s, 0xD65F0000u | ((uint32_t)A64_LR << 5)); return; }
    if (!strcmp(mnem, "svc")) {
        unsigned imm = (unsigned)ops[0].disp;
        w(s, 0xD4000001u | ((imm & 0xffff) << 5));
        return;
    }
    if (!strcmp(mnem, "br"))  { w(s, 0xD61F0000u | ((uint32_t)(ops[0].reg & 31) << 5)); return; }
    if (!strcmp(mnem, "blr")) { w(s, 0xD63F0000u | ((uint32_t)(ops[0].reg & 31) << 5)); return; }

    if (!strcmp(mnem, "add") || !strcmp(mnem, "sub")) {
        int op = mnem[0] == 's';
        int sf = gsf(&ops[0]);
        if (!op && ops[2].kind == AO_SYM) {     /* add Xd, Xn, _sym@PAGEOFF */
            uint32_t off = (uint32_t)s->len;
            addsub_imm(s, sf, 0, ops[0].reg, ops[1].reg, 0, 0);
            if (emit) {
                int idx = sym_lookup(&a->st, ops[2].sym);
                if (idx < 0)
                    idx = sym_add(&a->st, ops[2].sym);
                sec_add_reloc_t(s, off, idx, ops[2].add,
                                R_AARCH64_ADD_ABS_LO12_NC);
            }
            return;
        }
        if (ops[2].kind == AO_IMM) {
            int lsl12 = ops[2].lsl12 || (nops > 3 && ops[3].kind == AO_SHIFT);
            addsub_imm(s, sf, op, ops[0].reg, ops[1].reg,
                       (unsigned)ops[2].disp, lsl12);
        } else if (ops[0].is_sp || ops[1].is_sp) {
            addsub_ext(s, sf, op, 0, ops[0].reg, ops[1].reg, ops[2].reg);
        } else {
            addsub_reg(s, sf, op, 0, ops[0].reg, ops[1].reg, ops[2].reg);
        }
        return;
    }
    if (!strcmp(mnem, "cmp")) {
        addsub_reg(s, gsf(&ops[0]), 1, 1, A64_ZR, ops[0].reg, ops[1].reg);
        return;
    }
    if (!strcmp(mnem, "neg")) {
        addsub_reg(s, gsf(&ops[0]), 1, 0, ops[0].reg, A64_ZR, ops[1].reg);
        return;
    }

    if (!strcmp(mnem, "and") || !strcmp(mnem, "orr") || !strcmp(mnem, "eor") ||
        !strcmp(mnem, "bic")) {
        int opc = !strcmp(mnem, "orr") ? 1 : !strcmp(mnem, "eor") ? 2 : 0;
        int sf = gsf(&ops[0]);
        if (ops[2].kind == AO_IMM) {
            uint64_t v = (uint64_t)ops[2].disp;
            if (!strcmp(mnem, "bic"))
                v = ~v;                 /* bic #imm == and #~imm */
            if (sf == 0) v &= 0xffffffffu;
            if (logic_imm(s, sf, opc, ops[0].reg, ops[1].reg, v) < 0)
                die("aarch64: value is not a valid logical immediate");
        } else {
            int N = !strcmp(mnem, "bic") ? 1 : 0;
            logic_reg(s, sf, opc, N, ops[0].reg, ops[1].reg, ops[2].reg);
        }
        return;
    }
    if (!strcmp(mnem, "mvn")) {
        logic_reg(s, gsf(&ops[0]), 1, 1, ops[0].reg, A64_ZR, ops[1].reg);
        return;
    }

    if (!strcmp(mnem, "mov")) {
        if (ops[1].kind == AO_IMM) {            /* mov Rd, #imm */
            mov_imm(s, gsf(&ops[0]), ops[0].reg, ops[1].disp);
            return;
        }
        if (ops[0].rclass == A_RC_FP || ops[1].rclass == A_RC_FP) {
            fp1(s, fsz(ops[0].rclass == A_RC_FP ? &ops[0] : &ops[1]), 0x00,
                ops[0].reg, ops[1].reg);        /* fmov */
            return;
        }
        if (ops[0].is_sp || ops[1].is_sp) {     /* mov to/from sp == add #0 */
            addsub_imm(s, 1, 0, ops[0].reg, ops[1].reg, 0, 0);
            return;
        }
        logic_reg(s, gsf(&ops[0]), 1, 0, ops[0].reg, A64_ZR, ops[1].reg);
        return;
    }

    if (!strcmp(mnem, "mul"))  { madd(s, gsf(&ops[0]), 0, ops[0].reg, ops[1].reg, ops[2].reg, A64_ZR); return; }
    if (!strcmp(mnem, "msub")) { madd(s, gsf(&ops[0]), 1, ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg); return; }
    if (!strcmp(mnem, "sdiv")) { dp2(s, gsf(&ops[0]), 0x03, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "udiv")) { dp2(s, gsf(&ops[0]), 0x02, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "lsl"))  { dp2(s, gsf(&ops[0]), 0x08, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "lsr"))  { dp2(s, gsf(&ops[0]), 0x09, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "asr"))  { dp2(s, gsf(&ops[0]), 0x0A, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "sxtw")) { a_sxtw(s, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "cset")) { a_cset(s, gsf(&ops[0]), ops[0].reg, ops[1].cond); return; }

    /* Loads and stores */
    if (!strncmp(mnem, "ldr", 3) || !strncmp(mnem, "str", 3) ||
        !strcmp(mnem, "ldur") || !strcmp(mnem, "stur") ||
        !strcmp(mnem, "ldrb") || !strcmp(mnem, "ldrsb") ||
        !strcmp(mnem, "ldrh") || !strcmp(mnem, "ldrsh") ||
        !strcmp(mnem, "strb") || !strcmp(mnem, "strh")) {
        struct a64_operand *rt = &ops[0], *m = &ops[1];
        int is_store = mnem[0] == 's' && mnem[1] == 't';
        int unscaled = !strcmp(mnem, "ldur") || !strcmp(mnem, "stur");

        /* ldr Rt, =literal : queue a pool entry and emit the LDR-literal word. */
        if (!strcmp(mnem, "ldr") && m->kind == AO_LIT) {
            uint32_t off = (uint32_t)s->len;
            int fp = rt->rclass == A_RC_FP;
            w(s, ldr_literal_word(rt->reg, fp, rt->size));
            a64_ref_lit(a, rt->size, m->is_sym, m->disp, m->sym, m->add, off);
            return;
        }

        if (rt->rclass == A_RC_FP) {            /* ldr/str Dt/St [mem] */
            ldst_fp(s, fsz(rt), is_store ? 0 : 1, rt->reg, m->base, m->disp,
                    unscaled);
            return;
        }
        {
            int size, opc;
            if (!strcmp(mnem, "ldrb"))  { size = 0; opc = 1; }
            else if (!strcmp(mnem, "ldrsb")) { size = 0; opc = rt->size == 8 ? 2 : 3; }
            else if (!strcmp(mnem, "ldrh"))  { size = 1; opc = 1; }
            else if (!strcmp(mnem, "ldrsh")) { size = 1; opc = rt->size == 8 ? 2 : 3; }
            else if (!strcmp(mnem, "strb"))  { size = 0; opc = 0; }
            else if (!strcmp(mnem, "strh"))  { size = 1; opc = 0; }
            else { size = rt->size == 8 ? 3 : 2; opc = is_store ? 0 : 1; }
            ldst_gpr(s, size, opc, rt->reg, m->base, m->disp, unscaled);
        }
        return;
    }

    /* FP data processing */
    if (!strcmp(mnem, "fmov")) { fp1(s, fsz(&ops[0]), 0x00, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fabs")) { fp1(s, fsz(&ops[0]), 0x01, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fneg")) { fp1(s, fsz(&ops[0]), 0x02, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fsqrt")){ fp1(s, fsz(&ops[0]), 0x03, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fmul")) { fp2(s, fsz(&ops[0]), 0x0, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "fdiv")) { fp2(s, fsz(&ops[0]), 0x1, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "fadd")) { fp2(s, fsz(&ops[0]), 0x2, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "fsub")) { fp2(s, fsz(&ops[0]), 0x3, ops[0].reg, ops[1].reg, ops[2].reg); return; }
    if (!strcmp(mnem, "fcmp")) { fp_fcmp(s, fsz(&ops[0]), ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fcvt")) {        /* fcvt Dd, Sn : dst size from ops[0] */
        int opcode = 0x04 | ftype(fsz(&ops[0]));
        fp1(s, fsz(&ops[1]), opcode, ops[0].reg, ops[1].reg);
        return;
    }
    if (!strcmp(mnem, "scvtf"))  { fcvt_int(s, gsf(&ops[1]), fsz(&ops[0]), 0, 2, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fcvtzs")) { fcvt_int(s, gsf(&ops[0]), fsz(&ops[1]), 3, 0, ops[0].reg, ops[1].reg); return; }
    if (!strcmp(mnem, "fcvtns")) { fcvt_int(s, gsf(&ops[0]), fsz(&ops[1]), 0, 0, ops[0].reg, ops[1].reg); return; }

    /* Control flow with PC-relative targets */
    if (!strcmp(mnem, "b") || !strcmp(mnem, "bl")) {
        uint32_t off = (uint32_t)s->len;
        int isbl = mnem[1] == 'l';
        w(s, isbl ? 0x94000000u : 0x14000000u);
        branch_fixup(a, s, off, FIX_IMM26,
                     isbl ? R_AARCH64_CALL26 : R_AARCH64_JUMP26,
                     ops[0].sym, ops[0].add, emit);
        return;
    }
    if (!strcmp(mnem, "cbz") || !strcmp(mnem, "cbnz")) {
        uint32_t off = (uint32_t)s->len;
        int nz = !strcmp(mnem, "cbnz");
        w(s, ((uint32_t)gsf(&ops[0]) << 31) | (nz ? 0x35000000u : 0x34000000u) |
             (uint32_t)(ops[0].reg & 31));
        branch_fixup(a, s, off, FIX_IMM19, R_AARCH64_CONDBR19,
                     ops[1].sym, ops[1].add, emit);
        return;
    }
    if (!strcmp(mnem, "adr")) {
        uint32_t off = (uint32_t)s->len;
        w(s, 0x10000000u | (uint32_t)(ops[0].reg & 31));
        branch_fixup(a, s, off, FIX_ADR, R_AARCH64_ADR_PREL_LO21,
                     ops[1].sym, ops[1].add, emit);
        return;
    }
    if (!strcmp(mnem, "adrp")) {         /* adrp Xd, _sym@PAGE (always relocated) */
        uint32_t off = (uint32_t)s->len;
        w(s, 0x90000000u | (uint32_t)(ops[0].reg & 31));
        if (emit) {
            int idx = sym_lookup(&a->st, ops[1].sym);
            if (idx < 0)
                idx = sym_add(&a->st, ops[1].sym);
            sec_add_reloc_t(s, off, idx, ops[1].add, R_AARCH64_ADR_PREL_PG_HI21);
        }
        return;
    }

    die("aarch64: unknown mnemonic '%s'", mnem);
}
