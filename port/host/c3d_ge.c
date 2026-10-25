/* citro3d backend for the GE on the New 3DS -- gl_ge.c's design on the PICA200.
 *
 * The runtime (psprecomp/src/hle/gpu.c) does the vertex work and hands each
 * draw over as a screen-space triangle list plus decoded state; this file
 * rasterizes it on the PICA200 and keeps emulated VRAM coherent with the
 * render targets it holds, as gl_ge.c does (its comments apply here):
 *
 *   - each colour buffer the game draws into is a 512x512 RGBA8 texture used
 *     as a citro3d render target, keyed by (VRAM offset, stride, format) and
 *     seeded from VRAM; depth buffers are kept by their VRAM address;
 *   - dirty rows are read back only when something is about to read VRAM,
 *     and a CPU access to VRAM reaches cpu_access through psp_mem_set_vram_hook;
 *   - overlapping targets go through VRAM (gl_ge.c's GPU transfer, rt_xfer,
 *     isn't done here yet).
 *
 * Rows: as in gl_ge.c, texture row y IS PSP row y: row 0 is the first in
 * memory, where the vertex shader's NDC y = -1 lands (checked by reading a
 * target back: c3d_ge_capture). The PICA keeps textures and
 * render targets in 8x8 tiles in Morton order (MORTON below); the CPU converts
 * to and from that when it uploads or reads back.
 *
 * Order: citro3d queues the GPU's commands and runs them when the frame (or
 * a part of it) is submitted. Before the CPU reads or rewrites memory that a
 * queued draw uses -- a target it reads back or uploads, a cached texture it
 * replaces, the vertex ring when full -- gpu_finish submits what is queued
 * and waits for the GPU.
 *
 * Memory: the PICA200 draws only into VRAM (6 MB; citro3d refuses a render
 * target anywhere else), so the targets and the depth buffer live there --
 * as many targets as fit (1 MB at 512 wide, 0.5 MB at 256), a 16-bit depth
 * buffer of 0.5 MB, the top screen's 0.4 MB -- and the CPU reads and writes
 * them in place. PSP2i's title uses five targets (two displayed buffers, the
 * scene, two 256-wide ones overlapping them); with four slots one was
 * evicted every frame and overlapping targets swapped garbage through VRAM. Textures and
 * the vertex ring are in linear (FCRAM) memory.
 *
 * Not done yet: logic ops, stencil, the doubled blend factors (approximated
 * as in gl_ge.c), partial colour masks (a channel is written or not), the
 * alpha test's mask, texture packs, stereo. */

#include <3ds.h>
#include <citro3d.h>

#include "c3d_ge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>

#include "c3d_ge_shbin.h"                       /* c3d_ge.v.pica, built by picasso */

#define VRAM_SIZE  0x200000u
#define RT_W       512u                          /* the widest target (stride <= 512); narrower fit their stride */
#define RT_H       512u                          /* every target's rows */
#define MAX_RT     8                             /* slots; VRAM decides how many are used */
#define MAX_DEPTH  1
#define MAX_TEX    256
#define TEX_BUDGET (12u << 20)                   /* bytes of decoded textures kept */
#define RING_VERTS 49152u                        /* psp_gpu_vertex (40 bytes): 1.9 MB */

static int g_ok;
static DVLB_s *g_dvlb;
static shaderProgram_s g_prog;
static int u_vp;
static C3D_AttrInfo *g_attr;
static psp_gpu_vertex *g_ring;                   /* linear memory: the GPU reads it */
static uint32_t g_ring_pos;
static C3D_RenderTarget *g_top;                  /* the top screen, left eye */

/* The frame: open from the first draw after a present until the next
 * present; g_seg counts the parts submitted (gpu_finish), so a resource used
 * in the current part (seg == g_seg) is still read by queued commands. */
static int g_in_frame;
static uint32_t g_seg = 1;
static C3D_RenderTarget *g_cur;                  /* what C3D_FrameDrawOn was given in this part */
static void *g_cur_depth;

typedef struct {
    int used;
    uint32_t off, stride, w, h;
    int fmt;
    C3D_Tex tex;
    C3D_RenderTarget *rt;
    void *depth;                                 /* depth buffer attached now */
    int dirty;
    uint32_t dy0, dy1;
    uint64_t drow[8];
    uint32_t rows;
    uint64_t synced_hash;
    uint64_t check_frame;
    uint64_t last_use;
    uint32_t seg;
    uint64_t n_drawn, n_up, n_back;              /* draws into it, uploads, readbacks (c3d_ge_report) */
} rt_entry;

typedef struct {
    int used;
    uint32_t off;
    void *buf;                                   /* RT_W x RT_H, 16-bit depth like the PSP's */
    uint64_t last_use;
} depth_entry;

typedef struct {
    int used;
    uint64_t key, hash, check_frame, last_use;
    uint32_t w, h, gw, gh;                       /* the PSP's size; the GPU's (8 at least) */
    C3D_Tex tex;
    uint32_t seg;
} tex_entry;

static rt_entry    g_rt[MAX_RT];
static depth_entry g_depth[MAX_DEPTH];
static tex_entry   g_tex[MAX_TEX];
static uint32_t    g_tex_bytes;
static C3D_Tex     g_scratch;                    /* a copy of a target sampled while drawn to */
static int         g_scratch_ok;
static uint64_t    g_use;

static uint64_t n_draws, n_tris, n_uploads, n_readbacks, n_tex_decodes, n_rt_tex, n_approx,
                n_finish, n_present, n_fail;

/* ---- helpers (as gl_ge.c) ------------------------------------------------------------- */

