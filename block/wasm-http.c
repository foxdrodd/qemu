/*
 * Read-only http(s):// block protocol for Emscripten builds.
 *
 * The curl driver isn't available in the browser, so this one fetches the
 * image lazily with synchronous XMLHttpRequest Range requests.  That is only
 * legal off the browser's main thread, which always holds here: with
 * PROXY_TO_PTHREAD all of QEMU (main loop, vCPU, I/O) runs in workers.
 *
 * The image is split into fixed-size chunks.  A miss fetches the chunk (plus
 * a few following ones when the access pattern is sequential) in a single
 * request and keeps it, up to cache-size MiB, evicting oldest-first.  A read
 * blocks its thread for the duration of the fetch, much like a seek on the
 * real drive.
 *
 *   -drive if=none,file=https://host/disc.cdi,format=raw,readonly=on
 *   -drive if=none,driver=raw,file.driver=https,file.url=...,file.cache-size=256
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include <emscripten.h>

#define WH_CHUNK            (256 * KiB)
#define WH_READAHEAD        4               /* chunks, on sequential access */
#define WH_DEFAULT_CACHE_MB 512

#define WH_OPT_URL          "url"
#define WH_OPT_CACHE        "cache-size"

typedef struct BDRVWasmHTTPState {
    char *url;
    int64_t len;
    int64_t n_chunks;
    uint8_t **chunks;           /* n_chunks entries, NULL = not cached */
    int64_t *fifo;              /* cached chunk indices, oldest first */
    int64_t fifo_head, fifo_count, fifo_cap;
    int64_t last_chunk;         /* for sequential-access detection */
    uint64_t fetched_bytes, requests;
    QemuMutex lock;
} BDRVWasmHTTPState;

static QemuOptsList wh_runtime_opts = {
    .name = "wasm-http",
    .head = QTAILQ_HEAD_INITIALIZER(wh_runtime_opts.head),
    .desc = {
        {
            .name = WH_OPT_URL,
            .type = QEMU_OPT_STRING,
            .help = "URL of the image",
        },
        {
            .name = WH_OPT_CACHE,
            .type = QEMU_OPT_SIZE,
            .help = "Chunk cache size in MiB (default 512)",
        },
        { /* end of list */ }
    },
};

/*
 * Size of the resource; -1 on error, -2 if the server doesn't advertise
 * byte ranges (a GET would then pull the whole image).  Runs in the calling
 * worker.
 */
/* NB: pointers arrive as BigInt (64-bit C pointers), hence the Number(). */
EM_JS(double, wh_js_length, (const char *url), {
    url = Number(url);
    try {
        var x = new XMLHttpRequest();
        x.open("HEAD", UTF8ToString(url), false);
        x.send();
        if (x.status < 200 || x.status >= 300) {
            console.error("wasm-http: HEAD " + UTF8ToString(url) + ": HTTP " + x.status);
            return -1;
        }
        if (!/bytes/i.test(x.getResponseHeader("Accept-Ranges") || "")) {
            return -2;
        }
        var n = Number(x.getResponseHeader("Content-Length"));
        return n > 0 ? n : -1;
    } catch (e) {
        console.error("wasm-http: " + e);
        return -1;
    }
});

/* Fetch [start, start + len) into dst.  Returns 0 or -1. */
EM_JS(int, wh_js_fetch, (const char *url, double start, double len,
                         uint8_t *dst), {
    url = Number(url);
    dst = Number(dst);
    try {
        var x = new XMLHttpRequest();
        x.open("GET", UTF8ToString(url), false);
        x.responseType = "arraybuffer";
        x.setRequestHeader("Range", "bytes=" + start + "-" + (start + len - 1));
        x.send();
        if (x.status != 206) {
            console.error("wasm-http: HTTP " + x.status + " for bytes " + start);
            return -1;
        }
        var buf = new Uint8Array(x.response);
        if (buf.length < len) {
            console.error("wasm-http: short read " + buf.length + " < " + len);
            return -1;
        }
        HEAPU8.set(buf.subarray(0, len), dst);
        return 0;
    } catch (e) {
        console.error("wasm-http: " + e);
        return -1;
    }
});

