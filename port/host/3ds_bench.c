/* A one-off measurement on the console (sdmc:/psp2i/bench): run before the
 * game starts, results to the log (3DS_PROGRESS.md T4, "the game's thread").
 *
 *  1. The VFPU compare-and-select forms. The generated code computes vmin /
 *     vmax / vsge / vslt with a float compare (vcmpe, vmrs, a conditional
 *     move); the alternative is the same answer from integer keys of the
 *     bits. Both are checked here against the hardware on every pair of
 *     special values (zeros, denormals, infinities, NaNs) -- the FPU's
 *     flush-to-zero bit decides how denormals compare -- and timed.
 *  2. The cost of one call of the game's 08D8BA7C (a bounding-box test, 6.8%
 *     of the game's thread on the field) on synthetic input; the CPU state
 *     and the RAM it uses are put back afterwards.
 *
 * Not free of side effects: on Azahar a start with the switch on ran the
 * title, then stopped presenting frames for good ~90 s later (the same
 * build without it reached the lobby). Measure with it, then remove it.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

void psp_func_08D8BA7C(void);                   /* generated (port/gen) */

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* the integer key: ordered like the floats, -0 and +0 equal; with ftz,
 * denormals are zero too. NaN is told apart by the caller. */
static inline int32_t key(uint32_t u, int ftz) {
    uint32_t m = u & 0x7FFFFFFFu;
    if (ftz && m < 0x00800000u) m = 0;
    return (int32_t)u < 0 ? -(int32_t)m : (int32_t)m;
}
static inline int is_nan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

/* a < b ? a : b, a > b ? a : b, a >= b, a < b -- as the generated code */
static inline uint32_t i_min(uint32_t a, uint32_t b, int ftz) { return !is_nan(a) && !is_nan(b) && key(a, ftz) < key(b, ftz) ? a : b; }
static inline uint32_t i_max(uint32_t a, uint32_t b, int ftz) { return !is_nan(a) && !is_nan(b) && key(a, ftz) > key(b, ftz) ? a : b; }
static inline int i_ge(uint32_t a, uint32_t b, int ftz) { return !is_nan(a) && !is_nan(b) && key(a, ftz) >= key(b, ftz); }
static inline int i_lt(uint32_t a, uint32_t b, int ftz) { return !is_nan(a) && !is_nan(b) && key(a, ftz) < key(b, ftz); }

static __attribute__((noinline)) float f_min(float a, float b) { return a < b ? a : b; }
static __attribute__((noinline)) float f_max(float a, float b) { return a > b ? a : b; }
static __attribute__((noinline)) int f_ge(float a, float b) { return a >= b; }
static __attribute__((noinline)) int f_lt(float a, float b) { return a < b; }

#define N 4096
/* not static, and summed after each loop: an output nobody reads lets the
 * compiler drop the loop (the first run timed 0.0 ns a lane) */
float psp2i_bench_a[N], psp2i_bench_b[N], psp2i_bench_o[N];
uint32_t psp2i_bench_ua[N], psp2i_bench_ub[N], psp2i_bench_uo[N];
#define g_a psp2i_bench_a
#define g_b psp2i_bench_b
#define g_o psp2i_bench_o
#define g_ua psp2i_bench_ua
#define g_ub psp2i_bench_ub
#define g_uo psp2i_bench_uo
static uint32_t sum_o(void) { uint32_t s = 0; for (int i = 0; i < N; i++) s = s * 31u + fbits(g_o[i]); return s; }
static uint32_t sum_uo(void) { uint32_t s = 0; for (int i = 0; i < N; i++) s = s * 31u + g_uo[i]; return s; }

static double us_since(uint64_t t0) { return (double)(svcGetSystemTick() - t0) * 1e6 / SYSCLOCK_ARM11; }

