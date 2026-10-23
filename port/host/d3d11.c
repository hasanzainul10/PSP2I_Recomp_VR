/* Direct3D 11 backend for the GE.
 *
 * The runtime (psprecomp/src/hle/gpu.c) does all vertex work and hands each
 * draw over as a screen-space triangle list plus decoded state; this file
 * rasterizes it on the GPU. The software rasterizer remains the reference
 * (`--renderer software`).
 *
 * ## Keeping VRAM coherent
 *
 * PSP games render into VRAM and then *read it back*: sampling an offscreen
 * buffer as a texture (glow and blur passes), displaying it, copying it with
 * block transfers, occasionally reading it from the CPU. So each colour buffer
 * the game draws to becomes a GPU render target keyed by (VRAM offset, stride,
 * format), seeded from emulated VRAM, and the two copies are reconciled:
 *
 *   - GPU -> VRAM ("readback") only for the rows drawn since the last sync, and
 *     only when something is about to read VRAM: the display (each flip), a
 *     block transfer, a texture that cannot be sampled from the GPU copy, or a
 *     software fallback draw. Reading back only drawn rows matters: the two
 *     framebuffers of a double-buffered game sit within 512 rows of each other.
 *   - VRAM -> GPU ("upload") when emulated VRAM changed behind the GPU's back:
 *     checked by hash on the first draw into a target each frame, and forced
 *     when the runtime reports a write (vram_written).
 *
 * ## Approximations (counted, see d3d11_report)
 *
 * Colour math runs in float rather than the GE's 8-bit integer math, so pixels
 * can differ by about one step. Blend factors with doubled alpha, an "absolute
 * difference" equation, two unrelated fixed colours, and partial per-bit write
 * masks have no D3D11 equivalent and are approximated. Depth buffers are not
 * read back to VRAM.
 *
 * ## Internal resolution (d3d11_set_scale)
 *
 * With a scale S > 1 every render target and depth buffer is S times the
 * PSP's size in each direction, so the game draws at S x 480x272 (texture
 * packs then show their detail). Everything keyed to VRAM stays in PSP
 * pixels (t->w, t->h, dirty rows, scissors): only the GPU side is larger.
 * Uploads from VRAM repeat each pixel S x S times; readbacks to VRAM take
 * the sample nearest each PSP pixel's centre; GPU transfers between targets
 * keep the sub-pixel position. d3d11_present also hands out the S-times
 * picture (d3d11_present_hires) for the window and the headset.
 * The scale down (g_scale_y, d3d11_set_scale_y) may be larger than across,
 * for VR eye pictures with square pixels; the S-times picture is then
 * resampled to S x S, and only the eye pictures keep the extra rows.
 */
#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>

#ifdef PSP2I_TEXREPLACE
#include "texreplace.h"
#endif

#define VRAM_SIZE 0x200000u
#define MAX_RT    32
#define MAX_DEPTH 8
#define MAX_TEX   512
#define VB_BYTES  (16u << 20)

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader  *g_vs;
static ID3D11PixelShader   *g_ps;
static ID3D11InputLayout   *g_layout;
static ID3D11Buffer        *g_vb, *g_vcb, *g_pcb;
static ID3D11VertexShader  *g_xvs;          /* GPU-side VRAM transfer between targets (xfer) */
static ID3D11PixelShader   *g_xps;
static ID3D11Buffer        *g_xcb;
static ID3D11RasterizerState *g_rs;
static uint32_t g_vb_pos;

typedef struct {
    int used;
    uint32_t off, stride, w, h;          /* VRAM offset, pixels per row, texture size */
    int fmt;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *staging;
    int dirty;                           /* GPU has rows newer than VRAM */
    uint32_t dy0, dy1;                   /* bounds of the dirty rows */
    uint64_t drow[8];                    /* the dirty rows themselves (h <= 512) */
    uint32_t rows;                       /* rows in use (seeded/compared) */
    uint64_t synced_hash;
    uint64_t check_frame;
    uint64_t last_use;
    /* Stereo (d3d_draw_eye): the same target as each eye sees it, made on
     * the first eye draw; eye_frame is the flip of the last one. */
    ID3D11Texture2D *eye_tex[2];
    ID3D11RenderTargetView *eye_rtv[2];
    ID3D11ShaderResourceView *eye_srv[2];
    uint64_t eye_frame;
    uint32_t eye_tag;                    /* head tracking: the pose its eye pictures were drawn with (psp_gpu_stereo_tag) */
    uint64_t gen;                        /* bumped whenever its GPU pixels change (d3d11_present) */
} rt_entry;

typedef struct {
    int used;
    uint32_t off, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t last_use;
    ID3D11Texture2D *eye_tex[2];             /* stereo: each eye's depth */
    ID3D11DepthStencilView *eye_dsv[2];
} depth_entry;

typedef struct {
    int used;
    uint64_t key, hash, check_frame, last_use;
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t rep_id;                 /* texture pack PNG drawn instead (texreplace.h), 0 = none */
    int rep_slot;                    /* where rep_id was last found in g_rep */
    uint32_t rep_wait;               /* a replacement still decoding (texreplace_pending): the original until then */
} tex_entry;

static rt_entry    g_rt[MAX_RT];
static depth_entry g_depth[MAX_DEPTH];
static tex_entry   g_tex[MAX_TEX];
static ID3D11Texture2D *g_scratch;       /* copy of a target sampled while drawn to */
static ID3D11ShaderResourceView *g_scratch_srv;
static uint32_t g_scratch_w, g_scratch_h;
static uint64_t g_use;
static int g_scale = 1;                  /* internal resolution: GPU pixels per PSP pixel (1..6; above 4 for VR) */
static int g_scale_y = 1;                /* ... down (g_scale..12): taller for the VR eyes */

/* Before d3d11_init: the internal resolution scale. */
void d3d11_set_scale(int s) { g_scale = g_scale_y = s < 1 ? 1 : s > 6 ? 6 : s; }
/* After d3d11_set_scale, before d3d11_init: GPU pixels per PSP pixel down,
 * when it isn't the same as across. VR head tracking widens the eyes' view
 * up and down more than across (vr_fov_scale_y), so the eye pictures need
 * more rows for square pixels in the headset. */
void d3d11_set_scale_y(int s) { g_scale_y = s < g_scale ? g_scale : s > 12 ? 12 : s; }

/* Image quality for VR (before d3d11_init; the window's game is drawn as
 * before when these are 0): `aniso` 2..16 = the game's linearly filtered
 * textures get mipmaps and anisotropic filtering (floors and walls at a
 * shallow angle stop shimmering and blurring); `sharpen` 0..1 = the eye
 * pictures go to the headset through a contrast-adaptive sharpening pass
 * (CAS-style) instead of a plain copy, against the softening of the
 * headset's resampling. */
static int   g_aniso;
static float g_sharpen;
static int   g_quality_on;           /* set before d3d11_init: VR is on (the sharpening shader is made) */
static int   g_tex_mips;             /* the game's textures have mipmaps: decided at d3d11_init (aniso then) */
static void  sampler_flush(void);
/* Again at run time (the engine menu): sharpening applies at once, the
 * filtering to the samplers at once; mipmaps only exist if filtering was
 * on at the start (d3d11_tex_mips). */
void d3d11_set_quality(int aniso, float sharpen) {
    g_aniso = aniso < 2 ? 0 : aniso > 16 ? 16 : aniso;
    g_sharpen = sharpen < 0 ? 0 : sharpen > 1 ? 1 : sharpen;
    if (g_dev) sampler_flush(); else g_quality_on = 1;
}
int d3d11_tex_mips(void) { return g_tex_mips; }
static ID3D11PixelShader *g_sps;
int d3d11_sharpen_ready(void) { return g_sps != NULL; }

static uint64_t n_draws, n_tris, n_uploads, n_readbacks, n_tex_decodes, n_rt_tex, n_approx;

/* ---- helpers ------------------------------------------------------------------- */

/* The backend's own view of VRAM: raw, past psp_mem_ptr and its access hook
 * (which is this backend, below). */
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

/* ---- coherence on CPU access ----------------------------------------------------------
 *
 * Every CPU access to VRAM reaches d3d_cpu_access (psp_mem_set_vram_hook)
 * with the bytes it touches. Two page maps (4 KB pages) keep that cheap:
 *
 *   g_pg_dirty  a target may hold rows newer than VRAM in this page: read
 *               back the targets overlapping the access, then rebuild.
 *   g_pg_armed  a target was checked against VRAM (rt_check) since the CPU
 *               last touched this page: the CPU may now write it, so make
 *               the overlapping targets re-check (hash) before their next
 *               draw, and disarm.
 *
 * This replaces flushing all of VRAM and re-checking every target at each
 * GE sync (psp_gpu_cpu_sync), which stalled the pipeline at every
 * sceGeDrawSync -- once or more per frame -- whether or not the CPU then
 * looked at VRAM. Coherence is stricter than before (any access, not only
 * after a sync), and the cost is paid only by the accesses that need it. */
#define PG_SHIFT 12
#define NPG (VRAM_SIZE >> PG_SHIFT)
static uint8_t g_pg_dirty[NPG], g_pg_armed[NPG];

static void pages_set(uint8_t *map, uint32_t a0, uint32_t a1) {
    if (a1 > VRAM_SIZE) a1 = VRAM_SIZE;
    if (a0 >= a1) return;
    for (uint32_t p = a0 >> PG_SHIFT; p <= (a1 - 1) >> PG_SHIFT; p++) map[p] = 1;
}

static uint32_t row_addr(const rt_entry *t, uint32_t y) { return t->off + y * t->stride * (uint32_t)bpp_of(t->fmt); }

/* Dirty rows: a bitmap, so that ownership of rows can move to another target
 * (rt_xfer) from anywhere in the range, plus its bounds for quick tests. */
static int row_dirty(const rt_entry *t, uint32_t y) { return y < 512 && ((t->drow[y >> 6] >> (y & 63)) & 1); }

static void rows_bounds(rt_entry *t) {
    uint32_t lo = 512, hi = 0;
    for (uint32_t y = 0; y < 512; y++) if (row_dirty(t, y)) { if (y < lo) lo = y; hi = y + 1; }
    if (lo >= hi) { t->dirty = 0; t->dy0 = t->dy1 = 0; memset(t->drow, 0, sizeof t->drow); }
    else { t->dirty = 1; t->dy0 = lo; t->dy1 = hi; }
}

static void rows_mark(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    if (y0 >= y1) return;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] |= 1ull << (y & 63);
    if (!t->dirty) { t->dy0 = y0; t->dy1 = y1; }
    else { if (y0 < t->dy0) t->dy0 = y0; if (y1 > t->dy1) t->dy1 = y1; }
    t->dirty = 1;
    pages_set(g_pg_dirty, row_addr(t, y0), row_addr(t, y1));
}

static void rows_clear(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] &= ~(1ull << (y & 63));
    rows_bounds(t);
}

/* ---- shaders ------------------------------------------------------------------- */