static uint8_t *vram(void) { return psp_mem.vram; }
static uint64_t frame_no(void) { return psp_display_flips(); }
static uint32_t vram_off(uint32_t addr) { return addr & 0x1FFFFFu; }

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t expand16(int fmt, uint16_t p) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0: r = p & 0x1F; g = (p >> 5) & 0x3F; b = (p >> 11) & 0x1F;
            return 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
    case 1: r = p & 0x1F; g = (p >> 5) & 0x1F; b = (p >> 10) & 0x1F;
            return ((p & 0x8000) ? 0xFF000000u : 0) | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
    default: r = p & 0xF; g = (p >> 4) & 0xF; b = (p >> 8) & 0xF; a = (p >> 12) & 0xF;
            return ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);
    }
}

static uint16_t pack16(int fmt, uint32_t c) {
    uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (fmt) {
    case 0:  return (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
    case 1:  return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
    default: return (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
    }
}

static int bpp_of(int fmt) { return fmt == 3 ? 4 : 2; }
static int overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }
static uint32_t rt_end(const rt_entry *t) { return t->off + t->rows * t->stride * (uint32_t)bpp_of(t->fmt); }

/* The PICA's RGBA8 texel is 0xRRGGBBAA; the PSP's (and the runtime's
 * decoded textures) 0xAABBGGRR. */
static uint32_t to_pica(uint32_t c) { return __builtin_bswap32(c); }
static uint32_t from_pica(uint32_t c) { return __builtin_bswap32(c); }

/* MORTON: where texel (x, y) of a w-wide texture is in memory -- 8x8 tiles in
 * rows, each tile's 64 texels in Morton (Z) order of x and y. */
static uint8_t g_mx[8], g_my[8];
static void morton_init(void) {
    for (int i = 0; i < 8; i++) {
        g_mx[i] = (uint8_t)((i & 1) | ((i & 2) << 1) | ((i & 4) << 2));
        g_my[i] = (uint8_t)(((i & 1) << 1) | ((i & 2) << 2) | ((i & 4) << 3));
    }
}
static uint32_t morton(uint32_t x, uint32_t y, uint32_t w) {
    return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + g_mx[x & 7] + g_my[y & 7];
}

/* ROWS: the PICA's sampler and its rendering agree with each other, and
 * both have a texture's PSP row y at memory row height-1-y as the CPU sees
 * it (seen in Azahar: decoded textures stored row 0 first came out upside
 * down, and the text's UVs read the empty bottom of PSP2i's glyph atlas).
 * So everything the CPU writes or reads in a texture or target goes through
 * mrow(): decoded textures, uploads from VRAM; read-backs through
 * g_readback_flip, measured at start-up (staging_selftest). */
static uint32_t mrow(uint32_t y, uint32_t h) { return h - 1 - y; }
static int g_readback_flip;                      /* staging_from's rows run h-1 .. 0 */

/* ---- the frame and the queue ---------------------------------------------------------- */

static void frame_open(void) {
    if (g_in_frame) return;
    C3D_FrameBegin(0);                           /* waits until the GPU has finished what was queued */
    g_in_frame = 1;
    g_ring_pos = 0;
    g_cur = NULL;
    g_cur_depth = NULL;
}

static void ring_flush(void) {
    if (g_ring_pos) GSPGPU_FlushDataCache(g_ring, g_ring_pos * sizeof(psp_gpu_vertex));
}

/* Submit what is queued and wait for the GPU: the CPU may then touch any
 * texture or target, and the vertex ring starts over. */
static void gpu_finish(void) {
    if (!g_in_frame) return;
    ring_flush();
    C3D_FrameEnd(0);
    g_in_frame = 0;
    g_seg++;
    n_finish++;
    frame_open();
}

/* Before the CPU reads or writes memory a queued draw may still use. Between
 * frames the last one is submitted but may still be running (present doesn't
 * wait), so opening a frame -- which waits for the GPU -- comes first; the
 * game reads its glyphs back from VRAM right after a present, and got
 * half-drawn ones before this waited. */
static void before_cpu_write(uint32_t seg) {
    if (!g_in_frame) frame_open();
    else if (seg == g_seg) gpu_finish();
}

/* ---- coherence on CPU access (as gl_ge.c) ------------------------------------------- */

#define PG_SHIFT 12
#define NPG (VRAM_SIZE >> PG_SHIFT)
static uint8_t g_pg_dirty[NPG], g_pg_armed[NPG];

static void pages_set(uint8_t *map, uint32_t a0, uint32_t a1) {
    if (a1 > VRAM_SIZE) a1 = VRAM_SIZE;
    if (a0 >= a1) return;
    for (uint32_t p = a0 >> PG_SHIFT; p <= (a1 - 1) >> PG_SHIFT; p++) map[p] = 1;
}

static uint32_t row_addr(const rt_entry *t, uint32_t y) { return t->off + y * t->stride * (uint32_t)bpp_of(t->fmt); }
static int row_dirty(const rt_entry *t, uint32_t y) { return y < 512 && ((t->drow[y >> 6] >> (y & 63)) & 1); }

static void rows_mark(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    if (y0 >= y1) return;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] |= 1ull << (y & 63);
    if (!t->dirty) { t->dy0 = y0; t->dy1 = y1; }
    else { if (y0 < t->dy0) t->dy0 = y0; if (y1 > t->dy1) t->dy1 = y1; }
    t->dirty = 1;
    pages_set(g_pg_dirty, row_addr(t, y0), row_addr(t, y1));
}

/* ---- moving targets to and from the CPU ------------------------------------------------
 * The CPU doesn't read or write a target's VRAM itself: the GPU's transfer
 * engine copies it, tiles and all, to and from g_staging (linear memory),
 * and the CPU works there. That's the 3DS's way, and the one Azahar follows:
 * it renders on the PC's GPU and puts the pixels into the 3DS's memory only
 * on such transfers (seen there: CPU reads of a target returned what the CPU
 * had written at its creation, never what the GPU drew). */
