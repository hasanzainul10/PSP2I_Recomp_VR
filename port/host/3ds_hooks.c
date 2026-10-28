/* The 3DS's quality hooks: game decisions made to draw less, for speed
 * (3DS_PROGRESS.md T4, "drawing less"). Only what this console draws
 * changes; nothing other players see (CLAUDE.md). Each is off unless
 * switched on.
 *
 * Draw distance: SetNearFar (0x08D7D8E0, from ApplyCamera with the camera's
 * near / far at +244 / +248) sets the far distance the game's frustum tests
 * cull against (0x08FA9818; ADDRESS_MAP.md "The frustum cull"). Scaling it
 * makes the game drop what lies beyond, before it builds any of it.
 * sdmc:/psp2i/draw_distance holds the percentage (e.g. 50); without it, 100.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"

#define BASE "sdmc:/psp2i"
#define FN_SET_NEAR_FAR 0x08D7D8E0u

static int g_far_pct = 100;
static uint64_t n_near_far;

/* draw_distance "cycle" (a dev measurement): 100, 20, 10, 5, 3, 2% in turn,
 * 6 s each, every change logged, so one standing view gives each one's
 * draws a frame (the 1 s report lines between the changes) */
static int g_far_cycle;
static void far_cycle_step(void) {
    static const int STEPS[6] = { 100, 20, 10, 5, 3, 2 };
    const int k = (int)((svcGetSystemTick() / (SYSCLOCK_ARM11 * 6ull)) % 6u);
    if (g_far_pct != STEPS[k]) {
        g_far_pct = STEPS[k];
        fprintf(stderr, "quality: draw distance now %d%%\n", g_far_pct);
    }
}

static void hook_set_near_far(void (*original)(void)) {
    if (g_far_cycle) far_cycle_step();
    const float far = psp_cpu.f[13];
    if (g_far_pct != 100) psp_cpu.f[13] = far * (float)g_far_pct / 100.0f;
    /* each (caller, far) the game asks for, the first time it does */
    static struct { uint32_t ra; float far; } seen[16];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i].ra == psp_cpu.r[31] && seen[i].far == far;
    if (!known && nseen < 16) {
        seen[nseen].ra = psp_cpu.r[31]; seen[nseen].far = far; nseen++;
        fprintf(stderr, "quality: SetNearFar near %.1f far %.1f -> %.1f (from 0x%08lX, call %llu)\n", (double)psp_cpu.f[12],
                (double)far, (double)psp_cpu.f[13], (unsigned long)psp_cpu.r[31], (unsigned long long)n_near_far);
    }
    n_near_far++;
    original();
}

int psp2i_far_pct(void) { return g_far_pct; }

/* sdmc:/psp2i/fx_trace (dev): who draws sprite arrays. 0x08D7A74C /
 * 0x08D7A80C wrap 0x08D7A4CC, which picks the vertex format by the mode at
 * 0x08EE229C (the effects' 0x19F is one); each call's caller (ra) and mode
 * counted, the top ones logged every 10 s with their vertices -- to find the
 * effect system among the game's many callers (3DS_PROGRESS.md T4). */
#define FN_SPRITES_A 0x08D7A74Cu
#define FN_SPRITES_B 0x08D7A80Cu
/* and sceGuDrawArray (0x08D94330, vtype in $a1): its draws per (caller,
 * vtype), to see whether the effects' 0x19F come through it at all (a
 * draw the game writes into the list itself wouldn't) */
#define FN_DRAW_ARRAY 0x08D94330u
/* The effect quads (0x19F, 4 vertices) mostly come from 0x08D79CE4, the
 * quad-array twin of 0x08D7A4CC (mode at 0x08EE2284, 2 = 0x19F), through
 * the wrappers 0x08D79F8C / 0x08D7A080: their callers counted too */
#define FN_QUADS_A 0x08D79F8Cu
#define FN_QUADS_B 0x08D7A080u
typedef struct { uint32_t ra, mode; uint64_t calls, verts; } fx_slot;
static fx_slot g_fx[64], g_da[64], g_qd[64];
static uint64_t g_fx_tick, g_da_tick, g_qd_tick;
static void fx_count(fx_slot *t, uint64_t *tick, const char *what, uint32_t ra, uint32_t mode, uint32_t n) {
    for (int i = 0; i < 64; i++)
        if ((t[i].ra == ra && t[i].mode == mode) || !t[i].calls) {
            t[i].ra = ra; t[i].mode = mode; t[i].calls++; t[i].verts += n;
            break;
        }
    const uint64_t now = svcGetSystemTick();
    if (!*tick) *tick = now;
    if (now - *tick < SYSCLOCK_ARM11 * 10ull) return;
    *tick = now;
    char line[640];
    int at = snprintf(line, sizeof line, "quality: %s in 10 s (caller/mode: calls, vertices):", what);
    for (int k = 0; k < 12 && at < (int)sizeof line - 48; k++) {
        int b = -1;
        for (int i = 0; i < 64; i++) if (t[i].calls && (b < 0 || t[i].calls > t[b].calls)) b = i;
        if (b < 0) break;
        at += snprintf(line + at, sizeof line - (size_t)at, " %08lX/%lX: %llu, %llu", (unsigned long)t[b].ra,
                       (unsigned long)t[b].mode, (unsigned long long)t[b].calls, (unsigned long long)t[b].verts);
        t[b].calls = 0;
    }
    memset(t, 0, 64 * sizeof *t);
    fprintf(stderr, "%s\n", line);
}
/* sdmc:/psp2i/skip_sprites (dev): which of the two busiest sprite callers
 * is what on screen. Bit 1 skips 0x08967FB8 (an object's 18x18 grid of
 * quads), bit 2 the draw method 0x08A07C30 (no result): the user saw it
 * draw the player and enemy markers on the minimap. */
