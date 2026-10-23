/* The local player's skeleton: see vr_body.h. Runs on the game thread. */
#include "vr_body.h"
#include "../host/gamelog.h"
#include "vr.h"

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

static int user_ptr(uint32_t a) { return a >= USER_LO && a < USER_HI && !(a & 3u); }

#ifndef PSP2I_PROD
#define MODELS_MAX 64
static FILE    *g_skel_f;
static uint64_t g_vb = ~0ull, g_last_dump, g_last_log;
static uint32_t g_models[MODELS_MAX], g_skels[MODELS_MAX];
static int      g_nmodels;
static uint32_t g_pl_model;                /* the local player's model, once found */
static uint32_t g_link_off[2];             /* how: pl + off0 (-> + off1) holds it; off1 = ~0 if direct */
static int      g_recs_done;

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

/* ---- Arms: 2-bone IK to the VR controllers (VR_PLAN M2) ---------------
 *
 * With INPUT MODE = MOTION and vr_arms on, each palette of the local
 * player's model is built from a pose whose arms reach for the
 * controllers: the shoulder (15 left, 20 right) turns so that the elbow
 * (16 / 21) lands where a 2-bone solve puts it, then the elbow turns so
 * that the wrist (17 / 22) reaches the target; everything below follows.
 * The nodes are put back after each palette, so only what this game draws
 * changes (the parts' three skeletons all read them: hands and weapon
 * follow, headset-checked with the shoulder test).
 *
 * The target is the controller's offset from the headset, in the
 * headset's heading (right, up, forward), from the model's eyes (the head
 * node, a little up and ahead), in model units: the model's own arm length
 * over a human's (shoulder to wrist ~0.55 m). Model space: +x the
 * character's left, +y up, +z forward (headset-checked). So it works the
 * same in first and third person, with no camera matrix. */
#define PLAYER_MODEL_BOX 0x178u               /* *(player+0x178)+0x8: the player's animated model */
#define ARM_HUMAN_M      0.55f
static const int ARM_NODE[2][3] = { { 15, 16, 17 }, { 20, 21, 22 } };   /* shoulder, elbow, wrist; left, right */

static int      g_arms_on;                     /* vr_body_arms */
static uint64_t g_arms_vb = ~0ull;
static int      g_arms_ok[2];                  /* this frame's target per hand */
static float    g_arms_off[2][3];              /* the hand from the eyes, metres: right, up, forward */
static float    g_arms_saved[NODES_MAX][16];
static int      g_arms_n_saved;
#ifndef PSP2I_PROD
static uint64_t g_arms_log;
#endif

void vr_body_arms(int on) {
    if (on != g_arms_on) fprintf(stderr, "vr: arms %s\n", on ? "on (they follow the controllers)" : "off");
    g_arms_on = on;
}

/* Once a frame: the controllers from the headset, in its heading. */
static void arms_frame(void) {
    const uint64_t vb = psp_sched_vblank_count();
    if (vb == g_arms_vb) return;
    g_arms_vb = vb;
    g_arms_ok[0] = g_arms_ok[1] = 0;
    vr_head h;
    vr_hand vh[2];
    if (!vr_head_get(&h) || !vr_hands_get(vh)) return;
    const float *q = h.q;                                     /* forward = -z of the head, level */
    float f[3] = { -2.0f * (q[0] * q[2] + q[3] * q[1]), 0.0f, -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) };
    const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (fl < 1e-3f) return;
    f[0] /= fl; f[2] /= fl;
    const float r[3] = { -f[2], 0.0f, f[0] };
    for (int hd = 0; hd < 2; hd++) {
        if (!vh[hd].ok) continue;
        float d[3];
        for (int k = 0; k < 3; k++) d[k] = vh[hd].p[k] - h.p[k];
        g_arms_off[hd][0] = d[0] * r[0] + d[2] * r[2];
        g_arms_off[hd][1] = d[1];
        g_arms_off[hd][2] = d[0] * f[0] + d[2] * f[2];
        g_arms_ok[hd] = 1;
    }
}

