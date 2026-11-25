/*
 * Sega Dreamcast: run device block I/O off the vCPU stack (Emscripten)
 *
 * The GD-ROM and VMU models read and write their BlockBackends synchronously
 * from MMIO and Maple DMA handlers, i.e. on the vCPU thread underneath
 * cpu_exec().  Outside a coroutine, blk_pread() and friends create a
 * coroutine and enter it right there.  With Emscripten that is a fiber swap,
 * which Asyncify implements by unwinding and later rewinding the whole wasm
 * stack, so every function on the TCG execution path would need Asyncify
 * instrumentation, and that slows the interpreter loop down considerably.
 *
 * On Emscripten, dc_run_io_sync() therefore hands such work to the main
 * loop thread as a bottom half and waits for it, dropping the BQL meanwhile.
 * The device still sees a synchronous call, and no fiber swap ever happens
 * on a vCPU thread, so the build can exclude the TCG hot path from Asyncify
 * (see contrib/dreamcast-web/build.sh).  Everywhere else it simply calls fn.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/coroutine.h"
#include "qemu/aio.h"
#include "hw/core/cpu.h"
#include "hw/sh4/sh.h"

#ifdef EMSCRIPTEN
typedef struct DCIOReq {
    void (*fn)(void *);
    void *opaque;
    bool done;
} DCIOReq;

static QemuCond dc_io_cond;

static void dc_io_bh(void *opaque)
{
    DCIOReq *req = opaque;

    req->fn(req->opaque);
    req->done = true;
    qemu_cond_broadcast(&dc_io_cond);
}

static void __attribute__((constructor)) dc_io_init(void)
{
    qemu_cond_init(&dc_io_cond);
}
#endif

void dc_run_io_sync(void (*fn)(void *), void *opaque)
{
#ifdef EMSCRIPTEN
    if (current_cpu && !qemu_in_coroutine()) {
        DCIOReq req = { .fn = fn, .opaque = opaque };

        assert(bql_locked());
        aio_bh_schedule_oneshot(qemu_get_aio_context(), dc_io_bh, &req);
        while (!req.done) {
            qemu_cond_wait_bql(&dc_io_cond);
        }
        return;
    }
#endif
    fn(opaque);
}
