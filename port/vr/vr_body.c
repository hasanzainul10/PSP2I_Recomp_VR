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
#include <psprecomp/render.h>

/* 0x08A846DC(model, &palette, skeleton) -> 0x08D74E54(palette, skeleton,
 * *(model+100), model+32): for each of the skeleton's +40 nodes (144-byte
 * records from +48: flags +0, palette slot +4 as a short, -1 = none, the
 * inverse bind matrix at +48 unless flag 0x8), palette[slot] = (model+32) x
 * node. Node matrices are 4x4, 64 bytes each, VFPU columns (disassembly and
 * a write watch on a palette entry, 2026-10-23). */
#define FN_PALETTE    0x08A846DCu
#define USER_LO       0x08000000u              /* main RAM: the drawing lists' objects sit below 0x08800000 (0x085F1B70) */
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
 * The target is the controller's offset from your shoulder (estimated
 * from the headset: ARM_SHOULDER_* in its heading), applied from the
 * model's shoulder joint, in model units: the model's own arm length over a
 * human's (shoulder to grip ~0.62 m). So a bent arm stays bent (from the
 * eyes, as first built, the target was past the arm's reach in half the
 * samples: the model's shoulders sit elsewhere than yours). Model space:
 * +x the character's left, +y up, +z forward (headset-checked). It works
 * the same in first and third person, with no camera matrix. */
#define PLAYER_MODEL_BOX 0x178u               /* *(player+0x178)+0x8: the player's animated model */
#define ARM_HUMAN_M      0.62f                 /* shoulder joint to the grip */
#define ARM_SHOULDER_X   0.18f                 /* your shoulder from your eyes, metres: to the side, */
#define ARM_SHOULDER_Y   0.22f                 /* down, */
#define ARM_SHOULDER_Z   0.08f                 /* back */
static const int ARM_NODE[2][3] = { { 15, 16, 17 }, { 20, 21, 22 } };   /* shoulder, elbow, wrist; left, right */

static int      g_arms_on;                     /* vr_body_arms */
static uint64_t g_arms_vb = ~0ull;
static int      g_arms_ok[2];                  /* this frame's target per hand */
static float    g_arms_off[2][3];              /* the hand from its shoulder, metres: right, up, forward */
static float    g_arms_saved[NODES_MAX][16];
static int      g_arms_n_saved;
/* The hands' rotation (both grips + both triggers: vr_body_hand_calibrate):
 * the hand node's rotation relative to the controller's, taken while you
 * hold the controllers as your character holds its hands; after it, each
 * hand turns with its controller by that offset. 3x3 row-major, model axes. */
static float    g_ctrl_rot[2][9];              /* this frame's controllers in model axes */
static float    g_cal[2][9];
static int      g_cal_ok[2], g_cal_req, g_cal_changed;
#ifndef PSP2I_PROD
static uint64_t g_arms_log;
#endif

void vr_body_hand_calibrate(void) { g_cal_req = 1; }
int vr_body_hand_cal_get(float cal[18]) {
    memcpy(cal, g_cal, sizeof g_cal);
    return g_cal_ok[0] && g_cal_ok[1];
}
void vr_body_hand_cal_set(const float cal[18]) {
    memcpy(g_cal, cal, sizeof g_cal);
    g_cal_ok[0] = g_cal_ok[1] = 1;
}
int vr_body_hand_cal_changed(void) {
    const int c = g_cal_changed;
    g_cal_changed = 0;
    return c;
}

static void mat3_mul(const float *a, const float *b, float *o) {        /* o = a b */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}
static void mat3_tmul(const float *a, const float *b, float *o) {       /* o = a^T b */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r] * b[c] + a[3 + r] * b[3 + c] + a[6 + r] * b[6 + c];
}
static void mat3_mult(const float *a, const float *b, float *o) {       /* o = a b^T */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r * 3] * b[c * 3] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2];
}

/* First person: the head (node 13 and what hangs on it, hair included)
 * drawn shrunk to a point at the neck, so the eyes can sit where the face
 * is (vr_fp_hide_head; drawn only, like the arms). */
#define HEAD_NODE  13
/* Shrunk from the neck (12, at the chest's top; 13 hangs 1.07 above it):
 * from 13 only the hair and accessories went, the face and head stayed (user). */
#define NECK_NODE  12
#define HEAD_SHRINK 0.001f
static int g_hide_head;
void vr_body_head(int hide) {
    if (hide != g_hide_head) fprintf(stderr, "vr: first person: own head %s\n", hide ? "hidden" : "shown");
    g_hide_head = hide;
    psp_gpu_set_skin_zero_drop(hide);
    if (!hide) psp_gpu_set_rigid_drop(0, NULL, 0.0f);
}
/* The head's rigid mesh (texture 0x09358BE0 in the GE dump: unskinned, its
 * world matrix from none of the palettes, so neither the zeroed bones nor
 * the shrunk palettes reach it; user: "still there") is dropped at the GE:
 * rigid draws within HEAD_RIGID_R of the head node, eye space, from the
 * animated pose of this frame. */
