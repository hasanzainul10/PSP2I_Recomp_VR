/* citro3d backend for the GE on the New 3DS -- gl_ge.c's design on the PICA200.
 *
 * The runtime (psprecomp/src/hle/gpu.c) does the vertex work and hands each
 * draw over as a screen-space triangle list plus decoded state; this file
 * rasterizes it on the PICA200 and keeps emulated VRAM coherent with the
 * render targets it holds, as gl_ge.c does (its comments apply here):
 *
 *   - each colour buffer the game draws into is an RGBA8 texture used as a
 *     citro3d render target (its stride wide and its rows tall, each rounded
 *     up to a power of two), keyed by (VRAM offset, stride, format) and
 *     seeded from VRAM; depth buffers are kept by their VRAM address;
 *   - dirty rows are read back only when something is about to read VRAM,
 *     and a CPU access to VRAM reaches cpu_access through psp_mem_set_vram_hook;
 *   - overlapping targets hand rows over on the GPU where the PICA can
 *     (rt_take; the rest goes through VRAM).
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
 * as many targets as fit (1 MB at 512x512, 128 KB at 256x128), a 16-bit
 * depth buffer of 0.5 MB, the top screen's 0.4 MB -- and the CPU reads and
 * writes them in place. PSP2i's title uses five targets (two displayed
 * buffers, the scene, two 256-wide ones overlapping them); with four slots
 * one was evicted every frame and overlapping targets swapped garbage through
 * VRAM. In the field four more 256x128 passes come every frame: at 512 rows
 * each they didn't fit and evicted each other every frame (a read-back, a
 * GPU wait and an upload each), so a target is only as tall as its rows.
 * Textures and the vertex ring are in linear (FCRAM) memory.
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
#include "c3d_test_shbin.h"                     /* c3d_test.v.pica: the self-test's reference */

#define VRAM_SIZE  0x200000u
#define RT_W       512u                          /* the widest target (stride <= 512); narrower fit their stride */
#define RT_H       512u                          /* the tallest target; shorter fit their rows */
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
    uint64_t back_why[6], up_why[3];             /* readbacks and uploads by cause (RB_*, UP_*) */
    uint64_t seq;                                /* when it was last drawn (g_draw_seq) */
} rt_entry;

/* Why a target goes to or comes from VRAM (counted, c3d_ge_report) */
enum { RB_EVICT, RB_ROWS, RB_TEXTURE, RB_OVERLAP, RB_SYNC, RB_CPU };
enum { UP_NEW, UP_ROWS, UP_CHANGED };
static const char *const RB_NAMES[6] = { "evict", "rows", "texture", "overlap", "sync", "cpu" };
static const char *const UP_NAMES[3] = { "new", "rows", "changed" };

typedef struct {
    int used;
    uint32_t off;
    void *buf[2];                                /* RT_W x RT_H, 16-bit depth like the PSP's: the copy
                                                    in VRAM bank A, in bank B (made when a draw needs it) */
    int cur;                                     /* the copy with the latest values; -1: none yet */
    uint64_t last_use;
} depth_entry;

