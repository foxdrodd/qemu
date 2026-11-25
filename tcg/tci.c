/*
 * Tiny Code Interpreter for QEMU
 *
 * Copyright (c) 2009, 2011, 2016 Stefan Weil
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/helper-info.h"
#include "tcg/tcg-ldst.h"
#include "disas/dis-asm.h"
#include "tcg-has.h"
#include <ffi.h>
#ifndef CONFIG_USER_ONLY
#include "hw/core/cpu.h"
#include "exec/target_page.h"
#include "exec/tlb-common.h"
#include "exec/memop.h"
#include "qemu/bswap.h"
#endif

#ifdef TCI_DIRECT_CALL
/*
 * Call func with the arguments in stack[] (one 64-bit slot each) through a
 * pointer of its exact type; the result goes to R0 like after libffi.
 * Returns false for signatures that need libffi (Int128).
 * Kept out of line: inlined, its ~770 call sites (each with Asyncify
 * bookkeeping) bloat the interpreter loop and slow everything else down.
 */
static __attribute__((noinline)) bool tci_call_direct(uint32_t sig, void *func,
                                          uint64_t *stack,
                                          tcg_target_ulong *regs)
{
    switch (sig) {
#include "tci-direct-call.c.inc"
    default:
        return false;
    }
    return true;
}
#endif


/*
 * Enable TCI assertions only when debugging TCG (and without NDEBUG defined).
 * Without assertions, the interpreter runs much faster.
 */
#if defined(CONFIG_DEBUG_TCG)
# define tci_assert(cond) assert(cond)
#else
# define tci_assert(cond) ((void)(cond))
#endif

__thread uintptr_t tci_tb_ptr;

/*
 * Load sets of arguments all at once.  The naming convention is:
 *   tci_args_<arguments>
 * where arguments is a sequence of
 *
 *   b = immediate (bit position)
 *   c = condition (TCGCond)
 *   i = immediate (uint32_t)
 *   I = immediate (tcg_target_ulong)
 *   l = label or pointer
 *   m = immediate (MemOpIdx)
 *   n = immediate (call return length)
 *   r = register
 *   s = signed ldst offset
 */

static void tci_args_l(uint32_t insn, const void *tb_ptr, void **l0)
{
    int diff = sextract32(insn, 12, 20);
    *l0 = diff ? (void *)tb_ptr + diff : NULL;
}

static void tci_args_r(uint32_t insn, TCGReg *r0)
{
    *r0 = extract32(insn, 8, 4);
}

static void tci_args_nl(uint32_t insn, const void *tb_ptr,
                        uint8_t *n0, void **l1)
{
    *n0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rl(uint32_t insn, const void *tb_ptr,
                        TCGReg *r0, void **l1)
{
    *r0 = extract32(insn, 8, 4);
    *l1 = sextract32(insn, 12, 20) + (void *)tb_ptr;
}

static void tci_args_rr(uint32_t insn, TCGReg *r0, TCGReg *r1)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
}

static void tci_args_ri(uint32_t insn, TCGReg *r0, tcg_target_ulong *i1)
{
    *r0 = extract32(insn, 8, 4);
    *i1 = sextract32(insn, 12, 20);
}

/* qemu_ld/st: data and address registers, MemOpIdx in the next word. */
static void tci_args_rrw(uint32_t insn, const uint32_t **tb_ptr,
                         TCGReg *r0, TCGReg *r1, uint64_t *taddr,
                         const tcg_target_ulong *regs, MemOpIdx *m2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *taddr = extract32(insn, 16, 1) ? (uint32_t)regs[*r1] : regs[*r1];
    *m2 = *(*tb_ptr)++;
}

static void tci_args_rrc(uint32_t insn, TCGReg *r0, TCGReg *r1, TCGCond *c2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *c2 = extract32(insn, 16, 4);
}

static void tci_args_rci(uint32_t insn, TCGReg *r0, TCGCond *c1,
                         tcg_target_ulong *i2)
{
    *r0 = extract32(insn, 8, 4);
    *c1 = extract32(insn, 12, 4);
    *i2 = sextract32(insn, 16, 16);
}

/* Store of a constant: base register and offset, value in the next word. */
static void *tci_args_sti(uint32_t insn, const uint32_t **tb_ptr,
                          const tcg_target_ulong *regs, tcg_target_ulong *val)
{
    *val = (int32_t)*(*tb_ptr)++;
    return (void *)(regs[extract32(insn, 8, 4)] + sextract32(insn, 16, 16));
}