#define HEAD_RIGID_R 3.0f
static void node_get(uint32_t nodes, int b, float *m);
static void head_rigid_mark(uint32_t model, uint32_t nodes) {
    float Mb[16], nb[16], W[16];
    for (uint32_t i = 0; i < 16; i++) Mb[i] = psp_read_f32(model + 32u + 4u * i);
    node_get(nodes, HEAD_NODE, nb);
    for (int c = 0; c < 4; c++)                                /* W = Mb x node, the translation column only */
        for (int r = 0; r < 4; r++)
            W[c * 4 + r] = Mb[r] * nb[c * 4] + Mb[4 + r] * nb[c * 4 + 1] + Mb[8 + r] * nb[c * 4 + 2] + Mb[12 + r] * nb[c * 4 + 3];
    psp_gpu_set_rigid_drop(1, &W[12], HEAD_RIGID_R);
}

void vr_body_arms(int on) {
    if (on != g_arms_on) fprintf(stderr, "vr: arms %s\n", on ? "on (they follow the controllers)" : "off");
    g_arms_on = on;
}

/* Once a frame: the controllers from your shoulders, in the headset's heading. */
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
        g_arms_off[hd][0] = d[0] * r[0] + d[2] * r[2] - (hd ? ARM_SHOULDER_X : -ARM_SHOULDER_X);
        g_arms_off[hd][1] = d[1] + ARM_SHOULDER_Y;
        g_arms_off[hd][2] = d[0] * f[0] + d[2] * f[2] + ARM_SHOULDER_Z;
        g_arms_ok[hd] = 1;
        /* The grip's axes (columns of its rotation), tracking -> model:
         * model x = minus the heading's right, y up, z = the heading's forward. */
        const float *g = vh[hd].q, x = g[0], y = g[1], z = g[2], w = g[3];
        const float Rq[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                              2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                              2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) };
        const float T[9] = { -r[0], 0, -r[2], 0, 1, 0, f[0], 0, f[2] };
        mat3_mul(T, Rq, g_ctrl_rot[hd]);
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
static int arms_pose(uint32_t model, uint32_t skel, int arms) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u), recs = psp_read32(skel + 48u);
    if (n < 23 || n > NODES_MAX || !user_ptr(nodes) || !user_ptr(recs)) return 0;
    for (int b = 0; b < n; b++) node_get(nodes, b, g_arms_saved[b]);
    g_arms_n_saved = n;
    for (int hd = 0; hd < 2; hd++) {
        if (!arms || !g_arms_ok[hd]) continue;
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
        /* model x = the character's left = minus the headset's right */
        const float *o = g_arms_off[hd];
        const float T0[3] = { S[0] - upm * o[0], S[1] + upm * o[1], S[2] + upm * o[2] };
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
        /* The hand: its node's axes now (normalized: the nodes carry scale). */
        node_get(nodes, J[2], m);
        float H[9];
        for (int c = 0; c < 3; c++) {
            const float l = len3(&m[c * 4]);
            for (int k = 0; k < 3; k++) H[k * 3 + c] = l > 1e-6f ? m[c * 4 + k] / l : 0.0f;
        }
        if (g_cal_req) {
            mat3_tmul(g_ctrl_rot[hd], H, g_cal[hd]);          /* C^T H: the hand in the controller's frame */
            g_cal_ok[hd] = 1;
        } else if (g_cal_ok[hd]) {
            float Ht[9];
            mat3_mul(g_ctrl_rot[hd], g_cal[hd], Ht);           /* where the hand should be: C x the offset */
            mat3_mult(Ht, H, R);                               /* Ht H^T: from where it is */
            turn_below(nodes, recs, n, J[2], R, &m[12]);
        }
#ifndef PSP2I_PROD
        if (hd == 1 && psp_sched_vblank_count() - g_arms_log >= 300) {
            g_arms_log = psp_sched_vblank_count();
            fprintf(stderr, "vr: arms: right hand %.2f right %.2f up %.2f ahead of its shoulder (m); reach %.0f%% of the arm"
                            " (%.2f + %.2f units, %.1f a metre)\n", (double)o[0], (double)o[1], (double)o[2],
                    (double)(100.0f * len3(st) / (a + b)), (double)a, (double)b, (double)upm);
        }
#endif
    }
    if (arms && g_cal_req) {
        g_cal_req = 0;
        g_cal_changed = 1;
        vr_haptic(-1, 0.6f, 0.2f);
        fprintf(stderr, "vr: arms: hands calibrated (left %s, right %s)\n", g_cal_ok[0] ? "yes" : "no", g_cal_ok[1] ? "yes" : "no");
    }
    return 1;
}

/* Shrinks node `top` and everything below it to its own origin. */
static void shrink_below(uint32_t nodes, uint32_t recs, int n, int top, float k) {
    float P[16];
    node_get(nodes, top, P);
    for (int b = 0; b < n; b++) {
        int x = b;
        for (int guard = 0; x >= 0 && x != top && guard < n; guard++) x = (int16_t)psp_read16(recs + 144u * (uint32_t)x + 6u);
        if (x != top) continue;
        float m[16];
        node_get(nodes, b, m);
        if (k == 0.0f) memset(m, 0, sizeof m);                /* all zero: a bone the GE drops its draws for */
        else {
            for (int i = 0; i < 12; i++) m[i] *= k;
            for (int r = 0; r < 3; r++) m[12 + r] = P[12 + r] + k * (m[12 + r] - P[12 + r]);
        }
        node_put(nodes, b, m);
    }
}