typedef struct {
    int used;
    uint64_t key, data_hash, clut_hash, check_frame, last_use;
    uint64_t next_data_check;                    /* tex_get: the bytes' next check (frame) */
    uint32_t data_gap;                           /* frames between those checks, 1..TEX_GAP_MAX */
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
static uint64_t    g_draw_seq;

/* c3d_draw_on's parts, ticks (D_* below; the c3d report gives us per draw) */
enum { D_TARGET, D_TEXTURE, D_RING, D_STATE, D_SUBMIT, D_ROWS, D_N };
static uint64_t g_dt[D_N];
static uint64_t g_hash_ticks, g_hash_n, g_hash_bytes, g_fill_ticks, g_tex_evictions, g_hash_skip;   /* tex_get's checks */

static uint64_t n_draws, n_tris, n_uploads, n_readbacks, n_tex_decodes, n_rt_tex, n_approx,
                n_finish, n_present, n_fail, n_evict, n_cmdfull;

/* ---- where the time goes (c3d_ge_stats) ------------------------------------------------
 * Exclusive buckets: prof(b) charges the time since the last switch to the
 * bucket that was running and makes b run; it returns that one, to switch
 * back to. Outside the backend (P_OUT) nothing is charged. */
enum { P_OUT, P_DRAW, P_WAIT, P_RB, P_UP, P_TEX, P_PRESENT, P_N };
static uint64_t g_pt[P_N], g_pt0;
static int g_pb;
static int prof(int b) {
    const uint64_t n = svcGetSystemTick();
    if (g_pb) g_pt[g_pb] += n - g_pt0;
    g_pt0 = n;
    const int o = g_pb;
    g_pb = b;
    return o;
}

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

static int g_diag_n;                             /* psp2i_diag: the first events only */
/* VRAM's two 3 MB banks (libctru: A from 0x1F000000, B from 0x1F300000) */
static char vram_bank(const void *p) { return (uintptr_t)p - 0x1F000000u >= 0x300000u ? 'B' : 'A'; }
static int g_diag_present, g_in_present;         /* a present's steps traced for the first 10 only */
#define DIAG(...) do { if (psp2i_diag_on && g_diag_n < 3000 && (!g_in_present || g_diag_present <= 10))                            { g_diag_n++; psp2i_diag(__VA_ARGS__); } } while (0)

static void frame_open(void) {
    if (g_in_frame) return;
    const int pb = prof(P_WAIT);
    DIAG("c3d: FrameBegin");
    C3D_FrameBegin(0);                           /* waits until the GPU has finished what was queued */
    DIAG("c3d: FrameBegin done");
    prof(pb);
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
    const int pb = prof(P_WAIT);
    DIAG("c3d: finish FrameEnd");
    C3D_FrameEnd(0);
    DIAG("c3d: finish FrameEnd done");
    prof(pb);
    g_in_frame = 0;
    g_seg++;
    n_finish++;
    frame_open();
}

/* Room in the command buffer for one more draw's commands, else submit and
 * wait first (gpu_finish: it resets the vertex ring, so before ring_put).
 * The buffer is reclaimed only when a frame begins, and citro3d's sync
 * transfers split it mid-frame; C3D_GetCmdBufUsage is the fill at the last
 * submit, not now. A town scene of 1,600 draws a frame ran it out:
 * svcBreak in GPUCMD_AddInternal (3DS_PROGRESS.md, T4). */
#define CMD_ROOM (16u << 10)
static void cmd_room(void) {
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    if ((size - off) * 4u < CMD_ROOM) { n_cmdfull++; gpu_finish(); }
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

static void rows_bounds(rt_entry *t) {
    uint32_t lo = 512, hi = 0;
    for (uint32_t y = 0; y < 512; y++) if (row_dirty(t, y)) { if (y < lo) lo = y; hi = y + 1; }
    if (lo >= hi) { t->dirty = 0; t->dy0 = t->dy1 = 0; memset(t->drow, 0, sizeof t->drow); }
    else { t->dirty = 1; t->dy0 = lo; t->dy1 = hi; }
}

static void rows_clear(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] &= ~(1ull << (y & 63));
    rows_bounds(t);
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
    const int pb = prof(P_WAIT);
    DIAG("c3d: readback transfer");
    C3D_SyncDisplayTransfer((u32 *)tex->data, dim, g_staging, dim, READBACK_FLAGS);
    DIAG("c3d: readback transfer done");
    GSPGPU_InvalidateDataCache(g_staging, tex->size);
    prof(pb);
    return g_staging;
}
/* which row of staging_from's picture is PSP row y */
static uint32_t srow(uint32_t y, uint32_t h) { return g_readback_flip ? h - 1 - y : y; }

/* What the CPU wrote in g_staging into tex (a target). */
static void staging_to(C3D_Tex *tex) {
    const int pb = prof(P_WAIT);
    GSPGPU_FlushDataCache(g_staging, tex->size);
    DIAG("c3d: upload copy");
    C3D_SyncTextureCopy(g_staging, 0, (u32 *)tex->data, 0, tex->size, GX_TRANSFER_RAW_COPY(1));
    DIAG("c3d: upload copy done");
    prof(pb);
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
            const int pb = prof(P_WAIT);
            C3D_FrameEnd(0);
            prof(pb);
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
static void rt_readback(rt_entry *t, int why) {
    if (!t->dirty) return;
    uint8_t *v = vram();
    uint32_t y0 = t->dy0, y1 = t->dy1 > t->h ? t->h : t->dy1;
    if (!v || y0 >= y1) { t->dirty = 0; memset(t->drow, 0, sizeof t->drow); return; }
    const int pb = prof(P_RB);
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
    t->back_why[why]++;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && &g_rt[i] != t && overlaps(t->off, t->off + bytes, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;
    n_readbacks++;
    prof(pb);
}

/* Emulated VRAM into the target (its first `rows` rows). */
static void rt_upload(rt_entry *t, int why) {
    uint8_t *v = vram();
    if (!v) return;
    const int pb = prof(P_UP);
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
    t->up_why[why]++;
    n_uploads++;
    prof(pb);
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
    rt_readback(t, RB_EVICT);
    before_cpu_write(t->seg);
    rt_free(t);
    frame_open();                                /* rt_free ended the frame */
    n_evict++;
}

static rt_entry *rt_get(uint32_t off, uint32_t stride, int fmt, uint32_t rows) {
    rt_entry *t = rt_find(off, stride, fmt);
    if (t && rows > t->h) {                      /* taller than its texture: made again, from VRAM */
        rt_evict(t);
        t = NULL;
    }
    if (!t) {
        for (int i = 0; i < MAX_RT && !t; i++) if (!g_rt[i].used) t = &g_rt[i];
        if (!t) {
            t = &g_rt[0];
            for (int i = 1; i < MAX_RT; i++) if (g_rt[i].last_use < t->last_use) t = &g_rt[i];
            rt_evict(t);
        }
        /* the narrowest power of two that holds the stride: VRAM is short */
        uint32_t w = 64, h = 64;
        while (w < stride) w <<= 1;
        while (h < rows) h <<= 1;
        while (!C3D_TexInitVRAM(&t->tex, (u16)w, (u16)h, GPU_RGBA8)) {
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
        DIAG("c3d: target create %06X %ux%u fmt %d", (unsigned)t->off, (unsigned)t->w, (unsigned)t->h, t->fmt);
        t->rt = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, -1);
        if (!t->rt) {
            fprintf(stderr, "c3d: C3D_RenderTargetCreateFromTex failed\n");
            C3D_TexDelete(&t->tex);
            memset(t, 0, sizeof *t);
            return NULL;
        }
        t->off = off; t->stride = stride; t->fmt = fmt;
        t->w = w; t->h = h;
        t->used = 1;
        t->rows = rows;
        fprintf(stderr, "c3d: render target VRAM+0x%06lX stride %lu format %d rows %lu (%lux%lu, VRAM free %lu KB)\n",
                (unsigned long)off, (unsigned long)stride, fmt, (unsigned long)rows, (unsigned long)w,
                (unsigned long)h, (unsigned long)(vramSpaceFree() >> 10));
        rt_upload(t, UP_NEW);
        t->check_frame = frame_no();
    }
    if (rows > t->rows) {
        rt_readback(t, RB_ROWS);
        t->rows = rows;
        rt_upload(t, UP_ROWS);
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
    if (v && hash_bytes(v + t->off, bytes) != t->synced_hash) rt_upload(t, UP_CHANGED);
    pages_set(g_pg_armed, t->off, t->off + bytes);
}

/* ---- depth buffers ---------------------------------------------------------------------- */

/* The depth buffer for a draw into a target whose colour buffer is in VRAM
 * bank colour_b (0 A, 1 B). The PICA200 locks up -- the whole console, Rosalina
 * included -- drawing with its colour and depth buffers in the same VRAM bank:
 * the game's first draw (a clear with depth) froze the console every time,
 * and citro3d's own C3D_RenderTargetCreate puts depth in the opposite bank
 * (3DS_PROGRESS.md, T4). Azahar doesn't mind. So the depth buffer has a copy
 * in each bank, made when a draw first needs it; switching banks copies the
 * latest values over on the GPU. Targets that share one depth buffer usually
 * sit in the same bank, which needs no copy. */
static uint64_t n_depth_copies;

static void *depth_get(uint32_t off, int colour_b) {
    const int want = !colour_b;
    const uint32_t size = RT_W * RT_H * 2;
    depth_entry *d = NULL;
    for (int i = 0; i < MAX_DEPTH; i++) if (g_depth[i].used && g_depth[i].off == off) d = &g_depth[i];
    if (!d) {
        for (int i = 0; i < MAX_DEPTH && !d; i++) if (!g_depth[i].used) d = &g_depth[i];
        if (!d) {
            d = &g_depth[0];
            for (int i = 1; i < MAX_DEPTH; i++) if (g_depth[i].last_use < d->last_use) d = &g_depth[i];
            gpu_finish();                        /* a queued draw may still use it */
            for (int i = 0; i < MAX_RT; i++)
                if (g_rt[i].used && g_rt[i].depth && (g_rt[i].depth == d->buf[0] || g_rt[i].depth == d->buf[1]))
                    g_rt[i].depth = NULL;
        }
        d->off = off; d->used = 1; d->cur = -1;  /* its values: cleared below */
    }
    if (!d->buf[want]) {
        d->buf[want] = vramAllocAt(size, want ? VRAM_ALLOC_B : VRAM_ALLOC_A);
        if (!d->buf[want]) {
            fprintf(stderr, "c3d: no VRAM in bank %c for a depth buffer (%lu KB free)\n", want ? 'B' : 'A',
                    (unsigned long)(vramSpaceFree() >> 10));
            return NULL;
        }
    }
    if (d->cur < 0) {
        /* cleared to 0, as gl_ge.c does a new one: by the GPU's fill engine,
         * since the CPU's view of VRAM isn't kept in step (see staging_from) */
        before_cpu_write(g_seg);
        C3D_SyncMemoryFill((u32 *)d->buf[want], 0, (u32 *)((uint8_t *)d->buf[want] + size),
                           GX_FILL_TRIGGER | GX_FILL_16BIT_DEPTH, NULL, 0, NULL, 0);
        d->cur = want;
    } else if (d->cur != want) {
        gpu_finish();                            /* the draws on the other copy first */
        const int pb = prof(P_WAIT);
        C3D_SyncTextureCopy((u32 *)d->buf[d->cur], 0, (u32 *)d->buf[want], 0, size, GX_TRANSFER_RAW_COPY(1));
        prof(pb);
        d->cur = want;
        n_depth_copies++;
    }
    d->last_use = ++g_use;
    return d->buf[want];
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
    if (old) { tex_free(old); g_tex_evictions++; }
    return old;
}

/* The paletted formats (CLUT4, CLUT8: almost every texture PSP2i decodes)
 * in one pass: the source walked in its own layout through one pointer, the
 * palette converted to the PICA's order once, each texel written straight to
 * its tile position. The generic path below decodes texel by texel into a
 * temporary, then tiles it: a 512x512 CLUT4 texture took ~150 ms in Azahar
 * (3DS_PROGRESS.md, T4). Same texels; 0 to leave the texture to that path. */
static int tex_fill_clut(tex_entry *e, const psp_gpu_state *st) {
    if ((st->tex_psm != 4 && st->tex_psm != 5) || e->gw != st->tex_w || e->gh != st->tex_h) return 0;
    psp_gpu_texinfo ti;
    if (!psp_gpu_texture_info(&ti) || ti.w != e->gw || ti.h != e->gh) return 0;
    const int c4 = ti.psm == 4;
    uint32_t pal[256];
    for (uint32_t i = 0; i < (c4 ? 16u : 256u); i++) pal[i] = to_pica(ti.pal[i]);
    const uint32_t row_bytes = c4 ? ti.bufw / 2 : ti.bufw;          /* the buffer's */
    const uint32_t bpr = row_bytes / 16 ? row_bytes / 16 : 1;          /* swizzle blocks a row */
    const uint32_t wbytes = c4 ? ti.w / 2 : ti.w;                     /* the texture's */
    uint32_t *tx = (uint32_t *)e->tex.data;
    for (uint32_t y = 0; y < ti.h; y++) {
        const uint32_t my = mrow(y, e->gh);
        uint32_t *trow = tx + (((my >> 3) * (e->gw >> 3)) << 6) + g_my[my & 7];
        for (uint32_t xb = 0; xb < wbytes; xb += 16) {
            const uint8_t *src = ti.swizzled ? ti.data + ((y / 8) * bpr + xb / 16) * 128 + (y % 8) * 16
                                             : ti.data + y * row_bytes + xb;
            const uint32_t nb = wbytes - xb < 16 ? wbytes - xb : 16;
            for (uint32_t k = 0; k < nb; k++) {
                const uint32_t b = src[k];
                if (c4) {
                    const uint32_t x = (xb + k) * 2;
                    trow[((x >> 3) << 6) + g_mx[x & 7]] = pal[b & 15];
                    trow[(((x + 1) >> 3) << 6) + g_mx[(x + 1) & 7]] = pal[b >> 4];
                } else {
                    const uint32_t x = xb + k;
                    trow[((x >> 3) << 6) + g_mx[x & 7]] = pal[b];
                }
            }
        }
    }
    return 1;
}

static void tex_fill(tex_entry *e, const psp_gpu_state *st) {
    {
        const int pb = prof(P_TEX);
        const int ok = tex_fill_clut(e, st);
        prof(pb);
        if (ok) {
            GSPGPU_FlushDataCache(e->tex.data, e->tex.size);
            n_tex_decodes++;
            return;
        }
    }
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * st->tex_h * 4);
    if (!px) return;
    const int pb = prof(P_TEX);
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
    prof(pb);
}

/* When a cached texture is checked against the PSP's memory. Its CLUT on the
 * first use in every frame (palette animations; at most 1 KB). Its bytes:
 * every frame for a texture in VRAM (the GE draws there) and while the game
 * is loading (a loading-like file read, psp_mem_load_writes, in the last
 * TEX_LOAD_MS: PSP2i decompresses what it reads after reading it; time, not
 * frames, as the 3DS draws a few frames a second), else on a schedule that doubles while they stay the same, up to
 * TEX_GAP_MAX frames, back to every frame when they change. Hashing every texture every frame was 4-6 ms a
 * frame in town (3DS_PROGRESS.md, T4). */
#define TEX_GAP_MAX     16u
#define TEX_LOAD_MS     3000u
static uint32_t g_io_seen;
static uint64_t g_io_tick;

static tex_entry *tex_get(const psp_gpu_state *st) {
    tex_entry *e = tex_find(st->tex_key);
    uint64_t f = frame_no();
    if (e && e->check_frame == f) { e->last_use = ++g_use; return e; }
    const uint32_t io = psp_mem_load_writes();
    const uint64_t now = svcGetSystemTick();
    if (io != g_io_seen) { g_io_seen = io; g_io_tick = now; }
    const int sure = (st->tex_addr & 0x0F000000u) == 0x04000000u ||
                     now - g_io_tick < (uint64_t)SYSCLOCK_ARM11 / 1000u * TEX_LOAD_MS;
    const uint64_t h0 = svcGetSystemTick();
    const uint64_t ch = psp_gpu_clut_hash();
    uint64_t dh = 0;
    const int skip = e && e->clut_hash == ch && !sure && f < e->next_data_check;
    if (skip) g_hash_skip++;
    if (!skip) {
        dh = psp_gpu_texture_data_hash();
        g_hash_n++;
        g_hash_bytes += (uint64_t)st->tex_bufw * st->tex_h;   /* texels, roughly */
    }
    g_hash_ticks += svcGetSystemTick() - h0;
    if (e && e->clut_hash == ch && (skip || e->data_hash == dh)) {
        if (!skip) {
            e->data_gap = e->data_gap * 2 > TEX_GAP_MAX ? TEX_GAP_MAX : e->data_gap * 2;
            e->next_data_check = f + e->data_gap;
        }
        e->check_frame = f; e->last_use = ++g_use;
        return e;
    }
    const int was_new = !e;
    const uint64_t ev0 = g_tex_evictions;
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
    const uint64_t f0 = svcGetSystemTick();
    tex_fill(e, st);
    const uint64_t fd = svcGetSystemTick() - f0;
    g_fill_ticks += fd;
    {   /* which textures are decoded (a few per 10 s) */
        static uint64_t win, logged;
        if (f / 600 != win) { win = f / 600; logged = 0; }
        if (logged++ < 30)
            fprintf(stderr, "c3d: decode %s%s addr %08X psm %d %ux%u bufw %u swz %d: %.2f ms\n",
                    was_new ? "new" : "changed", g_tex_evictions != ev0 ? " (evicted to fit)" : "",
                    (unsigned)st->tex_addr, st->tex_psm, (unsigned)st->tex_w, (unsigned)st->tex_h,
                    (unsigned)st->tex_bufw, st->tex_swz, (double)fd * 1000.0 / SYSCLOCK_ARM11);
    }
    e->data_hash = dh; e->clut_hash = ch;
    e->data_gap = 1; e->next_data_check = f + 1;
    e->check_frame = f; e->last_use = ++g_use;
    return e;
}

/* A copy of target t to sample while t is being drawn to. */
static C3D_Tex *scratch_copy(rt_entry *t) {
    gpu_finish();                                /* t's queued draws first; the scratch is free after */
    if (g_scratch_ok && (g_scratch.width != t->w || g_scratch.height != t->h)) { C3D_TexDelete(&g_scratch); g_scratch_ok = 0; }
    if (!g_scratch_ok) {
        if (!C3D_TexInit(&g_scratch, (u16)t->w, (u16)t->h, GPU_RGBA8)) return NULL;
        g_scratch_ok = 1;
    }
    const int pb = prof(P_WAIT);
    C3D_SyncTextureCopy((u32 *)t->tex.data, 0, (u32 *)g_scratch.data, 0, t->tex.size, GX_TRANSFER_RAW_COPY(1));
    prof(pb);
    return &g_scratch;
}

static int rt_resolve(rt_entry *r);

/* The texture for this draw (decoded, or a target the game drew), and the
 * UV scale (1 / the GPU texture's size). */
static C3D_Tex *texture_view(const psp_gpu_state *st, rt_entry *target, float scale[2]) {
    const uint32_t toff = vram_off(st->tex_addr);
    if ((st->tex_addr & 0x0F000000u) == 0x04000000u) {
        static const int tbpp[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
        uint32_t tend = toff + st->tex_bufw * st->tex_h * (uint32_t)tbpp[st->tex_psm > 10 ? 3 : st->tex_psm] / 8;
        if (st->tex_psm <= 3 && !st->tex_swz) {        /* a target, with what others drew over it (rt_resolve) */
            rt_entry *r = rt_find(toff, st->tex_bufw, st->tex_psm);
            if (r && tend <= rt_end(r) && rt_resolve(r)) {
                n_rt_tex++;
                scale[0] = 1.0f / (float)r->w;
                scale[1] = 1.0f / (float)r->h;
                if (r != target) { r->seg = g_seg; return &r->tex; }
                return scratch_copy(r);
            }
        }
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
            if (t->dirty && overlaps(toff, tend, row_addr(t, t->dy0), row_addr(t, t->dy1))) rt_readback(t, RB_TEXTURE);
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

/* citro3d sends a state group to the GPU when its setter marks it dirty, and
 * the setters mark it dirty even for the value it already has
 * (C3D_GetTexEnv does by being called). So the draws set what changed only:
 * the submit (C3D_DrawArrays) was 16 us a draw in town (3DS_PROGRESS.md,
 * T4). Code that sets state another way (the overlap copies, the present,
 * set-up) calls state_forget. */
static struct {
    int valid;
    u32 sc[4];
    int dt_on; GPU_TESTFUNC dt_fn; GPU_WRITEMASK dt_mask;
    GPU_BLENDEQUATION be_c, be_a; GPU_BLENDFACTOR bs_c, bd_c, bs_a, bd_a;
    u32 bcolor;
    int at_on; GPU_TESTFUNC at_fn; int at_ref;
    C3D_TexEnv env;
    const C3D_Tex *tex; C3D_Tex tex_snap;
    float vp[4];
} g_cs;

static void state_forget(void) { g_cs.valid = 0; g_cs.tex = NULL; }

static void cs_begin(void) {
    if (g_cs.valid) return;
    memset(&g_cs, 0, sizeof g_cs);
    g_cs.dt_fn = (GPU_TESTFUNC)-1; g_cs.at_fn = (GPU_TESTFUNC)-1; g_cs.be_c = (GPU_BLENDEQUATION)-1;
    g_cs.sc[2] = 0xFFFFFFFFu; g_cs.bcolor = 0x12345678u; g_cs.vp[0] = -1.0f;
    g_cs.env.srcRgb = 0xFFFF;                            /* matches no real stage */
    C3D_CullFace(GPU_CULL_NONE);                         /* culling is done before submission */
    g_cs.valid = 1;
}
static void cs_scissor(u32 x0, u32 y0, u32 x1, u32 y1) {
    if (g_cs.sc[0] == x0 && g_cs.sc[1] == y0 && g_cs.sc[2] == x1 && g_cs.sc[3] == y1) return;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
    g_cs.sc[0] = x0; g_cs.sc[1] = y0; g_cs.sc[2] = x1; g_cs.sc[3] = y1;
}
static void cs_depth(GPU_TESTFUNC fn, GPU_WRITEMASK mask) {
    if (g_cs.dt_on && g_cs.dt_fn == fn && g_cs.dt_mask == mask) return;
    C3D_DepthTest(true, fn, mask);
    g_cs.dt_on = 1; g_cs.dt_fn = fn; g_cs.dt_mask = mask;
}
static void cs_blend(GPU_BLENDEQUATION ec, GPU_BLENDEQUATION ea, GPU_BLENDFACTOR sc, GPU_BLENDFACTOR dc,
                     GPU_BLENDFACTOR sa, GPU_BLENDFACTOR da) {
    if (g_cs.be_c == ec && g_cs.be_a == ea && g_cs.bs_c == sc && g_cs.bd_c == dc && g_cs.bs_a == sa && g_cs.bd_a == da) return;
    C3D_AlphaBlend(ec, ea, sc, dc, sa, da);
    g_cs.be_c = ec; g_cs.be_a = ea; g_cs.bs_c = sc; g_cs.bd_c = dc; g_cs.bs_a = sa; g_cs.bd_a = da;
}
static void cs_bcolor(u32 c) {
    if (g_cs.bcolor == c) return;
    C3D_BlendingColor(c);
    g_cs.bcolor = c;
}
static void cs_atest(int on, GPU_TESTFUNC fn, int ref) {
    if (g_cs.at_fn != (GPU_TESTFUNC)-1 && g_cs.at_on == on && g_cs.at_fn == fn && g_cs.at_ref == ref) return;
    C3D_AlphaTest(on, fn, ref);
    g_cs.at_on = on; g_cs.at_fn = fn; g_cs.at_ref = ref;
}
static void cs_env(const C3D_TexEnv *env) {
    if (!memcmp(&g_cs.env, env, sizeof *env)) return;
    *C3D_GetTexEnv(0) = *env;
    g_cs.env = *env;
}
/* the texture with its filter and wrap already set; re-bound when it, or
 * what is in the struct (a texture re-made in the same slot), changed */
static void cs_tex(const C3D_Tex *tex) {
    if (g_cs.tex == tex && !memcmp(&g_cs.tex_snap, tex, sizeof *tex)) return;
    C3D_TexBind(0, (C3D_Tex *)tex);
    g_cs.tex = tex;
    g_cs.tex_snap = *tex;
}
static void cs_vp(float a, float b, float c, float d) {
    if (g_cs.vp[0] == a && g_cs.vp[1] == b && g_cs.vp[2] == c && g_cs.vp[3] == d) return;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, a, b, c, d);
    g_cs.vp[0] = a; g_cs.vp[1] = b; g_cs.vp[2] = c; g_cs.vp[3] = d;
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
    cs_depth(zon ? zfn : GPU_ALWAYS, (GPU_WRITEMASK)mask);

    uint32_t bc = 0xFFFFFFFFu;
    if (st->blend && !st->clear) {
        const GPU_BLENDFACTOR s = map_factor(st->bsrc, 1, st->fixa, &bc, approx);
        const GPU_BLENDFACTOR d = map_factor(st->bdst, 0, st->fixb, &bc, approx);
        static const GPU_BLENDEQUATION OPS[6] = { GPU_BLEND_ADD, GPU_BLEND_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT,
                                                  GPU_BLEND_MIN, GPU_BLEND_MAX, GPU_BLEND_MAX };
        if (st->beq >= 5) *approx = 1;
        cs_blend(OPS[st->beq < 6 ? st->beq : 0], GPU_BLEND_ADD, s, d, GPU_ONE, GPU_ZERO);   /* keeps the source alpha */
    } else cs_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    if (bc == 0xFFFFFFFFu) bc = 0;
    cs_bcolor(0xFF000000u | bc);

    if (st->atest && !st->clear) cs_atest(1, (GPU_TESTFUNC)(st->afunc & 7), (int)(st->aref & 0xFF));
    else cs_atest(0, GPU_ALWAYS, 0);
}

/* The texture function on TEV stage 0 (gl_ge.c's fragment shader), built
 * here and set by cs_env when it differs from the last draw's. */
static void build_tev(const psp_gpu_state *st, int textured, C3D_TexEnv *env) {
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

static void apply_tev(const psp_gpu_state *st, int textured) {
    C3D_TexEnv env;
    build_tev(st, textured, &env);
    cs_env(&env);
}

/* Draw on target t with depth buffer `depth` (or none). */
static void draw_on(rt_entry *t, void *depth) {
    if (g_cur == t->rt && g_cur_depth == depth && t->depth == depth) return;
    C3D_FrameBufDepth(&t->rt->frameBuf, depth, GPU_RB_DEPTH16);
    t->depth = depth;
    C3D_FrameDrawOn(t->rt);                      /* its C3D_SetViewport turns the scissor off */
    g_cs.sc[2] = 0xFFFFFFFFu;
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

/* ---- overlapping targets on the GPU -------------------------------------------------------------
 * gl_ge.c hands a target's newer rows to an overlapping one on the GPU
 * (rt_xfer), reinterpreting the bytes in a fragment shader. The PICA has no
 * fragment shader, so here:
 *   - same format and stride, whole rows apart: the rows are drawn from one
 *     target's texture into the other, queued with the draws (no waiting);
 *   - 16-bit pixels over 32-bit ones (PSP2i's bloom passes, drawn into the
 *     memory of the displayed buffers every frame): not reinterpreted, as in
 *     PPSSPP. The target drawn to now owns the bytes; the other's rows there
 *     stop being newer than VRAM. Before, each of these went to VRAM and back
 *     (about 4 read-backs and 4 uploads a frame in the field);
 *   - anything else goes through VRAM, as before.
 * sdmc:/psp2i/xfer_vram sends everything through VRAM, to compare. */
static uint64_t n_xfer, n_skip;
static int g_xfer_vram;

enum { TAKE_VRAM, TAKE_GPU, TAKE_SKIP };

static uint32_t rt_rowbytes(const rt_entry *t) { return t->stride * (uint32_t)bpp_of(t->fmt); }

static int take_kind(const rt_entry *o, const rt_entry *t) {
    if (g_xfer_vram) return TAKE_VRAM;
    if (bpp_of(o->fmt) != bpp_of(t->fmt)) return TAKE_SKIP;
    if (o->fmt != t->fmt || o->stride != t->stride) return TAKE_VRAM;
    const int32_t delta = (int32_t)o->off - (int32_t)t->off;
    return delta % (int32_t)rt_rowbytes(o) ? TAKE_VRAM : TAKE_GPU;
}

/* o's rows [*a, *b) whose bytes lie inside t */
static void take_rows(const rt_entry *o, const rt_entry *t, uint32_t *a, uint32_t *b) {
    const uint32_t ro = rt_rowbytes(o), oe = rt_end(o), te = rt_end(t);
    const uint32_t lo = o->off > t->off ? o->off : t->off, hi = oe < te ? oe : te;
    *a = *b = 0;
    if (lo >= hi) return;
    *a = (lo - o->off) / ro;
    *b = (hi - o->off + ro - 1) / ro;
    if (*b > o->h) *b = o->h;
}

/* o's rows [y0, y1) drawn into t at their own bytes (TAKE_GPU). own: t
 * now holds them for VRAM too (it's being drawn to); else t only gets a
 * copy to be sampled (rt_resolve), and o keeps them. */
static void rt_xfer(rt_entry *t, rt_entry *o, uint32_t y0, uint32_t y1, int own) {
    const int32_t d = ((int32_t)o->off - (int32_t)t->off) / (int32_t)rt_rowbytes(o);   /* t's row = o's + d */
    if ((int32_t)y0 + d < 0) y0 = (uint32_t)-d;
    if ((int32_t)y1 + d > (int32_t)t->h) y1 = (uint32_t)((int32_t)t->h - d);
    if (y0 >= y1) return;
    const float w = (float)t->stride, ty0 = (float)((int32_t)y0 + d), ty1 = (float)((int32_t)y1 + d);
    psp_gpu_vertex q[6];
    memset(q, 0, sizeof q);
    for (int i = 0; i < 6; i++) { q[i].rhw = 1.0f; q[i].r = q[i].g = q[i].b = q[i].a = 255.0f; }
    static const uint8_t RIGHT[6] = { 0, 1, 0, 0, 1, 1 }, BOTTOM[6] = { 0, 0, 1, 1, 0, 1 };
    for (int i = 0; i < 6; i++) {
        q[i].x = q[i].u = RIGHT[i] ? w : 0.0f;
        q[i].y = BOTTOM[i] ? ty1 : ty0;
        q[i].v = (float)(BOTTOM[i] ? y1 : y0);
    }
    cmd_room();
    const int first = ring_put(q, 6);
    if (first < 0) return;
    draw_on(t, NULL);
    state_forget();                              /* sets its own state below */
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, (u32)ty0, (u32)w, (u32)ty1);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexSetFilter(&o->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&o->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &o->tex);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / (float)t->w, 2.0f / (float)t->h, 1.0f / (float)o->w, 1.0f / (float)o->h);
    C3D_DrawArrays(GPU_TRIANGLES, first, 6);
    t->seg = o->seg = g_seg;
    if (own) { rows_mark(t, (uint32_t)ty0, (uint32_t)ty1); t->seq = ++g_draw_seq; }
    n_xfer++;
}

/* Before drawing on t: what the other targets drew over t's bytes, taken
 * on the GPU or dropped (take_kind). t has been checked against VRAM. */
static void rt_take(rt_entry *t) {
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == t || !o->dirty || !overlaps(t->off, rt_end(t), o->off, rt_end(o))) continue;
        const int kind = take_kind(o, t);
        if (kind == TAKE_VRAM) continue;
        uint32_t a, b;
        take_rows(o, t, &a, &b);
        if (a >= b) continue;
        if (kind == TAKE_GPU)
            for (uint32_t y = a; y < b; ) {
                if (!row_dirty(o, y)) { y++; continue; }
                uint32_t z = y;
                while (z < b && row_dirty(o, z)) z++;
                rt_xfer(t, o, y, z, 1);
                y = z;
            }
        if (kind == TAKE_GPU) rows_clear(o, a, b);
        /* TAKE_SKIP: o keeps its rows, newer than VRAM: it's sampled from them
         * (PSP2i draws its bloom into the displayed buffer's memory and samples
         * it later; dropping them sent that sample to stale VRAM: a flickering
         * glow in town). Where both are read back, the one drawn last wins
         * (readback_range). */
        else n_skip++;
    }
}

/* Before sampling target r: whether its texture is newer than VRAM, after
 * taking what other targets drew over its bytes (gl_ge.c rt_resolve_copy). */
static int rt_resolve(rt_entry *r) {
    const uint32_t r0 = r->off, r1 = rt_end(r);
    int flushed = 0, newer = r->dirty;
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        if (take_kind(o, r) != TAKE_VRAM) continue;
        rt_readback(o, RB_TEXTURE);
        flushed = 1;
    }
    if (flushed && !r->dirty) r->check_frame = (uint64_t)-1;
    rt_check(r);
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        if (take_kind(o, r) != TAKE_GPU || o->seq < r->seq) continue;   /* only rows newer than r's */
        uint32_t a, b;
        take_rows(o, r, &a, &b);
        for (uint32_t y = a; y < b; ) {
            if (!row_dirty(o, y)) { y++; continue; }
            uint32_t z = y;
            while (z < b && row_dirty(o, z)) z++;
            rt_xfer(r, o, y, z, 0);
            newer = 1;
            y = z;
        }
    }
    return newer;
}


/* ---- the draw ------------------------------------------------------------------------------------ */

static int c3d_draw_on(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
static int c3d_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    const int pb = prof(P_DRAW);
    const int r = c3d_draw_on(st, v, nverts);
    prof(pb);
    return r;
}

static int c3d_draw_on(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if (!g_ok || (st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || st->fb_stride > RT_W || nverts <= 0)
        return 1;
    if ((uint32_t)nverts > RING_VERTS) return 1;
    uint64_t tk = svcGetSystemTick(), tn;
#define DPART(p) (tn = svcGetSystemTick(), g_dt[p] += tn - tk, tk = tn)
    frame_open();
    cmd_room();
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > RT_H) rows = RT_H;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) { n_fail++; return 1; }
    {   /* overlapping targets: their newer rows on the GPU where it can (rt_take), else through VRAM */
        const uint32_t t0 = t->off, t1 = rt_end(t);
        int flushed = 0;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !o->dirty || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            if (take_kind(o, t) != TAKE_VRAM) continue;
            rt_readback(o, RB_OVERLAP);
            flushed = 1;
        }
        if (flushed && !t->dirty) t->check_frame = (uint64_t)-1;
        rt_check(t);
        rt_take(t);
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (o->used && o != t && overlaps(t0, t1, o->off, rt_end(o))) o->check_frame = (uint64_t)-1;
        }
    }

    DPART(D_TARGET);
    void *depth = NULL;
    if (st->zb_stride && (st->ztest || (st->clear && (st->clear_which & 4)))) depth = depth_get(vram_off(st->zb_addr), vram_bank(t->tex.data) == 'B');

    C3D_Tex *tex = NULL;
    float uvs[2] = { 1, 1 };
    if (st->tex && !st->clear) {
        tex = texture_view(st, t, uvs);
        if (!tex) { n_fail++; return 1; }
    }

    DPART(D_TEXTURE);
    const int first = ring_put(v, nverts);
    if (first < 0) return 1;
    DPART(D_RING);

    draw_on(t, depth);
    t->seg = g_seg;
    cs_begin();
    cs_scissor((u32)st->sx0, (u32)st->sy0, (u32)st->sx1 + 1, (u32)st->sy1 + 1);
    int approx = 0;
    apply_state(st, depth != NULL, &approx);
    if (approx) n_approx++;
    apply_tev(st, tex != NULL);
    if (tex) {
        C3D_TexSetFilter(tex, st->tex_linear ? GPU_LINEAR : GPU_NEAREST, st->tex_linear ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(tex, st->tex_clamp_u ? GPU_CLAMP_TO_EDGE : GPU_REPEAT, st->tex_clamp_v ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
        cs_tex(tex);
    }
    cs_vp(2.0f / (float)t->w, 2.0f / (float)t->h, uvs[0], uvs[1]);
    DPART(D_STATE);
    DIAG("c3d: draw %d verts to %06X %ux%u fmt %d tex %s depth %s; colour bank %c, depth bank %c", nverts,
         (unsigned)t->off, (unsigned)t->w, (unsigned)t->h, t->fmt, tex ? "yes" : "no", depth ? "yes" : "no",
         vram_bank(t->tex.data), depth ? vram_bank(depth) : '-');
    if (n_draws == 0) g_diag_present = 0;        /* the presents after the first draw traced too */
    C3D_DrawArrays(GPU_TRIANGLES, first, nverts);
    DPART(D_SUBMIT);

    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) rows_mark(t, (uint32_t)y0, (uint32_t)y1);
    t->n_drawn++;
    t->seq = ++g_draw_seq;
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    DPART(D_ROWS);
#undef DPART
    return 0;
}