static void tci_args_rrr(uint32_t insn, TCGReg *r0, TCGReg *r1, TCGReg *r2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
}

static void tci_args_rrs(uint32_t insn, TCGReg *r0, TCGReg *r1, int32_t *i2)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = sextract32(insn, 16, 16);
}

static void tci_args_rrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                          uint8_t *i2, uint8_t *i3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *i2 = extract32(insn, 16, 6);
    *i3 = extract32(insn, 22, 6);
}

static void tci_args_rrrc(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGCond *c3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *c3 = extract32(insn, 20, 4);
}

static void tci_args_rrrbb(uint32_t insn, TCGReg *r0, TCGReg *r1,
                           TCGReg *r2, uint8_t *i3, uint8_t *i4)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *i3 = extract32(insn, 20, 6);
    *i4 = extract32(insn, 26, 6);
}

static void tci_args_rrrr(uint32_t insn,
                          TCGReg *r0, TCGReg *r1, TCGReg *r2, TCGReg *r3)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
}

static void tci_args_rrrrrc(uint32_t insn, TCGReg *r0, TCGReg *r1,
                            TCGReg *r2, TCGReg *r3, TCGReg *r4, TCGCond *c5)
{
    *r0 = extract32(insn, 8, 4);
    *r1 = extract32(insn, 12, 4);
    *r2 = extract32(insn, 16, 4);
    *r3 = extract32(insn, 20, 4);
    *r4 = extract32(insn, 24, 4);
    *c5 = extract32(insn, 28, 4);
}