static uint32_t *g_staging;                      /* RT_W x RT_H x 4 bytes */

/* tex's pixels (a target), untiled, as the CPU reads them: row srow(y) of
 * tex->width texels each. A display transfer, not a raw texture copy:
 * Azahar writes the GPU's picture back into the 3DS's memory for display
 * transfers (how frames reach the screens), but a texture copy of a target
 * read stale bytes there. Call before_cpu_write first. */
#define READBACK_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
static const uint32_t *staging_from(C3D_Tex *tex) {
    const u32 dim = GX_BUFFER_DIM(tex->width, tex->height);
    C3D_SyncDisplayTransfer((u32 *)tex->data, dim, g_staging, dim, READBACK_FLAGS);
    GSPGPU_InvalidateDataCache(g_staging, tex->size);
    return g_staging;
}
/* which row of staging_from's picture is PSP row y */
static uint32_t srow(uint32_t y, uint32_t h) { return g_readback_flip ? h - 1 - y : y; }

/* What the CPU wrote in g_staging into tex (a target). */
static void staging_to(C3D_Tex *tex) {
    GSPGPU_FlushDataCache(g_staging, tex->size);
    C3D_SyncTextureCopy(g_staging, 0, (u32 *)tex->data, 0, tex->size, GX_TRANSFER_RAW_COPY(1));
}

/* Which way round staging_from's rows come (the display transfer's own
 * convention, which Azahar and the console may not share): a 64x64 VRAM
 * texture gets row y = y the way uploads write it, and is read back. */
static void staging_selftest(void) {
    C3D_Tex tex;
    if (!C3D_TexInitVRAM(&tex, 64, 64, GPU_RGBA8)) return;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) g_staging[morton(x, mrow(y, 64), 64)] = to_pica(0xFF000000u | y);
    staging_to(&tex);
    memset(g_staging, 0, 64 * 64 * 4);
    GSPGPU_FlushDataCache(g_staging, 64 * 64 * 4);
    const uint32_t *out = staging_from(&tex);
    const uint32_t first = from_pica(out[0]) & 0xFF, last = from_pica(out[63 * 64]) & 0xFF;
    g_readback_flip = first == 63 && last == 0;
    fprintf(stderr, "c3d: read-back self-test: first row holds %lu, last %lu: %s\n", (unsigned long)first,
            (unsigned long)last, first == 0 && last == 63 ? "in order" : g_readback_flip ? "reversed" : "UNEXPECTED");
    C3D_TexDelete(&tex);
}

/* ---- render targets ------------------------------------------------------------------ */

static void rt_free(rt_entry *t) {
    if (t->rt) {
        /* citro3d panics on a target deleted inside a frame: end it (the
         * next draw waits for the GPU and opens another) */
        if (g_in_frame) {
            ring_flush();
            C3D_FrameEnd(0);
            g_in_frame = 0;
            g_seg++;
            n_finish++;
        }
        C3D_RenderTargetDelete(t->rt);
    }
    if (t->tex.data) C3D_TexDelete(&t->tex);
    memset(t, 0, sizeof *t);
}

/* The target's dirty rows into emulated VRAM, converted to its format. */
static void rt_readback(rt_entry *t) {
    if (!t->dirty) return;
    uint8_t *v = vram();
    uint32_t y0 = t->dy0, y1 = t->dy1 > t->h ? t->h : t->dy1;
    if (!v || y0 >= y1) { t->dirty = 0; memset(t->drow, 0, sizeof t->drow); return; }
    before_cpu_write(t->seg);                    /* its queued draws must have run */
    const uint32_t *tx = staging_from(&t->tex);
    const int bpp = bpp_of(t->fmt);
    for (uint32_t y = y0; y < y1; y++) {
        if (!row_dirty(t, y)) continue;
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        if (o + t->stride * (uint32_t)bpp > VRAM_SIZE) break;
        for (uint32_t x = 0; x < t->stride; x++) {
            const uint32_t c = from_pica(tx[srow(y, t->h) * t->w + x]);
            if (bpp == 4) memcpy(v + o + x * 4, &c, 4);
            else { const uint16_t p = pack16(t->fmt, c); memcpy(v + o + x * 2, &p, 2); }
        }
    }
    t->dirty = 0;
    t->dy0 = t->dy1 = 0;
    memset(t->drow, 0, sizeof t->drow);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    t->n_back++;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && &g_rt[i] != t && overlaps(t->off, t->off + bytes, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;
    n_readbacks++;
}

/* Emulated VRAM into the target (its first `rows` rows). */
static void rt_upload(rt_entry *t) {
    uint8_t *v = vram();
    if (!v) return;
    before_cpu_write(t->seg);
    uint32_t *tx = g_staging;
    const int bpp = bpp_of(t->fmt);
    for (uint32_t y = 0; y < t->rows; y++) {
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        for (uint32_t x = 0; x < t->stride; x++) {
            uint32_t c = 0;
            if (o + (x + 1) * (uint32_t)bpp <= VRAM_SIZE) {
                if (bpp == 4) memcpy(&c, v + o + x * 4, 4);
                else { uint16_t p; memcpy(&p, v + o + x * 2, 2); c = expand16(t->fmt, p); }
            }
            tx[morton(x, mrow(y, t->h), t->w)] = to_pica(c);
        }
    }
    staging_to(&t->tex);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    pages_set(g_pg_armed, t->off, t->off + bytes);
    t->n_up++;
    n_uploads++;
}

static rt_entry *rt_find(uint32_t off, uint32_t stride, int fmt) {
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].off == off && g_rt[i].stride == stride && g_rt[i].fmt == fmt) return &g_rt[i];
    return NULL;
}