/* The dirty targets over VRAM [a0, a1) back to VRAM, the one drawn last
 * last, so it wins where they overlap (as the PSP's memory would hold). */
static void readback_range(uint32_t a0, uint32_t a1, int why, int recheck) {
    rt_entry *list[MAX_RT];
    int n = 0;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i]))) list[n++] = &g_rt[i];
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && list[k]->seq < list[k - 1]->seq; k--) { rt_entry *x = list[k]; list[k] = list[k - 1]; list[k - 1] = x; }
    for (int i = 0; i < n; i++) {
        rt_readback(list[i], why);
        if (recheck) list[i]->check_frame = (uint64_t)-1;
    }
}

static void c3d_sync_vram(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    readback_range(a0, a1, RB_SYNC, 0);
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
        readback_range(a0, a1, RB_CPU, 1);
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

static int c3d_ge_present_on(uint32_t addr, uint32_t stride, int fmt);
int c3d_ge_present(uint32_t addr, uint32_t stride, int fmt) {
    g_diag_present++;
    g_in_present = 1;
    const int r = c3d_ge_present_on(addr, stride, fmt);
    g_in_present = 0;
    return r;
}

static int c3d_ge_present_on(uint32_t addr, uint32_t stride, int fmt) {
    if (!g_ok) return -1;
    const int pb = prof(P_PRESENT);
    frame_open();
    rt_entry *t = NULL;
    if ((addr & 0x0F000000u) == 0x04000000u && stride && stride <= RT_W) {
        t = rt_get(vram_off(addr), stride, fmt, SCREEN_H);
        if (t) rt_check(t);
    }
    C3D_RenderTargetClear(g_top, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(g_top);
    state_forget();                              /* the scissor went off; the quad sets its own state */
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
            state_forget();                      /* sets its own state below */
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
    prof(P_WAIT);
    DIAG("c3d: present FrameEnd");
    C3D_FrameEnd(0);                             /* submits; the top screen shows it once drawn */
    DIAG("c3d: present FrameEnd done");
    prof(pb);
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

/* ---- GPU self-test (diag only) ----------------------------------------------------------------- */

/* The game's first draw froze the whole console, Rosalina included, with
 * depth in either bank; Azahar draws it fine. With sdmc:/psp2i/diag, the
 * features that draw uses run one at a time at start-up, safest first, each
 * waited for on the GPU and traced: the last "ok" line in diag.txt names
 * the step after it as the one that hangs (3DS_PROGRESS.md, T4). */
static float g_st_z;                             /* the quads' depth (0..65535) */
static void st_quad(float x0, float y0, float x1, float y1, float u1, float v1) {
    psp_gpu_vertex q[6];
    memset(q, 0, sizeof q);
    const float xs[6] = { x0, x1, x0, x0, x1, x1 }, ys[6] = { y0, y0, y1, y1, y0, y1 };
    for (int i = 0; i < 6; i++) {
        q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = g_st_z; q[i].rhw = 1.0f;
        q[i].u = xs[i] == x0 ? 0 : u1; q[i].v = ys[i] == y0 ? 0 : v1;
        q[i].r = 255.0f; q[i].g = 128.0f; q[i].b = 0; q[i].a = 255.0f;
    }
    const int first = ring_put(q, 6);
    C3D_DrawArrays(GPU_TRIANGLES, first, 6);
}

static void st_untextured(void) {
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

/* the GPU commands queued since the last submit, into the trace (8 words a line) */
static void st_dump(const char *name) {
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    psp2i_diag("selftest: commands for %s: %lu words", name, (unsigned long)off);
    for (u32 i = 0; i < off && i < 1024; i += 8) {
        char line[128];
        int n = 0;
        for (u32 j = i; j < off && j < i + 8; j++) n += snprintf(line + n, sizeof line - n, " %08lX", (unsigned long)buf[j]);
        psp2i_diag("cmd %04lX:%s", (unsigned long)i, line);
    }
}

#define ST_STEP(name) do { st_dump(name); gpu_finish(); psp2i_diag("selftest: ok after %s", name); } while (0)

static void gpu_selftest(void) {
    psp2i_diag("selftest: start (VRAM free %lu KB)", (unsigned long)(vramSpaceFree() >> 10));
    frame_open();
    ST_STEP("frame");
    {
        u32 *cb, cbsize, cboff;
        GPUCMD_GetBuffer(&cb, &cbsize, &cboff);
        psp2i_diag("selftest: command buffer at physical 0x%08lX, %lu words, %lu used",
                   (unsigned long)osConvertVirtToPhys(cb), (unsigned long)cbsize, (unsigned long)cboff);
    }

    /* 1a: the reference -- the simplest shader, its own vertex buffer */
    DVLB_s *tdvlb = DVLB_ParseFile((u32 *)c3d_test_shbin, c3d_test_shbin_size);
    static shaderProgram_s tprog;
    psp2i_diag("selftest: %d test shaders", tdvlb ? (int)tdvlb->numDVLE : 0);
    {
        float *vbo = (float *)linearAlloc(3 * 8 * sizeof(float));
        if (tdvlb && vbo) {
            shaderProgramInit(&tprog);
            shaderProgramSetVsh(&tprog, &tdvlb->DVLE[0]);
            const float tri[3][8] = { { -0.5f, -0.5f, -0.5f, 1, 1, 0, 0, 1 },
                                      {  0.5f, -0.5f, -0.5f, 1, 0, 1, 0, 1 },
                                      {  0.0f,  0.5f, -0.5f, 1, 0, 0, 1, 1 } };
            memcpy(vbo, tri, sizeof tri);
            GSPGPU_FlushDataCache(vbo, sizeof tri);
            const C3D_AttrInfo save_a = *C3D_GetAttrInfo();
            const C3D_BufInfo save_b = *C3D_GetBufInfo();
            C3D_AttrInfo ai;
            AttrInfo_Init(&ai);
            AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 4);
            AttrInfo_AddLoader(&ai, 1, GPU_FLOAT, 4);
            C3D_BufInfo bi;
            BufInfo_Init(&bi);
            const int bid = BufInfo_Add(&bi, vbo, 8 * sizeof(float), 2, 0x10);
            psp2i_diag("selftest: reference buffer at physical 0x%08lX (buffer %d)",
                       (unsigned long)osConvertVirtToPhys(vbo), bid);
            C3D_BindProgram(&tprog);
            C3D_SetAttrInfo(&ai);
            C3D_SetBufInfo(&bi);
            C3D_RenderTargetClear(g_top, C3D_CLEAR_ALL, 0x000000FF, 0);
            C3D_FrameDrawOn(g_top);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            st_untextured();
            C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
            ST_STEP("1a reference shader and buffer");
            C3D_BindProgram(&g_prog);
            C3D_SetAttrInfo((C3D_AttrInfo *)&save_a);
            C3D_SetBufInfo((C3D_BufInfo *)&save_b);
        } else psp2i_diag("selftest: no reference shader");
    }

    /* 2a-2d: c3d_ge.v.pica built up a feature at a time (c3d_test_t1-t4),
     * fed from the vertex ring as the game's draws are; 2e is c3d_ge.v.pica */
    /* by DVLE: 1-7 c3d_test_t1-t7, 8 and 9 (t8, t9) split 2b's two new
     * things -- arithmetic, and reading a uniform -- and run before it */
    static const char *const VAR[11] = { "", "2a pass-through, ring", "2b uniform", "2c constant", "2d rcp/max/min/neg",
                                         "2f + pixel to NDC mul", "2g + add of a negated constant",
                                         "2h c3d_ge.v.pica in the test shbin", "2a2 a multiply, no uniform",
                                         "2a3 a uniform moved, no arithmetic", "2a4 2a2 writing the whole register" };
    /* 2a2 (t8), 2a3 (t9) and 2b (t2) hung: each writes only part of an
     * output register; 2a4 (t10) is 2a2 writing it whole. The others write
     * part of one too, so they're left out; c3d_ge.v.pica no longer does. */
    static const int ORDER[2] = { 1, 10 };
    for (int oi = 0; tdvlb && oi < 2; oi++) {
        const int k = ORDER[oi] - 1;
        if (k + 1 >= tdvlb->numDVLE) continue;
        /* a program object each: C3D_BindProgram ignores the one already
         * bound, so a single reused one ran 2a's shader for all of these */
        static shaderProgram_s vprog[10];
        shaderProgramInit(&vprog[k]);
        shaderProgramSetVsh(&vprog[k], &tdvlb->DVLE[k + 1]);
        C3D_BindProgram(&vprog[k]);
        s8 loc = shaderInstanceGetUniformLocation(vprog[k].vertexShader, "vp");
        if (loc < 0) loc = shaderInstanceGetUniformLocation(vprog[k].vertexShader, "vq");
        psp2i_diag("selftest: %s: entry %u, uniform at %d", VAR[k + 1], (unsigned)tdvlb->DVLE[k + 1].mainOffset, loc);
        const int pix = k >= 4 && k <= 6;       /* 2f-2h take pixels, as c3d_ge.v.pica */
        if (loc >= 0) C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, pix ? 2.0f / (float)TOP_H : 1, pix ? 2.0f / (float)TOP_W : 1,
                                    1.0f / 64, 1.0f / 64);
        C3D_FrameDrawOn(g_top);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        st_untextured();
        psp_gpu_vertex q[3];
        memset(q, 0, sizeof q);
        const float xy[3][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } };
        for (int i = 0; i < 3; i++) {
            q[i].x = xy[i][0]; q[i].y = xy[i][1]; q[i].z = 0.5f; q[i].rhw = 1.0f;
            if (pix) { q[i].x = 32 + 32 * xy[i][0]; q[i].y = 32 + 32 * xy[i][1]; q[i].z = 32768; }
            q[i].r = q[i].a = 1.0f;
        }
        const int first = ring_put(q, 3);
        C3D_DrawArrays(GPU_TRIANGLES, first, 3);
        ST_STEP(VAR[k + 1]);
    }
    C3D_BindProgram(&g_prog);

    /* 1: a ring draw to the top screen, untextured, no depth */
    C3D_RenderTargetClear(g_top, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(g_top);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    st_untextured();
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / (float)TOP_H, 2.0f / (float)TOP_W, 1, 1);
    /* 3a-3d: c3d_ge.v.pica with vertices inside the clip volume, then on
     * its z = 0 plane (where z 0 comes out as -0.0), its far plane, its x edge */
    static const struct { const char *name; float x0, x1, z; } VD[4] = {
        { "3a inside, mid depth", 16, 48, 32768 }, { "3b inside, z 0", 16, 48, 0 },
        { "3c inside, z 65535", 16, 48, 65535 },  { "3d on the x edge, mid depth", 0, 48, 32768 } };
    for (int k = 0; k < 4; k++) {
        g_st_z = VD[k].z;
        st_quad(VD[k].x0, 16, VD[k].x1, 48, 1, 1);
        ST_STEP(VD[k].name);
    }
    g_st_z = 0;
    st_quad(0, 0, 64, 64, 1, 1);
    ST_STEP("2e c3d_ge.v.pica, ring (step 1)");

    /* 2: to a texture target (as the game's), no depth */
    C3D_Tex tex;
    if (!C3D_TexInitVRAM(&tex, 512, 512, GPU_RGBA8)) { psp2i_diag("selftest: no VRAM for a target"); return; }
    C3D_RenderTarget *rt = C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, -1);
    if (!rt) { psp2i_diag("selftest: no render target"); return; }
    psp2i_diag("selftest: target in bank %c", vram_bank(tex.data));
    C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(rt);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    st_untextured();
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / 512.0f, 2.0f / 512.0f, 1, 1);
    st_quad(0, 0, 64, 64, 1, 1);
    ST_STEP("2 ring draw, texture target");

    /* 3: the target sampled on the top screen (the present) */
    C3D_FrameDrawOn(g_top);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexBind(0, &tex);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / (float)TOP_H, 2.0f / (float)TOP_W, 1.0f / 512.0f, 1.0f / 512.0f);
    st_quad(0, 0, 128, 128, 512, 512);
    C3D_TexBind(0, NULL);
    ST_STEP("3 textured draw from the target");

    /* 4, 5: depth (16-bit, as the game's) in the opposite bank, then the same one */
    for (int same = 0; same < 2; same++) {
        const int b = vram_bank(tex.data) == 'B';
        void *depth = vramAllocAt(512 * 512 * 2, (same ? b : !b) ? VRAM_ALLOC_B : VRAM_ALLOC_A);
        if (!depth) { psp2i_diag("selftest: no VRAM for depth"); break; }
        psp2i_diag("selftest: depth in bank %c", vram_bank(depth));
        C3D_SyncMemoryFill((u32 *)depth, 0, (u32 *)((uint8_t *)depth + 512 * 512 * 2),
                           GX_FILL_TRIGGER | GX_FILL_16BIT_DEPTH, NULL, 0, NULL, 0);
        ST_STEP(same ? "5a depth fill" : "4a depth fill");
        C3D_FrameBufDepth(&rt->frameBuf, depth, GPU_RB_DEPTH16);
        C3D_FrameDrawOn(rt);
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 480, 272);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
        st_untextured();
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_vp, 2.0f / 512.0f, 2.0f / 512.0f, 1, 1);
        st_quad(0, 0, 480, 272, 1, 1);
        ST_STEP(same ? "5 depth in the same bank" : "4 depth in the opposite bank");
        C3D_FrameBufDepth(&rt->frameBuf, NULL, GPU_RB_DEPTH16);
        C3D_FrameDrawOn(g_top);
        ST_STEP("depth off");
        vramFree(depth);
    }
    if (g_in_frame) {                            /* citro3d panics on a target deleted inside a frame */
        ring_flush();
        C3D_FrameEnd(0);
        g_in_frame = 0;
        g_seg++;
    }
    C3D_RenderTargetDelete(rt);
    C3D_TexDelete(&tex);
    state_forget();
    g_cur = NULL; g_cur_depth = NULL;
    gpu_finish();
    psp2i_diag("selftest: done");
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
    FILE *xv = fopen("sdmc:/psp2i/xfer_vram", "r");
    if (xv) {
        fclose(xv);
        g_xfer_vram = 1;
        fprintf(stderr, "c3d: xfer_vram: overlapping targets go through VRAM\n");
    }
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    /* citro3d gives each buffer as an offset from 0x18000000 in a 28-bit
     * field, so only up to 0x27FFFFFF. In the .cia's 124 MB mode on a New 3DS
     * the linear heap is in the extended FCRAM above it: the offset wrapped
     * and the console's first draw locked up the GPU, Rosalina included
     * (3DS_PROGRESS.md, T4). The ring is the only buffer: the base is it. */
    const u32 ring_pa = osConvertVirtToPhys(g_ring);
    bi->base_paddr = ring_pa;
    const int bid = BufInfo_Add(bi, g_ring, sizeof(psp_gpu_vertex), 3, 0x210);
    fprintf(stderr, "c3d: vertex ring at physical 0x%08lX (buffer %d), vp uniform %d\n",
            (unsigned long)ring_pa, bid, u_vp);
    if (psp2i_diag_on)
        psp2i_diag("c3d: vertex ring at physical 0x%08lX (buffer %d), vp uniform %d", (unsigned long)ring_pa, bid, u_vp);

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
    if (psp2i_diag_on) gpu_selftest();
    fprintf(stderr, "c3d: rendering on the PICA200\n");
    return 0;
}