static bool tci_compare32(uint32_t u0, uint32_t u1, TCGCond condition)
{
    bool result = false;
    int32_t i0 = u0;
    int32_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static bool tci_compare64(uint64_t u0, uint64_t u1, TCGCond condition)
{
    bool result = false;
    int64_t i0 = u0;
    int64_t i1 = u1;
    switch (condition) {
    case TCG_COND_EQ:
        result = (u0 == u1);
        break;
    case TCG_COND_NE:
        result = (u0 != u1);
        break;
    case TCG_COND_LT:
        result = (i0 < i1);
        break;
    case TCG_COND_GE:
        result = (i0 >= i1);
        break;
    case TCG_COND_LE:
        result = (i0 <= i1);
        break;
    case TCG_COND_GT:
        result = (i0 > i1);
        break;
    case TCG_COND_LTU:
        result = (u0 < u1);
        break;
    case TCG_COND_GEU:
        result = (u0 >= u1);
        break;
    case TCG_COND_LEU:
        result = (u0 <= u1);
        break;
    case TCG_COND_GTU:
        result = (u0 > u1);
        break;
    case TCG_COND_TSTEQ:
        result = (u0 & u1) == 0;
        break;
    case TCG_COND_TSTNE:
        result = (u0 & u1) != 0;
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

#ifndef CONFIG_USER_ONLY
/*
 * Fast path for guest memory accesses, as the native backends emit inline:
 * an aligned access within one page whose TLB entry matches without any
 * flag bits (so: RAM, no watchpoint, not dirty-tracked, no byte swap
 * forced) goes straight to host memory.  Everything else (TLB miss, MMIO,
 * unaligned, page-crossing) takes the helper path.  Returns the host
 * address, or NULL.
 */
static inline void *tci_tlb_host(CPUArchState *env, uint64_t addr,
                                 MemOpIdx oi, bool is_ld)
{
    MemOp mop = get_memop(oi);
    unsigned size = memop_size(mop);
    uint64_t a_mask = (1u << memop_alignment_bits(mop)) - 1;
    /* The inverse of cpu_env(): CPUArchState follows CPUState. */
    CPUTLBDescFast *fast = cpu_tlb_fast((CPUState *)env - 1, get_mmuidx(oi));
    uintptr_t ofs = (addr >> (TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS)) &
                    fast->mask;
    CPUTLBEntry *e = (CPUTLBEntry *)((char *)fast->table + ofs);
    uint64_t page = addr & (uint64_t)(int64_t)TARGET_PAGE_MASK;
    uintptr_t cmp = is_ld ? e->addr_read : qatomic_read(&e->addr_write);

    if ((addr & a_mask) || cmp != page ||
        ((addr + size - 1) & (uint64_t)(int64_t)TARGET_PAGE_MASK) != page) {
        return NULL;
    }
    return (void *)(uintptr_t)(addr + e->addend);
}
#endif

static uint64_t tci_qemu_ld(CPUArchState *env, uint64_t taddr,
                            MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

#ifndef CONFIG_USER_ONLY
    void *h = tci_tlb_host(env, taddr, oi, true);

    if (h) {
        uint64_t v;

        switch (mop & MO_SIZE) {
        case MO_8:
            v = *(uint8_t *)h;
            break;
        case MO_16:
            v = (uint16_t)lduw_he_p(h);
            v = mop & MO_BSWAP ? bswap16(v) : v;
            break;
        case MO_32:
            v = (uint32_t)ldl_he_p(h);
            v = mop & MO_BSWAP ? bswap32(v) : v;
            break;
        default:
            v = ldq_he_p(h);
            v = mop & MO_BSWAP ? bswap64(v) : v;
            break;
        }
        if (mop & MO_SIGN) {
            switch (mop & MO_SIZE) {
            case MO_8:
                return (int8_t)v;
            case MO_16:
                return (int16_t)v;
            case MO_32:
                return (int32_t)v;
            }
        }
        return v;
    }
#endif

    switch (mop & MO_SSIZE) {
    case MO_UB:
        return helper_ldub_mmu(env, taddr, oi, ra);
    case MO_SB:
        return helper_ldsb_mmu(env, taddr, oi, ra);
    case MO_UW:
        return helper_lduw_mmu(env, taddr, oi, ra);
    case MO_SW:
        return helper_ldsw_mmu(env, taddr, oi, ra);
    case MO_UL:
        return helper_ldul_mmu(env, taddr, oi, ra);
    case MO_SL:
        return helper_ldsl_mmu(env, taddr, oi, ra);
    case MO_UQ:
        return helper_ldq_mmu(env, taddr, oi, ra);
    default:
        g_assert_not_reached();
    }
}

static void tci_qemu_st(CPUArchState *env, uint64_t taddr, uint64_t val,
                        MemOpIdx oi, const void *tb_ptr)
{
    MemOp mop = get_memop(oi);
    uintptr_t ra = (uintptr_t)tb_ptr;

#ifndef CONFIG_USER_ONLY
    void *h = tci_tlb_host(env, taddr, oi, false);

    if (h) {
        switch (mop & MO_SIZE) {
        case MO_8:
            *(uint8_t *)h = val;
            break;
        case MO_16:
            stw_he_p(h, mop & MO_BSWAP ? bswap16(val) : val);
            break;
        case MO_32:
            stl_he_p(h, mop & MO_BSWAP ? bswap32(val) : val);
            break;
        default:
            stq_he_p(h, mop & MO_BSWAP ? bswap64(val) : val);
            break;
        }
        return;
    }
#endif

    switch (mop & MO_SIZE) {
    case MO_UB:
        helper_stb_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UW:
        helper_stw_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UL:
        helper_stl_mmu(env, taddr, val, oi, ra);
        break;
    case MO_UQ:
        helper_stq_mmu(env, taddr, val, oi, ra);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Interpret pseudo code in tb. */
/*
 * Disable CFI checks.
 * One possible operation in the pseudo code is a call to binary code.
 * Therefore, disable CFI checks in the interpreter function
 */
uintptr_t QEMU_DISABLE_CFI tcg_qemu_tb_exec(CPUArchState *env,
                                            const void *v_tb_ptr)
{
    const uint32_t *tb_ptr = v_tb_ptr;
    tcg_target_ulong regs[TCG_TARGET_NB_REGS];
    uint64_t stack[(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE)
                   / sizeof(uint64_t)];
    bool carry = false;

    regs[TCG_AREG0] = (tcg_target_ulong)env;
    regs[TCG_REG_CALL_STACK] = (uintptr_t)stack;
    tci_assert(tb_ptr);

    for (;;) {
        uint32_t insn;
        TCGOpcode opc;
        TCGReg r0, r1, r2, r3, r4;
        tcg_target_ulong t1;
        TCGCond condition;
        uint8_t pos, len;
        uint32_t tmp32;
        uint64_t taddr;
        MemOpIdx oi;
        int32_t ofs;
        void *ptr;

        insn = *tb_ptr++;
        opc = extract32(insn, 0, 8);

        switch (opc) {
        case INDEX_op_call:
            {
                void *call_slots[MAX_CALL_IARGS];
                ffi_cif *cif;
                void *func;
                unsigned i, s, n;

                tci_args_nl(insn, tb_ptr, &len, &ptr);
                func = ((void **)ptr)[0];
                cif = ((void **)ptr)[1];

#ifdef TCI_DIRECT_CALL
                /* Helper functions may need to access the "return address" */
                tci_tb_ptr = (uintptr_t)tb_ptr;
                if (tci_call_direct(((TCIHelperCif *)cif)->sig, func,
                                    stack, regs)) {
                    break;
                }
#endif
                n = cif->nargs;
                for (i = s = 0; i < n; ++i) {
                    ffi_type *t = cif->arg_types[i];
                    call_slots[i] = &stack[s];
                    s += DIV_ROUND_UP(t->size, 8);
                }

                /* Helper functions may need to access the "return address" */
                tci_tb_ptr = (uintptr_t)tb_ptr;
                ffi_call(cif, func, stack, call_slots);
            }

            switch (len) {
            case 0: /* void */
                break;
            case 1: /* uint32_t */
                /*
                 * The result winds up "left-aligned" in the stack[0] slot.
                 * Note that libffi has an odd special case in that it will
                 * always widen an integral result to ffi_arg.
                 */
                if (sizeof(ffi_arg) == 8) {
                    regs[TCG_REG_R0] = (uint32_t)stack[0];
                } else {
                    regs[TCG_REG_R0] = *(uint32_t *)stack;
                }
                break;
            case 2: /* uint64_t */
                memcpy(&regs[TCG_REG_R0], stack, 8);
                break;
            case 3: /* Int128 */
                memcpy(&regs[TCG_REG_R0], stack, 16);
                break;
            default:
                g_assert_not_reached();
            }
            break;

        case INDEX_op_br:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = ptr;
            continue;
        case INDEX_op_setcond:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare64(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_movcond:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare64(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;
        case INDEX_op_mov:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = regs[r1];
            break;
        case INDEX_op_tci_movi:
            tci_args_ri(insn, &r0, &t1);
            regs[r0] = t1;
            break;
        case INDEX_op_tci_movl:
            tci_args_rl(insn, tb_ptr, &r0, &ptr);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_tci_setcarry:
            carry = true;
            break;

            /* Load/store operations (32 bit). */

        case INDEX_op_ld8u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint8_t *)ptr;
            break;
        case INDEX_op_ld8s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int8_t *)ptr;
            break;
        case INDEX_op_ld16u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint16_t *)ptr;
            break;
        case INDEX_op_ld16s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int16_t *)ptr;
            break;
        case INDEX_op_ld:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(tcg_target_ulong *)ptr;
            break;
        case INDEX_op_st8:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint8_t *)ptr = regs[r0];
            break;
        case INDEX_op_st16:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint16_t *)ptr = regs[r0];
            break;
        case INDEX_op_st:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(tcg_target_ulong *)ptr = regs[r0];
            break;
        case INDEX_op_tci_st8i:
            ptr = tci_args_sti(insn, &tb_ptr, regs, &t1);
            *(uint8_t *)ptr = t1;
            break;
        case INDEX_op_tci_st16i:
            ptr = tci_args_sti(insn, &tb_ptr, regs, &t1);
            *(uint16_t *)ptr = t1;
            break;
        case INDEX_op_tci_st32i:
            ptr = tci_args_sti(insn, &tb_ptr, regs, &t1);
            *(uint32_t *)ptr = t1;
            break;
        case INDEX_op_tci_sti:
            ptr = tci_args_sti(insn, &tb_ptr, regs, &t1);
            *(tcg_target_ulong *)ptr = t1;
            break;

            /* Arithmetic operations (mixed 32/64 bit). */

        case INDEX_op_add:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2];
            break;
        case INDEX_op_tci_addi:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            regs[r0] = regs[r1] + ofs;
            break;
        case INDEX_op_sub:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_mul:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] * regs[r2];
            break;
        case INDEX_op_and:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & regs[r2];
            break;
        case INDEX_op_or:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | regs[r2];
            break;
        case INDEX_op_xor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ^ regs[r2];
            break;
        case INDEX_op_andc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] & ~regs[r2];
            break;
        case INDEX_op_orc:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] | ~regs[r2];
            break;
        case INDEX_op_eqv:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] ^ regs[r2]);
            break;
        case INDEX_op_nand:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] & regs[r2]);
            break;
        case INDEX_op_nor:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ~(regs[r1] | regs[r2]);
            break;
        case INDEX_op_neg:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = -regs[r1];
            break;
        case INDEX_op_not:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ~regs[r1];
            break;
        case INDEX_op_ctpop:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = ctpop64(regs[r1]);
            break;
        case INDEX_op_addco:
            tci_args_rrr(insn, &r0, &r1, &r2);
            t1 = regs[r1] + regs[r2];
            carry = t1 < regs[r1];
            regs[r0] = t1;
            break;
        case INDEX_op_addci:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] + regs[r2] + carry;
            break;
        case INDEX_op_addcio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                t1 = regs[r1] + regs[r2] + 1;
                carry = t1 <= regs[r1];
            } else {
                t1 = regs[r1] + regs[r2];
                carry = t1 < regs[r1];
            }
            regs[r0] = t1;
            break;
        case INDEX_op_subbo:
            tci_args_rrr(insn, &r0, &r1, &r2);
            carry = regs[r1] < regs[r2];
            regs[r0] = regs[r1] - regs[r2];
            break;
        case INDEX_op_subbi:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] - regs[r2] - carry;
            break;
        case INDEX_op_subbio:
            tci_args_rrr(insn, &r0, &r1, &r2);
            if (carry) {
                carry = regs[r1] <= regs[r2];
                regs[r0] = regs[r1] - regs[r2] - 1;
            } else {
                carry = regs[r1] < regs[r2];
                regs[r0] = regs[r1] - regs[r2];
            }
            break;
        case INDEX_op_muls2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            muls64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
            break;
        case INDEX_op_mulu2:
            tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
            mulu64(&regs[r0], &regs[r1], regs[r2], regs[r3]);
            break;

            /* Arithmetic operations (32 bit). */

        case INDEX_op_tci_divs32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] / (int32_t)regs[r2];
            break;
        case INDEX_op_tci_divu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] / (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_rems32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int32_t)regs[r1] % (int32_t)regs[r2];
            break;
        case INDEX_op_tci_remu32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint32_t)regs[r1] % (uint32_t)regs[r2];
            break;
        case INDEX_op_tci_clz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? clz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_ctz32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            tmp32 = regs[r1];
            regs[r0] = tmp32 ? ctz32(tmp32) : regs[r2];
            break;
        case INDEX_op_tci_setcond32:
            tci_args_rrrc(insn, &r0, &r1, &r2, &condition);
            regs[r0] = tci_compare32(regs[r1], regs[r2], condition);
            break;
        case INDEX_op_tci_movcond32:
            tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &condition);
            tmp32 = tci_compare32(regs[r1], regs[r2], condition);
            regs[r0] = regs[tmp32 ? r3 : r4];
            break;

            /* Shift/rotate operations. */

        case INDEX_op_shl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] << (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_shr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] >> (regs[r2] % TCG_TARGET_REG_BITS);
            break;
        case INDEX_op_sar:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ((tcg_target_long)regs[r1]
                        >> (regs[r2] % TCG_TARGET_REG_BITS));
            break;
        case INDEX_op_tci_rotl32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_tci_rotr32:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror32(regs[r1], regs[r2] & 31);
            break;
        case INDEX_op_deposit:
            tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
            regs[r0] = deposit64(regs[r1], pos, len, regs[r2]);
            break;
        case INDEX_op_extract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = extract64(regs[r1], pos, len);
            break;
        case INDEX_op_sextract:
            tci_args_rrbb(insn, &r0, &r1, &pos, &len);
            regs[r0] = sextract64(regs[r1], pos, len);
            break;
        case INDEX_op_brcond:
            tci_args_rrc(insn, &r0, &r1, &condition);
            ofs = *tb_ptr++;
            if (tci_compare64(regs[r0], regs[r1], condition)) {
                tb_ptr = (const void *)tb_ptr + ofs;
            }
            break;
        case INDEX_op_tci_brcond32:
            tci_args_rrc(insn, &r0, &r1, &condition);
            ofs = *tb_ptr++;
            if (tci_compare32(regs[r0], regs[r1], condition)) {
                tb_ptr = (const void *)tb_ptr + ofs;
            }
            break;
        case INDEX_op_tci_brcondi:
            tci_args_rci(insn, &r0, &condition, &t1);
            ofs = *tb_ptr++;
            if (tci_compare64(regs[r0], t1, condition)) {
                tb_ptr = (const void *)tb_ptr + ofs;
            }
            break;
        case INDEX_op_tci_brcondi32:
            tci_args_rci(insn, &r0, &condition, &t1);
            ofs = *tb_ptr++;
            if (tci_compare32(regs[r0], t1, condition)) {
                tb_ptr = (const void *)tb_ptr + ofs;
            }
            break;
        case INDEX_op_bswap16:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap16(regs[r1]);
            break;
        case INDEX_op_bswap32:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap32(regs[r1]);
            break;

            /* Load/store operations (64 bit). */

        case INDEX_op_ld32u:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(uint32_t *)ptr;
            break;
        case INDEX_op_ld32s:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            regs[r0] = *(int32_t *)ptr;
            break;
        case INDEX_op_st32:
            tci_args_rrs(insn, &r0, &r1, &ofs);
            ptr = (void *)(regs[r1] + ofs);
            *(uint32_t *)ptr = regs[r0];
            break;

            /* Arithmetic operations (64 bit). */

        case INDEX_op_divs:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] / (int64_t)regs[r2];
            break;
        case INDEX_op_divu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] / (uint64_t)regs[r2];
            break;
        case INDEX_op_rems:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (int64_t)regs[r1] % (int64_t)regs[r2];
            break;
        case INDEX_op_remu:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = (uint64_t)regs[r1] % (uint64_t)regs[r2];
            break;
        case INDEX_op_clz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? clz64(regs[r1]) : regs[r2];
            break;
        case INDEX_op_ctz:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = regs[r1] ? ctz64(regs[r1]) : regs[r2];
            break;

            /* Shift/rotate operations (64 bit). */

        case INDEX_op_rotl:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = rol64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_rotr:
            tci_args_rrr(insn, &r0, &r1, &r2);
            regs[r0] = ror64(regs[r1], regs[r2] & 63);
            break;
        case INDEX_op_ext_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (int32_t)regs[r1];
            break;
        case INDEX_op_extu_i32_i64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = (uint32_t)regs[r1];
            break;
        case INDEX_op_bswap64:
            tci_args_rr(insn, &r0, &r1);
            regs[r0] = bswap64(regs[r1]);
            break;

            /* QEMU specific operations. */

        case INDEX_op_exit_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            return (uintptr_t)ptr;

        case INDEX_op_goto_tb:
            tci_args_l(insn, tb_ptr, &ptr);
            tb_ptr = *(void **)ptr;
            break;

        case INDEX_op_goto_ptr:
            tci_args_r(insn, &r0);
            ptr = (void *)regs[r0];
            if (!ptr) {
                return 0;
            }
            tb_ptr = ptr;
            break;

        case INDEX_op_qemu_ld:
            tci_args_rrw(insn, &tb_ptr, &r0, &r1, &taddr, regs, &oi);
            regs[r0] = tci_qemu_ld(env, taddr, oi, tb_ptr);
            break;

        case INDEX_op_qemu_st:
            tci_args_rrw(insn, &tb_ptr, &r0, &r1, &taddr, regs, &oi);
            tci_qemu_st(env, taddr, regs[r0], oi, tb_ptr);
            break;

        case INDEX_op_mb:
            /* Ensure ordering for all kinds */
            smp_mb();
            break;
        default:
            g_assert_not_reached();
        }
    }
}