static void arms_restore(uint32_t model) {
    const uint32_t nodes = psp_read32(model + 100u);
    for (int b = 0; b < g_arms_n_saved; b++) node_put(nodes, b, g_arms_saved[b]);
}

/* ---- What rides on the hands: the weapon
 *
 * The weapon isn't part of the body's model, so the arm IK alone leaves it
 * where the animation had it (user, in the headset). Nor is it the extra
 * model next to the body in its container: that one rides on the head (node
 * 13: hair or headgear, measured). So every matrix the game copies into a
 * display list (0x08D8D4E8: skinned palettes and rigid world matrices
 * alike, eye space, just before drawing) near the player's hands is
 * checked: its placement in the frame of each arm node, inv(body M x node)
 * x it, stays the same through the animation only for the node it hangs on.
 * Once found, it is moved by what the IK did to that node (D = posed node x
 * inv(animated node), model space, from the latest pose), for its copy only,
 * and put back after it. The body's own palettes are skipped (they are
 * posed already), and a party member's weapon never holds still against
 * our hands. */
#define ATT_MAX     128                        /* ~20-80 matrices sit near the hands each frame (logged) */
#define ATT_FRAMES  20                         /* frames of agreement before a node is trusted */
#define ATT_ERR_OK  0.05f                      /* model units: how still a true attachment stays */
#define ATT_NEAR    15.0f                      /* only matrices this close to a hand (eye space) */
#define ATT_HAND_NEAR 5.0f                     /* a held thing sits this close to its node */
#define ATT_GRIP_NEAR 2.5f                     /* ... and a gun in the aiming hand this close (grips 1.6-2.1; the head 6.5) */
#define ATT_STATE   0x214u                     /* player +0x214: 2 while R is held (lock-on) and a moment after */
#define ATT_EVIDENCE  1.0f                     /* units of drift against the torso, more than against the arm */
/* The arm nodes a held thing can hang on (left 16-18, right 21-23: forearm,
 * wrist, hand's end), then the torso's (hips, chest, head): standing still,
 * the head and a floating Rabbit look as still against the forearm as the
 * weapon does (user: "it drags the head and the Rabbit too"), so a thing is
 * held only once the arm has moved against the torso and it went with the
 * arm, not the torso. */
static const int ATT_NODE[] = { 16, 17, 18, 21, 22, 23, 1, 11, 13 };
#define ATT_NN  ((int)(sizeof ATT_NODE / sizeof ATT_NODE[0]))
#define ATT_ARM 6                              /* the first ATT_ARM are the arms' */
#define ATT_HEAD_I 8                           /* ATT_NODE[8]: the head, node 13 */
#define ATT_HEAD_NEAR 3.0f                     /* first person: rigid pieces this close to it are the head's */

static float    g_att_d[NODES_MAX][16];        /* per body node: the IK's change, model space */
static int      g_att_dn;                      /* nodes in it (0 = no pose yet) */
static int      g_att_moved[NODES_MAX];        /* D isn't the identity */
static uint32_t g_att_pal[4];                  /* this frame's body palettes (skipped) */
static uint64_t g_att_pal_vb;
typedef struct {
    uint32_t key;                              /* the matrix's address */
    uint64_t seen;
    uint32_t obj;                              /* dev log: whose, and the last fit */
    int      fit_node;
    float    fit_err, fit_torso, fit_dist;
    int      frames;
    int      node[2];                          /* the node it rides on per state (ATT_STATE), -1 = none */
    int      off[2];                           /* frames in a row it has drifted off that node */
    int      grip_node;                        /* learned with R held: the hand it is aimed from, -1 = none */
    int      head_told;                        /* dev log: hidden as part of the head, said once */
    float    grip[16];                         /* ... and where it sits in that hand */
    float    err[ATT_NN];                      /* recent drift per frame (decaying) */
    float    sum[2][ATT_NN];                   /* all its drift since it was first seen, per state */
    float    prev[ATT_NN][12];
} att_rec;
static att_rec g_att[ATT_MAX];
#ifndef PSP2I_PROD
/* Dev: what the copy hook sees, logged every 5 s with the arms on (which
 * step stops it when nothing follows). */
static unsigned g_ad_calls, g_ad_posed, g_ad_flags, g_ad_bodypal, g_ad_mats, g_ad_near;
static float    g_ad_min = 1e30f;
static uint32_t g_ad_min_mat, g_ad_min_obj, g_ad_cur_obj;
static uint64_t g_ad_log;
#endif