#define FN_GRID_RENDER 0x08967FB8u
#define FN_DRAW_METHOD 0x08A07C30u
static int g_skip_sprites;
/* "<bits> cycle": the skip on and off in turn, 6 s each, every change
 * logged, so one fight gives both sides' draws a frame (runs aren't
 * comparable: the route's timing varies) */
static int g_skip_bits, g_skip_cycle;
static void skip_cycle_step(void) {
    const int v = (svcGetSystemTick() / (SYSCLOCK_ARM11 * 6ull)) % 2u ? g_skip_bits : 0;
    if (g_skip_sprites != v) {
        g_skip_sprites = v;
        fprintf(stderr, "quality: skip_sprites now %d\n", g_skip_sprites);
    }
}
static void hook_grid_render(void (*original)(void)) {
    if (g_skip_cycle) skip_cycle_step();
    /* 1, as its own end does (0 is its "nothing to draw" exit, and with that
     * the game stopped presenting frames after the title on Azahar) */
    if (g_skip_sprites & 1) { psp_cpu.r[2] = 1; return; }
    original();
}
static void hook_draw_method(void (*original)(void)) {
    if (g_skip_cycle) skip_cycle_step();
    if (g_skip_sprites & 2) return;
    original();
}

static void hook_sprites(void (*original)(void)) {
    fx_count(g_fx, &g_fx_tick, "sprite arrays", psp_cpu.r[31], psp_read32(0x08EE229Cu), psp_cpu.r[6]);
    original();
}
static void hook_draw_array(void (*original)(void)) {
    fx_count(g_da, &g_da_tick, "sceGuDrawArray", psp_cpu.r[31], psp_cpu.r[5], psp_cpu.r[6]);
    original();
}
/* their two busiest callers (Azahar field fight), both reached only
 * through pointers: 0x08D3ED40 (a wrapper, everywhere) and 0x089BBC30 (the
 * field only); who calls them counted, 0x089BBC30 skippable (bit 4) */
#define FN_QUADS_WRAP 0x08D3ED40u
#define FN_FIELD_QUADS 0x089BBC30u
static fx_slot g_qc[64];
static uint64_t g_qc_tick;
static void hook_quads_wrap(void (*original)(void)) {
    fx_count(g_qc, &g_qc_tick, "quad callers (08D3ED40 / 089BBC30)", psp_cpu.r[31], 0x40, 0);
    original();
}
static void hook_field_quads(void (*original)(void)) {
    if (g_skip_cycle) skip_cycle_step();
    fx_count(g_qc, &g_qc_tick, "quad callers (08D3ED40 / 089BBC30)", psp_cpu.r[31], 0x30, 0);
    if (g_skip_sprites & 4) return;
    original();
}
static void hook_quads(void (*original)(void)) {
    fx_count(g_qd, &g_qd_tick, "quad arrays", psp_cpu.r[31], psp_read32(0x08EE2284u), psp_cpu.r[6]);
    original();
}

void psp2i_hooks_init(void) {
    FILE *f = fopen(BASE "/draw_distance", "r");
    if (f) {
        char w[16] = { 0 };
        if (fscanf(f, "%15s", w) == 1) {
            const int v = atoi(w);
            if (!strcmp(w, "cycle")) g_far_cycle = 1;
            else if (v >= 1 && v <= 100) g_far_pct = v;
        }
        fclose(f);
    }
    psp_hook_set(FN_SET_NEAR_FAR, hook_set_near_far);
    if ((f = fopen(BASE "/skip_sprites", "r")) != NULL) {
        char w[16] = { 0 };
        if (fscanf(f, "%d %15s", &g_skip_sprites, w) < 1) g_skip_sprites = 0;
        if (!strcmp(w, "cycle")) { g_skip_bits = g_skip_sprites; g_skip_cycle = 1; }
        fclose(f);
        psp_hook_set(FN_GRID_RENDER, hook_grid_render);
        psp_hook_set(FN_DRAW_METHOD, hook_draw_method);
        psp_hook_set(FN_FIELD_QUADS, hook_field_quads);
        fprintf(stderr, "quality: skip_sprites %d\n", g_skip_sprites);
    }
    if ((f = fopen(BASE "/fx_trace", "r")) != NULL) {
        fclose(f);
        psp_hook_set(FN_SPRITES_A, hook_sprites);
        psp_hook_set(FN_SPRITES_B, hook_sprites);
        psp_hook_set(FN_DRAW_ARRAY, hook_draw_array);
        psp_hook_set(FN_QUADS_A, hook_quads);
        psp_hook_set(FN_QUADS_B, hook_quads);
        psp_hook_set(FN_QUADS_WRAP, hook_quads_wrap);
        psp_hook_set(FN_FIELD_QUADS, hook_field_quads);   /* counted, skipped or not */
        fprintf(stderr, "quality: sprite-array callers counted (fx_trace)\n");
    }
    fprintf(stderr, "quality: draw distance %d%%\n", g_far_pct);
}