static int64_t wh_chunk_len(BDRVWasmHTTPState *s, int64_t idx)
{
    return MIN(WH_CHUNK, s->len - idx * WH_CHUNK);
}

static void wh_cache_insert(BDRVWasmHTTPState *s, int64_t idx, uint8_t *data)
{
    if (s->fifo_count == s->fifo_cap) {
        int64_t old = s->fifo[s->fifo_head];

        g_free(s->chunks[old]);
        s->chunks[old] = NULL;
        s->fifo_head = (s->fifo_head + 1) % s->fifo_cap;
        s->fifo_count--;
    }
    s->fifo[(s->fifo_head + s->fifo_count) % s->fifo_cap] = idx;
    s->fifo_count++;
    s->chunks[idx] = data;
}

/* Make chunk idx resident, reading ahead if the access looks sequential. */
static int wh_fill(BDRVWasmHTTPState *s, int64_t idx)
{
    int64_t n = 1, start, total = 0;
    uint8_t *buf;
    int ret;

    if (s->chunks[idx]) {
        return 0;
    }
    if (idx > 0 && idx - 1 == s->last_chunk) {
        while (n < WH_READAHEAD && n < s->fifo_cap && idx + n < s->n_chunks &&
               !s->chunks[idx + n]) {
            n++;
        }
    }
    start = idx * WH_CHUNK;
    for (int64_t i = 0; i < n; i++) {
        total += wh_chunk_len(s, idx + i);
    }

    buf = g_try_malloc(total);
    if (!buf) {
        return -ENOMEM;
    }
    ret = wh_js_fetch(s->url, start, total, buf);
    if (ret < 0) {
        g_free(buf);
        return -EIO;
    }
    s->requests++;
    s->fetched_bytes += total;

    for (int64_t i = 0; i < n; i++) {
        int64_t clen = wh_chunk_len(s, idx + i);
        uint8_t *c = g_malloc(clen);

        memcpy(c, buf + i * WH_CHUNK, clen);
        wh_cache_insert(s, idx + i, c);
    }
    g_free(buf);
    return 0;
}

static int coroutine_fn wh_co_preadv(BlockDriverState *bs, int64_t offset,
                                     int64_t bytes, QEMUIOVector *qiov,
                                     BdrvRequestFlags flags)
{
    BDRVWasmHTTPState *s = bs->opaque;
    size_t done = 0;
    int ret = 0;

    if (offset < 0) {
        return -EIO;
    }
    /*
     * The block layer works on 512-byte sectors and rounds the length up,
     * so reads may run up to 511 bytes past EOF: those read as zeroes.
     */
    if (offset + bytes > s->len) {
        int64_t tail = offset + bytes - MAX(offset, s->len);

        qemu_iovec_memset(qiov, bytes - tail, 0, tail);
        bytes -= tail;
    }
    qemu_mutex_lock(&s->lock);
    while (bytes > 0) {
        int64_t idx = offset / WH_CHUNK;
        int64_t in = offset % WH_CHUNK;
        int64_t n = MIN(bytes, wh_chunk_len(s, idx) - in);

        ret = wh_fill(s, idx);
        if (ret < 0) {
            break;
        }
        qemu_iovec_from_buf(qiov, done, s->chunks[idx] + in, n);
        s->last_chunk = idx;
        offset += n;
        bytes -= n;
        done += n;
    }
    qemu_mutex_unlock(&s->lock);
    return ret;
}

static void wh_parse_filename(const char *filename, QDict *options,
                              Error **errp)
{
    qdict_put_str(options, WH_OPT_URL, filename);
}