static const char *SHADER_SRC =
"cbuffer V : register(b0) { float4 vp; };\n"
"struct VI { float4 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
"struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
"VO vsmain(VI i) {\n"
"  VO o; float w = 1.0 / i.p.w;\n"
"  float nx = i.p.x / vp.x * 2.0 - 1.0;\n"
"  float ny = 1.0 - i.p.y / vp.y * 2.0;\n"
"  float nz = saturate(i.p.z / 65535.0);\n"
"  o.p = float4(nx * w, ny * w, nz * w, w);\n"
"  o.uv = i.uv * vp.zw; o.c = i.c / 255.0; return o;\n"
"}\n"
"cbuffer P : register(b1) { float4 env; uint4 tf; uint4 at; uint4 misc; };\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"float4 psmain(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (misc.x != 0) return c;\n"                       /* clear mode */
"  if (tf.x != 0) {\n"
"    float4 t = T.Sample(S, i.uv);\n"
"    float3 rgb; float a = c.a;\n"
"    uint f = tf.y;\n"
"    if (f == 0) { rgb = c.rgb * t.rgb; if (tf.z) a = c.a * t.a; }\n"
"    else if (f == 1) { rgb = tf.z ? lerp(c.rgb, t.rgb, t.a) : t.rgb; }\n"
"    else if (f == 2) { rgb = lerp(c.rgb, env.rgb, t.rgb); if (tf.z) a = c.a * t.a; }\n"
"    else if (f == 3) { rgb = t.rgb; if (tf.z) a = t.a; }\n"
"    else { rgb = c.rgb + t.rgb; if (tf.z) a = c.a * t.a; }\n"
"    if (tf.w) rgb *= 2.0;\n"
"    c = float4(saturate(rgb), a);\n"
"  }\n"
"  if (at.x != 0) {\n"
"    uint ai = ((uint)round(c.a * 255.0)) & at.w; uint r = at.z & at.w; bool ok;\n"
"    uint fn = at.y;\n"
"    if (fn == 0) ok = false; else if (fn == 1) ok = true;\n"
"    else if (fn == 2) ok = ai == r; else if (fn == 3) ok = ai != r;\n"
"    else if (fn == 4) ok = ai < r; else if (fn == 5) ok = ai <= r;\n"
"    else if (fn == 6) ok = ai > r; else ok = ai >= r;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return c;\n"
"}\n";

/* The bytes of one render target (src) as another target's pixels (dst),
 * where the two cover the same VRAM in different layouts: exactly what
 * reading src back to VRAM and seeding dst from VRAM would produce
 * (pack16/expand16 below are the C ones), without the round trip. */
static const char *XFER_SRC =
"cbuffer X : register(b2) { uint4 dst; uint4 src; uint4 rng; };\n"   /* off, stride, fmt, bpp; ...; lo, hi, scale, scale down */
"Texture2D<float4> Y : register(t1);\n"
"float4 vsx(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"static uint2 sub;\n"                                    /* the GPU pixel within its PSP pixel */
"uint ld(uint i) {\n"
"  uint4 b = (uint4)round(saturate(Y.Load(int3((i % src.y) * rng.z + sub.x, (i / src.y) * rng.w + sub.y, 0))) * 255.0);\n"
"  return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);\n"
"}\n"
"uint pack16(uint f, uint c) {\n"
"  uint r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;\n"
"  if (f == 0) return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);\n"
"  if (f == 1) return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);\n"
"  return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);\n"
"}\n"
"uint expand16(uint f, uint p) {\n"
"  uint r, g, b, a;\n"
"  if (f == 0) { r = p & 31; g = (p >> 5) & 63; b = (p >> 11) & 31;\n"
"    return 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2)); }\n"
"  if (f == 1) { r = p & 31; g = (p >> 5) & 31; b = (p >> 10) & 31;\n"
"    return ((p & 0x8000) ? 0xFF000000 : 0) | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2)); }\n"
"  r = p & 15; g = (p >> 4) & 15; b = (p >> 8) & 15; a = (p >> 12) & 15;\n"
"  return ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);\n"
"}\n"
"float4 bytes4(uint w) { return float4(w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) / 255.0; }\n"
"float4 psx(float4 pos : SV_Position) : SV_Target {\n"
"  uint x = (uint)pos.x / rng.z, y = (uint)pos.y / rng.w;\n"
"  sub = uint2((uint)pos.x % rng.z, (uint)pos.y % rng.w);\n"
"  uint A = dst.x + (y * dst.y + x) * dst.w;\n"
"  if (A < rng.x || A + dst.w > rng.y) discard;\n"
"  uint rel = A - src.x;\n"
"  if (dst.w == 4) {\n"
"    uint w = src.w == 4 ? ld(rel >> 2) : (pack16(src.z, ld(rel >> 1)) | (pack16(src.z, ld((rel >> 1) + 1)) << 16));\n"
"    return bytes4(w);\n"
"  }\n"
"  uint p;\n"
"  if (src.w == 4) { uint w = ld(rel >> 2); p = ((rel >> 1) & 1) ? (w >> 16) : (w & 0xFFFF); }\n"
"  else p = pack16(src.z, ld(rel >> 1));\n"
"  return bytes4(expand16(dst.z, p));\n"
"}\n";

typedef struct { uint32_t dst[4], src[4], rng[4]; } xfer_consts;

typedef struct { float vp[4]; } vs_consts;
typedef struct { float env[4]; uint32_t tf[4], at[4], misc[4]; } ps_consts;

static ID3DBlob *compile_src(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "ge", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "d3d11: shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}
static ID3DBlob *compile(const char *entry, const char *target) { return compile_src(SHADER_SRC, entry, target); }

/* ---- render targets ---------------------------------------------------------------- */

static void rt_free(rt_entry *t) {
    for (int e = 0; e < 2; e++) {
        if (t->eye_srv[e]) ID3D11ShaderResourceView_Release(t->eye_srv[e]);
        if (t->eye_rtv[e]) ID3D11RenderTargetView_Release(t->eye_rtv[e]);
        if (t->eye_tex[e]) ID3D11Texture2D_Release(t->eye_tex[e]);
    }
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    if (t->staging) ID3D11Texture2D_Release(t->staging);
    memset(t, 0, sizeof *t);
}

/* Readbacks by what caused them (d3d11_report): 0 relayout, 1 evict,
 * 2 grow, 3 texture, 4 overlap, 5 sync (display, block transfer), 6 a CPU
 * access to the bytes (d3d_cpu_access). The caller sets g_rb_reason. */
static uint64_t n_rb_reason[7];
static int g_rb_reason;
static void rt_readback_inner(rt_entry *t);
static int trace_on(void);
static void rt_readback(rt_entry *t) {
    if (t->dirty) n_rb_reason[g_rb_reason]++;
    if (t->dirty && trace_on())
        fprintf(stderr, "readback: 0x%06X/%u fmt %d rows %u-%u reason %d\n", t->off, t->stride, t->fmt, t->dy0, t->dy1, g_rb_reason);
    rt_readback_inner(t);
}

/* Copy rows [y0, y1) of the target into emulated VRAM. */
static void rt_readback_inner(rt_entry *t) {
    if (!t->dirty) return;
    uint8_t *v = vram();
    uint32_t y0 = t->dy0, y1 = t->dy1 > t->h ? t->h : t->dy1;
    if (!v || y0 >= y1) { t->dirty = 0; memset(t->drow, 0, sizeof t->drow); return; }
    if (!t->staging) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(t->tex, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->staging))) return;
    }
    const uint32_t S = (uint32_t)g_scale, c = S / 2;     /* the GPU sample nearest a PSP pixel's centre */
    const uint32_t SY = (uint32_t)g_scale_y, cy = SY / 2;
    D3D11_BOX box = { 0, y0 * SY, 0, t->w * S, y1 * SY, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)t->staging, 0, 0, y0 * SY, 0,
                                              (ID3D11Resource *)t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const int bpp = bpp_of(t->fmt);
    for (uint32_t y = y0; y < y1; y++) {
        if (!row_dirty(t, y)) continue;                 /* not ours: another target or VRAM has it */
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        if (o + t->stride * (uint32_t)bpp > VRAM_SIZE) break;
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)(y * SY + cy) * m.RowPitch);
        if (S == 1 && bpp == 4) memcpy(v + o, src, t->stride * 4u);
        else if (bpp == 4) for (uint32_t x = 0; x < t->stride; x++) memcpy(v + o + x * 4, &src[x * S + c], 4);
        else for (uint32_t x = 0; x < t->stride; x++) {
            uint16_t p = pack16(t->fmt, src[x * S + c]);
            memcpy(v + o + x * 2, &p, 2);
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)t->staging, 0);
    t->dirty = 0;
    t->dy0 = t->dy1 = 0;
    memset(t->drow, 0, sizeof t->drow);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    /* Other targets over these bytes may hold an older copy: re-check. */
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && &g_rt[i] != t && overlaps(t->off, t->off + bytes, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;
    n_readbacks++;
}

/* Seed rows [0, rows) of the target from emulated VRAM. */
static void rt_upload(rt_entry *t) {
    uint8_t *v = vram();
    if (!v) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("PSP2I_RT_TRACE") != NULL;
    if (trace) fprintf(stderr, "d3d11: upload VRAM+0x%06X stride %u fmt %d rows %u (flip %llu, %s)\n",
                       t->off, t->stride, t->fmt, t->rows, (unsigned long long)frame_no(),
                       t->dirty ? "DIRTY" : "clean");
    const int bpp = bpp_of(t->fmt);
    uint32_t *px = (uint32_t *)malloc((size_t)t->stride * t->rows * 4);
    if (!px) return;
    for (uint32_t y = 0; y < t->rows; y++) {
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        for (uint32_t x = 0; x < t->stride; x++) {
            uint32_t c = 0;
            if (o + (x + 1) * (uint32_t)bpp <= VRAM_SIZE) {
                if (bpp == 4) memcpy(&c, v + o + x * 4, 4);
                else { uint16_t p; memcpy(&p, v + o + x * 2, 2); c = expand16(t->fmt, p); }
            }
            px[y * t->stride + x] = c;
        }
    }
    if (g_scale > 1 || g_scale_y > 1) {                /* each PSP pixel as S x SY GPU pixels */
        const uint32_t S = (uint32_t)g_scale, SY = (uint32_t)g_scale_y, W = t->stride * S;
        uint32_t *big = (uint32_t *)malloc((size_t)W * t->rows * SY * 4);
        if (!big) { free(px); return; }
        for (uint32_t y = 0; y < t->rows; y++) {
            uint32_t *row = big + (size_t)y * SY * W;
            for (uint32_t x = 0; x < t->stride; x++)
                for (uint32_t k = 0; k < S; k++) row[x * S + k] = px[y * t->stride + x];
            for (uint32_t k = 1; k < SY; k++) memcpy(row + (size_t)k * W, row, (size_t)W * 4);
        }
        free(px);
        px = big;
    }
    D3D11_BOX box = { 0, 0, 0, t->stride * (uint32_t)g_scale, t->rows * (uint32_t)g_scale_y, 1 };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)t->tex, 0, &box, px, t->stride * (uint32_t)g_scale * 4, 0);
    free(px);
    for (int e = 0; e < 2; e++)                         /* the eyes see the same new bytes */
        if (t->eye_tex[e]) ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)t->eye_tex[e], (ID3D11Resource *)t->tex);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    pages_set(g_pg_armed, t->off, t->off + bytes);
    t->gen++;
    n_uploads++;
}

/* A target is its VRAM offset AND layout: PSP2i uses its back buffer as a
 * 256-wide 32-bit bloom buffer and then as the 512-wide 16-bit frame, every
 * frame. Both stay resident; the bytes move between them on the GPU
 * (rt_xfer, from the overlap rule in d3d_draw). */
static rt_entry *rt_find(uint32_t off, uint32_t stride, int fmt) {
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].off == off && g_rt[i].stride == stride && g_rt[i].fmt == fmt) return &g_rt[i];
    return NULL;
}

static rt_entry *rt_get(uint32_t off, uint32_t stride, int fmt, uint32_t rows) {
    rt_entry *t = rt_find(off, stride, fmt);
    if (!t) {
        for (int i = 0; i < MAX_RT && !t; i++) if (!g_rt[i].used) t = &g_rt[i];
        if (!t) {                                       /* evict the least recently used */
            t = &g_rt[0];
            for (int i = 1; i < MAX_RT; i++) if (g_rt[i].last_use < t->last_use) t = &g_rt[i];
            g_rb_reason = 1; rt_readback(t);
            rt_free(t);
        }
        t->off = off; t->stride = stride; t->fmt = fmt;
        t->w = stride; t->h = 512;
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = t->w * (uint32_t)g_scale; d.Height = t->h * (uint32_t)g_scale_y; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->tex))) { memset(t, 0, sizeof *t); return NULL; }
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
        t->used = 1;
        t->rows = rows;
        if (getenv("PSP2I_RT_TRACE"))
            fprintf(stderr, "d3d11: render target VRAM+0x%06X stride %u format %d rows %u (flip %llu)\n",
                    off, stride, fmt, rows, (unsigned long long)frame_no());
        rt_upload(t);
        t->check_frame = frame_no();
    }
    if (rows > t->rows) {                               /* drawing further down than seeded */
        if (t->dirty) { g_rb_reason = 2; rt_readback(t); }
        t->rows = rows;
        rt_upload(t);
    }
    t->last_use = ++g_use;
    return t;
}

/* On the first draw of a frame, pick up CPU writes to VRAM. */
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



/* ---- depth buffers ------------------------------------------------------------------ */

static depth_entry *depth_get(uint32_t off, uint32_t w, uint32_t h) {
    depth_entry *d = NULL;
    for (int i = 0; i < MAX_DEPTH; i++)
        if (g_depth[i].used && g_depth[i].off == off && g_depth[i].w == w && g_depth[i].h == h) d = &g_depth[i];
    if (!d) {
        for (int i = 0; i < MAX_DEPTH && !d; i++) if (!g_depth[i].used) d = &g_depth[i];
        if (!d) {
            d = &g_depth[0];
            for (int i = 1; i < MAX_DEPTH; i++) if (g_depth[i].last_use < d->last_use) d = &g_depth[i];
            if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
            if (d->tex) ID3D11Texture2D_Release(d->tex);
            for (int e = 0; e < 2; e++) {
                if (d->eye_dsv[e]) ID3D11DepthStencilView_Release(d->eye_dsv[e]);
                if (d->eye_tex[e]) ID3D11Texture2D_Release(d->eye_tex[e]);
            }
            memset(d, 0, sizeof *d);
        }
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w * (uint32_t)g_scale; td.Height = h * (uint32_t)g_scale_y; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D16_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &d->tex))) return NULL;
        ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, d->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        d->off = off; d->w = w; d->h = h; d->used = 1;
    }
    d->last_use = ++g_use;
    return d;
}

/* ---- textures ------------------------------------------------------------------------ */

/* key -> slot index, so a lookup costs the same with 5 or 512 textures
 * cached (the linear scan grew with play time as the cache filled). Open
 * addressing; slots hold index + 1, 0 = empty; rebuilt when an entry is
 * replaced (a miss with the cache full). */
#define TEX_INDEX 2048
static uint16_t g_tex_index[TEX_INDEX];

static uint32_t tex_slot(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 53) & (TEX_INDEX - 1); }

