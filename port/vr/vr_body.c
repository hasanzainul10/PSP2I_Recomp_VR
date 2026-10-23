/* The local player's skeleton: see vr_body.h. Runs on the game thread. */
#include "vr_body.h"
#include "../host/gamelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

/* 0x08A846DC(model, &palette, skeleton) -> 0x08D74E54(palette, skeleton,
 * *(model+100), model+32): for each of the skeleton's +40 nodes (144-byte
 * records from +48: flags +0, palette slot +4 as a short, -1 = none, the
 * inverse bind matrix at +48 unless flag 0x8), palette[slot] = (model+32) x
 * node. Node matrices are 4x4, 64 bytes each, VFPU columns (disassembly and
 * a write watch on a palette entry, 2026-10-23). */
#define FN_PALETTE    0x08A846DCu
#define USER_LO       0x08800000u
#define USER_HI       0x0A000000u
#define NODES_MAX     128

static int g_skel_map;

#ifndef PSP2I_PROD
#define MODELS_MAX 64
static FILE    *g_skel_f;
static uint64_t g_vb = ~0ull, g_last_dump, g_last_log;
static uint32_t g_models[MODELS_MAX], g_skels[MODELS_MAX];
static int      g_nmodels;
static uint32_t g_pl_model;                /* the local player's model, once found */
static uint32_t g_link_off[2];             /* how: pl + off0 (-> + off1) holds it; off1 = ~0 if direct */
static int      g_recs_done;

static int user_ptr(uint32_t a) { return a >= USER_LO && a < USER_HI && !(a & 3u); }

/* The player object (or an object one pointer below it) holds the address
 * of its model. Searched for each model seen this frame. */
static int find_link(uint32_t pl, uint32_t model, uint32_t *o0, uint32_t *o1) {
    for (uint32_t o = 0; o < 0x1000u; o += 4)
        if (psp_read32(pl + o) == model) { *o0 = o; *o1 = ~0u; return 1; }
    for (uint32_t o = 0; o < 0x1000u; o += 4) {
        const uint32_t p = psp_read32(pl + o);
        if (!user_ptr(p) || p == pl) continue;
        for (uint32_t q = 0; q < 0x200u; q += 4)
            if (psp_read32(p + q) == model) { *o0 = o; *o1 = q; return 1; }
    }
    return 0;
}

static void dump_model(uint64_t vb, uint32_t model, uint32_t skel) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u);
    if (n <= 0 || n > NODES_MAX || !user_ptr(nodes)) return;
    if (!g_recs_done) {
        g_recs_done = 1;
        fprintf(g_skel_f, "skeleton 0x%08X n %d records 0x%08X model 0x%08X callback 0x%08X flags 0x%08X\n", skel, n,
                psp_read32(skel + 48u), model, psp_read32(model + 112u), psp_read32(model + 8u));
        for (int i = 0; i < n; i++) {
            const uint32_t r = psp_read32(skel + 48u) + 144u * (uint32_t)i;
            fprintf(g_skel_f, "rec %d", i);
            for (uint32_t w = 0; w < 36; w++) fprintf(g_skel_f, " %08X", psp_read32(r + 4u * w));
            fputc('\n', g_skel_f);
        }
    }
    fprintf(g_skel_f, "snap %llu obj 0x%08X mats 0x%08X n %d\n", (unsigned long long)vb, model, nodes, n);
    for (int b = 0; b < n; b++) {
        fprintf(g_skel_f, "b %d", b);
        for (uint32_t i = 0; i < 16; i++) fprintf(g_skel_f, " %.6g", (double)psp_read_f32(nodes + 64u * (uint32_t)b + 4u * i));
        fputc('\n', g_skel_f);
    }
    fprintf(g_skel_f, "m");                    /* model -> eye, for reference */
    for (uint32_t i = 0; i < 16; i++) fprintf(g_skel_f, " %.6g", (double)psp_read_f32(model + 32u + 4u * i));
    fputc('\n', g_skel_f);
    fflush(g_skel_f);
}

/* Once a frame (at its first palette): who was seen last frame. */
static void new_frame(uint64_t vb) {
    const uint32_t pl = gamelog_local_player();
    if (vb - g_last_log >= 150 && pl) {
        g_last_log = vb;
        int found = 0;
        for (int i = 0; i < g_nmodels && !found; i++) {
            uint32_t o0, o1;
            if (!find_link(pl, g_models[i], &o0, &o1)) continue;
            found = 1;
            if (g_pl_model != g_models[i] || g_link_off[0] != o0 || g_link_off[1] != o1) {
                if (o1 == ~0u) fprintf(stderr, "vr: skeleton: the player's model 0x%08X is at player +0x%X\n", g_models[i], o0);
                else fprintf(stderr, "vr: skeleton: the player's model 0x%08X is at *(player +0x%X) +0x%X\n", g_models[i], o0, o1);
                g_recs_done = 0;
            }
            g_pl_model = g_models[i]; g_link_off[0] = o0; g_link_off[1] = o1;
            fprintf(stderr, "vr: skeleton: %d models this frame; the player's has %d nodes (skeleton 0x%08X, node matrices 0x%08X)\n",
                    g_nmodels, (int)psp_read32(g_skels[i] + 40u), g_skels[i], psp_read32(g_models[i] + 100u));
        }
        if (!found) {
            g_pl_model = 0;
            fprintf(stderr, "vr: skeleton: %d models this frame, none linked from the player 0x%08X\n", g_nmodels, pl);
        }
    }
    g_nmodels = 0;
}

static void skel_look(uint32_t model, uint32_t skel) {
    const uint64_t vb = psp_sched_vblank_count();
    if (vb != g_vb) { new_frame(vb); g_vb = vb; }
    if (!user_ptr(skel) || !user_ptr(model)) return;
    if (g_nmodels < MODELS_MAX) { g_skels[g_nmodels] = skel; g_models[g_nmodels++] = model; }
    if (model == g_pl_model && g_skel_f && vb - g_last_dump >= 30) {
        g_last_dump = vb;
        dump_model(vb, model, skel);
    }
}
#endif

static void hook_palette(void (*original)(void)) {
#ifndef PSP2I_PROD
    if (g_skel_map) skel_look(psp_cpu.r[4], psp_cpu.r[6]);
#endif
    original();
}

void vr_body_init(void) {
#ifndef PSP2I_PROD
    const char *p = getenv("PSP2I_SKEL");
    if (p && *p) {
        g_skel_f = fopen(p, "w");
        g_skel_map = 1;
        fprintf(stderr, "vr: skeleton map: %s%s\n", p, g_skel_f ? "" : " (can't open it; logging only)");
    }
#endif
    if (g_skel_map) psp_hook_set(FN_PALETTE, hook_palette);
}