static void mul44(const float *a, const float *b, float *o) {            /* o = a b, VFPU columns */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}
/* An affine matrix's inverse (3x3 with scale, then the translation). */
static int inv_affine(const float *m, float *o) {
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) return 0;
    const float k = 1.0f / det;
    o[0] = (e * i - f * h) * k; o[4] = (c * h - b * i) * k; o[8] = (b * f - c * e) * k;
    o[1] = (f * g - d * i) * k; o[5] = (a * i - c * g) * k; o[9] = (c * d - a * f) * k;
    o[2] = (d * h - e * g) * k; o[6] = (b * g - a * h) * k; o[10] = (a * e - b * d) * k;
    o[3] = o[7] = o[11] = 0.0f;
    for (int r = 0; r < 3; r++) o[12 + r] = -(o[r] * m[12] + o[4 + r] * m[13] + o[8 + r] * m[14]);
    o[15] = 1.0f;
    return 1;
}

/* After a pose (nodes posed, g_arms_saved the animated ones): each node's change. */
static void att_keep_pose(uint32_t nodes, int n) {
    for (int b = 0; b < n; b++) {
        float now[16], inv[16];
        node_get(nodes, b, now);
        g_att_moved[b] = memcmp(now, g_arms_saved[b], sizeof now) != 0;
        if (!g_att_moved[b] || !inv_affine(g_arms_saved[b], inv)) {
            static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            memcpy(g_att_d[b], I, sizeof I);
            g_att_moved[b] = 0;
            continue;
        }
        mul44(now, inv, g_att_d[b]);
    }
    g_att_dn = n;
}

/* One matrix (eye space, at `mat`): learns which arm node it hangs on and,
 * once known, moves it with that node. Returns 1 with it changed (the
 * original in `keep`). */
static int att_follow(uint32_t mat, int st, uint32_t body, const float *Mb, const float (*W)[16], const float (*Wi)[16], float *keep) {
    float Mw[16];
    for (uint32_t i = 0; i < 16; i++) Mw[i] = psp_read_f32(mat + 4u * i);
    float near = 1e30f;
    for (int j = 0; j < ATT_ARM; j++) {
        const float dx = Mw[12] - W[j][12], dy = Mw[13] - W[j][13], dz = Mw[14] - W[j][14];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < near) near = d;
    }
#ifndef PSP2I_PROD
    g_ad_mats++;
    if (near < g_ad_min) { g_ad_min = near; g_ad_min_mat = mat; g_ad_min_obj = g_ad_cur_obj; }
#endif
    if (near > ATT_NEAR * ATT_NEAR) return 0;
#ifndef PSP2I_PROD
    g_ad_near++;
#endif
    const uint64_t vb = psp_sched_vblank_count();
    att_rec *a = NULL, *old = &g_att[0];
    for (int i = 0; i < ATT_MAX && !a; i++) if (g_att[i].key == mat) a = &g_att[i];
    if (!a) {
        for (int i = 0; i < ATT_MAX; i++) if (g_att[i].seen < old->seen) old = &g_att[i];
        a = old;
        memset(a, 0, sizeof *a);
        a->key = mat;
        a->node[0] = a->node[1] = -1;
        a->grip_node = -1;
    }
    a->seen = vb;
#ifndef PSP2I_PROD
    a->obj = g_ad_cur_obj;