/* Target t back to VRAM and gone, its slot free. */
static void rt_evict(rt_entry *t) {
    fprintf(stderr, "c3d: evicting render target VRAM+0x%06lX stride %lu format %d\n",
            (unsigned long)t->off, (unsigned long)t->stride, t->fmt);
    rt_readback(t);
    before_cpu_write(t->seg);
    rt_free(t);
    frame_open();                                /* rt_free ended the frame */
}

static rt_entry *rt_get(uint32_t off, uint32_t stride, int fmt, uint32_t rows) {
    rt_entry *t = rt_find(off, stride, fmt);
    if (!t) {
        for (int i = 0; i < MAX_RT && !t; i++) if (!g_rt[i].used) t = &g_rt[i];
        if (!t) {
            t = &g_rt[0];
            for (int i = 1; i < MAX_RT; i++) if (g_rt[i].last_use < t->last_use) t = &g_rt[i];
            rt_evict(t);
        }
        /* the narrowest power of two that holds the stride: VRAM is short */
        uint32_t w = 64;
        while (w < stride) w <<= 1;
        while (!C3D_TexInitVRAM(&t->tex, (u16)w, RT_H, GPU_RGBA8)) {
            rt_entry *old = NULL;                /* VRAM is full: the least recently used target goes */
            for (int i = 0; i < MAX_RT; i++)
                if (g_rt[i].used && (!old || g_rt[i].last_use < old->last_use)) old = &g_rt[i];
            if (!old) {
                fprintf(stderr, "c3d: no VRAM for a render target (%lu KB free)\n", (unsigned long)(vramSpaceFree() >> 10));
                memset(t, 0, sizeof *t);
                return NULL;
            }
            rt_evict(old);
        }
        t->rt = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, -1);
        if (!t->rt) {
            fprintf(stderr, "c3d: C3D_RenderTargetCreateFromTex failed\n");
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof *t);
            return NULL;
        }
        t->off = off; t->stride = stride; t->fmt = fmt;
        t->w = w; t->h = RT_H;
        t->used = 1;
        t->rows = rows;
        fprintf(stderr, "c3d: render target VRAM+0x%06lX stride %lu format %d rows %lu\n",
                (unsigned long)off, (unsigned long)stride, fmt, (unsigned long)rows);
        rt_upload(t);
        t->check_frame = frame_no();
    }
    if (rows > t->rows) {
        rt_readback(t);
        t->rows = rows;
        rt_upload(t);
    }
    t->last_use = ++g_use;
    return t;
}

static void rt_check(rt_entry *t) {
    uint64_t f = frame_no();
    if (t->check_frame == f || t->dirty) return;
    t->check_frame = f;
    uint8_t *v = vram();
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp_of(t->fmt);
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    if (v && hash_bytes(v + t->off, bytes) != t->synced_hash) rt_upload(t);
    pages_set(g_pg_armed, t->off, t->off + bytes);
}

/* ---- depth buffers ---------------------------------------------------------------------- */

static void *depth_get(uint32_t off) {
    depth_entry *d = NULL;
    for (int i = 0; i < MAX_DEPTH; i++) if (g_depth[i].used && g_depth[i].off == off) d = &g_depth[i];
    if (!d) {
        for (int i = 0; i < MAX_DEPTH && !d; i++) if (!g_depth[i].used) d = &g_depth[i];
        if (!d) {
            d = &g_depth[0];
            for (int i = 1; i < MAX_DEPTH; i++) if (g_depth[i].last_use < d->last_use) d = &g_depth[i];
            gpu_finish();                        /* a queued draw may still use it */
            for (int i = 0; i < MAX_RT; i++) if (g_rt[i].used && g_rt[i].depth == d->buf) g_rt[i].depth = NULL;
        } else {
            d->buf = vramAlloc(RT_W * RT_H * 2);
            if (!d->buf) {
                fprintf(stderr, "c3d: no VRAM for a depth buffer (%lu KB free)\n", (unsigned long)(vramSpaceFree() >> 10));
                return NULL;
            }
        }
        /* cleared to 0, as gl_ge.c does a new one: by the GPU's fill engine,
         * since the CPU's view of VRAM isn't kept in step (see staging_from) */
        before_cpu_write(g_seg);
        C3D_SyncMemoryFill((u32 *)d->buf, 0, (u32 *)((uint8_t *)d->buf + RT_W * RT_H * 2),
                           GX_FILL_TRIGGER | GX_FILL_16BIT_DEPTH, NULL, 0, NULL, 0);
        d->off = off; d->used = 1;
    }
    d->last_use = ++g_use;
    return d->buf;
}

/* ---- textures ----------------------------------------------------------------------------- */

static tex_entry *tex_find(uint64_t key) {
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].used && g_tex[i].key == key) return &g_tex[i];
    return NULL;
}

static void tex_free(tex_entry *e) {
    if (e->tex.data) { g_tex_bytes -= e->tex.size; C3D_TexDelete(&e->tex); }
    memset(e, 0, sizeof *e);
}

/* The least recently used entry that no queued draw reads (or any, after a
 * gpu_finish), freed; NULL if none is in use. */
static tex_entry *tex_evict(void) {
    tex_entry *old = NULL;
    for (int i = 0; i < MAX_TEX; i++)
        if (g_tex[i].used && g_tex[i].seg != g_seg && (!old || g_tex[i].last_use < old->last_use)) old = &g_tex[i];
    if (!old) {
        gpu_finish();
        for (int i = 0; i < MAX_TEX; i++)
            if (g_tex[i].used && (!old || g_tex[i].last_use < old->last_use)) old = &g_tex[i];
    }
    if (old) tex_free(old);
    return old;
}