static void node_get(uint32_t nodes, int b, float *m) { for (uint32_t i = 0; i < 16; i++) m[i] = psp_read_f32(nodes + 64u * (uint32_t)b + 4u * i); }
static void node_put(uint32_t nodes, int b, const float *m) { for (uint32_t i = 0; i < 16; i++) psp_write_f32(nodes + 64u * (uint32_t)b + 4u * i, m[i]); }
static float len3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* The rotation taking direction a to direction b (both unit), 3x3 row-major. */
static void rot_between(const float *a, const float *b, float *R) {
    float ax[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    const float s = len3(ax), c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (s < 1e-6f) {                                          /* parallel: none (opposite is not met: the reach is clamped) */
        static const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        memcpy(R, I, sizeof I);
        return;
    }
    for (int k = 0; k < 3; k++) ax[k] /= s;
    const float t = 1.0f - c, x = ax[0], y = ax[1], z = ax[2];
    R[0] = c + x * x * t;     R[1] = x * y * t - z * s; R[2] = x * z * t + y * s;
    R[3] = y * x * t + z * s; R[4] = c + y * y * t;     R[5] = y * z * t - x * s;
    R[6] = z * x * t - y * s; R[7] = z * y * t + x * s; R[8] = c + z * z * t;
}

/* Turns node `top` and every node below it (the records' parents) by R
 * about the point P, in place. */
static void turn_below(uint32_t nodes, uint32_t recs, int n, int top, const float *R, const float *P) {
    for (int b = 0; b < n; b++) {
        int x = b;
        for (int guard = 0; x >= 0 && x != top && guard < n; guard++) x = (int16_t)psp_read16(recs + 144u * (uint32_t)x + 6u);
        if (x != top) continue;
        float m[16];
        node_get(nodes, b, m);
        for (int col = 0; col < 4; col++) {
            float v[3] = { m[col * 4], m[col * 4 + 1], m[col * 4 + 2] };
            if (col == 3) for (int k = 0; k < 3; k++) v[k] -= P[k];
            for (int k = 0; k < 3; k++) {
                m[col * 4 + k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2];
                if (col == 3) m[col * 4 + k] += P[k];
            }
        }
        node_put(nodes, b, m);
    }
}

/* Poses both arms; returns 1 with the nodes saved for arms_restore. */
static int arms_pose(uint32_t model, uint32_t skel) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u), recs = psp_read32(skel + 48u);
    if (n < 23 || n > NODES_MAX || !user_ptr(nodes) || !user_ptr(recs)) return 0;
    for (int b = 0; b < n; b++) node_get(nodes, b, g_arms_saved[b]);
    g_arms_n_saved = n;
    float head[16];
    node_get(nodes, 13, head);
    for (int hd = 0; hd < 2; hd++) {
        if (!g_arms_ok[hd]) continue;
        const int *J = ARM_NODE[hd];
        float S[3], E[3], W[3], m[16];
        node_get(nodes, J[0], m); memcpy(S, &m[12], sizeof S);
        node_get(nodes, J[1], m); memcpy(E, &m[12], sizeof E);
        node_get(nodes, J[2], m); memcpy(W, &m[12], sizeof W);
        float se[3], ew[3];
        for (int k = 0; k < 3; k++) { se[k] = E[k] - S[k]; ew[k] = W[k] - E[k]; }
        const float a = len3(se), b = len3(ew);
        if (a < 1e-3f || b < 1e-3f) continue;
        const float upm = (a + b) / ARM_HUMAN_M;              /* model units per metre */
        /* model x = the character's left = minus the headset's right; the
         * eyes 6 cm above and 8 cm ahead of the head node */
        const float *o = g_arms_off[hd];
        const float T0[3] = { head[12] - upm * o[0], head[13] + upm * (o[1] + 0.06f), head[14] + upm * (o[2] + 0.08f) };
        float st[3];
        for (int k = 0; k < 3; k++) st[k] = T0[k] - S[k];
        float d = len3(st);
        if (d < 1e-3f) continue;
        const float u[3] = { st[0] / d, st[1] / d, st[2] / d };
        const float dmax = 0.999f * (a + b), dmin = fabsf(a - b) + 0.01f * (a + b);
        if (d > dmax) d = dmax;
        if (d < dmin) d = dmin;
        float T[3];
        for (int k = 0; k < 3; k++) T[k] = S[k] + u[k] * d;
        /* The elbow: in the plane of the reach and the pole (down, a little
         * out to its side and back). */
        const float side = hd ? -1.0f : 1.0f;
        float pole[3] = { 0.4f * side, -1.0f, -0.3f };
        const float pu = pole[0] * u[0] + pole[1] * u[1] + pole[2] * u[2];
        for (int k = 0; k < 3; k++) pole[k] -= pu * u[k];
        const float pl = len3(pole);
        if (pl < 1e-4f) continue;
        for (int k = 0; k < 3; k++) pole[k] /= pl;
        const float ca = (a * a + d * d - b * b) / (2.0f * a * d);
        const float sa = sqrtf(fmaxf(0.0f, 1.0f - ca * ca));
        float E2[3], from[3], to[3], R[9];
        for (int k = 0; k < 3; k++) E2[k] = S[k] + a * (ca * u[k] + sa * pole[k]);
        for (int k = 0; k < 3; k++) { from[k] = se[k] / a; to[k] = (E2[k] - S[k]) / a; }
        rot_between(from, to, R);
        turn_below(nodes, recs, n, J[0], R, S);
        node_get(nodes, J[2], m);                             /* the wrist after the shoulder turn */
        float ew2[3], et[3];
        for (int k = 0; k < 3; k++) { ew2[k] = m[12 + k] - E2[k]; et[k] = T[k] - E2[k]; }
        const float l1 = len3(ew2), l2 = len3(et);
        if (l1 < 1e-4f || l2 < 1e-4f) continue;
        for (int k = 0; k < 3; k++) { ew2[k] /= l1; et[k] /= l2; }
        rot_between(ew2, et, R);
        turn_below(nodes, recs, n, J[1], R, E2);
#ifndef PSP2I_PROD
        if (hd == 1 && psp_sched_vblank_count() - g_arms_log >= 300) {
            g_arms_log = psp_sched_vblank_count();
            fprintf(stderr, "vr: arms: right hand %.2f right %.2f up %.2f ahead of the eyes (m); reach %.0f%% of the arm"
                            " (%.2f + %.2f units, %.1f a metre)\n", (double)o[0], (double)o[1], (double)o[2],
                    (double)(100.0f * len3(st) / (a + b)), (double)a, (double)b, (double)upm);
        }
#endif
    }
    return 1;
}

