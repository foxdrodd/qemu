/*
 * Sega Dreamcast PowerVR2 (CLX2) Tile Accelerator + ISP/TSP - software renderer
 *
 * The real PVR2 is a tile-based deferred renderer: the TA bins submitted
 * polygons into per-tile object lists in VRAM, then on STARTRENDER the ISP/TSP
 * walk each tile doing hidden-surface removal, texturing and blending.  We do
 * NOT reproduce the tile/OPB memory layout bit-for-bit (nothing in our stack
 * reads it back); instead we parse the real TA parameter stream into host-side
 * triangle lists and rasterise them straight into the framebuffer on
 * STARTRENDER.  Because we parse the genuine parameter format, guest code that
 * speaks it (KOS/libpvr, and by extension real PVR software) drives us directly.
 *
 * Pipeline entry points, all reached from the PVR register block in
 * dreamcast_pvr.c and from the TA input FIFO mapped at 0x10000000:
 *   - TA_LIST_INIT  -> dc_ta_list_init(): drop all captured geometry
 *   - FIFO writes   -> ta_fifo_write():  accumulate 32-byte parameters, parse
 *   - STARTRENDER   -> dc_ta_start_render(): rasterise, raise the render-done IRQ
 *
 * Supported: opaque / punch-through / translucent lists; Gouraud-shaded and
 * textured (twiddled + non-twiddled, ARGB1555/RGB565/ARGB4444; point & bilinear)
 * triangle strips; per-pixel Z (all eight ISP depth-compare modes); back-face
 * culling; the common source/inverse-source blend factors.  Not yet: modifier
 * volumes, sprites, palette/VQ/YUV/bump textures, 64-byte vertex parameters.
 * Textures are fetched linearly from VRAM (libpvr uploads them linearly), so the
 * 64-bit-area bank interleave the texture-DMA path applies is intentionally
 * bypassed here.
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "system/dma.h"
#include "qemu/timer.h"
#include <math.h>

#define TYPE_DC_TA "dc-ta"
OBJECT_DECLARE_SIMPLE_TYPE(DCTaState, DC_TA)

/* TA polygon input FIFO window (SH-4 Area 4).  0x10000000-0x10ffffff. */
#define TA_FIFO_SIZE   (16 * MiB)

#define VRAM_PHYS_BASE 0x05000000
#define VRAM_SIZE      (8 * MiB)

/* Parameter Control Word (first word of every 32-byte TA parameter). */
#define PCW_PARA_TYPE(w)   (((w) >> 29) & 7)
#define PCW_END_OF_STRIP(w) (((w) >> 28) & 1)
#define PCW_LIST_TYPE(w)   (((w) >> 24) & 7)
#define PCW_TEXTURED(w)    (((w) >> 3) & 1)
#define PCW_OFFSET(w)      (((w) >> 2) & 1)
#define PCW_GOURAUD(w)     (((w) >> 1) & 1)

enum {                          /* PCW para-type field */
    PARA_END_OF_LIST = 0,
    PARA_USER_CLIP   = 1,
    PARA_OBJ_LIST    = 2,
    PARA_POLY        = 4,       /* polygon / modifier-volume global param */
    PARA_SPRITE      = 5,
    PARA_VERTEX      = 7,
};

enum {                          /* list types we keep, in render order */
    LIST_OPAQUE      = 0,
    LIST_OPAQUE_MOD  = 1,
    LIST_TRANS       = 2,
    LIST_TRANS_MOD   = 3,
    LIST_PUNCH       = 4,
    NR_LISTS         = 5,
};

typedef struct {
    float x, y, z;              /* screen-space position; z = 1/w */
    float u, v;                 /* texture coordinates */
    uint32_t argb;              /* base colour (Gouraud) */
    uint32_t oargb;             /* offset (specular) colour */
} TaVertex;