static void tex_fill(tex_entry *e, const psp_gpu_state *st) {
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * st->tex_h * 4);
    if (!px) return;
    psp_gpu_decode_texture(px);
    uint32_t *tx = (uint32_t *)e->tex.data;
    /* a texture under 8 texels a side repeats to fill the GPU's 8 (the UVs
     * are scaled by the GPU's size, so wrapping still repeats it); rows: ROWS */
    for (uint32_t y = 0; y < e->gh; y++)
        for (uint32_t x = 0; x < e->gw; x++)
            tx[morton(x, mrow(y, e->gh), e->gw)] = to_pica(px[(y % st->tex_h) * st->tex_w + (x % st->tex_w)]);
    free(px);
    GSPGPU_FlushDataCache(e->tex.data, e->tex.size);
    n_tex_decodes++;
}

static tex_entry *tex_get(const psp_gpu_state *st) {
    tex_entry *e = tex_find(st->tex_key);
    uint64_t f = frame_no();
    if (e && e->check_frame == f) { e->last_use = ++g_use; return e; }
    uint64_t h = psp_gpu_texture_hash();
    if (e && e->hash == h) { e->check_frame = f; e->last_use = ++g_use; return e; }
    if (e) before_cpu_write(e->seg);             /* new bytes under the same key */
    else {
        const uint32_t gw = st->tex_w < 8 ? 8 : st->tex_w, gh = st->tex_h < 8 ? 8 : st->tex_h;
        while (g_tex_bytes + gw * gh * 4 > TEX_BUDGET && tex_evict()) {}
        for (int i = 0; i < MAX_TEX && !e; i++) if (!g_tex[i].used) e = &g_tex[i];
        if (!e) e = tex_evict();
        if (!e) return NULL;
        if (!C3D_TexInit(&e->tex, (u16)gw, (u16)gh, GPU_RGBA8)) {
            while (tex_evict()) if (C3D_TexInit(&e->tex, (u16)gw, (u16)gh, GPU_RGBA8)) break;
            if (!e->tex.data) { memset(e, 0, sizeof *e); return NULL; }
        }
        g_tex_bytes += e->tex.size;
        e->key = st->tex_key; e->w = st->tex_w; e->h = st->tex_h; e->gw = gw; e->gh = gh; e->used = 1;
    }
    tex_fill(e, st);
    e->hash = h; e->check_frame = f; e->last_use = ++g_use;
    return e;
}

/* A copy of target t to sample while t is being drawn to. */
static C3D_Tex *scratch_copy(rt_entry *t) {
    gpu_finish();                                /* t's queued draws first; the scratch is free after */
    if (g_scratch_ok && g_scratch.width != t->w) { C3D_TexDelete(&g_scratch); g_scratch_ok = 0; }
    if (!g_scratch_ok) {
        if (!C3D_TexInit(&g_scratch, (u16)t->w, (u16)t->h, GPU_RGBA8)) return NULL;
        g_scratch_ok = 1;
    }
    C3D_SyncTextureCopy((u32 *)t->tex.data, 0, (u32 *)g_scratch.data, 0, t->tex.size, GX_TRANSFER_RAW_COPY(1));
    return &g_scratch;
}

/* The texture for this draw (decoded, or a target the game drew), and the
 * UV scale (1 / the GPU texture's size). */
static C3D_Tex *texture_view(const psp_gpu_state *st, rt_entry *target, float scale[2]) {
    const uint32_t toff = vram_off(st->tex_addr);
    if ((st->tex_addr & 0x0F000000u) == 0x04000000u) {
        static const int tbpp[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
        uint32_t tend = toff + st->tex_bufw * st->tex_h * (uint32_t)tbpp[st->tex_psm > 10 ? 3 : st->tex_psm] / 8;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *t = &g_rt[i];
            if (!t->used || !overlaps(toff, tend, t->off, rt_end(t))) continue;
            const int same_fmt = st->tex_psm <= 3 && st->tex_psm == t->fmt && !st->tex_swz;
            if (t->dirty && t->off == toff && st->tex_bufw == t->stride && same_fmt) {
                n_rt_tex++;
                scale[0] = 1.0f / (float)t->w;
                scale[1] = 1.0f / (float)t->h;
                if (t != target) { t->seg = g_seg; return &t->tex; }
                return scratch_copy(t);
            }
            if (t->dirty && overlaps(toff, tend, row_addr(t, t->dy0), row_addr(t, t->dy1))) rt_readback(t);
        }
    }
    tex_entry *e = tex_get(st);
    if (!e) return NULL;
    e->seg = g_seg;
    scale[0] = 1.0f / (float)e->gw;
    scale[1] = 1.0f / (float)e->gh;
    return &e->tex;
}

/* ---- state ------------------------------------------------------------------------------- */

static GPU_BLENDFACTOR map_factor(int f, int is_src, uint32_t fix, uint32_t *blend_color, int *approx) {
    switch (f) {
    case 0:  return is_src ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case 1:  return is_src ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case 2:  return GPU_SRC_ALPHA;
    case 3:  return GPU_ONE_MINUS_SRC_ALPHA;
    case 4:  return GPU_DST_ALPHA;
    case 5:  return GPU_ONE_MINUS_DST_ALPHA;
    case 6:  *approx = 1; return GPU_SRC_ALPHA;            /* 2 x src alpha */
    case 7:  *approx = 1; return GPU_ONE_MINUS_SRC_ALPHA;
    case 8:  *approx = 1; return GPU_DST_ALPHA;
    case 9:  *approx = 1; return GPU_ONE_MINUS_DST_ALPHA;
    default:
        if (fix == 0xFFFFFF) return GPU_ONE;
        if (fix == 0) return GPU_ZERO;
        if (*blend_color == 0xFFFFFFFFu || *blend_color == fix) { *blend_color = fix; return GPU_CONSTANT_COLOR; }
        if (*blend_color == (~fix & 0xFFFFFF)) return GPU_ONE_MINUS_CONSTANT_COLOR;
        *approx = 1;
        return GPU_CONSTANT_COLOR;
    }
}