static void arms_restore(uint32_t model) {
    const uint32_t nodes = psp_read32(model + 100u);
    for (int b = 0; b < g_arms_n_saved; b++) node_put(nodes, b, g_arms_saved[b]);
}

static void hook_palette(void (*original)(void)) {
    const uint32_t model = psp_cpu.r[4], skel = psp_cpu.r[6];
#ifndef PSP2I_PROD
    if (g_skel_map) skel_look(model, skel);
    if (g_test_node >= 0 && model && model == g_pl_model) {
        const int changed = test_pose(model, skel);
        original();
        if (changed && !g_test_keep) test_restore(model, skel);
        return;
    }
#endif
    if (g_arms_on && user_ptr(model) && user_ptr(skel)) {
        const uint32_t pl = gamelog_local_player();
        const uint32_t box = pl ? psp_read32(pl + PLAYER_MODEL_BOX) : 0;
        if (user_ptr(box) && psp_read32(box + 8u) == model) {
            arms_frame();
            if ((g_arms_ok[0] || g_arms_ok[1]) && arms_pose(model, skel)) {
                original();
                arms_restore(model);
                return;
            }
        }
    }
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
    psp_hook_set(FN_PALETTE, hook_palette);                 /* a no-op unless the map, the test or the arms are on */
}