static void tex_index_add(int i) {
    uint32_t s = tex_slot(g_tex[i].key);
    while (g_tex_index[s]) s = (s + 1) & (TEX_INDEX - 1);
    g_tex_index[s] = (uint16_t)(i + 1);
}

static void tex_index_rebuild(void) {
    memset(g_tex_index, 0, sizeof g_tex_index);
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].used) tex_index_add(i);
}

static tex_entry *tex_find(uint64_t key) {
    for (uint32_t s = tex_slot(key); g_tex_index[s]; s = (s + 1) & (TEX_INDEX - 1)) {
        tex_entry *t = &g_tex[g_tex_index[s] - 1];
        if (t->used && t->key == key) return t;
    }
    return NULL;
}

#ifdef PSP2I_TEXREPLACE
/* ---- texture packs (texreplace.c) ---------------------------------------------------------
 *
 * One GPU texture per pack PNG, shared by every game texture that maps to it,
 * evicted least recently used beyond a budget. Each gets a full mip chain:
 * pack PNGs are often 4 to 8 times the original's size, and minifying them
 * through the game's single-level sampling would shimmer. UVs need no change,
 * since the shader samples in normalized coordinates (texel / original size). */
#define MAX_REP   256
#define REP_BYTES (1024ull << 20)
typedef struct {
    uint32_t id;
    uint64_t bytes, last_use;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} rep_entry;
static rep_entry g_rep[MAX_REP];
static uint64_t g_rep_bytes, n_rep_uploads;

static void rep_free(rep_entry *r) {
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    g_rep_bytes -= r->bytes;
    memset(r, 0, sizeof *r);
}

/* The view of e's replacement, uploading the PNG if it isn't on the GPU;
 * NULL if it can't be (the caller draws the original). */
static ID3D11ShaderResourceView *rep_view(tex_entry *e) {
    rep_entry *r = &g_rep[e->rep_slot];
    if (r->id != e->rep_id) {
        r = NULL;
        for (int i = 0; i < MAX_REP && !r; i++) if (g_rep[i].id == e->rep_id) r = &g_rep[i];
    }
    if (!r) {
        int w, h;
        const uint32_t *px = texreplace_pixels(e->rep_id, &w, &h);
        if (!px) return NULL;
        const uint64_t bytes = (uint64_t)w * h * 4 * 4 / 3;
        for (;;) {
            rep_entry *slot = NULL, *old = NULL;
            for (int i = 0; i < MAX_REP; i++) {
                if (!g_rep[i].id) { if (!slot) slot = &g_rep[i]; }
                else if (!old || g_rep[i].last_use < old->last_use) old = &g_rep[i];
            }
            if (slot && g_rep_bytes + bytes <= REP_BYTES) { r = slot; break; }
            if (!old) { r = slot; break; }
            rep_free(old);
        }
        if (!r) return NULL;
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = 0; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &r->tex))) { r->tex = NULL; return NULL; }
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)r->tex, NULL, &r->srv))) {
            ID3D11Texture2D_Release(r->tex); r->tex = NULL; r->srv = NULL;
            return NULL;
        }
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)r->tex, 0, NULL, px, (UINT)w * 4, 0);
        ID3D11DeviceContext_GenerateMips(g_ctx, r->srv);
        r->id = e->rep_id; r->bytes = bytes;
        g_rep_bytes += bytes;
        n_rep_uploads++;
    }
    e->rep_slot = (int)(r - g_rep);
    r->last_use = ++g_use;
    return r->srv;
}
#endif

/* allow_rep 0: decode the original even if a pack replaces it. */
static tex_entry *tex_get(const psp_gpu_state *st, int allow_rep) {
    tex_entry *e = tex_find(st->tex_key);
    uint64_t f = frame_no();
#ifdef PSP2I_TEXREPLACE
    /* Its replacement has finished decoding: look the texture up again. */
    if (e && e->rep_wait && !texreplace_pending(e->rep_wait)) { e->rep_wait = 0; e->hash = 0; e->check_frame = 0; }
#endif
    if (e && e->check_frame == f) { e->last_use = ++g_use; return e; }
    uint64_t h = psp_gpu_texture_hash();
    if (e && e->hash == h) { e->check_frame = f; e->last_use = ++g_use; return e; }
    if (!e) {
        for (int i = 0; i < MAX_TEX && !e; i++) if (!g_tex[i].used) e = &g_tex[i];
        if (!e) {
            e = &g_tex[0];
            for (int i = 1; i < MAX_TEX; i++) if (g_tex[i].last_use < e->last_use) e = &g_tex[i];
        }
        const int evicted = e->used;
        if (e->used && (e->w != st->tex_w || e->h != st->tex_h)) {
            ID3D11ShaderResourceView_Release(e->srv); ID3D11Texture2D_Release(e->tex);
            e->srv = NULL; e->tex = NULL;
        }
        if (!e->tex) {
            D3D11_TEXTURE2D_DESC d;
            memset(&d, 0, sizeof d);
            d.Width = st->tex_w; d.Height = st->tex_h; d.MipLevels = g_tex_mips ? 0 : 1; d.ArraySize = 1;   /* 0: a full chain */
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (g_tex_mips ? D3D11_BIND_RENDER_TARGET : 0);
            d.MiscFlags = g_tex_mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &e->tex))) return NULL;
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        }
        e->key = st->tex_key; e->w = st->tex_w; e->h = st->tex_h; e->used = 1;
        if (evicted) tex_index_rebuild(); else tex_index_add((int)(e - g_tex));
    }
#ifdef PSP2I_TEXREPLACE
    /* A texture pack's PNG instead, when one replaces this texture: the
     * original isn't decoded (texture_view samples the PNG). */
    e->rep_id = allow_rep ? texreplace_find() : 0;
    if (e->rep_id && rep_view(e)) { e->hash = h; e->check_frame = f; e->last_use = ++g_use; e->rep_wait = 0; return e; }
    e->rep_wait = e->rep_id && texreplace_pending(e->rep_id) ? e->rep_id : 0;
    e->rep_id = 0;
#else
    (void)allow_rep;
#endif
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * st->tex_h * 4);
    if (!px) return NULL;
    psp_gpu_decode_texture(px);
#if defined(PSP2I_TEXREPLACE) && !defined(PSP2I_PROD)
    if (allow_rep && texreplace_on()) texreplace_dump_miss(px, (int)st->tex_w, (int)st->tex_h);
#endif
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)e->tex, 0, NULL, px, st->tex_w * 4, 0);
    if (g_tex_mips && e->srv) ID3D11DeviceContext_GenerateMips(g_ctx, e->srv);
    free(px);
    e->hash = h; e->check_frame = f; e->last_use = ++g_use;
    n_tex_decodes++;
    return e;
}

/* ---- state objects (created on demand, cached by description) ------------------------ */

typedef struct { uint64_t key; void *obj; } state_slot;
static state_slot g_blend[256], g_dss[64], g_samp[16];

/* The samplers again from the next draw (the filtering changed). */
static void sampler_flush(void) {
    for (int i = 0; i < 16; i++) {
        if (g_samp[i].obj) ID3D11SamplerState_Release((ID3D11SamplerState *)g_samp[i].obj);
        g_samp[i].obj = NULL;
    }
}

static void *cache_find(state_slot *c, int n, uint64_t key) {
    for (int i = 0; i < n; i++) if (c[i].obj && c[i].key == key) return c[i].obj;
    return NULL;
}
static void cache_put(state_slot *c, int n, uint64_t key, void *obj) {
    for (int i = 0; i < n; i++) if (!c[i].obj) { c[i].key = key; c[i].obj = obj; return; }
    c[n - 1].key = key; c[n - 1].obj = obj;          /* full: replace the last (leaks one object) */
}

/* A PSP blend factor as a D3D11 one. `fix` is that side's fixed colour. */
static D3D11_BLEND map_factor(int f, int is_src, uint32_t fix, uint32_t *blend_factor, int *approx) {
    switch (f) {
    case 0:  return is_src ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
    case 1:  return is_src ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
    case 2:  return D3D11_BLEND_SRC_ALPHA;
    case 3:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 4:  return D3D11_BLEND_DEST_ALPHA;
    case 5:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 6:  *approx = 1; return D3D11_BLEND_SRC_ALPHA;       /* 2 x src alpha */
    case 7:  *approx = 1; return D3D11_BLEND_INV_SRC_ALPHA;
    case 8:  *approx = 1; return D3D11_BLEND_DEST_ALPHA;
    case 9:  *approx = 1; return D3D11_BLEND_INV_DEST_ALPHA;
    default:
        if (fix == 0xFFFFFF) return D3D11_BLEND_ONE;
        if (fix == 0) return D3D11_BLEND_ZERO;
        if (*blend_factor == 0xFFFFFFFFu || *blend_factor == fix) { *blend_factor = fix; return D3D11_BLEND_BLEND_FACTOR; }
        if (*blend_factor == (~fix & 0xFFFFFF)) return D3D11_BLEND_INV_BLEND_FACTOR;
        *approx = 1;
        return D3D11_BLEND_BLEND_FACTOR;
    }
}

/* A draw into the HUD picture (d3d_draw_hud): it starts transparent, so
 * alpha builds up as "over" (the colour then comes out premultiplied, which
 * the VR layer expects), additive draws (glows) add light without adding
 * alpha, and alpha is always written. */
static int g_hud_draw;

static ID3D11BlendState *blend_state(const psp_gpu_state *st, float factor[4], int *approx) {
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    D3D11_RENDER_TARGET_BLEND_DESC *b = &d.RenderTarget[0];
    uint8_t wm = 0;
    if (st->clear) {
        if (st->clear_which & 1) wm |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (st->clear_which & 2) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    } else {
        for (int ch = 0; ch < 3; ch++) {
            uint32_t m = (st->mask_rgb >> (ch * 8)) & 0xFF;
            if (m != 0xFF) wm |= (uint8_t)(1u << ch);
            if (m != 0 && m != 0xFF) *approx = 1;
        }
        if (st->mask_a != 0xFF) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (st->mask_a != 0 && st->mask_a != 0xFF) *approx = 1;
    }
    if (st->fb_fmt == 0) wm &= (uint8_t)~D3D11_COLOR_WRITE_ENABLE_ALPHA;   /* 565 has no alpha */
    if (g_hud_draw && !st->clear) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    b->RenderTargetWriteMask = wm;
    uint32_t bf = 0xFFFFFFFFu;
    if (st->blend && !st->clear) {
        b->BlendEnable = TRUE;
        b->SrcBlend = map_factor(st->bsrc, 1, st->fixa, &bf, approx);
        b->DestBlend = map_factor(st->bdst, 0, st->fixb, &bf, approx);
        static const D3D11_BLEND_OP OPS[6] = {
            D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
            D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX, D3D11_BLEND_OP_MAX };
        if (st->beq >= 5) *approx = 1;
        b->BlendOp = OPS[st->beq < 6 ? st->beq : 0];
        b->SrcBlendAlpha = D3D11_BLEND_ONE;               /* the result keeps the source alpha */
        b->DestBlendAlpha = D3D11_BLEND_ZERO;
        b->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (g_hud_draw) {
            const int additive = b->DestBlend == D3D11_BLEND_ONE;
            b->SrcBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
            b->DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        }
    }
    if (bf == 0xFFFFFFFFu) bf = 0;
    factor[0] = (float)(bf & 0xFF) / 255.0f;
    factor[1] = (float)((bf >> 8) & 0xFF) / 255.0f;
    factor[2] = (float)((bf >> 16) & 0xFF) / 255.0f;
    factor[3] = 1.0f;
    uint64_t key = (uint64_t)wm | (uint64_t)b->BlendEnable << 8 | (uint64_t)b->SrcBlend << 9 |
                   (uint64_t)b->DestBlend << 14 | (uint64_t)b->BlendOp << 19 | (uint64_t)(g_hud_draw != 0) << 24;
    ID3D11BlendState *s = (ID3D11BlendState *)cache_find(g_blend, 256, key);
    if (!s && SUCCEEDED(ID3D11Device_CreateBlendState(g_dev, &d, &s))) cache_put(g_blend, 256, key, s);
    return s;
}

static ID3D11DepthStencilState *depth_state(const psp_gpu_state *st) {
    static const D3D11_COMPARISON_FUNC F[8] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_LESS, D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_GREATER_EQUAL };
    D3D11_DEPTH_STENCIL_DESC d;
    memset(&d, 0, sizeof d);
    if (st->clear) {
        d.DepthEnable = (st->clear_which & 4) ? TRUE : FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    } else {
        d.DepthEnable = st->ztest ? TRUE : FALSE;
        d.DepthWriteMask = st->zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = F[st->zfunc & 7];
    }
    uint64_t key = (uint64_t)d.DepthEnable | (uint64_t)d.DepthWriteMask << 1 | (uint64_t)d.DepthFunc << 2;
    ID3D11DepthStencilState *s = (ID3D11DepthStencilState *)cache_find(g_dss, 64, key);
    if (!s && SUCCEEDED(ID3D11Device_CreateDepthStencilState(g_dev, &d, &s))) cache_put(g_dss, 64, key, s);
    return s;
}