/* Blending, the colour mask, the depth test and the alpha test. */
static void apply_state(const psp_gpu_state *st, int have_depth, int *approx) {
    int mask = 0;
    if (st->clear) {
        if (st->clear_which & 1) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
        if (st->clear_which & 2) mask |= GPU_WRITE_ALPHA;
    } else {
        for (int ch = 0; ch < 3; ch++) {
            uint32_t m = (st->mask_rgb >> (ch * 8)) & 0xFF;
            if (m != 0xFF) mask |= GPU_WRITE_RED << ch;
            if (m != 0 && m != 0xFF) *approx = 1;
        }
        if (st->mask_a != 0xFF) mask |= GPU_WRITE_ALPHA;
        if (st->mask_a != 0 && st->mask_a != 0xFF) *approx = 1;
    }
    if (st->fb_fmt == 0) mask &= ~GPU_WRITE_ALPHA;        /* 565 has no alpha */

    int zon, zwrite;
    GPU_TESTFUNC zfn;
    if (st->clear) { zon = (st->clear_which & 4) != 0; zwrite = 1; zfn = GPU_ALWAYS; }
    else { zon = st->ztest; zwrite = st->zwrite; zfn = (GPU_TESTFUNC)(st->zfunc & 7); }   /* same order as the GE's */
    if (!have_depth) zon = 0;
    if (zon && zwrite) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(true, zon ? zfn : GPU_ALWAYS, (GPU_WRITEMASK)mask);

    uint32_t bc = 0xFFFFFFFFu;
    if (st->blend && !st->clear) {
        const GPU_BLENDFACTOR s = map_factor(st->bsrc, 1, st->fixa, &bc, approx);
        const GPU_BLENDFACTOR d = map_factor(st->bdst, 0, st->fixb, &bc, approx);
        static const GPU_BLENDEQUATION OPS[6] = { GPU_BLEND_ADD, GPU_BLEND_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT,
                                                  GPU_BLEND_MIN, GPU_BLEND_MAX, GPU_BLEND_MAX };
        if (st->beq >= 5) *approx = 1;
        C3D_AlphaBlend(OPS[st->beq < 6 ? st->beq : 0], GPU_BLEND_ADD, s, d, GPU_ONE, GPU_ZERO);   /* keeps the source alpha */
    } else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    if (bc == 0xFFFFFFFFu) bc = 0;
    C3D_BlendingColor(0xFF000000u | bc);

    if (st->atest && !st->clear) C3D_AlphaTest(true, (GPU_TESTFUNC)(st->afunc & 7), (int)(st->aref & 0xFF));
    else C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

/* The texture function on TEV stage 0 (gl_ge.c's fragment shader). */
static void apply_tev(const psp_gpu_state *st, int textured) {
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (!textured || st->clear) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        return;
    }
    const int a_tex = st->trgba != 0;
    switch (st->tfunc) {
    case 0:                                                /* modulate */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        break;
    case 1:                                                /* decal */
        if (a_tex) {
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        } else {
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        }
        break;
    case 2:                                                /* blend: mix(colour, env, texture) */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env, 0xFF000000u | (st->env & 0xFFFFFF));
        break;
    case 3:                                                /* replace */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        break;
    default:                                               /* add */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
        break;
    }
    if (!a_tex || st->tfunc == 1) {                        /* the vertex alpha */
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    } else if (st->tfunc == 3) {                           /* the texture's */
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    } else {                                               /* both */
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    }
    if (st->tdbl) C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
}

/* Draw on target t with depth buffer `depth` (or none). */
static void draw_on(rt_entry *t, void *depth) {
    if (g_cur == t->rt && g_cur_depth == depth && t->depth == depth) return;
    C3D_FrameBufDepth(&t->rt->frameBuf, depth, GPU_RB_DEPTH16);
    t->depth = depth;
    C3D_FrameDrawOn(t->rt);
    g_cur = t->rt;
    g_cur_depth = depth;
}

/* nverts vertices into the ring; the index of the first, or -1 if they don't
 * fit even in an empty ring. */
static int ring_put(const psp_gpu_vertex *v, int nverts) {
    if ((uint32_t)nverts > RING_VERTS) return -1;
    if (g_ring_pos + (uint32_t)nverts > RING_VERTS) gpu_finish();
    const int first = (int)g_ring_pos;
    memcpy(g_ring + g_ring_pos, v, (size_t)nverts * sizeof *v);
    g_ring_pos += (uint32_t)nverts;
    return first;
}


/* ---- the draw ------------------------------------------------------------------------------------ */

