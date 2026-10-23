/* The local player's skeleton: see vr_body.h. Runs on the game thread. */
#include "vr_body.h"
#include "../host/gamelog.h"

#include <math.h>
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

/* The character's other parts (the hands and lower legs have no palette
 * slot in the body): each model drawn this frame that sits next to the
 * player's in its container (*(player+off0)), shares its node matrices, or
 * is placed where it is (model+32's translation), with what it uses. */
static void log_parts(uint32_t pl, int me) {
    const uint32_t box = g_link_off[1] != ~0u ? psp_read32(pl + g_link_off[0]) : 0;
    const uint32_t my_nodes = psp_read32(g_models[me] + 100u);
    float my_t[3];
    for (int k = 0; k < 3; k++) my_t[k] = psp_read_f32(g_models[me] + 32u + 48u + 4u * (uint32_t)k);
    for (int i = 0; i < g_nmodels; i++) {
        if (i == me) continue;
        const uint32_t m = g_models[i], nodes = psp_read32(m + 100u);
        int slot = -1;
        for (uint32_t q = 0; box && q < 0x200u; q += 4) if (psp_read32(box + q) == m) { slot = (int)q; break; }
        float d = 0;
        for (int k = 0; k < 3; k++) { const float e = psp_read_f32(m + 32u + 48u + 4u * (uint32_t)k) - my_t[k]; d += e * e; }
        d = sqrtf(d);
        if (slot < 0 && nodes != my_nodes && d > 0.5f) continue;
        fprintf(stderr, "vr: skeleton:   part 0x%08X (container +0x%X; FFFFFFFF = not in it): %d nodes (skeleton 0x%08X),"
                        " node matrices 0x%08X (%s), %.2f from the body\n",
                m, (unsigned)slot, (int)psp_read32(g_skels[i] + 40u), g_skels[i], nodes,
                nodes == my_nodes ? "the body's" : "its own", (double)d);
    }
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
            log_parts(pl, i);
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

#ifndef PSP2I_PROD
/* Dev: PSP2I_SKEL_TEST=NODE,DEGREES[,keep] turns that node and everything
 * below it (from the skeleton records' parents) about model +z at the
 * node, swinging 0..DEGREES and back every 4 s, on the local player only.
 * The nodes are changed for the palette and put back after it, unless
 * "keep" (then the rest of the frame, e.g. separate hand models, sees the
 * change too, until the next animation step rewrites them). */
static int   g_test_node = -1, g_test_keep;
static float g_test_deg;
static float g_saved[NODES_MAX][16];

static int test_pose(uint32_t model, uint32_t skel) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u), recs = psp_read32(skel + 48u);
    if (n <= g_test_node || n > NODES_MAX || !user_ptr(nodes) || !user_ptr(recs)) return 0;
    const float a = g_test_deg * 0.0174533f * (0.5f - 0.5f * cosf((float)psp_sched_vblank_count() * (6.2831853f / 240.0f)));
    const float c = cosf(a), sn = sinf(a);
    float piv[3];
    for (int k = 0; k < 3; k++) piv[k] = psp_read_f32(nodes + 64u * (uint32_t)g_test_node + 48u + 4u * (uint32_t)k);
    for (int b = 0; b < n; b++) {
        int x = b;                                         /* is b at or below the test node? */
        for (int guard = 0; x >= 0 && x != g_test_node && guard < n; guard++)
            x = (int16_t)psp_read16(recs + 144u * (uint32_t)x + 6u);
        if (x != g_test_node) continue;
        const uint32_t m = nodes + 64u * (uint32_t)b;
        float f[16];
        for (uint32_t i = 0; i < 16; i++) f[i] = g_saved[b][i] = psp_read_f32(m + 4u * i);
        for (int col = 0; col < 4; col++) {                 /* rotate about z; the translation about the pivot */
            float *v = &f[col * 4];
            const float px = col == 3 ? piv[0] : 0.0f, py = col == 3 ? piv[1] : 0.0f;
            const float vx = v[0] - px, vy = v[1] - py;
            v[0] = px + c * vx - sn * vy;
            v[1] = py + sn * vx + c * vy;
        }
        for (uint32_t i = 0; i < 16; i++) psp_write_f32(m + 4u * i, f[i]);
    }
    return 1;
}

static void test_restore(uint32_t model, uint32_t skel) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u), recs = psp_read32(skel + 48u);
    for (int b = 0; b < n; b++) {
        int x = b;
        for (int guard = 0; x >= 0 && x != g_test_node && guard < n; guard++)
            x = (int16_t)psp_read16(recs + 144u * (uint32_t)x + 6u);
        if (x != g_test_node) continue;
        for (uint32_t i = 0; i < 16; i++) psp_write_f32(nodes + 64u * (uint32_t)b + 4u * i, g_saved[b][i]);
    }
}
#endif

static void hook_palette(void (*original)(void)) {
#ifndef PSP2I_PROD
    const uint32_t model = psp_cpu.r[4], skel = psp_cpu.r[6];
    if (g_skel_map) skel_look(model, skel);
    if (g_test_node >= 0 && model && model == g_pl_model) {
        const int changed = test_pose(model, skel);
        original();
        if (changed && !g_test_keep) test_restore(model, skel);
        return;
    }
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
    const char *t = getenv("PSP2I_SKEL_TEST");
    if (t && sscanf(t, "%d,%f", &g_test_node, &g_test_deg) == 2 && g_test_node >= 0 && g_test_node < NODES_MAX) {
        g_test_keep = strstr(t, "keep") != NULL;
        g_skel_map = 1;                                     /* the player's model is found by the map */
        fprintf(stderr, "vr: skeleton test: node %d swings 0..%.0f degrees about model +z every 4 s (%s)\n",
                g_test_node, (double)g_test_deg, g_test_keep ? "kept for the frame" : "for the palette only");
    } else g_test_node = -1;
#endif
    if (g_skel_map) psp_hook_set(FN_PALETTE, hook_palette);
}