static ID3D11SamplerState *sampler(int linear, int cu, int cv) {
    uint64_t key = (uint64_t)linear | (uint64_t)cu << 1 | (uint64_t)cv << 2;
    ID3D11SamplerState *s = (ID3D11SamplerState *)cache_find(g_samp, 16, key);
    if (s) return s;
    D3D11_SAMPLER_DESC d;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? (g_aniso ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR) : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.MaxAnisotropy = g_aniso ? (UINT)g_aniso : 1;
    d.AddressU = cu ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressV = cv ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(ID3D11Device_CreateSamplerState(g_dev, &d, &s))) cache_put(g_samp, 16, key, s);
    return s;
}

/* ---- the draw ------------------------------------------------------------------------- */

/* The view to sample for this draw's texture, and the texel-to-UV scale. */
static int rt_resolve_copy(rt_entry *r);
static int xfer_enabled(void);

/* What the last texture_view sampled, for the eye draws that follow it: a
 * render target (sampled directly, or through the scratch copy when it is
 * also the target), or NULL for an ordinary texture. */
static rt_entry *g_tv_src;
static int g_tv_self;

static ID3D11ShaderResourceView *texture_view(const psp_gpu_state *st, rt_entry *target, float scale[2]) {
    g_tv_src = NULL; g_tv_self = 0;
    const uint32_t toff = vram_off(st->tex_addr);
    const int in_vram = (st->tex_addr & 0x0F000000u) == 0x04000000u;
    if (in_vram) {
        static const int tbpp[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
        uint32_t tend = toff + st->tex_bufw * st->tex_h * (uint32_t)tbpp[st->tex_psm > 10 ? 3 : st->tex_psm] / 8;
        /* A target with exactly the texture's layout: bring it up to date
         * from whichever targets own newer bytes, on the GPU and without
         * moving ownership (rt_resolve_copy), then sample it -- instead of
         * reading the owners back and decoding VRAM. Only when it then holds
         * bytes newer than VRAM; otherwise VRAM is exact and decoded below. */
        if (xfer_enabled() && st->tex_psm <= 3 && !st->tex_swz) {
            rt_entry *r = rt_find(toff, st->tex_bufw, st->tex_psm);
            if (r && tend <= rt_end(r) && rt_resolve_copy(r)) {
                n_rt_tex++;
                scale[0] = 1.0f / (float)r->w;
                scale[1] = 1.0f / (float)r->h;
                g_tv_src = r; g_tv_self = r == target;
                if (r != target) return r->srv;
                if (!g_scratch || g_scratch_w != r->w || g_scratch_h != r->h) {
                    if (g_scratch_srv) ID3D11ShaderResourceView_Release(g_scratch_srv);
                    if (g_scratch) ID3D11Texture2D_Release(g_scratch);
                    D3D11_TEXTURE2D_DESC d;
                    ID3D11Texture2D_GetDesc(r->tex, &d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_scratch);
                    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_scratch, NULL, &g_scratch_srv);
                    g_scratch_w = r->w; g_scratch_h = r->h;
                }
                ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_scratch, (ID3D11Resource *)r->tex);
                return g_scratch_srv;
            }
        }
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *t = &g_rt[i];
            if (!t->used || !overlaps(toff, tend, t->off, rt_end(t))) continue;
            const int same_fmt = st->tex_psm <= 3 && st->tex_psm == t->fmt && !st->tex_swz;
            /* Stereo: a target with eye copies is sampled from the GPU even
             * when VRAM is current, so each eye samples its own copy (the eye
             * draws follow g_tv_src). Decoded from VRAM, both eyes got the
             * mono (left) picture: PSP2i's glow buffer 0x154000 is sampled as
             * 512x512, taller than its 272 rows, so the exact-layout path
             * above skips it, and the glow came out the same in both eyes,
             * at no depth, apart from what glows (user, 2026-10-23: "the
             * glow from enemy somehow become separate from body"; 0 px
             * between the eyes against the body's 3). Without stereo no
             * target has eye copies, so nothing changes. */
            const int eyes = t->eye_rtv[0] || t->eye_rtv[1];
            if (eyes && !t->dirty && t->off == toff && st->tex_bufw == t->stride && same_fmt) rt_check(t);
            if ((t->dirty || eyes) && t->off == toff && st->tex_bufw == t->stride && same_fmt) {
                /* Render-to-texture: sample the GPU copy directly. */
                n_rt_tex++;
                scale[0] = 1.0f / (float)t->w;
                scale[1] = 1.0f / (float)t->h;
                g_tv_src = t; g_tv_self = t == target;
                if (t != target) return t->srv;
                /* Sampled while being drawn to: sample a copy. */
                if (!g_scratch || g_scratch_w != t->w || g_scratch_h != t->h) {
                    if (g_scratch_srv) ID3D11ShaderResourceView_Release(g_scratch_srv);
                    if (g_scratch) ID3D11Texture2D_Release(g_scratch);
                    D3D11_TEXTURE2D_DESC d;
                    ID3D11Texture2D_GetDesc(t->tex, &d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_scratch);
                    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_scratch, NULL, &g_scratch_srv);
                    g_scratch_w = t->w; g_scratch_h = t->h;
                }
                ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_scratch, (ID3D11Resource *)t->tex);
                return g_scratch_srv;
            }
            /* Read in another shape: make VRAM current and decode normally --
             * if this target's newer rows are in the texture at all. */
            if (t->dirty && overlaps(toff, tend, row_addr(t, t->dy0), row_addr(t, t->dy1))) {
                static int log = -1, logged;
                if (log < 0) log = getenv("PSP2I_RB_LOG") != NULL;
                if (log && logged++ < 12)
                    fprintf(stderr, "texture readback: tex 0x%06X..0x%06X bufw %u psm %d %ux%u swz %d; target 0x%06X/%u fmt %d rows %u-%u (flip %llu)%s\n",
                            toff, tend, st->tex_bufw, st->tex_psm, st->tex_w, st->tex_h, st->tex_swz,
                            t->off, t->stride, t->fmt, t->dy0, t->dy1, (unsigned long long)frame_no(), t == target ? " SELF" : "");
                g_rb_reason = 3; rt_readback(t);
            }
        }
    }
    tex_entry *e = tex_get(st, 1);
    scale[0] = 1.0f / (float)st->tex_w;
    scale[1] = 1.0f / (float)st->tex_h;
#ifdef PSP2I_TEXREPLACE
    if (e && e->rep_id) {
        ID3D11ShaderResourceView *v = rep_view(e);
        if (v) return v;
        /* Its PNG can't be read back (or uploaded): decode the original. */
        e->rep_id = 0; e->hash = 0; e->check_frame = 0;
        e = tex_get(st, 0);
    }
#endif
    return e ? e->srv : NULL;
}

/* PSP2I_DRAW_TRACE=FLIP: describe every draw of that frame (diagnostics). */
static uint64_t g_trace_flip = (uint64_t)-1;
static int trace_on(void) {
    static int init;
    if (!init) { const char *e = getenv("PSP2I_DRAW_TRACE"); if (e) g_trace_flip = strtoull(e, NULL, 0); init = 1; }
    return frame_no() == g_trace_flip;
}

/* ---- moving bytes between overlapping targets on the GPU -------------------------------
 *
 * Invariant: for any VRAM byte, at most one target holds rows newer than VRAM
 * (dirty). Before drawing into t, every other target o whose dirty rows
 * overlap t gives those bytes to t: rt_xfer converts them into t's layout on
 * the GPU, t's dirty rows grow to cover them and o's shrink to exclude them.
 * That keeps every reader correct -- a readback (display, CPU access, block
 * transfer, eviction) only ever finds the newest bytes in exactly one place --
 * without the readback-and-reseed round trip, which stalled the pipeline
 * about three times per frame in PSP2i (bloom buffers inside the back buffer).
 * When the overlap cannot be split along row boundaries the old path is
 * used: read o back, then t re-reads VRAM. PSP2I_XFER=0 forces the old path. */
static uint64_t n_xfer;

static int xfer_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PSP2I_XFER"); on = !(e && e[0] == '0'); }
    return on && g_xps;
}

static uint32_t rt_rowbytes(const rt_entry *t) { return t->stride * (uint32_t)bpp_of(t->fmt); }

/* Which of o's rows lie inside t's bytes: [*r0, *r1). Returns 0 if a dirty
 * row of o is only partly inside t, or the bytes do not fall on t's pixels
 * -- then the bytes go through VRAM instead. */
static int xfer_rows(const rt_entry *o, const rt_entry *t, uint32_t *r0, uint32_t *r1) {
    const uint32_t ro = rt_rowbytes(o), oe = o->off + o->h * ro;
    const uint32_t lo = o->off > t->off ? o->off : t->off, e = rt_end(t), hi = oe < e ? oe : e;
    *r0 = *r1 = 0;
    if (lo >= hi) return 1;
    uint32_t a = (lo - o->off) / ro, b = (hi - o->off + ro - 1) / ro;    /* rows touching t */
    if (b > o->h) b = o->h;
    if ((lo - o->off) % ro && row_dirty(o, a)) return 0;                  /* partial first row */
    if ((hi - o->off) % ro && row_dirty(o, b - 1)) return 0;              /* partial last row */
    if ((lo - o->off) % ro) a++;
    if ((hi - o->off) % ro) b--;
    if (((row_addr(o, a) - t->off) % (uint32_t)bpp_of(t->fmt))) return 0;
    *r0 = a; *r1 = b > a ? b : a;
    return 1;
}

/* PSP2I_XFER_VERIFY: check every transfer against the CPU path (readback
 * then upload, computed here from both targets' pixels), and the ownership
 * invariant after every draw. Slow; for validation only. */
static uint64_t n_xv_checked, n_xv_bad_px, n_xv_bad_xfers, n_xv_overlap;
static int xverify(void) {
    static int on = -1;
    if (on < 0) on = getenv("PSP2I_XFER_VERIFY") != NULL;
    return on && g_scale == 1 && g_scale_y == 1;                         /* it compares PSP pixels 1:1 */
}

static int map_rows(rt_entry *t, uint32_t y0, uint32_t y1, D3D11_MAPPED_SUBRESOURCE *m) {
    if (!t->staging) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(t->tex, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->staging))) return 0;
    }
    D3D11_BOX box = { 0, y0, 0, t->w, y1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)t->staging, 0, 0, y0, 0, (ID3D11Resource *)t->tex, 0, &box);
    return SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ, 0, m));
}

static void xverify_xfer(rt_entry *t, rt_entry *o, uint32_t a, uint32_t b, uint32_t y0, uint32_t y1) {
    const uint32_t i0 = row_addr(o, a), i1 = row_addr(o, b), ob = (uint32_t)bpp_of(o->fmt), tb = (uint32_t)bpp_of(t->fmt);
    uint8_t *bytes = (uint8_t *)malloc(i1 - i0);
    if (!bytes) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!map_rows(o, a, b, &m)) { free(bytes); return; }
    for (uint32_t y = a; y < b; y++) {                  /* what a readback of o stores */
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        uint8_t *dst = bytes + (row_addr(o, y) - i0);
        if (ob == 4) memcpy(dst, src, o->stride * 4u);
        else for (uint32_t x = 0; x < o->stride; x++) { uint16_t p = pack16(o->fmt, src[x]); memcpy(dst + x * 2, &p, 2); }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)o->staging, 0);
    if (!map_rows(t, y0, y1, &m)) { free(bytes); return; }
    uint64_t bad = 0;
    for (uint32_t y = y0; y < y1; y++) {                /* what seeding t from those bytes gives */
        const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        for (uint32_t x = 0; x < t->stride; x++) {
            const uint32_t A = t->off + (y * t->stride + x) * tb;
            if (A < i0 || A + tb > i1) continue;
            uint32_t want;
            if (tb == 4) memcpy(&want, bytes + (A - i0), 4);
            else { uint16_t p; memcpy(&p, bytes + (A - i0), 2); want = expand16(t->fmt, p); }
            if (row[x] != want) bad++;
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)t->staging, 0);
    free(bytes);
    n_xv_checked++;
    if (bad) {
        n_xv_bad_px += bad;
        if (n_xv_bad_xfers++ < 5)
            fprintf(stderr, "xfer-verify: 0x%06X/%u fmt %d rows %u-%u -> 0x%06X/%u fmt %d: %llu pixels differ\n",
                    o->off, o->stride, o->fmt, a, b, t->off, t->stride, t->fmt, (unsigned long long)bad);
    }
}

/* No two targets may both hold newer-than-VRAM rows for the same bytes. */
static void xverify_owners(void) {
    for (int i = 0; i < MAX_RT; i++) {
        const rt_entry *p = &g_rt[i];
        if (!p->used || !p->dirty) continue;
        for (int j = i + 1; j < MAX_RT; j++) {
            const rt_entry *q = &g_rt[j];
            if (!q->used || !q->dirty || !overlaps(row_addr(p, p->dy0), row_addr(p, p->dy1), row_addr(q, q->dy0), row_addr(q, q->dy1))) continue;
            for (uint32_t y = p->dy0; y < p->dy1; y++) {
                if (!row_dirty(p, y)) continue;
                const uint32_t a0 = row_addr(p, y), a1 = row_addr(p, y + 1);
                const uint32_t rq = rt_rowbytes(q);
                uint32_t z0 = a0 > q->off ? (a0 - q->off) / rq : 0, z1 = a1 > q->off ? (a1 - q->off + rq - 1) / rq : 0;
                for (uint32_t z = z0; z < z1 && z < 512; z++)
                    if (row_dirty(q, z) && overlaps(a0, a1, row_addr(q, z), row_addr(q, z + 1))) {
                        if (n_xv_overlap++ < 5)
                            fprintf(stderr, "xfer-verify: 0x%06X/%u row %u and 0x%06X/%u row %u are both ahead of VRAM\n",
                                    p->off, p->stride, y, q->off, q->stride, z);
                        goto next_pair;
                    }
            }
        next_pair:;
        }
    }
}