static int c3d_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if (!g_ok || (st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || st->fb_stride > RT_W || nverts <= 0)
        return 1;
    if ((uint32_t)nverts > RING_VERTS) return 1;
    frame_open();
    if (C3D_GetCmdBufUsage() > 0.85f) gpu_finish();      /* room for this draw's commands */
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > RT_H) rows = RT_H;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) { n_fail++; return 1; }
    {   /* overlapping targets: their newer rows through VRAM (gl_ge.c without rt_xfer) */
        const uint32_t t0 = t->off, t1 = rt_end(t);
        int flushed = 0;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !o->dirty || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            rt_readback(o);
            flushed = 1;
        }
        if (flushed && !t->dirty) t->check_frame = (uint64_t)-1;
        rt_check(t);
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (o->used && o != t && overlaps(t0, t1, o->off, rt_end(o))) o->check_frame = (uint64_t)-1;
        }
    }

    void *depth = NULL;
    if (st->zb_stride && (st->ztest || (st->clear && (st->clear_which & 4)))) depth = depth_get(vram_off(st->zb_addr));

    C3D_Tex *tex = NULL;
    float uvs[2] = { 1, 1 };
    if (st->tex && !st->clear) {
        tex = texture_view(st, t, uvs);
        if (!tex) { n_fail++; return 1; }
    }

    const int first = ring_put(v, nverts);
    if (first < 0) return 1;

    draw_on(t, depth);
    t->seg = g_seg;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)st->sx0, (u32)st->sy0, (u32)st->sx1 + 1, (u32)st->sy1 + 1);
    C3D_CullFace(GPU_CULL_NONE);                         /* culling is done before submission */
    int approx = 0;
    apply_state(st, depth != NULL, &approx);
    if (approx) n_approx++;
    apply_tev(st, tex != NULL);
    if (tex) {
        C3D_TexSetFilter(tex, st->tex_linear ? GPU_LINEAR : GPU_NEAREST, st->tex_linear ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(tex, st->tex_clamp_u ? GPU_CLAMP_TO_EDGE : GPU_REPEAT, st->tex_clamp_v ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
        C3D_TexBind(0, tex);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / (float)t->w, 2.0f / (float)t->h, uvs[0], uvs[1]);
    C3D_DrawArrays(GPU_TRIANGLES, first, nverts);

    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) rows_mark(t, (uint32_t)y0, (uint32_t)y1);
    t->n_drawn++;
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    return 0;
}

static void c3d_sync_vram(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i]))) rt_readback(&g_rt[i]);
}

static void c3d_vram_written(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i]))) g_rt[i].check_frame = (uint64_t)-1;
}

/* Every CPU access to VRAM (psp_mem_set_vram_hook), as gl_cpu_access. */
static void cpu_access(uint32_t off, uint32_t size) {
    const uint32_t a0 = off, a1 = off + size > VRAM_SIZE ? VRAM_SIZE : off + size;
    if (a0 >= a1) return;
    const uint32_t p0 = a0 >> PG_SHIFT, p1 = (a1 - 1) >> PG_SHIFT;
    int dirty = 0, armed = 0;
    for (uint32_t p = p0; p <= p1; p++) { dirty |= g_pg_dirty[p]; armed |= g_pg_armed[p]; }
    if (!dirty && !armed) return;
    const uint32_t q0 = p0 << PG_SHIFT, q1 = (p1 + 1) << PG_SHIFT;
    if (dirty) {
        for (int i = 0; i < MAX_RT; i++)
            if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
                { rt_readback(&g_rt[i]); g_rt[i].check_frame = (uint64_t)-1; }
        memset(g_pg_dirty + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {
            const rt_entry *t = &g_rt[i];
            if (!t->used || !t->dirty) continue;
            uint32_t d0 = row_addr(t, t->dy0), d1 = row_addr(t, t->dy1);
            if (d0 < q0) d0 = q0;
            if (d1 > q1) d1 = q1;
            pages_set(g_pg_dirty, d0, d1);
        }
    }
    if (armed) {
        for (int i = 0; i < MAX_RT; i++)
            if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i]))) g_rt[i].check_frame = (uint64_t)-1;
        memset(g_pg_armed + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {
            const rt_entry *t = &g_rt[i];
            if (!t->used || t->check_frame == (uint64_t)-1) continue;
            uint32_t d0 = t->off, d1 = rt_end(t);
            if (d0 < q0) d0 = q0;
            if (d1 > q1) d1 = q1;
            pages_set(g_pg_armed, d0, d1);
        }
    }
}

/* ---- presenting ------------------------------------------------------------------------------------
 * The displayed buffer's target (made from VRAM when the GPU didn't draw it,
 * e.g. the CPU's movie frames) as one quad on the top screen's render
 * target, 480x272 scaled to 400x240.
 *
 * The screen's target is 240x400: the LCD is mounted sideways, so screen
 * pixel (sx, sy) is target (240 - sy, 400 - sx) in the vertex shader's
 * convention (checked in Azahar on the disclaimer's text). */
#define SCREEN_W 480
#define SCREEN_H 272
#define TOP_W    400
#define TOP_H    240
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void quad_vertex(psp_gpu_vertex *o, float sx, float sy, float u, float v) {
    memset(o, 0, sizeof *o);
    o->x = (float)TOP_H - sy;
    o->y = (float)TOP_W - sx;
    o->rhw = 1.0f;
    o->u = u; o->v = v;
    o->r = o->g = o->b = o->a = 255.0f;
}

int c3d_ge_present(uint32_t addr, uint32_t stride, int fmt) {
    if (!g_ok) return -1;
    frame_open();
    rt_entry *t = NULL;
    if ((addr & 0x0F000000u) == 0x04000000u && stride && stride <= RT_W) {
        t = rt_get(vram_off(addr), stride, fmt, SCREEN_H);
        if (t) rt_check(t);
    }
    C3D_RenderTargetClear(g_top, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(g_top);
    g_cur = g_top;
    g_cur_depth = NULL;
    if (t) {
        psp_gpu_vertex q[6];
        quad_vertex(&q[0], 0, 0, 0, 0);
        quad_vertex(&q[1], TOP_W, 0, SCREEN_W, 0);
        quad_vertex(&q[2], 0, TOP_H, 0, SCREEN_H);
        q[3] = q[2];
        q[4] = q[1];
        quad_vertex(&q[5], TOP_W, TOP_H, SCREEN_W, SCREEN_H);
        const int first = ring_put(q, 6);
        if (first >= 0) {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_TexEnv *env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, &t->tex);
            t->seg = g_seg;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / (float)TOP_H, 2.0f / (float)TOP_W,
                          1.0f / (float)t->w, 1.0f / (float)t->h);
            C3D_DrawArrays(GPU_TRIANGLES, first, 6);
        }
    }
    ring_flush();
    C3D_FrameEnd(0);                             /* submits; the top screen shows it once drawn */
    g_in_frame = 0;
    g_seg++;
    n_present++;
    return 0;
}