#endif
    /* Its drift against each node, frame to frame: recent (err) and all of
     * it (sum). Standing still, everything near the hands (the head, the
     * Rabbit, the scenery) drifts as little against the arm as against the
     * torso (log, 2026-10-24: both ~0.000), so the evidence is kept: a thing
     * is held once it has drifted clearly less against an arm node than
     * against the torso, and sits by the hand (the weapons' grips 1.6-2.1
     * units from it; the head, the Rabbit and the scenery 6.5-12). */
    float L[16], use_L[16];
    int best = -1, have = 0;
    float best_s = 1e30f, torso_s = 1e30f;
    for (int j = 0; j < ATT_NN; j++) {
        mul44(Wi[j], Mw, L);
        float e = 0.0f;
        for (int k = 0; k < 12; k++) {
            const int idx = k < 9 ? (k / 3) * 4 + k % 3 : 12 + (k - 9);
            e += fabsf(L[idx] - a->prev[j][k]);
            a->prev[j][k] = L[idx];
        }
        a->err[j] = a->frames ? 0.9f * a->err[j] + 0.1f * e : 1.0f;
        if (a->frames) a->sum[st][j] += e;
        if (j == a->node[st]) { memcpy(use_L, L, sizeof L); have = 1; }
        if (j < ATT_ARM) { if (a->sum[st][j] < best_s) { best_s = a->sum[st][j]; best = j; } }
        else if (a->sum[st][j] < torso_s) torso_s = a->sum[st][j];
    }
    if (a->frames < 100000) a->frames++;
    /* Of nodes that fit about as well on the same arm, the one nearest the
     * hand (the wrist's calibration turns it and what is below it). */
    if (best >= 0) {
        const int arm = best / 3;
        for (int j = arm * 3 + 2; j > best; j--)
            if (a->sum[st][j] <= 1.5f * best_s + 0.05f) { best = j; break; }
    }
    float dist = 1e30f;
    if (best >= 0) {
        mul44(Wi[best], Mw, L);
        dist = sqrtf(L[12] * L[12] + L[13] * L[13] + L[14] * L[14]);
        a->fit_node = ATT_NODE[best];                       /* for the dev log */
        a->fit_err = a->err[best];
        a->fit_torso = torso_s;
        a->fit_dist = dist;
    }
    /* Let go only of what has left the hand for good (put away, dropped):
     * a gun coming up into the hand drifts for a few frames. */
    if (a->node[st] >= 0) a->off[st] = a->err[a->node[st]] > 4.0f * ATT_ERR_OK ? a->off[st] + 1 : 0;
    if (a->node[st] >= 0 && a->off[st] > 30) {
        fprintf(stderr, "vr: arms: the matrix at 0x%08X left the hand: let go\n", mat);
        a->node[st] = -1;
        memset(a->sum[st], 0, sizeof a->sum[st]);
        return 0;
    }
    /* With R held the arm aims, still against the chest: no torso evidence
     * comes, so a thing rigid to the hand and at the grip is enough then. */
    const int held = best >= 0 && a->err[best] < ATT_ERR_OK && dist < ATT_HAND_NEAR &&
                     (torso_s > 4.0f * a->sum[st][best] + ATT_EVIDENCE || (st && dist < ATT_GRIP_NEAR));
    if (a->frames >= ATT_FRAMES && held && best != a->node[st] && (a->node[st] < 0 || best / 3 == a->node[st] / 3 ||
                                                               a->sum[st][best] < 0.5f * a->sum[st][a->node[st]])) {
        a->node[st] = best;
        fprintf(stderr, "vr: arms: the matrix at 0x%08X rides on body node %d (drift %.2f against it, %.2f against the"
                        " torso, %.2f units from it): it follows the hand\n", mat, ATT_NODE[best], (double)a->sum[st][best],
                (double)torso_s, (double)dist);
        return 0;                                              /* use_L is the old node's: from the next frame */
    }
    /* First person: a rigid piece at the head that no hand holds (the face
     * and head: they stayed with the body's node 13 and the container's
     * model hidden, which took only the hair and the accessories, user) is
     * drawn shrunk away. ATT_NODE[ATT_HEAD_I] is node 13. */
    if (g_hide_head && a->node[0] < 0 && a->node[1] < 0 && a->grip_node < 0) {
        mul44(Wi[ATT_HEAD_I], Mw, L);
        if (L[12] * L[12] + L[13] * L[13] + L[14] * L[14] < ATT_HEAD_NEAR * ATT_HEAD_NEAR) {
#ifndef PSP2I_PROD
            if (!a->head_told) {
                a->head_told = 1;
                fprintf(stderr, "vr: first person: the matrix at 0x%08X (obj 0x%08X) sits %.2f units from the head: hidden\n",
                        mat, g_ad_cur_obj, (double)sqrtf(L[12] * L[12] + L[13] * L[13] + L[14] * L[14]));
            }
#endif
            for (uint32_t i = 0; i < 16; i++) {
                keep[i] = Mw[i];
                psp_write_f32(mat + 4u * i, i < 12 ? Mw[i] * HEAD_SHRINK : Mw[i]);
            }
            return 1;
        }
    }
    /* A gun is in the hand only while R is held; in VR your hand keeps it:
     * without R, what was learned with R stays in the hand at its grip. */
    int nb_i;
    if (a->node[st] >= 0 && have && a->err[a->node[st]] <= 4.0f * ATT_ERR_OK) {
        nb_i = ATT_NODE[a->node[st]];
        if (st) { memcpy(a->grip, use_L, sizeof use_L); a->grip_node = a->node[st]; }
    } else if (!st && a->node[0] < 0 && a->grip_node >= 0) {
        nb_i = ATT_NODE[a->grip_node];
        memcpy(use_L, a->grip, sizeof use_L);
    } else return 0;
    if (!g_att_moved[nb_i]) return 0;
    /* M' = Mb x (D x node) x L: the posed node, the same attachment. */
    const uint32_t nodes = psp_read32(body + 100u);
    float nb[16], np[16], t[16], out[16];
    node_get(nodes, nb_i, nb);
    mul44(g_att_d[nb_i], nb, np);
    mul44(Mb, np, t);
    mul44(t, use_L, out);
    for (uint32_t i = 0; i < 16; i++) { keep[i] = Mw[i]; psp_write_f32(mat + 4u * i, out[i]); }
    return 1;
}

#ifndef PSP2I_PROD
static int      g_pal_dump;                    /* dev: the capture frame (vr_body_dump_palettes, below) */
static uint64_t g_pal_dump_vb;
#endif