/* Copy o's dirty rows [a, b) (all inside t) into t, in t's layout. own: t
 * takes ownership of the rows (the caller clears them in o); otherwise t only
 * gets a fresh copy to sample, and o stays the owner. */
static void rt_xfer(rt_entry *t, rt_entry *o, uint32_t a, uint32_t b, int own) {
    const uint32_t i0 = row_addr(o, a), i1 = row_addr(o, b);
    if (i0 >= i1) return;
    const uint32_t rt = rt_rowbytes(t);
    uint32_t y0 = (i0 - t->off) / rt, y1 = (i1 - t->off + rt - 1) / rt;
    if (y1 > t->h) y1 = t->h;
    xfer_consts xc = {
        { t->off, t->stride, (uint32_t)t->fmt, (uint32_t)bpp_of(t->fmt) },
        { o->off, o->stride, (uint32_t)o->fmt, (uint32_t)bpp_of(o->fmt) },
        { i0, i1, (uint32_t)g_scale, (uint32_t)g_scale_y } };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_xcb, 0, NULL, &xc, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &t->rtv, NULL);
    const LONG S = g_scale, SY = g_scale_y;
    D3D11_VIEWPORT vp = { 0, 0, (float)(t->w * S), (float)(t->h * SY), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    D3D11_RECT sc = { 0, (LONG)y0 * SY, (LONG)t->w * S, (LONG)y1 * SY };
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_xvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_xps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &g_xcb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &o->srv);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    for (int e = 0; e < 2; e++) {                       /* and the same bytes in each eye's copy */
        if (!t->eye_rtv[e]) continue;
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &t->eye_rtv[e], NULL);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, o->eye_srv[e] ? &o->eye_srv[e] : &o->srv);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, &none);
    if (xverify()) xverify_xfer(t, o, a, b, y0, y1);
    if (own) rows_mark(t, y0, y1);
    t->gen++;
    n_xfer++;
}

/* Make r's pixels the newest bytes for its whole range, for sampling:
 * fallbacks through VRAM first, then r re-checks VRAM, then copies (not
 * ownership) of other targets' newer rows. Returns 1 if r then holds bytes
 * newer than VRAM (its own dirty rows or copies). */
static int rt_resolve_copy(rt_entry *r) {
    const uint32_t r0 = r->off, r1 = rt_end(r);
    int flushed = 0, newer = r->dirty;
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        uint32_t a, b;
        if (xfer_rows(o, r, &a, &b)) continue;
        g_rb_reason = 3; rt_readback(o); flushed = 1;
    }
    if (flushed && !r->dirty) r->check_frame = (uint64_t)-1;
    rt_check(r);
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        uint32_t a, b;
        if (!xfer_rows(o, r, &a, &b) || a >= b) continue;
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

/* One draw into rtv (a target tw x th PSP pixels, at the internal
 * resolution) with depth dsv and texture srv. -1 if it could not be queued. */
static int submit_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts, ID3D11RenderTargetView *rtv,
                       ID3D11DepthStencilView *dsv, ID3D11ShaderResourceView *srv, uint32_t tw, uint32_t th,
                       const float uvs[2]) {
    /* Vertices into the ring buffer. */
    UINT bytes = (UINT)nverts * (UINT)sizeof(psp_gpu_vertex);
    if (bytes > VB_BYTES) return -1;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vb_pos + bytes > VB_BYTES) { g_vb_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, mode, 0, &m))) return -1;
    memcpy((uint8_t *)m.pData + g_vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
    UINT stride = sizeof(psp_gpu_vertex), vboff = g_vb_pos;
    g_vb_pos += bytes;

    vs_consts vc = { { (float)tw, (float)th, uvs[0], uvs[1] } };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_vcb, 0, NULL, &vc, 0, 0);
    ps_consts pc;
    memset(&pc, 0, sizeof pc);
    pc.env[0] = (float)(st->env & 0xFF) / 255.0f;
    pc.env[1] = (float)((st->env >> 8) & 0xFF) / 255.0f;
    pc.env[2] = (float)((st->env >> 16) & 0xFF) / 255.0f;
    pc.tf[0] = (uint32_t)(st->tex != 0); pc.tf[1] = (uint32_t)st->tfunc;
    pc.tf[2] = (uint32_t)st->trgba; pc.tf[3] = (uint32_t)st->tdbl;
    pc.at[0] = (uint32_t)(st->atest && !st->clear); pc.at[1] = (uint32_t)st->afunc;
    pc.at[2] = st->aref; pc.at[3] = st->amask;
    pc.misc[0] = (uint32_t)st->clear;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_pcb, 0, NULL, &pc, 0, 0);

    int approx = 0;
    float bf[4];
    ID3D11BlendState *bs = blend_state(st, bf, &approx);
    if (approx) n_approx++;

    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, dsv);
    /* The vertex shader maps PSP pixels (vc.vp) to the target; the viewport
     * and scissor are the target's own, S times larger (SY down). */
    const LONG S = g_scale, SY = g_scale_y;
    D3D11_VIEWPORT vp = { 0, 0, (float)(tw * S), (float)(th * SY), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    D3D11_RECT sc = { st->sx0 * S, st->sy0 * SY, (st->sx1 + 1) * S, (st->sy1 + 1) * SY };
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(st), 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &vboff);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_vcb);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 1, 1, &g_pcb);
    if (srv) {
        ID3D11SamplerState *ss = sampler(st->tex_linear, st->tex_clamp_u, st->tex_clamp_v);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &ss);
    }
    ID3D11DeviceContext_Draw(g_ctx, (UINT)nverts, 0);
    if (srv) {                                          /* unbind, so the view can become a target */
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);
    }
    return 0;
}

/* The last draw d3d_draw accepted, for its eye draws (which follow it at
 * once, with the same state): its target, depth and texture. */
static struct {
    rt_entry *t;
    depth_entry *d;
    ID3D11ShaderResourceView *srv;
    rt_entry *src;                       /* the render target sampled, if any */
    int self;                            /* ... which is also the target */
    float uvs[2];
} g_last;
static uint64_t n_eye_draws;

/* Stereo with the mono draw as the left eye (psp_gpu_stereo.mono_is_left):
 * the left eye's picture is the target itself, and only the right eye has a
 * copy. */
static int g_mono_left;
void d3d11_set_mono_left(int on) { g_mono_left = on; }
static ID3D11Texture2D *eye_picture(rt_entry *t, int e) { return e == 0 && g_mono_left ? t->tex : t->eye_tex[e]; }

/* Where the renderer's time goes (d3d11_report: milliseconds per presented
 * frame): the game's draws, the eyes' draws, the HUD picture's, sharing the
 * eyes with the headset and presenting (the copies for the window). CPU time
 * on the game thread, including any wait on the GPU inside those calls. */
static uint64_t t_draw, t_eye, t_hud, t_share, t_present, n_present_calls;
static inline uint64_t qpc_now(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }

static int d3d_draw_body(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
static int d3d_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    const uint64_t t0 = qpc_now();
    const int r = d3d_draw_body(st, v, nverts);
    t_draw += qpc_now() - t0;
    return r;
}

static int d3d_draw_body(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if ((st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || nverts <= 0) return 1;
    if (trace_on()) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < nverts; i++) {
            if (v[i].x < x0) x0 = v[i].x; if (v[i].x > x1) x1 = v[i].x;
            if (v[i].y < y0) y0 = v[i].y; if (v[i].y > y1) y1 = v[i].y;
        }
        fprintf(stderr, "draw: fb 0x%06X/%u fmt %d  %s  verts %d  box %.0f,%.0f-%.0f,%.0f  scissor %d,%d-%d,%d",
                vram_off(st->fb_addr), st->fb_stride, st->fb_fmt, st->clear ? "CLEAR" : "     ", nverts,
                x0, y0, x1, y1, st->sx0, st->sy0, st->sx1, st->sy1);
        if (st->clear) fprintf(stderr, "  which %u", st->clear_which);
        if (st->tex) fprintf(stderr, "  tex 0x%08X/%u psm %d %ux%u", st->tex_addr, st->tex_bufw, st->tex_psm, st->tex_w, st->tex_h);
        if (st->blend) fprintf(stderr, "  blend %d/%d eq %d", st->bsrc, st->bdst, st->beq);
        fprintf(stderr, "  mask %06X/%02X\n", st->mask_rgb, st->mask_a);
    }
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > 512) rows = 512;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) return 1;
    /* Targets that share VRAM with this one at a different start or layout
     * (PSP2i blurs its bloom in two 256-wide buffers that sit inside the
     * 512-wide display buffer, which the final copy then overwrites). VRAM
     * holds one truth, so at most one of an overlapping set may be ahead of
     * it: flush any other dirty one before drawing here, take its bytes, and
     * make the others re-read VRAM before their next draw. Without this a
     * stale blur buffer was read back over the finished frame at display
     * time, as a striped band. */
    {
        /* t first matches VRAM where VRAM is the newest copy (rt_check);
         * then the overlapping dirty bytes come over -- on the GPU when
         * ownership splits cleanly (rt_xfer), else through VRAM. */
        const uint32_t t0 = t->off, t1 = rt_end(t);
        int flushed = 0;
        for (int i = 0; i < MAX_RT; i++) {               /* pass 1: the fallbacks */
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !o->dirty || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            uint32_t a, b;
            if (xfer_enabled() && xfer_rows(o, t, &a, &b)) continue;
            g_rb_reason = 4; rt_readback(o); flushed = 1;
        }
        if (flushed && !t->dirty) t->check_frame = (uint64_t)-1;
        rt_check(t);
        for (int i = 0; i < MAX_RT; i++) {               /* pass 2: on the GPU */
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            o->check_frame = (uint64_t)-1;
            uint32_t a, b;
            if (!o->dirty || !xfer_rows(o, t, &a, &b) || a >= b) continue;
            for (uint32_t y = a; y < b; ) {               /* each run of dirty rows */
                if (!row_dirty(o, y)) { y++; continue; }
                uint32_t z = y;
                while (z < b && row_dirty(o, z)) z++;
                rt_xfer(t, o, y, z, 1);
                y = z;
            }
            rows_clear(o, a, b);
        }
    }

    g_last.t = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    depth_entry *d = NULL;
    if (st->zb_stride && (st->ztest || (st->clear && (st->clear_which & 4)))) {
        d = depth_get(vram_off(st->zb_addr), t->w, t->h);
        if (d) dsv = d->dsv;
    }

    ID3D11ShaderResourceView *srv = NULL;
    float uvs[2] = { 1, 1 };
    if (st->tex) {
        srv = texture_view(st, t, uvs);
        if (!srv) return 1;
    }

    if (submit_draw(st, v, nverts, t->rtv, dsv, srv, t->w, t->h, uvs) != 0) return 1;
    g_last.t = t; g_last.d = d; g_last.srv = srv;
    g_last.src = st->tex ? g_tv_src : NULL; g_last.self = st->tex && g_tv_self;
    g_last.uvs[0] = uvs[0]; g_last.uvs[1] = uvs[1];
    t->gen++;

    /* Rows touched: the scissor, narrowed to the geometry's extent. */
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) rows_mark(t, (uint32_t)y0, (uint32_t)y1);
    if (xverify()) xverify_owners();
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    return 0;
}

/* Stereo: make t's copy for eye e, starting as the mono picture. */
static int eye_target(rt_entry *t, int e) {
    if (t->eye_rtv[e]) return 1;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t->tex, &d);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->eye_tex[e]))) return 0;
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->eye_tex[e], NULL, &t->eye_rtv[e]);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->eye_tex[e], NULL, &t->eye_srv[e]);
    if (!t->eye_rtv[e] || !t->eye_srv[e]) return 0;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)t->eye_tex[e], (ID3D11Resource *)t->tex);
    return 1;
}

static ID3D11DepthStencilView *eye_depth(depth_entry *d, int e) {
    if (d->eye_dsv[e]) return d->eye_dsv[e];
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(d->tex, &td);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &d->eye_tex[e]))) return NULL;
    ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)d->eye_tex[e], NULL, &d->eye_dsv[e]);
    if (d->eye_dsv[e]) ID3D11DeviceContext_ClearDepthStencilView(g_ctx, d->eye_dsv[e], D3D11_CLEAR_DEPTH, 0.0f, 0);
    return d->eye_dsv[e];
}

/* The draw d3d_draw just made, again into eye e's copies of its target and
 * depth (psp_gpu_set_stereo). VRAM never sees these: the mono target keeps
 * coherence, and its uploads and transfers are repeated into the eye copies
 * (rt_upload, rt_xfer). A render target sampled as a texture is sampled in
 * the same eye's copy, so bloom and blur stay per eye. */