static int wh_open(BlockDriverState *bs, QDict *options, int flags,
                   Error **errp)
{
    BDRVWasmHTTPState *s = bs->opaque;
    QemuOpts *opts;
    uint64_t cache_mb;
    double len;
    int ret;

    ret = bdrv_apply_auto_read_only(bs, "wasm-http is read-only", errp);
    if (ret < 0) {
        return ret;
    }
    opts = qemu_opts_create(&wh_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto out;
    }
    if (!qemu_opt_get(opts, WH_OPT_URL)) {
        error_setg(errp, "wasm-http: no URL given");
        ret = -EINVAL;
        goto out;
    }
    s->url = g_strdup(qemu_opt_get(opts, WH_OPT_URL));
    cache_mb = qemu_opt_get_size(opts, WH_OPT_CACHE, WH_DEFAULT_CACHE_MB);

    len = wh_js_length(s->url);
    if (len == -2) {
        error_setg(errp, "wasm-http: %s: server does not support byte ranges "
                   "(no 'Accept-Ranges: bytes')", s->url);
        ret = -EIO;
        goto out;
    }
    if (len <= 0) {
        error_setg(errp, "wasm-http: cannot determine the size of %s", s->url);
        ret = -EIO;
        goto out;
    }
    s->len = (int64_t)len;
    s->n_chunks = DIV_ROUND_UP(s->len, WH_CHUNK);
    s->chunks = g_new0(uint8_t *, s->n_chunks);
    s->fifo_cap = MAX(WH_READAHEAD, MIN(s->n_chunks,
                                        (int64_t)(cache_mb * MiB / WH_CHUNK)));
    s->fifo = g_new(int64_t, s->fifo_cap);
    s->last_chunk = -2;
    qemu_mutex_init(&s->lock);
    ret = 0;
out:
    qemu_opts_del(opts);
    if (ret < 0) {
        g_free(s->url);
        s->url = NULL;
    }
    return ret;
}

static void wh_close(BlockDriverState *bs)
{
    BDRVWasmHTTPState *s = bs->opaque;

    info_report("wasm-http: %s: %" PRIu64 " requests, %" PRIu64 " KiB fetched",
                s->url, s->requests, s->fetched_bytes / KiB);
    for (int64_t i = 0; i < s->n_chunks; i++) {
        g_free(s->chunks[i]);
    }
    g_free(s->chunks);
    g_free(s->fifo);
    g_free(s->url);
    qemu_mutex_destroy(&s->lock);
}

static int64_t coroutine_fn wh_co_getlength(BlockDriverState *bs)
{
    return ((BDRVWasmHTTPState *)bs->opaque)->len;
}

static void wh_refresh_filename(BlockDriverState *bs)
{
    BDRVWasmHTTPState *s = bs->opaque;

    pstrcpy(bs->exact_filename, sizeof(bs->exact_filename), s->url);
}

static const char *const wh_strong_runtime_opts[] = {
    WH_OPT_URL,
    NULL
};

#define WH_DRIVER(proto)                                        \
    {                                                           \
        .format_name            = proto,                        \
        .protocol_name          = proto,                        \
        .instance_size          = sizeof(BDRVWasmHTTPState),    \
        .bdrv_parse_filename    = wh_parse_filename,            \
        .bdrv_open              = wh_open,                      \
        .bdrv_close             = wh_close,                     \
        .bdrv_co_getlength      = wh_co_getlength,              \
        .bdrv_co_preadv         = wh_co_preadv,                 \
        .bdrv_refresh_filename  = wh_refresh_filename,          \
        .strong_runtime_opts    = wh_strong_runtime_opts,       \
    }

static BlockDriver bdrv_wasm_http = WH_DRIVER("http");
static BlockDriver bdrv_wasm_https = WH_DRIVER("https");

static void wh_block_init(void)
{
    bdrv_register(&bdrv_wasm_http);
    bdrv_register(&bdrv_wasm_https);
}

block_init(wh_block_init);
