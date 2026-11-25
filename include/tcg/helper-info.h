/*
 * TCG Helper Information Structure
 *
 * Copyright (c) 2023 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TCG_HELPER_INFO_H
#define TCG_HELPER_INFO_H

#ifdef CONFIG_TCG_INTERPRETER
/*
 * MacOSX 15 uses an old version of libffi which contains
 *   #if FFI_GO_CLOSURES
 * but does not define that in <ffitarget.h>, included from <ffi.h>.
 * This was fixed upstream with
 *   https://github.com/libffi/libffi/commit/c23e9a1c
 * We don't care about go closures one way or the other;
 * just suppress the warning.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include <ffi.h>
#pragma GCC diagnostic pop
#endif
#include "tcg/target-reg-bits.h"

#define MAX_CALL_IARGS  7

/*
 * Describe the calling convention of a given argument type.
 */
typedef enum {
    TCG_CALL_RET_NORMAL,         /* by registers */
    TCG_CALL_RET_BY_REF,         /* for i128, by reference */
    TCG_CALL_RET_BY_VEC,         /* for i128, by vector register */
} TCGCallReturnKind;

typedef enum {
    TCG_CALL_ARG_NORMAL,         /* by registers (continuing onto stack) */
    TCG_CALL_ARG_EVEN,           /* like normal, but skipping odd slots */
    TCG_CALL_ARG_EXTEND,         /* for i32, as a sign/zero-extended i64 */
    TCG_CALL_ARG_EXTEND_U,       /*      ... as a zero-extended i64 */
    TCG_CALL_ARG_EXTEND_S,       /*      ... as a sign-extended i64 */
    TCG_CALL_ARG_BY_REF,         /* for i128, by reference, first */
    TCG_CALL_ARG_BY_REF_N,       /*       ... by reference, subsequent */
} TCGCallArgumentKind;

typedef struct TCGCallArgumentLoc {
    TCGCallArgumentKind kind    : 8;
    unsigned arg_slot           : 8;
    unsigned ref_slot           : 8;
    unsigned arg_idx            : 4;
    unsigned tmp_subindex       : 2;
} TCGCallArgumentLoc;

#ifdef CONFIG_TCG_INTERPRETER
/*
 * With TCI_DIRECT_CALL, the interpreter calls helpers through a function
 * pointer of the exact type instead of libffi, where it can: on Emscripten
 * libffi is JavaScript, and calls through it cost more than the helpers.
 * TCI_SIG encodes the signature: return kind r (0 void, 1 32-bit, 2 64-bit),
 * argument count n and the mask m of 64-bit arguments.  The generated cases
 * are in tcg/tci-direct-call.c.inc.
 */
#if defined(EMSCRIPTEN) && TCG_TARGET_REG_BITS == 64
#define TCI_DIRECT_CALL 1
#endif
#define TCI_SIG(r, n, m)    ((r) | (n) << 2 | (m) << 5)
#define TCI_SIG_NONE        UINT32_MAX

/* What TCGHelperInfo.cif points to. */
typedef struct TCIHelperCif {
    ffi_cif cif;                /* first: used as a plain ffi_cif * too */
    uint32_t sig;               /* TCI_SIG(), or TCI_SIG_NONE */
} TCIHelperCif;
#endif

struct TCGHelperInfo {
    void *func;
    const char *name;

    /* Used with g_once_init_enter. */
#ifdef CONFIG_TCG_INTERPRETER
    ffi_cif *cif;
#else
    uintptr_t init;
#endif

    unsigned typemask           : 32;
    unsigned flags              : 8;
    unsigned nr_in              : 8;
    unsigned nr_out             : 8;
    TCGCallReturnKind out_kind  : 8;

    /* Maximum physical arguments are constrained by TCG_TYPE_I128. */
    TCGCallArgumentLoc in[MAX_CALL_IARGS * (128 / TCG_TARGET_REG_BITS)];
};

#endif /* TCG_HELPER_INFO_H */