static int d3d_draw_eye_body(int e, const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
static int d3d_draw_eye(int e, const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    const uint64_t t0 = qpc_now();
    const int r = d3d_draw_eye_body(e, st, v, nverts);
    t_eye += qpc_now() - t0;
    return r;
}

static int d3d_draw_eye_body(int e, const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    rt_entry *t = g_last.t;
    if (!t || e < 0 || e > 1 || nverts <= 0 || !eye_target(t, e)) return 1;
    ID3D11DepthStencilView *dsv = g_last.d ? eye_depth(g_last.d, e) : NULL;
    ID3D11ShaderResourceView *srv = g_last.srv;
    if (g_last.src && g_last.src->eye_srv[e]) {
        srv = g_last.src->eye_srv[e];
        if (g_last.self) {                              /* sampled while drawn to: a copy, as for mono */
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_scratch, (ID3D11Resource *)t->eye_tex[e]);
            srv = g_scratch_srv;
        }
    }
#ifndef PSP2I_PROD
    else if (st->tex && (st->tex_addr & 0x0F000000u) == 0x04000000u) {
        /* Dev: an eye draw sampling a target's bytes without that eye's own
         * copy (the mono picture, or VRAM): once per texture address. */
        const uint32_t toff = vram_off(st->tex_addr);
        static uint32_t seen[32];
        static int n_seen;
        int k = 0;
        while (k < n_seen && seen[k] != toff) k++;
        for (int i = 0; i < MAX_RT && k == n_seen && n_seen < 32; i++) {
            const rt_entry *o = &g_rt[i];
            if (!o->used || (!o->eye_rtv[0] && !o->eye_rtv[1]) || toff < o->off || toff >= rt_end(o)) continue;
            seen[n_seen++] = toff;
            fprintf(stderr, "d3d11: eye %d samples VRAM+0x%06X (%ux%u psm %d bufw %u) without its own copy of target 0x%06X/%u fmt %d%s\n",
                    e, toff, st->tex_w, st->tex_h, st->tex_psm, st->tex_bufw, o->off, o->stride, o->fmt,
                    g_last.src ? " (the source has no eye copy)" : "");
        }
    }
#endif
    if (submit_draw(st, v, nverts, t->eye_rtv[e], dsv, srv, t->w, t->h, g_last.uvs) != 0) return 1;
    const uint32_t tag = psp_gpu_stereo_tag();
    if (t->eye_frame != frame_no()) t->eye_tag = 0;
    if (tag) t->eye_tag = tag;
    t->eye_frame = frame_no();
    t->gen++;
    n_eye_draws++;
    return 0;
}

/* The HUD pictures (psp_gpu_backend.draw_hud): the HUD pass's panel draws, as
 * the game drew them, on a transparent target the size of the 3D target, for
 * the VR HUD panel (shared with the eyes, share_eyes). One per frame in a
 * ring, each named by its frame: the eyes of frame N go to the headset at the
 * vblank after its flip, when the game has often begun frame N+1's HUD
 * already, so share_eyes takes the picture of the eyes' own frame. (With one
 * picture the frames never matched and the headset got no HUD.) */
#define HUD_RING 3
static ID3D11Texture2D *g_hud_tex[HUD_RING], *g_hud_empty;
static ID3D11RenderTargetView *g_hud_rtv[HUD_RING];
static uint64_t g_hud_fr[HUD_RING] = { (uint64_t)-1, (uint64_t)-1, (uint64_t)-1 };
static int g_hud_cur;
static UINT g_hud_w, g_hud_h;                   /* PSP pixels */
static uint64_t n_hud_draws, n_hud_shared, n_hud_none;

static int hud_target(UINT w, UINT h) {
    if (g_hud_tex[0] && g_hud_w == w && g_hud_h == h) return 1;
    for (int i = 0; i < HUD_RING; i++) {
        if (g_hud_rtv[i]) ID3D11RenderTargetView_Release(g_hud_rtv[i]);
        if (g_hud_tex[i]) ID3D11Texture2D_Release(g_hud_tex[i]);
        g_hud_rtv[i] = NULL; g_hud_tex[i] = NULL; g_hud_fr[i] = (uint64_t)-1;
    }
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = w * (UINT)g_scale; d.Height = h * (UINT)g_scale_y; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < HUD_RING; i++)
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_hud_tex[i])) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_hud_tex[i], NULL, &g_hud_rtv[i]))) return 0;
    g_hud_w = w; g_hud_h = h;
    return 1;
}

/* The ring slot holding frame f's HUD, or -1. */
static int hud_slot(uint64_t f) {
    for (int i = 0; i < HUD_RING; i++) if (g_hud_tex[i] && g_hud_fr[i] == f) return i;
    return -1;
}

static int d3d_draw_hud_body(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
static int d3d_draw_hud(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    const uint64_t t0 = qpc_now();
    const int r = d3d_draw_hud_body(st, v, nverts);
    t_hud += qpc_now() - t0;
    return r;
}

static int d3d_draw_hud_body(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if ((st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || nverts <= 0) return 1;
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > 512) rows = 512;
    rt_entry *t = rt_get(vram_off(st->fb_addr), st->fb_stride, st->fb_fmt, rows);
    if (!t || !hud_target(t->w, t->h)) return 1;
    if (g_hud_fr[g_hud_cur] != frame_no()) {             /* a new frame: the next slot, cleared */
        static const float CLEAR[4] = { 0, 0, 0, 0 };
        g_hud_cur = (g_hud_cur + 1) % HUD_RING;
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_hud_rtv[g_hud_cur], CLEAR);
        g_hud_fr[g_hud_cur] = frame_no();
    }
    ID3D11ShaderResourceView *srv = NULL;
    float uvs[2] = { 1, 1 };
    if (st->tex && !(srv = texture_view(st, t, uvs))) return 1;
    g_hud_draw = 1;
    const int r = submit_draw(st, v, nverts, g_hud_rtv[g_hud_cur], NULL, srv, t->w, t->h, uvs);
    g_hud_draw = 0;
    if (r != 0) return 1;
    n_hud_draws++;
    return 0;
}

/* Dev (VR captures): the HUD picture read back now, over dark grey (it is
 * premultiplied), 0xAARRGGBB, w x h at the internal resolution; NULL if none.
 * Stalls the GPU: for captures only. */
const uint32_t *d3d11_hud_picture(int *w, int *h) {
    static uint32_t *px;
    static size_t cap;
    ID3D11Texture2D *src = g_hud_tex[g_hud_cur];
    if (!src) return NULL;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st))) return NULL;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    const uint32_t *out = NULL;
    if ((size_t)d.Width * d.Height > cap) { free(px); cap = (size_t)d.Width * d.Height; px = (uint32_t *)malloc(cap * 4); }
    uint64_t n_col = 0, n_alpha = 0;                  /* the panel layer needs alpha: count both */
    if (px && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            for (UINT x = 0; x < d.Width; x++) {
                const uint32_t r = src[x * 4], g = src[x * 4 + 1], b = src[x * 4 + 2], a = src[x * 4 + 3];
                if (r | g | b) n_col++;
                if (a) n_alpha++;
                const uint32_t k = 40 * (255 - a) / 255;   /* premultiplied over grey 40 */
                px[(size_t)y * d.Width + x] = 0xFF000000u | ((r + k > 255 ? 255 : r + k) << 16) |
                                              ((g + k > 255 ? 255 : g + k) << 8) | (b + k > 255 ? 255 : b + k);
            }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        fprintf(stderr, "vr: capture: HUD picture (frame %llu): %llu pixels coloured, %llu with alpha\n",
                (unsigned long long)g_hud_fr[g_hud_cur], (unsigned long long)n_col, (unsigned long long)n_alpha);
        *w = (int)d.Width; *h = (int)d.Height;
        out = px;
    }
    ID3D11Texture2D_Release(st);
    return out;
}

static void d3d_sync_vram(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            {
                static int log = -1, logged;
                if (log < 0) log = getenv("PSP2I_RB_LOG") != NULL;
                if (log && logged++ < 40)
                    fprintf(stderr, "sync readback: range 0x%06X+0x%X; target 0x%06X/%u fmt %d rows %u-%u (flip %llu)\n",
                            a0, bytes, g_rt[i].off, g_rt[i].stride, g_rt[i].fmt, g_rt[i].dy0, g_rt[i].dy1,
                            (unsigned long long)frame_no());
                g_rb_reason = 5; rt_readback(&g_rt[i]);
            }
}

static void d3d_vram_written(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;          /* re-hash on the next draw */
}