/* 0x08D8D4E8(obj): its matrices into its display list (see FN_BONE_PATCH). */
#define FN_BONE_PATCH 0x08D8D4E8u
#define ATT_KEEP_MAX  64
static void hook_bone_patch(void (*original)(void)) {
    const uint32_t obj = psp_cpu.r[4];
    uint32_t kept_at[ATT_KEEP_MAX];
    static float kept[ATT_KEEP_MAX][16];
    int nk = 0;
    const uint32_t pl = (g_arms_on && g_att_dn) || g_hide_head ? gamelog_local_player() : 0;
    const uint32_t box = pl ? psp_read32(pl + PLAYER_MODEL_BOX) : 0;
    const uint32_t body = user_ptr(box) ? psp_read32(box + 8u) : 0;
    const uint32_t nodes = user_ptr(body) ? psp_read32(body + 100u) : 0;
    const uint32_t list = user_ptr(obj) ? psp_read32(obj + 64u) : 0;
    const int n = user_ptr(obj) ? (int)psp_read32(obj + 8u) : 0;
#ifndef PSP2I_PROD
    g_ad_cur_obj = obj;
    if (g_arms_on && g_skel_map) {                     /* with PSP2I_SKEL: the copy hook's counters and candidates */
        g_ad_calls++;
        if (user_ptr(nodes)) g_ad_posed++;
        if (user_ptr(nodes) && user_ptr(list) && n > 0 && n <= NODES_MAX && (psp_read32(obj + 16u) & 0x08000018u)) g_ad_flags++;
        const uint64_t vb = psp_sched_vblank_count();
        if (vb - g_ad_log >= 300) {
            g_ad_log = vb;
            fprintf(stderr, "vr: arms: copy hook in 5 s: %u calls, %u with a posed body, %u with matrices; %u body palettes"
                            " skipped; %u matrices looked at, %u near a hand; nearest %.1f units (0x%08X of obj 0x%08X);"
                            " body palettes this frame 0x%08X 0x%08X 0x%08X\n",
                    g_ad_calls, g_ad_posed, g_ad_flags, g_ad_bodypal, g_ad_mats, g_ad_near,
                    (double)sqrtf(g_ad_min < 1e29f ? g_ad_min : 0.0f), g_ad_min_mat, g_ad_min_obj, g_att_pal[0], g_att_pal[1], g_att_pal[2]);
            g_ad_calls = g_ad_posed = g_ad_flags = g_ad_bodypal = g_ad_mats = g_ad_near = 0;
            g_ad_min = 1e30f;
            /* The candidates that sit still on an arm node, stillest first:
             * the weapon against the head and the Rabbit. */
            int order[ATT_MAX], no = 0;
            for (int i = 0; i < ATT_MAX; i++)
                if (g_att[i].key && vb - g_att[i].seen < 30 && g_att[i].frames >= ATT_FRAMES && g_att[i].fit_err < ATT_ERR_OK)
                    order[no++] = i;
            for (int i = 1; i < no; i++)
                for (int j = i; j > 0 && g_att[order[j]].fit_err < g_att[order[j - 1]].fit_err; j--) {
                    const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
                }
            for (int i = 0; i < no && i < 10; i++) {
                const att_rec *c = &g_att[order[i]];
                fprintf(stderr, "vr: arms:   candidate 0x%08X (obj 0x%08X): node %d drift now %.3f, all of it against the torso %.2f,"
                                " %.2f units from the node%s\n", c->key, c->obj, c->fit_node, (double)c->fit_err,
                        (double)c->fit_torso, (double)c->fit_dist, c->node[0] >= 0 || c->node[1] >= 0 ? " (held)" : "");
            }
        }
    }
#endif
    if (user_ptr(nodes) && user_ptr(list) && n > 0 && n <= NODES_MAX && (psp_read32(obj + 16u) & 0x08000018u)) {
        float Mb[16], W[ATT_NN][16], Wi[ATT_NN][16];
        int ok = 1;
        for (uint32_t i = 0; i < 16; i++) Mb[i] = psp_read_f32(body + 32u + 4u * i);
        for (int j = 0; j < ATT_NN && ok; j++) {               /* the arm and torso nodes as animated, eye space */
            float nb[16];
            node_get(nodes, ATT_NODE[j], nb);
            mul44(Mb, nb, W[j]);
            ok = inv_affine(W[j], Wi[j]);
        }
        /* R held (lock-on, player +0x214 != 0, as fp_nudge reads it): a gun
         * comes up into the hand only then (user), so what rides on the hand
         * is learned, and followed, for each state apart. */
        const int st = psp_read32(pl + ATT_STATE) != 0;
        const int parts = (int)psp_read8(obj + 21u);
        for (int part = 0; part < parts && ok; part++) {
            const uint32_t mats = psp_read32(list + 4u * (uint32_t)part);
            int body_pal = !user_ptr(mats);
            for (int k = 0; k < 4 && !body_pal; k++) body_pal = g_att_pal_vb + 2 >= psp_sched_vblank_count() && mats == g_att_pal[k];
            if (body_pal) {
#ifndef PSP2I_PROD
                if (g_pal_dump) fprintf(stderr, "vr: at the head: obj 0x%08X part %d is a body palette (skipped)\n", obj, part);
                g_ad_bodypal++;
                static uint64_t told;                          /* that the skip finds them */
                if (psp_sched_vblank_count() - told >= 600) {
                    told = psp_sched_vblank_count();
                    fprintf(stderr, "vr: arms: the body's palette 0x%08X skipped in the copy (obj 0x%08X)\n", mats, obj);
                }
#endif
                continue;
            }
            for (int i = 0; i < n && nk < ATT_KEEP_MAX; i++) {
                const uint32_t m = mats + 64u * (uint32_t)i;
#ifndef PSP2I_PROD
                if (g_pal_dump) {                              /* the capture frame: what sits at the head */
                    const float dx = psp_read_f32(m + 48u) - W[ATT_HEAD_I][12], dy = psp_read_f32(m + 52u) - W[ATT_HEAD_I][13],
                                dz = psp_read_f32(m + 56u) - W[ATT_HEAD_I][14];
                    if (dx * dx + dy * dy + dz * dz < 16.0f)
                        fprintf(stderr, "vr: at the head: obj 0x%08X part %d matrix %d (0x%08X) t %.3f %.3f %.3f, %.2f from node 13,"
                                        " flags 0x%08X\n", obj, part, i, m, (double)psp_read_f32(m + 48u),
                                (double)psp_read_f32(m + 52u), (double)psp_read_f32(m + 56u),
                                (double)sqrtf(dx * dx + dy * dy + dz * dz), psp_read32(obj + 16u));
                }
#endif
                if (att_follow(m, st, body, Mb, (const float (*)[16])W, (const float (*)[16])Wi, kept[nk])) kept_at[nk++] = m;
            }
        }
    }
    original();
    for (int k = 0; k < nk; k++)
        for (uint32_t i = 0; i < 16; i++) psp_write_f32(kept_at[k] + 4u * i, kept[k][i]);
}

