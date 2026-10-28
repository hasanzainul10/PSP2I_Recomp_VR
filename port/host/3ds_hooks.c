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
static struct { uint32_t ra, mode; uint64_t calls, verts; } g_fx[64];
static uint64_t g_fx_tick;
static void fx_count(uint32_t ra, uint32_t mode, uint32_t n) {
    for (int i = 0; i < 64; i++)
        if ((g_fx[i].ra == ra && g_fx[i].mode == mode) || !g_fx[i].calls) {
            g_fx[i].ra = ra; g_fx[i].mode = mode; g_fx[i].calls++; g_fx[i].verts += n;
            break;
        }
    const uint64_t now = svcGetSystemTick();
    if (!g_fx_tick) g_fx_tick = now;
    if (now - g_fx_tick < SYSCLOCK_ARM11 * 10ull) return;
    g_fx_tick = now;
    char line[512];
    int at = snprintf(line, sizeof line, "quality: sprite arrays in 10 s (caller/mode: calls, vertices):");
    for (int k = 0; k < 10 && at < (int)sizeof line - 48; k++) {
        int b = -1;
        for (int i = 0; i < 64; i++) if (g_fx[i].calls && (b < 0 || g_fx[i].calls > g_fx[b].calls)) b = i;
        if (b < 0) break;
        at += snprintf(line + at, sizeof line - (size_t)at, " %08lX/%lu: %llu, %llu", (unsigned long)g_fx[b].ra,
                       (unsigned long)g_fx[b].mode, (unsigned long long)g_fx[b].calls, (unsigned long long)g_fx[b].verts);
        g_fx[b].calls = 0;
    }
    memset(g_fx, 0, sizeof g_fx);
    fprintf(stderr, "%s\n", line);
}
/* sdmc:/psp2i/skip_sprites (dev): which of the two busiest sprite callers
 * is what on screen. Bit 1 skips 0x08967FB8 (an object's 18x18 grid of
 * quads), bit 2 the draw method 0x08A07C30 (no result): the user saw it
 * draw the player and enemy markers on the minimap. */
#define FN_GRID_RENDER 0x08967FB8u
#define FN_DRAW_METHOD 0x08A07C30u
static int g_skip_sprites;
static void hook_grid_render(void (*original)(void)) {
    /* 1, as its own end does (0 is its "nothing to draw" exit, and with that
     * the game stopped presenting frames after the title on Azahar) */
    if (g_skip_sprites & 1) { psp_cpu.r[2] = 1; return; }
    original();
}
static void hook_draw_method(void (*original)(void)) {
    if (g_skip_sprites & 2) return;
    original();
}

static void hook_sprites(void (*original)(void)) {
    fx_count(psp_cpu.r[31], psp_read32(0x08EE229Cu), psp_cpu.r[6]);
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
        if (fscanf(f, "%d", &g_skip_sprites) != 1) g_skip_sprites = 0;
        fclose(f);
        psp_hook_set(FN_GRID_RENDER, hook_grid_render);
        psp_hook_set(FN_DRAW_METHOD, hook_draw_method);
        fprintf(stderr, "quality: skip_sprites %d\n", g_skip_sprites);
    }
    if ((f = fopen(BASE "/fx_trace", "r")) != NULL) {
        fclose(f);
        psp_hook_set(FN_SPRITES_A, hook_sprites);
        psp_hook_set(FN_SPRITES_B, hook_sprites);
        fprintf(stderr, "quality: sprite-array callers counted (fx_trace)\n");
    }
    fprintf(stderr, "quality: draw distance %d%%\n", g_far_pct);
}