static void d3d_cpu_access(uint32_t off, uint32_t size) {
    const uint32_t a0 = off, a1 = off + size > VRAM_SIZE ? VRAM_SIZE : off + size;
    if (a0 >= a1) return;
    const uint32_t p0 = a0 >> PG_SHIFT, p1 = (a1 - 1) >> PG_SHIFT;
    int dirty = 0, armed = 0;
    for (uint32_t p = p0; p <= p1; p++) { dirty |= g_pg_dirty[p]; armed |= g_pg_armed[p]; }
    if (!dirty && !armed) return;
    const uint32_t q0 = p0 << PG_SHIFT, q1 = (p1 + 1) << PG_SHIFT;    /* the pages, as bytes */
    if (dirty) {
        for (int i = 0; i < MAX_RT; i++)
            if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
                { g_rb_reason = 6; rt_readback(&g_rt[i]); g_rt[i].check_frame = (uint64_t)-1; }
        memset(g_pg_dirty + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {                /* others may share these pages */
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
            if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
                g_rt[i].check_frame = (uint64_t)-1;      /* re-hash before the next draw */
        memset(g_pg_armed + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {                /* still-armed targets sharing these pages */
            const rt_entry *t = &g_rt[i];
            if (!t->used || t->check_frame == (uint64_t)-1) continue;
            uint32_t d0 = t->off, d1 = rt_end(t);
            if (d0 < q0) d0 = q0;
            if (d1 > q1) d1 = q1;
            pages_set(g_pg_armed, d0, d1);
        }
    }
}

/* ---- presenting without a stall --------------------------------------------------------
 *
 * The window shows the displayed framebuffer every vblank. Reading it back
 * synchronously (Map right after the copy) made the CPU wait for the GPU to
 * finish the whole frame, every frame. Instead, while the display target is
 * ahead of VRAM, each vblank queues a copy into one of PRESENT_RING staging
 * textures and shows the newest copy that the GPU has already finished
 * (Map with DO_NOT_WAIT): the picture is at most a vblank or two late, and
 * nobody waits. When VRAM is current (target clean, or none) the caller
 * reads VRAM as before -- through psp_mem_ptr, so it is exact. Pixels are
 * quantized to the framebuffer format exactly as a readback would store them,
 * so the window shows the same image either way. */
#define PRESENT_RING 3
static ID3D11Texture2D *g_present[PRESENT_RING];
static uint32_t g_present_w[PRESENT_RING];
static int g_present_fmt[PRESENT_RING], g_present_pending[PRESENT_RING];
static uint64_t g_present_seq[PRESENT_RING], g_present_next;
static int g_present_head;
static uint64_t n_present_async, n_present_vram, n_present_kept;

/* The S-times picture of the last d3d11_present that returned 1 (scale > 1). */
static uint32_t *g_hires;
static int g_hires_w, g_hires_h, g_hires_fresh;

static int hires_buffer(int w, int h) {
    if (g_hires && g_hires_w == w && g_hires_h == h) return 1;
    free(g_hires);
    g_hires = (uint32_t *)malloc((size_t)w * h * 4);
    g_hires_w = g_hires ? w : 0; g_hires_h = g_hires ? h : 0;
    return g_hires != NULL;
}

const uint32_t *d3d11_present_hires(int *w, int *h) {
    if (!g_hires_fresh) return NULL;
    *w = g_hires_w; *h = g_hires_h;
    return g_hires;
}

/* Stereo: each eye's copy of the displayed target, read back with it (same
 * ring slot, same frame), S-times size even at S = 1. */
static ID3D11Texture2D *g_eye_present[PRESENT_RING][2];
static int g_present_eyes[PRESENT_RING];
static uint32_t *g_eye_px[2];
static int g_eye_w, g_eye_h, g_eye_fresh;
static uint32_t g_present_tag[PRESENT_RING], g_eye_tag;   /* the eye pictures' head pose (rt_entry.eye_tag) */
static uint64_t n_present_eyes;
static int g_present_stereo[PRESENT_RING];   /* the frame had eyes: no mono S-times picture */

/* Stereo to the headset on the GPU (d3d11_eye_share): both eyes copied side
 * by side into one texture the VR thread's device opens (a DXGI shared
 * handle, keyed mutex 0 on both sides), instead of read back, converted and
 * uploaded again. The copy keeps the GPU's 8-bit colour (the CPU path
 * quantizes to the framebuffer's format, 5551 for PSP2i's display). */
static int g_share_on, g_share_fresh;
static ID3D11Texture2D *g_share;
static IDXGIKeyedMutex *g_share_mx;
static HANDLE g_share_handle;
static UINT g_share_w, g_share_h;
static uint32_t g_share_tag;
static uint64_t n_share, n_share_busy;

void d3d11_eye_share(int on) { g_share_on = on; }
/* Also read the eyes back to the CPU while they are shared (a VR capture):
 * the headset keeps its GPU pictures, so a capture no longer shows as a
 * flat frame in the headset. */
static int g_eye_readback;
void d3d11_eye_readback(int on) { g_eye_readback = on; }

void *d3d11_present_eye_shared(int *w, int *h, uint32_t *tag) {
    if (!g_share_fresh) return NULL;
    *w = (int)g_share_w / 3; *h = (int)g_share_h;
    *tag = g_share_tag;
    return g_share_handle;
}

/* The sharpening pass (d3d11_set_quality): each eye drawn into its part
 * of the shared texture. Cross-shaped CAS: the more contrast around a pixel
 * already, the less it is sharpened (no halos, flat areas left alone). */
static const char *SHARPEN_SRC =
"cbuffer S : register(b3) { float4 sp; };\n"   /* x: the eye's x in the target, y: sharpness 0..1, zw: its size */
"Texture2D<float4> E : register(t2);\n"
"float4 vss(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float4 ld(int2 c) { return E.Load(int3(clamp(c, int2(0, 0), int2(sp.zw) - 1), 0)); }\n"
"float4 pss(float4 pos : SV_Position) : SV_Target {\n"
"  int2 c = int2(pos.xy) - int2((int)sp.x, 0);\n"
"  float4 e = ld(c);\n"
"  float3 b = ld(c + int2(0, -1)).rgb, d = ld(c + int2(-1, 0)).rgb, f = ld(c + int2(1, 0)).rgb, h = ld(c + int2(0, 1)).rgb;\n"
"  float3 mn = min(min(min(d, e.rgb), min(f, b)), h), mx = max(max(max(d, e.rgb), max(f, b)), h);\n"
"  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
"  float3 w = amp * (-1.0 / lerp(8.0, 5.0, sp.y));\n"
"  return float4(saturate((b * w + d * w + f * w + h * w + e.rgb) / (1.0 + 4.0 * w)), e.a);\n"
"}\n";
static ID3D11VertexShader     *g_svs;
static ID3D11Buffer           *g_scb;
static ID3D11RenderTargetView *g_share_rtv;

static void share_free(void) {
    if (g_share_rtv) ID3D11RenderTargetView_Release(g_share_rtv);
    g_share_rtv = NULL;
    if (g_share_mx) IDXGIKeyedMutex_Release(g_share_mx);
    if (g_share) ID3D11Texture2D_Release(g_share);
    g_share_mx = NULL; g_share = NULL; g_share_handle = NULL; g_share_w = g_share_h = 0;
}

/* Copy t's eye pictures (W x H each, at the internal resolution) into the
 * shared texture, and the HUD picture of the same frame (transparent if the
 * frame drew none) as a third: left | right | HUD. 0 if it can't (then the
 * CPU path is used, without the HUD panel). */
static int share_eyes_body(rt_entry *t, UINT W, UINT H);
static int share_eyes(rt_entry *t, UINT W, UINT H) {
    const uint64_t t0 = qpc_now();
    const int r = share_eyes_body(t, W, H);
    t_share += qpc_now() - t0;
    return r;
}

static int share_eyes_body(rt_entry *t, UINT W, UINT H) {
    if (!g_share || g_share_w != 3 * W || g_share_h != H) {
        share_free();
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = 3 * W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        IDXGIResource *r = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_share)) ||
            FAILED(ID3D11Texture2D_QueryInterface(g_share, &IID_IDXGIKeyedMutex, (void **)&g_share_mx)) ||
            FAILED(ID3D11Texture2D_QueryInterface(g_share, &IID_IDXGIResource, (void **)&r)) ||
            FAILED(IDXGIResource_GetSharedHandle(r, &g_share_handle))) {
            if (r) IDXGIResource_Release(r);
            fprintf(stderr, "d3d11: no shared texture for the headset; eyes go through the CPU\n");
            share_free();
            g_share_on = 0;
            return 0;
        }
        IDXGIResource_Release(r);
        if (g_sps) ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_share, NULL, &g_share_rtv);
        g_share_w = 3 * W; g_share_h = H;
        if (g_hud_empty) ID3D11Texture2D_Release(g_hud_empty);
        g_hud_empty = NULL;
        D3D11_TEXTURE2D_DESC e;
        memset(&e, 0, sizeof e);
        e.Width = W; e.Height = H; e.MipLevels = 1; e.ArraySize = 1;
        e.Format = DXGI_FORMAT_R8G8B8A8_UNORM; e.SampleDesc.Count = 1; e.Usage = D3D11_USAGE_DEFAULT;
        uint32_t *zero = (uint32_t *)calloc((size_t)W * H, 4);
        D3D11_SUBRESOURCE_DATA init = { zero, W * 4, 0 };
        if (zero) ID3D11Device_CreateTexture2D(g_dev, &e, &init, &g_hud_empty);
        free(zero);
    }
    /* The VR thread holds it while it copies out: skip this frame then
     * (it keeps showing the previous one). */
    if (IDXGIKeyedMutex_AcquireSync(g_share_mx, 0, 0) != S_OK) { n_share_busy++; return 1; }
    D3D11_BOX box = { 0, 0, 0, W, H, 1 };
    for (int e = 0; e < 2; e++) {
        ID3D11ShaderResourceView *src = e == 0 && g_mono_left ? t->srv : t->eye_srv[e];
        if (g_sharpen > 0 && g_share_rtv && src) {      /* sharpened (d3d11_set_quality) */
            const float c[4] = { (float)(e * W), g_sharpen, (float)W, (float)H };
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_scb, 0, NULL, c, 0, 0);
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_share_rtv, NULL);
            D3D11_VIEWPORT vp = { (float)(e * W), 0, (float)W, (float)H, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
            D3D11_RECT sc = { (LONG)(e * W), 0, (LONG)((e + 1) * W), (LONG)H };
            ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
            ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
            ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
            ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(g_ctx, g_svs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(g_ctx, g_sps, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 3, 1, &g_scb);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 2, 1, &src);
            ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            ID3D11ShaderResourceView *none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 2, 1, &none);
            ID3D11RenderTargetView *no_rt = NULL;
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &no_rt, NULL);
        } else {
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_share, 0, (UINT)e * W, 0, 0,
                                                      (ID3D11Resource *)eye_picture(t, e), 0, &box);
        }
    }
    const int hs = hud_slot(t->eye_frame);
    if (hs >= 0) n_hud_shared++; else n_hud_none++;
    ID3D11Resource *third = hs >= 0 ? (ID3D11Resource *)g_hud_tex[hs] : (ID3D11Resource *)g_hud_empty;
    if (third) ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_share, 0, 2 * W, 0, 0, third, 0, &box);
    IDXGIKeyedMutex_ReleaseSync(g_share_mx, 0);
    ID3D11DeviceContext_Flush(g_ctx);
    g_share_tag = t->eye_tag;
    g_share_fresh = 1;
    n_share++;
    return 1;
}

const uint32_t *d3d11_present_eye(int eye, int *w, int *h) {
    if (!g_eye_fresh || eye < 0 || eye > 1 || !g_eye_px[eye]) return NULL;
    *w = g_eye_w; *h = g_eye_h;
    return g_eye_px[eye];
}

uint32_t d3d11_present_eye_tag(void) { return g_eye_tag; }

/* One GPU pixel (RGBA8) as the window shows it: 0xAARRGGBB, quantized to
 * the framebuffer format through the lookup tables; 8888 needs no table. */
static inline uint32_t present_px(uint32_t c, int f, const uint32_t (*L)[256]) {
    if (f == 3) return 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
    return L[0][c & 0xFF] | L[1][(c >> 8) & 0xFF] | L[2][(c >> 16) & 0xFF];
}

/* While the eyes are drawn the window only needs the 480x272 picture, and
 * the copy for it was the whole S x SY target (at 6x, 2880x2448: ~5 ms a
 * frame to bring back). This pass picks the same pixels the CPU picked (the
 * one nearest each PSP pixel's centre) into a w x h target on the GPU, and
 * only that is copied back. VR off, and frames without eyes, copy as before. */
static const char *PICK_SRC =
"cbuffer K : register(b4) { int4 kp; };\n"     /* x: S, y: SY */
"Texture2D<float4> T : register(t3);\n"
"float4 vsk(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float4 psk(float4 pos : SV_Position) : SV_Target {\n"
"  int2 c = int2(pos.xy);\n"
"  return T.Load(int3(c.x * kp.x + kp.x / 2, c.y * kp.y + kp.y / 2, 0));\n"
"}\n";
static ID3D11VertexShader     *g_kvs;
static ID3D11PixelShader      *g_kps;
static ID3D11Buffer           *g_kcb;
static ID3D11Texture2D        *g_pick_tex;
static ID3D11RenderTargetView *g_pick_rtv;
static UINT                    g_pick_w, g_pick_h;

/* t's picture as w x h PSP pixels into g_pick_tex; 0 if it can't. */
static int pick_small(rt_entry *t, UINT w, UINT h) {
    if (!g_kvs || !g_kps || !g_kcb || !t->srv) return 0;
    if (!g_pick_tex || g_pick_w != w || g_pick_h != h) {
        if (g_pick_rtv) ID3D11RenderTargetView_Release(g_pick_rtv);
        if (g_pick_tex) ID3D11Texture2D_Release(g_pick_tex);
        g_pick_rtv = NULL; g_pick_tex = NULL;
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_pick_tex)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_pick_tex, NULL, &g_pick_rtv))) return 0;
        g_pick_w = w; g_pick_h = h;
    }
    const int32_t k[4] = { g_scale, g_scale_y, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_kcb, 0, NULL, k, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_pick_rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    D3D11_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_kvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_kps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 4, 1, &g_kcb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1, &t->srv);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 3, 1, &none);
    ID3D11RenderTargetView *no_rt = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &no_rt, NULL);
    return 1;
}

static ID3D11Texture2D *g_present_small[PRESENT_RING];   /* w x h staging copies of g_pick_tex */
static int g_present_is_small[PRESENT_RING];
static uint64_t n_present_small;

static int present_body(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h);
int d3d11_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    const uint64_t t0 = qpc_now();
    const int r = present_body(addr, stride, fmt, out, w, h);
    t_present += qpc_now() - t0;
    if (r == 1) n_present_calls++;
    return r;
}