/* A model next to the body in the player's container (the first 16 slots)
 * placed at the head: its origin within HEAD_NEAR of node 13. */
#define HEAD_NEAR 6.0f                          /* the face model sits 4.54 from node 13 (its origin); the body's, at the feet, ~13 */
static int head_model(uint32_t box, uint32_t body, uint32_t model) {
    int in_box = 0;
    for (uint32_t q = 0; q < 0x40u && !in_box; q += 4) in_box = psp_read32(box + q) == model;
    const uint32_t nodes = psp_read32(body + 100u);
    if (!in_box || !user_ptr(nodes)) return 0;
    float Mb[16], nb[16], W[16], Wi[16], Mw[16], L[16];
    for (uint32_t i = 0; i < 16; i++) { Mb[i] = psp_read_f32(body + 32u + 4u * i); Mw[i] = psp_read_f32(model + 32u + 4u * i); }
    node_get(nodes, HEAD_NODE, nb);
    mul44(Mb, nb, W);
    if (!inv_affine(W, Wi)) return 0;
    mul44(Wi, Mw, L);
    const int near = L[12] * L[12] + L[13] * L[13] + L[14] * L[14] < HEAD_NEAR * HEAD_NEAR;
#ifndef PSP2I_PROD
    static uint32_t told;
    if (told != model) {
        told = model;
        fprintf(stderr, "vr: first person: model 0x%08X in the container is %.2f units from the head node: %s\n", model,
                (double)sqrtf(L[12] * L[12] + L[13] * L[13] + L[14] * L[14]), near ? "the head, hidden" : "not the head");
    }
#endif
    return near;
}

#ifndef PSP2I_PROD
/* Dev: with a VR capture (vr_body_dump_palettes), the body's palettes of
 * the next frame in the log: slot -> node and each entry's translation, the
 * same numbers as the GE dump's "bones" of its draws (which nodes a mesh
 * such as the face hangs on). */