typedef struct {                /* polygon context captured from the header */
    uint32_t isp;               /* ISP/TSP instruction word */
    uint32_t tsp;               /* TSP instruction word */
    uint32_t tcw;               /* texture control word */
    bool textured;
    bool offset;
    bool gouraud;
} TaCtx;

typedef struct {
    TaVertex v[3];
    TaCtx ctx;
} TaTri;

struct DCTaState {
    SysBusDevice parent_obj;

    MemoryRegion fifo;          /* TA input FIFO at 0x10000000 */
    MemoryRegion *vram;         /* the machine's 8 MB VRAM region */
    qemu_irq render_irq;        /* Holly "end of render (TSP)" event */
    QEMUTimer *render_timer;    /* defers the render-done IRQ (see below) */

    /* FIFO parameter assembly: 8 words == one 32-byte parameter. */
    uint32_t param[8];
    int pidx;

    /* Current strip state while parsing vertices. */
    TaCtx cur;
    int cur_list;               /* list type of the current header */
    bool have_ctx;
    TaVertex strip[3];
    int nverts;
    int parity;

    GArray *lists[NR_LISTS];    /* captured TaTri per list type */
};

static inline float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* ---- colour helpers (all internal work in 0xAARRGGBB, 8 bits/channel) ---- */

static inline void argb_unpack(uint32_t c, int *a, int *r, int *g, int *b)
{
    *a = (c >> 24) & 0xff;
    *r = (c >> 16) & 0xff;
    *g = (c >> 8) & 0xff;
    *b = c & 0xff;
}

static inline int clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* ------------------------------- textures -------------------------------- */

/* Morton (Z-order) interleave for twiddled textures (square case). */
static inline uint32_t twiddle(uint32_t x, uint32_t y)
{
    uint32_t r = 0;
    int i;

    for (i = 0; i < 16; i++) {
        r |= ((x >> i) & 1) << (2 * i);
        r |= ((y >> i) & 1) << (2 * i + 1);
    }
    return r;
}

/* Decode one 16-bit texel of the given TCW pixel format to 0xAARRGGBB. */
static uint32_t texel16_to_argb(uint16_t v, uint32_t fmt)
{
    int a, r, g, b;

    switch (fmt) {
    case 0:                     /* ARGB1555 */
        a = (v & 0x8000) ? 0xff : 0;
        r = ((v >> 10) & 0x1f) << 3;
        g = ((v >> 5) & 0x1f) << 3;
        b = (v & 0x1f) << 3;
        break;
    case 2:                     /* ARGB4444 */
        a = ((v >> 12) & 0xf) * 0x11;
        r = ((v >> 8) & 0xf) * 0x11;
        g = ((v >> 4) & 0xf) * 0x11;
        b = (v & 0xf) * 0x11;
        break;
    case 1:                     /* RGB565 */
    default:
        a = 0xff;
        r = ((v >> 11) & 0x1f) << 3;
        g = ((v >> 5) & 0x3f) << 2;
        b = (v & 0x1f) << 3;
        break;
    }
    return ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
}

/* Fetch a single texel (nearest) at integer (tx,ty) from a VRAM texture. */
static uint32_t tex_fetch(const uint8_t *vram, uint32_t addr, uint32_t fmt,
                          bool twiddled, int tx, int ty, int tw, int th)
{
    uint32_t idx = twiddled ? twiddle(tx, ty) : (uint32_t)ty * tw + tx;
    uint32_t off = addr + idx * 2;
    uint16_t raw;

    (void)th;
    if (off + 2 > VRAM_SIZE) {
        return 0;
    }
    raw = lduw_le_p(vram + off);
    return texel16_to_argb(raw, fmt);
}