static int present_body(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    g_hires_fresh = 0;
    g_eye_fresh = 0;
    g_share_fresh = 0;
    if (!g_dev || (addr & 0x0F000000u) != 0x04000000u) return 0;
    rt_entry *t = rt_find(vram_off(addr), stride, fmt);
    const int ahead = t && t->dirty && t->stride == stride && t->fmt == fmt && t->w >= (uint32_t)w;
    /* Nothing drawn since the last copy (the game shows a frame for two or
     * more vblanks): no new copy, so no readback and no conversion. With
     * stereo at 3x that was three 1440x816 pictures every vblank, and the
     * game got a few frames a second. */
    static rt_entry *copied_t;
    static uint64_t copied_gen;
    if (ahead && t == copied_t && t->gen == copied_gen) {
        /* still show a copy that finishes now (below) */
    } else if (ahead) {                                /* queue a copy of this frame */
        copied_t = t; copied_gen = t->gen;
        const int k = g_present_head;
        g_present_head = (g_present_head + 1) % PRESENT_RING;
        if (g_present[k] && g_present_w[k] != t->w) {
            ID3D11Texture2D_Release(g_present[k]); g_present[k] = NULL;
            for (int e = 0; e < 2; e++)
                if (g_eye_present[k][e]) { ID3D11Texture2D_Release(g_eye_present[k][e]); g_eye_present[k][e] = NULL; }
        }
        if (!g_present[k]) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(t->tex, &d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_present[k]))) return 0;
            g_present_w[k] = t->w;
        }
        D3D11_BOX box = { 0, 0, 0, (UINT)(w * g_scale), (UINT)(h * g_scale_y), 1 };
        /* Stereo: the eye copies too, when this frame was drawn for the eyes. */
        g_present_eyes[k] = eye_picture(t, 0) && eye_picture(t, 1) && t->eye_frame + 2 >= frame_no();
        g_present_is_small[k] = 0;
        if (g_present_eyes[k] && (g_scale > 1 || g_scale_y > 1) && pick_small(t, (UINT)w, (UINT)h)) {
            if (!g_present_small[k]) {
                D3D11_TEXTURE2D_DESC d;
                ID3D11Texture2D_GetDesc(g_pick_tex, &d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_present_small[k]))) g_present_small[k] = NULL;
            }
            if (g_present_small[k]) {
                ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_present_small[k], (ID3D11Resource *)g_pick_tex);
                g_present_is_small[k] = 1;
                n_present_small++;
            }
        }
        if (!g_present_is_small[k])
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_present[k], 0, 0, 0, 0,
                                                      (ID3D11Resource *)t->tex, 0, &box);
        g_present_stereo[k] = g_present_eyes[k];
        g_present_tag[k] = t->eye_tag;
        if (g_present_eyes[k] && g_share_on && share_eyes(t, (UINT)(w * g_scale), (UINT)(h * g_scale_y)) &&
            !g_eye_readback)
            g_present_eyes[k] = 0;                     /* the headset has them already */
        for (int e = 0; e < 2 && g_present_eyes[k]; e++) {
            if (!g_eye_present[k][e]) {
                D3D11_TEXTURE2D_DESC d;
                ID3D11Texture2D_GetDesc(t->tex, &d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_eye_present[k][e]))) { g_present_eyes[k] = 0; break; }
            }
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_eye_present[k][e], 0, 0, 0, 0,
                                                      (ID3D11Resource *)eye_picture(t, e), 0, &box);
        }
        ID3D11DeviceContext_Flush(g_ctx);              /* start it now, not at the next stall */
        g_present_fmt[k] = fmt;
        g_present_pending[k] = 1;
        g_present_seq[k] = ++g_present_next;
    } else {
        /* VRAM is current: drop queued copies (they are older) and say so. */
        copied_t = NULL;
        for (int k = 0; k < PRESENT_RING; k++) g_present_pending[k] = 0;
        n_present_vram++;
        return 0;
    }
    /* Show the newest finished copy, if any; otherwise keep the last picture. */
    int shown = 2;
    for (int pass = 0; pass < PRESENT_RING; pass++) {
        int best = -1;
        for (int k = 0; k < PRESENT_RING; k++)
            if (g_present_pending[k] == 1 && (best < 0 || g_present_seq[k] > g_present_seq[best])) best = k;
        if (best < 0) break;
        D3D11_MAPPED_SUBRESOURCE m;
        /* PSP2I_PRESENT_VERIFY: wait for the newest copy (for comparing with VRAM). */
        static int verify = -1;
        if (verify < 0) verify = getenv("PSP2I_PRESENT_VERIFY") != NULL;
        ID3D11Texture2D *const src_tex = g_present_is_small[best] ? g_present_small[best] : g_present[best];
        HRESULT hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)src_tex, 0, D3D11_MAP_READ,
                                             verify ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            /* Not done: try the next older one (it was queued earlier). */
            g_present_pending[best] = 2;              /* busy: try the next older one */
            continue;
        }
        if (FAILED(hr)) break;
        /* Per channel: pack16 (what a readback would store) then grab_frame's
         * expansion, which act on each channel separately -- so three lookups. */
        const int f = g_present_fmt[best];
        static uint32_t lut[4][3][256];
        static int lut_ready;
        if (!lut_ready) {
            for (int fm = 0; fm < 4; fm++)
                for (uint32_t c = 0; c < 256; c++) {
                    uint32_t r, g, b;
                    switch (fm) {
                    case 0:  r = (c >> 3) * 255 / 31; g = (c >> 2) * 255 / 63; b = (c >> 3) * 255 / 31; break;
                    case 1:  r = g = b = (c >> 3) * 255 / 31; break;
                    case 2:  r = g = b = (c >> 4) * 17; break;
                    default: r = g = b = c; break;
                    }
                    lut[fm][0][c] = 0xFF000000u | (r << 16);
                    lut[fm][1][c] = g << 8;
                    lut[fm][2][c] = b;
                }
            lut_ready = 1;
        }
        const uint32_t (*L)[256] = lut[f & 3];
        const int S = g_scale, c0 = S / 2, SY = g_scale_y;
        if (g_present_is_small[best]) {                 /* already picked on the GPU */
            for (int y = 0; y < h; y++) {
                const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                uint32_t *dst = out + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) dst[x] = present_px(src[x], f, L);
            }
        } else
        for (int y = 0; y < h; y++) {                   /* PSP pixels: the sample nearest each centre */
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)(y * SY + SY / 2) * m.RowPitch);
            uint32_t *dst = out + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                dst[x] = present_px(src[x * S + c0], f, L);
            }
        }
        /* And the whole S-times picture -- not with stereo, where the eyes
         * are the headset's pictures and the window makes do with 480x272. */
        if (S > 1 && !g_present_stereo[best] && hires_buffer(w * S, h * S)) {
            for (int y = 0; y < h * S; y++) {           /* S x S: the nearest of SY rows */
                const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)(y * SY / S) * m.RowPitch);
                uint32_t *dst = g_hires + (size_t)y * (size_t)(w * S);
                for (int x = 0; x < w * S; x++) dst[x] = present_px(src[x], f, L);
            }
            g_hires_fresh = 1;
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)src_tex, 0);
        if (g_present_eyes[best]) {                       /* copied just after: done or nearly */
            const int W = w * S, H = h * SY;              /* the eyes keep their SY rows */
            int ok = 1;
            for (int e = 0; e < 2 && ok; e++) {
                if (!g_eye_px[e] || g_eye_w != W || g_eye_h != H) {
                    free(g_eye_px[e]);
                    g_eye_px[e] = (uint32_t *)malloc((size_t)W * H * 4);
                }
                D3D11_MAPPED_SUBRESOURCE em;
                if (!g_eye_px[e] || FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_eye_present[best][e], 0, D3D11_MAP_READ, 0, &em))) { ok = 0; break; }
                for (int y = 0; y < H; y++) {
                    const uint32_t *src = (const uint32_t *)((const uint8_t *)em.pData + (size_t)y * em.RowPitch);
                    uint32_t *dst = g_eye_px[e] + (size_t)y * W;
                    for (int x = 0; x < W; x++) dst[x] = present_px(src[x], f, L);
                }
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_eye_present[best][e], 0);
            }
            g_eye_w = W; g_eye_h = H;
            g_eye_fresh = ok;
            g_eye_tag = g_present_tag[best];
            if (ok) n_present_eyes++;
        }
        /* This copy and every older one are done with. */
        const uint64_t s = g_present_seq[best];
        for (int k = 0; k < PRESENT_RING; k++) if (g_present_pending[k] && g_present_seq[k] <= s) g_present_pending[k] = 0;
        shown = 1;
        n_present_async++;
        break;
    }
    for (int k = 0; k < PRESENT_RING; k++) if (g_present_pending[k] == 2) g_present_pending[k] = 1;
    if (shown == 2) n_present_kept++;
    return shown;
}

static const psp_gpu_backend D3D11_BACKEND = { "d3d11", d3d_draw, d3d_sync_vram, d3d_vram_written, 1, d3d_draw_eye,
                                                d3d_draw_hud };

/* ---- setup ------------------------------------------------------------------------------ */

int d3d11_init(void) {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                   &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "d3d11: no hardware device (0x%08lX)\n", (unsigned long)hr); return -1; }

    ID3DBlob *vsb = compile("vsmain", "vs_4_0"), *psb = compile("psmain", "ps_4_0");
    if (!vsb || !psb) return -1;
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(g_dev, il, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    {
        g_tex_mips = g_quality_on && g_aniso > 0;
        if (g_quality_on) {                             /* VR: sharpening can be turned on later too */
            ID3DBlob *sv = compile_src(SHARPEN_SRC, "vss", "vs_4_0"), *spb = compile_src(SHARPEN_SRC, "pss", "ps_4_0");
            if (sv) ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(sv), ID3D10Blob_GetBufferSize(sv), NULL, &g_svs);
            if (spb) ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(spb), ID3D10Blob_GetBufferSize(spb), NULL, &g_sps);
            if (sv) ID3D10Blob_Release(sv);
            if (spb) ID3D10Blob_Release(spb);
            D3D11_BUFFER_DESC sb;
            memset(&sb, 0, sizeof sb);
            sb.ByteWidth = 16; sb.Usage = D3D11_USAGE_DEFAULT; sb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Device_CreateBuffer(g_dev, &sb, NULL, &g_scb);
            if (!g_svs || !g_sps || !g_scb) { fprintf(stderr, "d3d11: no sharpening (shader)\n"); g_sharpen = 0; }
        }
        {
            ID3DBlob *kv = compile_src(PICK_SRC, "vsk", "vs_4_0"), *kpb = compile_src(PICK_SRC, "psk", "ps_4_0");
            if (kv) ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(kv), ID3D10Blob_GetBufferSize(kv), NULL, &g_kvs);
            if (kpb) ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(kpb), ID3D10Blob_GetBufferSize(kpb), NULL, &g_kps);
            if (kv) ID3D10Blob_Release(kv);
            if (kpb) ID3D10Blob_Release(kpb);
            D3D11_BUFFER_DESC kb;
            memset(&kb, 0, sizeof kb);
            kb.ByteWidth = 16; kb.Usage = D3D11_USAGE_DEFAULT; kb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Device_CreateBuffer(g_dev, &kb, NULL, &g_kcb);
        }
        ID3DBlob *xv = compile_src(XFER_SRC, "vsx", "vs_4_0"), *xp = compile_src(XFER_SRC, "psx", "ps_4_0");
        if (xv) ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(xv), ID3D10Blob_GetBufferSize(xv), NULL, &g_xvs);
        if (xp) ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(xp), ID3D10Blob_GetBufferSize(xp), NULL, &g_xps);
        if (xv) ID3D10Blob_Release(xv);
        if (xp) ID3D10Blob_Release(xp);
        D3D11_BUFFER_DESC xb;
        memset(&xb, 0, sizeof xb);
        xb.ByteWidth = sizeof(xfer_consts); xb.Usage = D3D11_USAGE_DEFAULT; xb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(g_dev, &xb, NULL, &g_xcb);
        if (!g_xvs || !g_xps || !g_xcb) { g_xps = NULL; fprintf(stderr, "d3d11: transfer shader unavailable; overlaps go through VRAM\n"); }
    }

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(vs_consts); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vcb);
    bd.ByteWidth = sizeof(ps_consts);
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_pcb);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;  /* culling is done before submission */
    rd.ScissorEnable = TRUE; rd.DepthClipEnable = FALSE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);

    if (!g_vs || !g_ps || !g_layout || !g_vb || !g_vcb || !g_pcb || !g_rs) {
        fprintf(stderr, "d3d11: device object creation failed\n");
        return -1;
    }
    psp_gpu_set_backend(&D3D11_BACKEND);
    psp_mem_set_vram_hook(d3d_cpu_access);
    fprintf(stderr, "d3d11: rendering on the GPU (feature level %x.%x)\n", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
    return 0;
}

void d3d11_report(FILE *out) {
    if (!g_dev) return;
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double ms = n_present_calls ? 1000.0 / (double)f.QuadPart / (double)n_present_calls : 0.0;
        fprintf(out, "  d3d11 time / frame  draws %.2f ms, eye draws %.2f, HUD %.2f, sharing %.2f, present %.2f (%llu frames)\n",
                (double)t_draw * ms, (double)t_eye * ms, (double)t_hud * ms, (double)t_share * ms, (double)t_present * ms,
                (unsigned long long)n_present_calls);
    }
    fprintf(out, "  d3d11               %llu draws, %llu triangles; %llu VRAM uploads, %llu readbacks; "
                 "%llu texture decodes, %llu render-to-texture samples; %llu draws approximated\n",
            (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_uploads,
            (unsigned long long)n_readbacks, (unsigned long long)n_tex_decodes,
            (unsigned long long)n_rt_tex, (unsigned long long)n_approx);
    fprintf(out, "  d3d11 readbacks by cause: relayout %llu, evict %llu, grow %llu, texture %llu, overlap %llu, sync %llu, cpu access %llu\n",
            (unsigned long long)n_rb_reason[0], (unsigned long long)n_rb_reason[1], (unsigned long long)n_rb_reason[2],
            (unsigned long long)n_rb_reason[3], (unsigned long long)n_rb_reason[4], (unsigned long long)n_rb_reason[5],
            (unsigned long long)n_rb_reason[6]);
    fprintf(out, "  d3d11 transfers     %llu GPU-side target-to-target transfers\n", (unsigned long long)n_xfer);
#ifdef PSP2I_TEXREPLACE
    if (texreplace_on())
        fprintf(out, "  d3d11 texture packs %llu PNG uploads, %.0f MB on the GPU\n",
                (unsigned long long)n_rep_uploads, (double)g_rep_bytes / (1024.0 * 1024.0));
#endif
    if (xverify())
        fprintf(out, "  d3d11 xfer-verify   %llu transfers checked, %llu wrong (%llu pixels); %llu ownership overlaps\n",
                (unsigned long long)n_xv_checked, (unsigned long long)n_xv_bad_xfers,
                (unsigned long long)n_xv_bad_px, (unsigned long long)n_xv_overlap);
    if (n_eye_draws)
        fprintf(out, "  d3d11 stereo        %llu HUD panel draws; frames shared with their HUD %llu, without (none drawn) %llu\n",
                (unsigned long long)n_hud_draws, (unsigned long long)n_hud_shared, (unsigned long long)n_hud_none);
        fprintf(out, "  d3d11 stereo        %llu eye draws, %llu frames read back for both eyes, %llu shared on the GPU (%llu skipped: busy)\n",
                (unsigned long long)n_eye_draws, (unsigned long long)n_present_eyes,
                (unsigned long long)n_share, (unsigned long long)n_share_busy);
    if (n_present_small) fprintf(out, "  d3d11 present       %llu of them picked to the PSP's size on the GPU first (stereo)\n",
                                 (unsigned long long)n_present_small);
    fprintf(out, "  d3d11 present       %llu frames from asynchronous copies, %llu from VRAM, %llu vblanks kept the last picture\n",
            (unsigned long long)n_present_async, (unsigned long long)n_present_vram, (unsigned long long)n_present_kept);
}

#endif