/*
 * Disassembler that matches the interpreter
 */

static const char *str_r(TCGReg r)
{
    static const char regs[TCG_TARGET_NB_REGS][4] = {
        "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "env", "sp"
    };

    QEMU_BUILD_BUG_ON(TCG_AREG0 != TCG_REG_R14);
    QEMU_BUILD_BUG_ON(TCG_REG_CALL_STACK != TCG_REG_R15);

    assert((unsigned)r < TCG_TARGET_NB_REGS);
    return regs[r];
}

static const char *str_c(TCGCond c)
{
    static const char cond[16][8] = {
        [TCG_COND_NEVER] = "never",
        [TCG_COND_ALWAYS] = "always",
        [TCG_COND_EQ] = "eq",
        [TCG_COND_NE] = "ne",
        [TCG_COND_LT] = "lt",
        [TCG_COND_GE] = "ge",
        [TCG_COND_LE] = "le",
        [TCG_COND_GT] = "gt",
        [TCG_COND_LTU] = "ltu",
        [TCG_COND_GEU] = "geu",
        [TCG_COND_LEU] = "leu",
        [TCG_COND_GTU] = "gtu",
        [TCG_COND_TSTEQ] = "tsteq",
        [TCG_COND_TSTNE] = "tstne",
    };

    assert((unsigned)c < ARRAY_SIZE(cond));
    assert(cond[c][0] != 0);
    return cond[c];
}