/* Sample a texture at normalised (u,v), point or bilinear. */
static uint32_t tex_sample(const uint8_t *vram, const TaCtx *ctx,
                           float u, float v, bool bilinear)
{
    uint32_t tcw = ctx->tcw;
    uint32_t fmt = (tcw >> 27) & 7;
    bool twiddled = !((tcw >> 26) & 1);
    uint32_t addr = (tcw & 0x1fffff) << 3;
    /* TSP instruction word: U size at bits 5-3, V size at bits 2-0 (8<<n). */
    int tw = 8 << ((ctx->tsp >> 3) & 7);
    int th = 8 << (ctx->tsp & 7);
    float fu = u - floorf(u);           /* wrap into [0,1) */
    float fv = v - floorf(v);

    if (fmt > 2) {                      /* palette/VQ/YUV/bump: unsupported */
        return 0xffff00ff;             /* magenta flag colour */
    }

    if (!bilinear) {
        int tx = (int)(fu * tw) & (tw - 1);
        int ty = (int)(fv * th) & (th - 1);
        return tex_fetch(vram, addr, fmt, twiddled, tx, ty, tw, th);
    } else {
        float gx = fu * tw - 0.5f, gy = fv * th - 0.5f;
        int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
        float dx = gx - x0, dy = gy - y0;
        int a[4], r[4], g[4], b[4];
        int i;
        uint32_t c[4] = {
            tex_fetch(vram, addr, fmt, twiddled, x0 & (tw - 1), y0 & (th - 1), tw, th),
            tex_fetch(vram, addr, fmt, twiddled, (x0 + 1) & (tw - 1), y0 & (th - 1), tw, th),
            tex_fetch(vram, addr, fmt, twiddled, x0 & (tw - 1), (y0 + 1) & (th - 1), tw, th),
            tex_fetch(vram, addr, fmt, twiddled, (x0 + 1) & (tw - 1), (y0 + 1) & (th - 1), tw, th),
        };
        float w00 = (1 - dx) * (1 - dy), w10 = dx * (1 - dy);
        float w01 = (1 - dx) * dy, w11 = dx * dy;

        for (i = 0; i < 4; i++) {
            argb_unpack(c[i], &a[i], &r[i], &g[i], &b[i]);
        }
        return ((uint32_t)clamp8((int)(a[0]*w00 + a[1]*w10 + a[2]*w01 + a[3]*w11)) << 24)
             | (clamp8((int)(r[0]*w00 + r[1]*w10 + r[2]*w01 + r[3]*w11)) << 16)
             | (clamp8((int)(g[0]*w00 + g[1]*w10 + g[2]*w01 + g[3]*w11)) << 8)
             |  clamp8((int)(b[0]*w00 + b[1]*w10 + b[2]*w01 + b[3]*w11));
    }
}

/* --------------------------- framebuffer write --------------------------- */