void vr_body_dump_palettes(void) { g_pal_dump = 1; }
static void pal_dump(uint32_t pal, uint32_t skel) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t recs = psp_read32(skel + 48u);
    if (n <= 0 || n > NODES_MAX || !user_ptr(recs) || !user_ptr(pal)) return;
    fprintf(stderr, "vr: palette 0x%08X (skeleton 0x%08X):", pal, skel);
    for (int b = 0; b < n; b++) {
        const int slot = (int16_t)psp_read16(recs + 144u * (uint32_t)b + 4u);
        if (slot < 0) continue;
        const uint32_t e = pal + 64u * (uint32_t)slot;
        fprintf(stderr, " slot %d node %d (%.2f %.2f %.2f)", slot, b, (double)psp_read_f32(e + 48u),
                (double)psp_read_f32(e + 52u), (double)psp_read_f32(e + 56u));
    }
    fputc('\n', stderr);
}
#endif

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
    if ((g_arms_on || g_hide_head) && user_ptr(model) && user_ptr(skel)) {
        const uint32_t pl = gamelog_local_player();
        const uint32_t box = pl ? psp_read32(pl + PLAYER_MODEL_BOX) : 0;
        const uint32_t body = user_ptr(box) ? psp_read32(box + 8u) : 0;
        if (body == model) {
            const uint64_t vb = psp_sched_vblank_count();     /* its palettes: skipped by hook_bone_patch */
            if (vb != g_att_pal_vb) { memset(g_att_pal, 0, sizeof g_att_pal); g_att_pal_vb = vb; }
            const uint32_t pal = psp_read32(psp_cpu.r[5]);
            for (int k = 0; k < 4; k++) if (!g_att_pal[k] || g_att_pal[k] == pal) { g_att_pal[k] = pal; break; }
            if (g_arms_on) arms_frame();
            const int arms = g_arms_on && (g_arms_ok[0] || g_arms_ok[1]);
            if ((arms || g_hide_head) && arms_pose(model, skel, arms)) {
                const uint32_t nodes = psp_read32(model + 100u);
                if (arms) att_keep_pose(nodes, g_arms_n_saved);    /* before the head: only the arms' changes */
                else g_att_dn = 0;
                if (g_hide_head && g_arms_n_saved > HEAD_NODE) head_rigid_mark(model, nodes);   /* before it is zeroed */
                if (g_hide_head && g_arms_n_saved > HEAD_NODE)    /* zero: the GE drops the draws that use them */
                    shrink_below(nodes, psp_read32(skel + 48u), g_arms_n_saved, NECK_NODE, 0.0f);
                original();
#ifndef PSP2I_PROD
                if (g_pal_dump) {                              /* this frame's three palettes */
                    const uint64_t vb = psp_sched_vblank_count();
                    if (!g_pal_dump_vb) g_pal_dump_vb = vb;
                    if (vb == g_pal_dump_vb) pal_dump(pal, skel);          /* $a1 is gone after the call */
                    else g_pal_dump = 0, g_pal_dump_vb = 0;
                }
#endif
                arms_restore(model);
                return;
            }
            g_att_dn = 0;                                      /* no pose: nothing to follow */
        } else if (g_hide_head && user_ptr(body) && head_model(box, body, model)) {
            /* The face is its own model (8 nodes, in the container at
             * +0x10, riding on node 13): shrinking the body's node 13 hid
             * the head and hair but left the face (user). Drawn shrunk to
             * its origin for this palette. */
            float keep[16];
            for (uint32_t i = 0; i < 16; i++) keep[i] = psp_read_f32(model + 32u + 4u * i);
            for (uint32_t i = 0; i < 12; i++) psp_write_f32(model + 32u + 4u * i, keep[i] * HEAD_SHRINK);
            original();
            for (uint32_t i = 0; i < 16; i++) psp_write_f32(model + 32u + 4u * i, keep[i]);
            return;
        }
    }
    original();
}

/* 0x08D74E54(palette, skeleton, nodes, M): every model's palette, from all
 * five callers (0x08A846DC above, and 0x08A86AF4 / 0x08ADE47C with a matrix
 * of their own: a model attached to another). In first person the head
 * stayed drawn whole (a rigid mesh, texture 0x09358BE0 in the GE dump) after
 * the container's head model was shrunk at 0x08A846DC: it is placed through
 * another caller. Here any palette but the body's whose M puts the model
 * within HEAD_AT of the body's node 13 is built shrunk, M put back after. */
#define FN_PALETTE_ANY 0x08D74E54u
#define HEAD_AT        3.0f
static void hook_palette_any(void (*original)(void)) {
    const uint32_t nodes = psp_cpu.r[6], mp = psp_cpu.r[7];
    if (!g_hide_head || !user_ptr(mp)) { original(); return; }
    const uint32_t pl = gamelog_local_player();
    const uint32_t box = pl ? psp_read32(pl + PLAYER_MODEL_BOX) : 0;
    const uint32_t body = user_ptr(box) ? psp_read32(box + 8u) : 0;
    const uint32_t bn = user_ptr(body) ? psp_read32(body + 100u) : 0;
    if (!user_ptr(bn) || nodes == bn) { original(); return; }
    float Mb[16], nb[16], W[16], M[16];
    for (uint32_t i = 0; i < 16; i++) { Mb[i] = psp_read_f32(body + 32u + 4u * i); M[i] = psp_read_f32(mp + 4u * i); }
    node_get(bn, HEAD_NODE, nb);
    mul44(Mb, nb, W);
    const float dx = M[12] - W[12], dy = M[13] - W[13], dz = M[14] - W[14];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > HEAD_AT) { original(); return; }
#ifndef PSP2I_PROD
    static uint32_t told[8];
    int seen = 0;
    for (int k = 0; k < 8 && !seen; k++) seen = told[k] == psp_cpu.r[5];
    if (!seen) {
        for (int k = 7; k > 0; k--) told[k] = told[k - 1];
        told[0] = psp_cpu.r[5];
        fprintf(stderr, "vr: first person: skeleton 0x%08X (nodes 0x%08X) placed %.2f from the head: built shrunk\n",
                psp_cpu.r[5], nodes, (double)d);
    }
#endif
    for (uint32_t i = 0; i < 12; i++) psp_write_f32(mp + 4u * i, M[i] * HEAD_SHRINK);
    original();
    for (uint32_t i = 0; i < 16; i++) psp_write_f32(mp + 4u * i, M[i]);
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
    psp_hook_set(FN_BONE_PATCH, hook_bone_patch);
    psp_hook_set(FN_PALETTE_ANY, hook_palette_any);         /* first person: the head's other models; a no-op otherwise */           /* the weapon follows the hand: a no-op unless the arms are on */
}
