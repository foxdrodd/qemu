/*
 * Browser canvas display + input for Emscripten builds.
 *
 * Every graphic console gets a DisplayChangeListener.  On each refresh tick
 * (QEMU main-loop thread, BQL held) dirty surfaces are converted to RGBA and
 * handed to the browser main thread, which paints them via
 * Module.qemuCanvasDraw(index, rgba, width, height).
 *
 * Input goes the other way: the page calls the exported qemu_canvas_input()
 * from the browser main thread.  Events land in a lock-free single-producer /
 * single-consumer ring and are injected into the QEMU input layer from the
 * refresh tick, so no QEMU state is touched outside the BQL.
 *
 * Use with "-display none"; the listeners register themselves once machine
 * init is done.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "ui/qemu-pixman.h"
#include "system/system.h"
#include <emscripten.h>
#include <emscripten/threading.h>

#define WC_MAX_CONSOLES 4
#define WC_RING         512          /* power of two */
#define WC_REFRESH_MS   16

enum {
    WC_EV_KEY = 1,                   /* a = linux keycode, b = down */
    WC_EV_REL = 2,                   /* a = dx, b = dy */
    WC_EV_BTN = 3,                   /* a = button (0 L, 1 M, 2 R), b = down */
    WC_EV_WHEEL = 4,                 /* a = +1 down / -1 up */
};

typedef struct WasmCanvas {
    DisplayChangeListener dcl;
    int index;
    bool dirty;
    int busy;                        /* 1 while the browser paints a frame */
    pixman_image_t *rgba;
    int w, h;
} WasmCanvas;

typedef struct WcEvent {
    int32_t type, a, b;
} WcEvent;

static WasmCanvas canvases[WC_MAX_CONSOLES];
static int n_canvases;
static WcEvent ring[WC_RING];
static uint32_t ring_head;           /* written by the browser thread */
static uint32_t ring_tail;           /* written by the QEMU thread */

EMSCRIPTEN_KEEPALIVE
int qemu_canvas_input(int type, int a, int b)
{
    uint32_t head = __atomic_load_n(&ring_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);

    if (head - tail >= WC_RING) {
        return 0;                    /* full: drop */
    }
    ring[head % WC_RING] = (WcEvent){ type, a, b };
    __atomic_store_n(&ring_head, head + 1, __ATOMIC_RELEASE);
    return 1;
}

static void wc_drain_input(void)
{
    uint32_t tail = __atomic_load_n(&ring_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    bool pointer = false;
    static const InputButton btns[] = {
        INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
    };

    for (; tail != head; tail++) {
        WcEvent *ev = &ring[tail % WC_RING];

        switch (ev->type) {
        case WC_EV_KEY:
            qemu_input_event_send_key_linux(NULL, ev->a, ev->b);
            break;
        case WC_EV_REL:
            qemu_input_queue_rel(NULL, INPUT_AXIS_X, ev->a);
            qemu_input_queue_rel(NULL, INPUT_AXIS_Y, ev->b);
            pointer = true;
            break;
        case WC_EV_BTN:
            if (ev->a >= 0 && ev->a < ARRAY_SIZE(btns)) {
                qemu_input_queue_btn(NULL, btns[ev->a], ev->b);
                pointer = true;
            }
            break;
        case WC_EV_WHEEL:
            qemu_input_queue_btn(NULL, ev->a > 0 ? INPUT_BUTTON_WHEEL_DOWN
                                                 : INPUT_BUTTON_WHEEL_UP, true);
            qemu_input_event_sync();
            qemu_input_queue_btn(NULL, ev->a > 0 ? INPUT_BUTTON_WHEEL_DOWN
                                                 : INPUT_BUTTON_WHEEL_UP, false);
            pointer = true;
            break;
        }
        if (pointer) {
            qemu_input_event_sync();
            pointer = false;
        }
    }
    __atomic_store_n(&ring_tail, tail, __ATOMIC_RELEASE);
}

static void wc_present(WasmCanvas *wc)
{
    DisplaySurface *surf = qemu_console_surface(wc->dcl.con);
    int w, h;

    if (!surf || __atomic_load_n(&wc->busy, __ATOMIC_ACQUIRE)) {
        return;
    }
    w = surface_width(surf);
    h = surface_height(surf);
    if (!wc->rgba || wc->w != w || wc->h != h) {
        if (wc->rgba) {
            pixman_image_unref(wc->rgba);
        }
        /* a8b8g8r8 on little endian = R,G,B,A bytes, what ImageData wants */
        wc->rgba = pixman_image_create_bits(PIXMAN_a8b8g8r8, w, h, NULL, 0);
        wc->w = w;
        wc->h = h;
    }
    pixman_image_composite(PIXMAN_OP_SRC, surf->image, NULL, wc->rgba,
                           0, 0, 0, 0, 0, 0, w, h);
    wc->dirty = false;
    __atomic_store_n(&wc->busy, 1, __ATOMIC_RELEASE);

    /* No top-level commas in the JS: they would split the macro arguments. */
    MAIN_THREAD_ASYNC_EM_ASM({
        try {
            if (Module.qemuCanvasDraw) {
                Module.qemuCanvasDraw($0, new Uint8ClampedArray(
                    HEAPU8.subarray($1, $1 + $2 * $3 * 4)), $2, $3);
            }
        } finally {
            Atomics.store(HEAP32, $4 >> 2, 0);
        }
    }, wc->index, pixman_image_get_data(wc->rgba), w, h, &wc->busy);
}

static void wc_refresh(DisplayChangeListener *dcl)
{
    WasmCanvas *wc = container_of(dcl, WasmCanvas, dcl);

    if (wc->index == 0) {
        wc_drain_input();
    }
    qemu_console_hw_update(dcl->con);
    if (wc->dirty) {
        wc_present(wc);
    }
}

static void wc_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    container_of(dcl, WasmCanvas, dcl)->dirty = true;
}

static void wc_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *s)
{
    container_of(dcl, WasmCanvas, dcl)->dirty = true;
}

static const DisplayChangeListenerOps wc_ops = {
    .dpy_name       = "wasm-canvas",
    .dpy_refresh    = wc_refresh,
    .dpy_gfx_update = wc_gfx_update,
    .dpy_gfx_switch = wc_gfx_switch,
};

static void wc_machine_done(Notifier *n, void *data)
{
    QemuConsole *con;

    for (int i = 0; n_canvases < WC_MAX_CONSOLES &&
                    (con = qemu_console_lookup_by_index(i)); i++) {
        WasmCanvas *wc;

        if (!qemu_console_is_graphic(con)) {
            continue;
        }
        wc = &canvases[n_canvases];
        wc->index = n_canvases++;
        wc->dcl.update_interval = WC_REFRESH_MS;
        qemu_console_register_listener(con, &wc->dcl, &wc_ops);
    }
    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module.qemuCanvasReady) {
            Module.qemuCanvasReady($0);
        }
    }, n_canvases);
}

static Notifier wc_notifier = { .notify = wc_machine_done };

static void wc_init(void)
{
    qemu_add_machine_init_done_notifier(&wc_notifier);
}
type_init(wc_init);