int c3d_ge_capture(uint32_t addr, uint32_t stride, int fmt, const char *path) {
    if (!g_ok || (addr & 0x0F000000u) != 0x04000000u || !stride || stride > RT_W) return -1;
    rt_entry *t = rt_find(vram_off(addr), stride, fmt);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    const uint32_t *tx = NULL;
    if (t && t->dirty) {                         /* the GPU's picture: wait for it, read it in place */
        before_cpu_write(t->seg);
        tx = staging_from(&t->tex);
    }
    const uint8_t *v = vram();
    const int bpp = bpp_of(fmt);
    for (uint32_t y = 0; y < SCREEN_H; y++)
        for (uint32_t x = 0; x < SCREEN_W; x++) {
            uint32_t c = 0;
            if (tx) c = from_pica(tx[srow(y, t->h) * t->w + x]);
            else {
                const uint32_t o = vram_off(addr) + (y * stride + x) * (uint32_t)bpp;
                if (v && o + (uint32_t)bpp <= VRAM_SIZE) {
                    if (bpp == 4) memcpy(&c, v + o, 4);
                    else { uint16_t p; memcpy(&p, v + o, 2); c = expand16(fmt, p); }
                }
            }
            const uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 0;
}

/* ---- set-up ---------------------------------------------------------------------------------------- */

static const psp_gpu_backend C3D_BACKEND = { "citro3d", c3d_draw, c3d_sync_vram, c3d_vram_written, 1 };

int c3d_ge_init(void) {
    morton_init();
    g_dvlb = DVLB_ParseFile((u32 *)c3d_ge_shbin, c3d_ge_shbin_size);
    if (!g_dvlb) return -1;
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_prog);
    u_vp = shaderInstanceGetUniformLocation(g_prog.vertexShader, "vp");

    g_attr = C3D_GetAttrInfo();
    AttrInfo_Init(g_attr);
    AttrInfo_AddLoader(g_attr, 0, GPU_FLOAT, 4);          /* x, y, z, 1/w */
    AttrInfo_AddLoader(g_attr, 1, GPU_FLOAT, 2);          /* u, v */
    AttrInfo_AddLoader(g_attr, 2, GPU_FLOAT, 4);          /* r, g, b, a */

    g_ring = (psp_gpu_vertex *)linearAlloc(RING_VERTS * sizeof(psp_gpu_vertex));
    if (!g_ring) return -1;
    g_staging = (uint32_t *)linearAlloc(RT_W * RT_H * 4);
    if (!g_staging) return -1;
    staging_selftest();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g_ring, sizeof(psp_gpu_vertex), 3, 0x210);

    for (int i = 1; i < 6; i++) {                        /* stages 1-5 pass stage 0 through */
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
    }
    C3D_CullFace(GPU_CULL_NONE);                         /* culling is done before submission */
    C3D_DepthMap(true, -1.0f, 0.0f);                     /* depth = the shader's 0..1 */

    g_top = C3D_RenderTargetCreate(TOP_H, TOP_W, GPU_RB_RGBA8, -1);
    if (!g_top) return -1;
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    g_ok = 1;
    psp_gpu_set_backend(&C3D_BACKEND);
    psp_mem_set_vram_hook(cpu_access);
    fprintf(stderr, "c3d: rendering on the PICA200\n");
    return 0;
}

void c3d_ge_report(FILE *out) {
    int rts = 0, depths = 0, texs = 0;
    for (int i = 0; i < MAX_RT; i++) rts += g_rt[i].used;
    for (int i = 0; i < MAX_DEPTH; i++) depths += g_depth[i].used;
    for (int i = 0; i < MAX_TEX; i++) texs += g_tex[i].used;
    fprintf(out, "c3d: %llu draws, %llu tris, %llu presents, %llu finishes; uploads %llu, readbacks %llu, "
                 "texture decodes %llu (%d cached, %lu KB), target textures %llu, approximated %llu, "
                 "to software %llu; %d targets, %d depth buffers; linear free %lu KB, VRAM free %lu KB\n",
            (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_present,
            (unsigned long long)n_finish, (unsigned long long)n_uploads, (unsigned long long)n_readbacks,
            (unsigned long long)n_tex_decodes, texs, (unsigned long)(g_tex_bytes >> 10),
            (unsigned long long)n_rt_tex, (unsigned long long)n_approx, (unsigned long long)n_fail,
            rts, depths, (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(vramSpaceFree() >> 10));
    for (int i = 0; i < MAX_RT; i++) {
        const rt_entry *t = &g_rt[i];
        if (!t->used) continue;
        fprintf(out, "c3d:   target VRAM+0x%06lX/%lu fmt %d: %llu draws, %llu uploads, %llu readbacks, %s\n",
                (unsigned long)t->off, (unsigned long)t->stride, t->fmt, (unsigned long long)t->n_drawn,
                (unsigned long long)t->n_up, (unsigned long long)t->n_back, t->dirty ? "GPU ahead" : "in step with VRAM");
    }
}