/* Pack 0xAARRGGBB into the render target's pixel format (FB_W_CTRL & 7). */
static void fb_store(uint8_t *vram, uint32_t off, uint32_t fmt, uint32_t argb)
{
    int a, r, g, b;

    argb_unpack(argb, &a, &r, &g, &b);
    switch (fmt & 7) {
    case 0:                     /* 0555 KRGB */
        if (off + 2 <= VRAM_SIZE) {
            stw_le_p(vram + off,
                     ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        }
        break;
    case 2:                     /* 4444 ARGB */
        if (off + 2 <= VRAM_SIZE) {
            stw_le_p(vram + off,
                     ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        }
        break;
    case 5:                     /* 8888 ARGB */
        if (off + 4 <= VRAM_SIZE) {
            stl_le_p(vram + off, argb);
        }
        break;
    case 1:                     /* 565 RGB */
    default:
        if (off + 2 <= VRAM_SIZE) {
            stw_le_p(vram + off,
                     ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        break;
    }
}

static uint32_t fb_load(const uint8_t *vram, uint32_t off, uint32_t fmt)
{
    uint16_t v;

    switch (fmt & 7) {
    case 5:
        return off + 4 <= VRAM_SIZE ? ldl_le_p(vram + off) : 0;
    case 0:
        v = off + 2 <= VRAM_SIZE ? lduw_le_p(vram + off) : 0;
        return texel16_to_argb(v, 0);
    case 2:
        v = off + 2 <= VRAM_SIZE ? lduw_le_p(vram + off) : 0;
        return texel16_to_argb(v, 2);
    case 1:
    default:
        v = off + 2 <= VRAM_SIZE ? lduw_le_p(vram + off) : 0;
        return texel16_to_argb(v, 1);
    }
}

/* ------------------------------- blending -------------------------------- */

/* One PVR blend factor (codes per the TSP instruction word). */
static void blend_factor(int code, int sa, int da, float f[4])
{
    float s = sa / 255.0f, d = da / 255.0f;

    switch (code) {
    case 0: f[0] = f[1] = f[2] = f[3] = 0.0f; break;              /* zero */
    case 4: f[0] = f[1] = f[2] = f[3] = s; break;                 /* src alpha */
    case 5: f[0] = f[1] = f[2] = f[3] = 1.0f - s; break;          /* inv src a */
    case 6: f[0] = f[1] = f[2] = f[3] = d; break;                 /* dst alpha */
    case 7: f[0] = f[1] = f[2] = f[3] = 1.0f - d; break;          /* inv dst a */
    case 1:
    default: f[0] = f[1] = f[2] = f[3] = 1.0f; break;            /* one */
    }
}

/* -------------------------- depth-compare modes -------------------------- */

static bool depth_pass(int mode, float src, float dst)
{
    switch (mode) {
    case 0: return false;                       /* never */
    case 1: return src < dst;                   /* less */
    case 2: return src == dst;                  /* equal */
    case 3: return src <= dst;                  /* less-or-equal */
    case 4: return src > dst;                   /* greater */
    case 5: return src != dst;                  /* not-equal */
    case 6: return src >= dst;                  /* greater-or-equal */
    case 7:
    default: return true;                       /* always */
    }
}

/* ------------------------------ rasteriser ------------------------------- */

static inline float edge(const TaVertex *a, const TaVertex *b, float px, float py)
{
    return (px - a->x) * (b->y - a->y) - (py - a->y) * (b->x - a->x);
}

static void raster_tri(uint8_t *vram, float *zbuf, int w, int h,
                       uint32_t fb_base, uint32_t fb_fmt, int fb_bpp,
                       int list, const TaTri *t)
{
    const TaVertex *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
    const TaCtx *ctx = &t->ctx;
    int depth_mode = (ctx->isp >> 29) & 7;
    int cull_mode = (ctx->isp >> 27) & 3;
    bool bilinear = ((ctx->tsp >> 13) & 3) != 0;
    int sf = (ctx->tsp >> 29) & 7;
    int df = (ctx->tsp >> 26) & 7;
    float area = edge(v0, v1, v2->x, v2->y);
    int minx, maxx, miny, maxy, px, py;
    const uint8_t *vro = vram;

    if (area == 0.0f) {
        return;                 /* degenerate */
    }
    /* Back-face culling: modes 2/3 discard one winding. */
    if (cull_mode >= 2) {
        if ((cull_mode == 2 && area < 0) || (cull_mode == 3 && area > 0)) {
            return;
        }
    }

    minx = (int)floorf(fminf(fminf(v0->x, v1->x), v2->x));
    maxx = (int)ceilf(fmaxf(fmaxf(v0->x, v1->x), v2->x));
    miny = (int)floorf(fminf(fminf(v0->y, v1->y), v2->y));
    maxy = (int)ceilf(fmaxf(fmaxf(v0->y, v1->y), v2->y));
    minx = MAX(minx, 0); miny = MAX(miny, 0);
    maxx = MIN(maxx, w - 1); maxy = MIN(maxy, h - 1);

    for (py = miny; py <= maxy; py++) {
        for (px = minx; px <= maxx; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float e0 = edge(v1, v2, fx, fy);
            float e1 = edge(v2, v0, fx, fy);
            float e2 = edge(v0, v1, fx, fy);
            float l0, l1, l2, z, iw, tu, tv;
            int fa, fr, fg, fb, idx;
            uint32_t frag, zoff;

            /* Inside test tolerant of either winding. */
            if (area > 0) {
                if (e0 < 0 || e1 < 0 || e2 < 0) {
                    continue;
                }
            } else if (e0 > 0 || e1 > 0 || e2 > 0) {
                continue;
            }
            l0 = e0 / area; l1 = e1 / area; l2 = e2 / area;

            /* Screen-linear interpolation of z (=1/w) is the depth value. */
            z = l0 * v0->z + l1 * v1->z + l2 * v2->z;
            idx = py * w + px;
            if (!depth_pass(depth_mode, z, zbuf[idx])) {
                continue;
            }

            /* Gouraud base colour (affine). */
            {
                int a0, r0, g0, b0, a1, r1, g1, b1, a2, r2, g2, b2;
                argb_unpack(v0->argb, &a0, &r0, &g0, &b0);
                argb_unpack(v1->argb, &a1, &r1, &g1, &b1);
                argb_unpack(v2->argb, &a2, &r2, &g2, &b2);
                fa = clamp8((int)(l0 * a0 + l1 * a1 + l2 * a2));
                fr = clamp8((int)(l0 * r0 + l1 * r1 + l2 * r2));
                fg = clamp8((int)(l0 * g0 + l1 * g1 + l2 * g2));
                fb = clamp8((int)(l0 * b0 + l1 * b1 + l2 * b2));
            }

            /* Texture: perspective-correct u,v via 1/w. */
            if (ctx->textured) {
                uint32_t tex;
                int ta, tr, tg, tb;

                iw = l0 * v0->z + l1 * v1->z + l2 * v2->z;
                if (iw == 0.0f) {
                    iw = 1e-6f;
                }
                tu = (l0 * v0->u * v0->z + l1 * v1->u * v1->z +
                      l2 * v2->u * v2->z) / iw;
                tv = (l0 * v0->v * v0->z + l1 * v1->v * v1->z +
                      l2 * v2->v * v2->z) / iw;
                tex = tex_sample(vro, ctx, tu, tv, bilinear);
                argb_unpack(tex, &ta, &tr, &tg, &tb);
                /* Modulate. */
                fr = fr * tr / 255;
                fg = fg * tg / 255;
                fb = fb * tb / 255;
                fa = fa * ta / 255;
            }

            /* Offset (specular) colour add. */
            if (ctx->offset) {
                int oa, orr, og, ob;
                float ll0 = l0, ll1 = l1, ll2 = l2;
                int oa0, or0, og0, ob0, oa1, or1, og1, ob1, oa2, or2, og2, ob2;
                argb_unpack(v0->oargb, &oa0, &or0, &og0, &ob0);
                argb_unpack(v1->oargb, &oa1, &or1, &og1, &ob1);
                argb_unpack(v2->oargb, &oa2, &or2, &og2, &ob2);
                orr = (int)(ll0 * or0 + ll1 * or1 + ll2 * or2);
                og = (int)(ll0 * og0 + ll1 * og1 + ll2 * og2);
                ob = (int)(ll0 * ob0 + ll1 * ob1 + ll2 * ob2);
                oa = 0;
                fr = clamp8(fr + orr);
                fg = clamp8(fg + og);
                fb = clamp8(fb + ob);
                (void)oa;
            }

            frag = ((uint32_t)fa << 24) | (fr << 16) | (fg << 8) | fb;
            zoff = fb_base + (uint32_t)idx * fb_bpp;

            /* Punch-through: alpha test at 0x80. */
            if (list == LIST_PUNCH && fa < 0x80) {
                continue;
            }

            if (list == LIST_TRANS) {
                uint32_t dstc = fb_load(vram, zoff, fb_fmt);
                int da, dr, dg, db;
                float fs[4], fd[4];

                argb_unpack(dstc, &da, &dr, &dg, &db);
                blend_factor(sf, fa, da, fs);
                blend_factor(df, fa, da, fd);
                fr = clamp8((int)(fr * fs[1] + dr * fd[1]));
                fg = clamp8((int)(fg * fs[2] + dg * fd[2]));
                fb = clamp8((int)(fb * fs[3] + db * fd[3]));
                frag = ((uint32_t)fa << 24) | (fr << 16) | (fg << 8) | fb;
            }

            fb_store(vram, zoff, fb_fmt, frag);
            zbuf[idx] = z;
        }
    }
}

/* ----------------------------- TA parsing -------------------------------- */

static void ta_reset_strip(DCTaState *s)
{
    s->nverts = 0;
    s->parity = 0;
}

/* Process one assembled 32-byte parameter. */
static void ta_param(DCTaState *s)
{
    uint32_t pcw = s->param[0];
    int type = PCW_PARA_TYPE(pcw);

    switch (type) {
    case PARA_END_OF_LIST:
        ta_reset_strip(s);
        s->have_ctx = false;
        break;

    case PARA_POLY:
    case PARA_SPRITE: {
        int lt = PCW_LIST_TYPE(pcw);

        if (lt >= NR_LISTS) {
            lt = LIST_OPAQUE;
        }
        s->cur.isp = s->param[1];
        s->cur.tsp = s->param[2];
        s->cur.tcw = s->param[3];
        s->cur.textured = PCW_TEXTURED(pcw);
        s->cur.offset = PCW_OFFSET(pcw);
        s->cur.gouraud = PCW_GOURAUD(pcw);
        s->cur_list = lt;
        s->have_ctx = true;
        ta_reset_strip(s);
        break;
    }

    case PARA_VERTEX: {
        TaVertex v;

        if (!s->have_ctx) {
            break;
        }
        v.x = u2f(s->param[1]);
        v.y = u2f(s->param[2]);
        v.z = u2f(s->param[3]);
        v.u = u2f(s->param[4]);
        v.v = u2f(s->param[5]);
        v.argb = s->param[6];
        v.oargb = s->param[7];

        if (s->nverts < 3) {
            s->strip[s->nverts++] = v;
        } else {
            s->strip[0] = s->strip[1];
            s->strip[1] = s->strip[2];
            s->strip[2] = v;
            s->parity ^= 1;
        }

        if (s->nverts == 3) {
            TaTri t;
            t.ctx = s->cur;
            if (s->parity) {
                t.v[0] = s->strip[0];
                t.v[1] = s->strip[2];
                t.v[2] = s->strip[1];
            } else {
                t.v[0] = s->strip[0];
                t.v[1] = s->strip[1];
                t.v[2] = s->strip[2];
            }
            g_array_append_val(s->lists[s->cur_list], t);
        }

        if (PCW_END_OF_STRIP(pcw)) {
            ta_reset_strip(s);
        }
        break;
    }

    default:                    /* user clip, object list set: ignore */
        break;
    }
}

static void ta_fifo_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    DCTaState *s = opaque;

    s->param[s->pidx++] = (uint32_t)val;
    if (s->pidx == 8) {
        s->pidx = 0;
        ta_param(s);
    }
}

static uint64_t ta_fifo_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;                   /* the FIFO is write-only */
}

static const MemoryRegionOps ta_fifo_ops = {
    .read = ta_fifo_read,
    .write = ta_fifo_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------- exported -------------------------------- */

void dc_ta_list_init(DeviceState *dev)
{
    DCTaState *s = DC_TA(dev);
    int i;

    for (i = 0; i < NR_LISTS; i++) {
        g_array_set_size(s->lists[i], 0);
    }
    s->pidx = 0;
    s->have_ctx = false;
    ta_reset_strip(s);
}

void dc_ta_start_render(DeviceState *dev, uint32_t fb_base, uint32_t fb_ctrl,
                        uint32_t linestride, uint32_t xclip, uint32_t yclip)
{
    DCTaState *s = DC_TA(dev);
    uint8_t *vram = memory_region_get_ram_ptr(s->vram);
    uint32_t fmt = fb_ctrl & 7;
    int bpp = (fmt == 5) ? 4 : 2;
    int w = ((xclip >> 16) & 0x7ff) + 1;
    int h = ((yclip >> 16) & 0x3ff) + 1;
    uint32_t base = (fb_base & 0x1fffffff);
    static const int order[] = { LIST_OPAQUE, LIST_PUNCH, LIST_TRANS };
    float *zbuf;
    int i, o, px;

    if (base >= VRAM_PHYS_BASE) {
        base -= VRAM_PHYS_BASE;
    }
    base &= (VRAM_SIZE - 1);

    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        w = 640; h = 480;
    }

    /* Clear the render target to opaque black, and the depth buffer. */
    for (px = 0; px < w * h; px++) {
        fb_store(vram, base + (uint32_t)px * bpp, fmt, 0xff000000);
    }
    zbuf = g_new0(float, (size_t)w * h);

    for (o = 0; o < (int)ARRAY_SIZE(order); o++) {
        int list = order[o];
        GArray *arr = s->lists[list];

        for (i = 0; i < (int)arr->len; i++) {
            const TaTri *t = &g_array_index(arr, TaTri, i);
            raster_tri(vram, zbuf, w, h, base, fmt, bpp, list, t);
        }
    }

    g_free(zbuf);
    (void)linestride;

    /*
     * Defer "end of render (TSP)" by a short interval instead of raising it
     * synchronously here.  Real ISP/TSP rendering takes milliseconds, and a
     * guest that kicks STARTRENDER then blocks waiting for the IRQ must be
     * asleep before it fires - otherwise the completion is lost (the wait
     * samples an already-incremented counter).  ~0.5 ms of virtual time is
     * ample for the guest to reach its wait.
     */
    timer_mod(s->render_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000);
}

/* Timer callback: latch the render-done event (Holly, edge-triggered). */
static void dc_ta_render_done(void *opaque)
{
    DCTaState *s = opaque;

    qemu_set_irq(s->render_irq, 1);
}

/* ------------------------------ QOM plumbing ----------------------------- */

static void dc_ta_realize(DeviceState *dev, Error **errp)
{
    DCTaState *s = DC_TA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    if (!s->vram) {
        error_setg(errp, "dc-ta: vram region not connected");
        return;
    }
    memory_region_init_io(&s->fifo, OBJECT(s), &ta_fifo_ops, s, "dc-ta-fifo",
                          TA_FIFO_SIZE);
    sysbus_init_mmio(sbd, &s->fifo);
    sysbus_init_irq(sbd, &s->render_irq);
    s->render_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dc_ta_render_done, s);

    for (i = 0; i < NR_LISTS; i++) {
        s->lists[i] = g_array_new(FALSE, FALSE, sizeof(TaTri));
    }
}

static void dc_ta_reset_hold(Object *obj, ResetType type)
{
    dc_ta_list_init(DEVICE(obj));
}

static void dc_ta_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dc_ta_realize;
    rc->phases.hold = dc_ta_reset_hold;
    /* Captured geometry is transient host state; nothing to migrate. */
}

static const TypeInfo dc_ta_info = {
    .name          = TYPE_DC_TA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCTaState),
    .class_init    = dc_ta_class_init,
};

static void dc_ta_register_types(void)
{
    type_register_static(&dc_ta_info);
}

type_init(dc_ta_register_types)

/*
 * Board helper: create the Tile Accelerator, connect VRAM, map its input FIFO
 * at 0x10000000, and wire the render-done IRQ to Holly.  Returns the device so
 * the board can hand it to dc_pvr_init() (the PVR register block forwards
 * TA_LIST_INIT / STARTRENDER to it).
 */
DeviceState *dc_ta_init(hwaddr fifo_base, MemoryRegion *vram,
                        qemu_irq render_irq)
{
    DeviceState *dev = qdev_new(TYPE_DC_TA);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    DC_TA(dev)->vram = vram;
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, fifo_base);
    sysbus_connect_irq(sbd, 0, render_irq);
    return dev;
}