/* Disassemble TCI bytecode. */
int print_insn_tci(bfd_vma addr, disassemble_info *info)
{
    const uint32_t *tb_ptr = (const void *)(uintptr_t)addr;
    const TCGOpDef *def;
    const char *op_name;
    uint32_t insn;
    TCGOpcode op;
    TCGReg r0, r1, r2, r3, r4;
    tcg_target_ulong i1;
    int32_t s2;
    TCGCond c;
    MemOpIdx oi;
    uint8_t pos, len;
    void *ptr;

    /* TCI is always the host, so we don't need to load indirect. */
    insn = *tb_ptr++;

    info->fprintf_func(info->stream, "%08x  ", insn);

    op = extract32(insn, 0, 8);
    def = &tcg_op_defs[op];
    op_name = def->name;

    switch (op) {
    case INDEX_op_br:
    case INDEX_op_exit_tb:
    case INDEX_op_goto_tb:
        tci_args_l(insn, tb_ptr, &ptr);
        info->fprintf_func(info->stream, "%-12s  %p", op_name, ptr);
        break;

    case INDEX_op_goto_ptr:
        tci_args_r(insn, &r0);
        info->fprintf_func(info->stream, "%-12s  %s", op_name, str_r(r0));
        break;

    case INDEX_op_call:
        tci_args_nl(insn, tb_ptr, &len, &ptr);
        info->fprintf_func(info->stream, "%-12s  %d, %p", op_name, len, ptr);
        break;

    case INDEX_op_tci_brcondi:
    case INDEX_op_tci_brcondi32:
        r0 = extract32(insn, 8, 4);
        c = extract32(insn, 12, 4);
        i1 = sextract32(insn, 16, 16);
        s2 = *tb_ptr++;
        info->fprintf_func(info->stream, "%-12s  %s, %" PRId64 ", %s, %p",
                           op_name, str_r(r0), (int64_t)i1, str_c(c),
                           (const void *)tb_ptr + s2);
        return 2 * sizeof(insn);

    case INDEX_op_tci_addi:
        tci_args_rrs(insn, &r0, &r1, &s2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %d",
                           op_name, str_r(r0), str_r(r1), s2);
        break;

    case INDEX_op_tci_st8i:
    case INDEX_op_tci_st16i:
    case INDEX_op_tci_st32i:
    case INDEX_op_tci_sti:
        r1 = extract32(insn, 8, 4);
        s2 = sextract32(insn, 16, 16);
        i1 = (int32_t)*tb_ptr++;
        info->fprintf_func(info->stream, "%-12s  %" PRId64 ", %s, %d",
                           op_name, (int64_t)i1, str_r(r1), s2);
        return 2 * sizeof(insn);

    case INDEX_op_brcond:
    case INDEX_op_tci_brcond32:
        tci_args_rrc(insn, &r0, &r1, &c);
        s2 = *tb_ptr++;
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %p",
                           op_name, str_r(r0), str_r(r1), str_c(c),
                           (const void *)tb_ptr + s2);
        return 2 * sizeof(insn);

    case INDEX_op_setcond:
    case INDEX_op_tci_setcond32:
        tci_args_rrrc(insn, &r0, &r1, &r2, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2), str_c(c));
        break;

    case INDEX_op_tci_movi:
        tci_args_ri(insn, &r0, &i1);
        info->fprintf_func(info->stream, "%-12s  %s, 0x%" TCG_PRIlx,
                           op_name, str_r(r0), i1);
        break;

    case INDEX_op_tci_movl:
        tci_args_rl(insn, tb_ptr, &r0, &ptr);
        info->fprintf_func(info->stream, "%-12s  %s, %p",
                           op_name, str_r(r0), ptr);
        break;

    case INDEX_op_tci_setcarry:
        info->fprintf_func(info->stream, "%-12s", op_name);
        break;

    case INDEX_op_ld8u:
    case INDEX_op_ld8s:
    case INDEX_op_ld16u:
    case INDEX_op_ld16s:
    case INDEX_op_ld32u:
    case INDEX_op_ld:
    case INDEX_op_st8:
    case INDEX_op_st16:
    case INDEX_op_st32:
    case INDEX_op_st:
        tci_args_rrs(insn, &r0, &r1, &s2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %d",
                           op_name, str_r(r0), str_r(r1), s2);
        break;

    case INDEX_op_bswap16:
    case INDEX_op_bswap32:
    case INDEX_op_ctpop:
    case INDEX_op_mov:
    case INDEX_op_neg:
    case INDEX_op_not:
    case INDEX_op_ext_i32_i64:
    case INDEX_op_extu_i32_i64:
    case INDEX_op_bswap64:
        tci_args_rr(insn, &r0, &r1);
        info->fprintf_func(info->stream, "%-12s  %s, %s",
                           op_name, str_r(r0), str_r(r1));
        break;

    case INDEX_op_add:
    case INDEX_op_addci:
    case INDEX_op_addcio:
    case INDEX_op_addco:
    case INDEX_op_and:
    case INDEX_op_andc:
    case INDEX_op_clz:
    case INDEX_op_ctz:
    case INDEX_op_divs:
    case INDEX_op_divu:
    case INDEX_op_eqv:
    case INDEX_op_mul:
    case INDEX_op_nand:
    case INDEX_op_nor:
    case INDEX_op_or:
    case INDEX_op_orc:
    case INDEX_op_rems:
    case INDEX_op_remu:
    case INDEX_op_rotl:
    case INDEX_op_rotr:
    case INDEX_op_sar:
    case INDEX_op_shl:
    case INDEX_op_shr:
    case INDEX_op_sub:
    case INDEX_op_subbi:
    case INDEX_op_subbio:
    case INDEX_op_subbo:
    case INDEX_op_xor:
    case INDEX_op_tci_ctz32:
    case INDEX_op_tci_clz32:
    case INDEX_op_tci_divs32:
    case INDEX_op_tci_divu32:
    case INDEX_op_tci_rems32:
    case INDEX_op_tci_remu32:
    case INDEX_op_tci_rotl32:
    case INDEX_op_tci_rotr32:
        tci_args_rrr(insn, &r0, &r1, &r2);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2));
        break;

    case INDEX_op_deposit:
        tci_args_rrrbb(insn, &r0, &r1, &r2, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %d, %d",
                           op_name, str_r(r0), str_r(r1), str_r(r2), pos, len);
        break;

    case INDEX_op_extract:
    case INDEX_op_sextract:
        tci_args_rrbb(insn, &r0, &r1, &pos, &len);
        info->fprintf_func(info->stream, "%-12s  %s,%s,%d,%d",
                           op_name, str_r(r0), str_r(r1), pos, len);
        break;

    case INDEX_op_tci_movcond32:
    case INDEX_op_movcond:
        tci_args_rrrrrc(insn, &r0, &r1, &r2, &r3, &r4, &c);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1), str_r(r2),
                           str_r(r3), str_r(r4), str_c(c));
        break;

    case INDEX_op_muls2:
    case INDEX_op_mulu2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case INDEX_op_qemu_ld:
    case INDEX_op_qemu_st:
        r0 = extract32(insn, 8, 4);
        r1 = extract32(insn, 12, 4);
        oi = *tb_ptr++;
        info->fprintf_func(info->stream, "%-12s  %s, %s%s, %x",
                           op_name, str_r(r0), str_r(r1),
                           extract32(insn, 16, 1) ? " (a32)" : "", oi);
        return 2 * sizeof(insn);

    case INDEX_op_qemu_ld2:
    case INDEX_op_qemu_st2:
        tci_args_rrrr(insn, &r0, &r1, &r2, &r3);
        info->fprintf_func(info->stream, "%-12s  %s, %s, %s, %s",
                           op_name, str_r(r0), str_r(r1),
                           str_r(r2), str_r(r3));
        break;

    case 0:
        /* tcg_out_nop_fill uses zeros */
        if (insn == 0) {
            info->fprintf_func(info->stream, "align");
            break;
        }
        /* fall through */

    default:
        info->fprintf_func(info->stream, "illegal opcode %d", op);
        break;
    }

    return sizeof(insn);
}