void psp2i_bench(FILE *out) {
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fprintf(out, "bench: fpscr %08lX (flush-to-zero %s, default NaN %s)\n", (unsigned long)fpscr,
            (fpscr >> 24) & 1 ? "on" : "off", (fpscr >> 25) & 1 ? "on" : "off");

    /* 1a: exactness on special values, both key variants */
    static const uint32_t S[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
        0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x3F800001u, 0x7F7FFFFFu, 0xFF7FFFFFu,
        0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FFFFFFFu, 0x00400000u, 0x42280000u };
    const int ns = (int)(sizeof S / sizeof S[0]);
    for (int ftz = 0; ftz <= 1; ftz++) {
        int bad = 0;
        for (int i = 0; i < ns; i++)
            for (int j = 0; j < ns; j++) {
                const float a = bitsf(S[i]), b = bitsf(S[j]);
                bad += fbits(f_min(a, b)) != i_min(S[i], S[j], ftz);
                bad += fbits(f_max(a, b)) != i_max(S[i], S[j], ftz);
                bad += f_ge(a, b) != i_ge(S[i], S[j], ftz);
                bad += f_lt(a, b) != i_lt(S[i], S[j], ftz);
            }
        fprintf(out, "bench: integer keys%s vs the FPU on %d x %d special values: %d of %d results differ\n",
                ftz ? " (denormals as zero)" : "", ns, ns, bad, ns * ns * 4);
    }

    /* 1b: speed, on ordinary values (the game's) */
    uint32_t seed = 12345;
    for (int i = 0; i < N; i++) {
        seed = seed * 1664525u + 1013904223u;
        g_a[i] = (float)((int32_t)(seed >> 8) % 20000) / 100.0f;
        seed = seed * 1664525u + 1013904223u;
        g_b[i] = (float)((int32_t)(seed >> 8) % 20000) / 100.0f;
        g_ua[i] = fbits(g_a[i]); g_ub[i] = fbits(g_b[i]);
    }
    const int ftz = (fpscr >> 24) & 1, reps = 50;
    uint64_t t0 = svcGetSystemTick();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++) { const float a = g_a[i], b = g_b[i]; g_o[i] = a < b ? a : b; }
    const double fm = us_since(t0);
    uint32_t check = sum_o();
    t0 = svcGetSystemTick();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++) g_uo[i] = i_min(g_ua[i], g_ub[i], ftz);
    const double im = us_since(t0);
    check ^= sum_uo();
    t0 = svcGetSystemTick();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++) { const float a = g_a[i], b = g_b[i]; g_o[i] = a >= b ? 1.0f : 0.0f; }
    const double fg = us_since(t0);
    check ^= sum_o();
    t0 = svcGetSystemTick();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++) g_o[i] = i_ge(g_ua[i], g_ub[i], ftz) ? 1.0f : 0.0f;
    const double ig = us_since(t0);
    check ^= sum_o();
    /* the integer form with the values in float registers, as in the
     * generated code: moved to the integer side and back */
    t0 = svcGetSystemTick();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < N; i++) { const float a = g_a[i], b = g_b[i]; g_o[i] = bitsf(i_min(fbits(a), fbits(b), ftz)); }
    const double imf = us_since(t0);
    check ^= sum_o();
    const double ops = (double)N * reps / 1000.0;
    fprintf(out, "bench: ns per lane: min float %.1f, integer %.1f (from float registers %.1f); set-if-ge float %.1f, integer %.1f (check %08lX)\n",
            fm / ops, im / ops, imf / ops, fg / ops, ig / ops, (unsigned long)check);

    /* 2: one call of 08D8BA7C on synthetic input, in RAM near the top of the
     * user partition, all put back */
    static psp_cpu_state saved;
    saved = psp_cpu;
    const uint32_t at = 0x09F00000u;
    uint8_t keep[128];
    psp_mem_read_block(keep, at, sizeof keep);
    for (int i = 0; i < 16; i++) psp_write_f32(at + 4u * (uint32_t)i, (i % 5) == 0 ? 1.0f : 0.1f * (float)i);  /* a0: matrix */
    for (int i = 0; i < 4; i++) psp_write_f32(at + 64u + 4u * (uint32_t)i, 2.0f + (float)i);                /* a1: scale */
    for (int i = 0; i < 3; i++) psp_write32(at + 80u + 4u * (uint32_t)i, 0x00100010u * (uint32_t)(i + 1));     /* a2: packed */
    for (int i = 112; i < 128; i++) psp_cpu.v[i] = (float)(i - 120) * 0.5f;                                   /* the planes */
    psp_cpu.r[4] = at; psp_cpu.r[5] = at + 64u; psp_cpu.r[6] = at + 80u;
    psp_cpu.r[31] = 0x08000000u;
    const int calls = 20000;
    uint32_t sum = 0;
    t0 = svcGetSystemTick();
    for (int k = 0; k < calls; k++) { psp_cpu.r[4] = at; psp_cpu.r[5] = at + 64u; psp_cpu.r[6] = at + 80u; psp_func_08D8BA7C(); sum += psp_cpu.r[2]; }
    const double bt = us_since(t0);
    fprintf(out, "bench: 08D8BA7C %.2f us a call (%d calls, result %08lX)\n", bt / calls, calls, (unsigned long)sum);
    psp_mem_write_block(at, keep, sizeof keep);
    psp_cpu = saved;
    fflush(out);
}