void c3d_ge_stats_get(c3d_ge_stats *s) {
    prof(g_pb);                                  /* charge the running bucket up to now */
    s->draw = g_pt[P_DRAW]; s->wait = g_pt[P_WAIT]; s->readback = g_pt[P_RB];
    s->upload = g_pt[P_UP]; s->texture = g_pt[P_TEX]; s->present = g_pt[P_PRESENT];
    s->draws = n_draws; s->finishes = n_finish; s->readbacks = n_readbacks;
    s->uploads = n_uploads; s->evictions = n_evict;
}

void c3d_ge_report(FILE *out) {
    int rts = 0, depths = 0, texs = 0;
    for (int i = 0; i < MAX_RT; i++) rts += g_rt[i].used;
    for (int i = 0; i < MAX_DEPTH; i++) depths += g_depth[i].used;
    for (int i = 0; i < MAX_TEX; i++) texs += g_tex[i].used;
    fprintf(out, "c3d: %llu draws, %llu tris, %llu presents, %llu finishes (%llu for command room); uploads %llu, readbacks %llu, "
                 "texture decodes %llu (%d cached, %lu KB), target textures %llu, approximated %llu, "
                 "to software %llu, evictions %llu, GPU transfers %llu, overlaps dropped %llu; %d targets, %d depth buffers; linear free %lu KB, VRAM free %lu KB\n",
            (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_present,
            (unsigned long long)n_finish, (unsigned long long)n_cmdfull, (unsigned long long)n_uploads,
            (unsigned long long)n_readbacks, (unsigned long long)n_tex_decodes, texs, (unsigned long)(g_tex_bytes >> 10),
            (unsigned long long)n_rt_tex, (unsigned long long)n_approx, (unsigned long long)n_fail,
            (unsigned long long)n_evict, (unsigned long long)n_xfer, (unsigned long long)n_skip, rts, depths, (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(vramSpaceFree() >> 10));
    fprintf(out, "c3d: depth copies between VRAM banks %llu\n", (unsigned long long)n_depth_copies);
    {   /* where a draw's time goes, since the last report (nested waits and decodes included) */
        static uint64_t was[D_N], draws_was;
        const double nd = n_draws > draws_was ? (double)(n_draws - draws_was) : 1.0, us = 1e6 / SYSCLOCK_ARM11 / nd;
        fprintf(out, "c3d: us per draw: target %.1f, depth+texture %.1f, ring %.1f, state %.1f, submit %.1f, rows %.1f\n",
                (double)(g_dt[D_TARGET] - was[D_TARGET]) * us, (double)(g_dt[D_TEXTURE] - was[D_TEXTURE]) * us,
                (double)(g_dt[D_RING] - was[D_RING]) * us, (double)(g_dt[D_STATE] - was[D_STATE]) * us,
                (double)(g_dt[D_SUBMIT] - was[D_SUBMIT]) * us, (double)(g_dt[D_ROWS] - was[D_ROWS]) * us);
        memcpy(was, g_dt, sizeof was);
        draws_was = n_draws;
        static uint64_t hn_was, ht_was, hb_was, ft_was, fr_was;
        const uint64_t fr = frame_no();
        const double nf = fr > fr_was ? (double)(fr - fr_was) : 1.0;
        static uint64_t sk_was;
        fprintf(out, "c3d: texture checks per frame: %.0f hashes (%.0f skipped), %.0f K texels, %.1f ms hashing, %.1f ms decoding\n",
                (double)(g_hash_n - hn_was) / nf, (double)(g_hash_skip - sk_was) / nf, (double)(g_hash_bytes - hb_was) / nf / 1024.0,
                (double)(g_hash_ticks - ht_was) * 1000.0 / SYSCLOCK_ARM11 / nf,
                (double)(g_fill_ticks - ft_was) * 1000.0 / SYSCLOCK_ARM11 / nf);
        sk_was = g_hash_skip; hn_was = g_hash_n; ht_was = g_hash_ticks; hb_was = g_hash_bytes; ft_was = g_fill_ticks; fr_was = fr;
    }
    for (int i = 0; i < MAX_RT; i++) {
        const rt_entry *t = &g_rt[i];
        if (!t->used) continue;
        fprintf(out, "c3d:   target VRAM+0x%06lX/%lu fmt %d %lux%lu: %llu draws, %llu uploads, %llu readbacks, %s\n",
                (unsigned long)t->off, (unsigned long)t->stride, t->fmt, (unsigned long)t->w, (unsigned long)t->h,
                (unsigned long long)t->n_drawn,
                (unsigned long long)t->n_up, (unsigned long long)t->n_back, t->dirty ? "GPU ahead" : "in step with VRAM");
        if (!t->n_up && !t->n_back) continue;
        fprintf(out, "c3d:     readbacks by cause:");
        for (int k = 0; k < 6; k++) if (t->back_why[k]) fprintf(out, " %s %llu", RB_NAMES[k], (unsigned long long)t->back_why[k]);
        fprintf(out, "; uploads:");
        for (int k = 0; k < 3; k++) if (t->up_why[k]) fprintf(out, " %s %llu", UP_NAMES[k], (unsigned long long)t->up_why[k]);
        fprintf(out, "\n");
    }
}
