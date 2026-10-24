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
 * from the headset: ARM_SHOULDER_* in the recentred heading), applied from the
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
/* The avatar (below): this frame's pose is its, and how far the gaze is
 * ahead of its turn (radians). */
static int      g_av_live;
static float    g_av_lag;
static float wrap_pi(float a) { return a - 6.2831853f * floorf((a + 3.1415927f) / 6.2831853f); }

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
static float    g_head_eye[3], g_head_fwd[3];  /* vr_body_head_eye */
static uint64_t g_head_vb;
static void node_get(uint32_t nodes, int b, float *m);
/* `anim` the head node as the game animated it (where the game puts the
 * rigid head), `drawn` as posed (the avatar's: where the eyes go), `beta`
 * the drawn body's turn. */
static void head_rigid_mark(uint32_t model, const float *anim, const float *drawn, float beta) {
    float Mb[16], W[3], E[3];
    for (uint32_t i = 0; i < 16; i++) Mb[i] = psp_read_f32(model + 32u + 4u * i);
    for (int r = 0; r < 3; r++) {                              /* Mb x node, the translation only */
        W[r] = Mb[r] * anim[12] + Mb[4 + r] * anim[13] + Mb[8 + r] * anim[14] + Mb[12 + r];
        E[r] = Mb[r] * drawn[12] + Mb[4 + r] * drawn[13] + Mb[8 + r] * drawn[14] + Mb[12 + r];
    }
    psp_gpu_set_rigid_drop(1, W, HEAD_RIGID_R);
    /* For the first-person eyes: the head joint and the body's forward
     * (model +z, turned by beta), in the game camera's view space, this frame. */
    memcpy(g_head_eye, E, sizeof g_head_eye);
    for (int r = 0; r < 3; r++) g_head_fwd[r] = Mb[r] * sinf(beta) + Mb[8 + r] * cosf(beta);
    g_head_vb = psp_sched_vblank_count();
}
int vr_body_head_eye(float head[3], float fwd[3]) {
    if (!g_head_vb || psp_sched_vblank_count() - g_head_vb > 4) return 0;
    memcpy(head, g_head_eye, sizeof g_head_eye);
    memcpy(fwd, g_head_fwd, sizeof g_head_fwd);
    return 1;
}

void vr_body_arms(int on) {
    if (on != g_arms_on) fprintf(stderr, "vr: arms %s\n", on ? "on (they follow the controllers)" : "off");
    g_arms_on = on;
    if (!on) psp_gpu_set_rigid_move(0, NULL, 0.0f, NULL);         /* the avatar's head (needs the arms) */
}

/* Once a frame: the controllers from your shoulders, in the body's frame:
 * the recentred heading (vr_recenter's forward, the frame vr_head_get and
 * vr_hands_get already are in), not the head's. In the head's heading, a
 * look left put the still hands to the right of the gaze, and the
 * character's arms swung right (user, first and third person). */
static void arms_frame(void) {
    const uint64_t vb = psp_sched_vblank_count();
    if (vb == g_arms_vb) return;
    g_arms_vb = vb;
    g_arms_ok[0] = g_arms_ok[1] = 0;
    vr_head h;
    vr_hand vh[2];
    if (!vr_head_get(&h) || !vr_hands_get(vh)) return;
    /* Forward and right: at recenter, or with the avatar the drawn body's
     * heading (it turns to the gaze: the head's yaw less the body's lag),
     * so the hands stay where yours are as the body turns. */
    float yaw = 0.0f;
    if (g_av_live) {
        const float *q = h.q, x = q[0], y = q[1], z = q[2], w = q[3];
        yaw = wrap_pi(atan2f(2 * (x * z + y * w), 1 - 2 * (x * x + y * y)) - g_av_lag);   /* the head's back axis */
    }
    const float f[3] = { -sinf(yaw), 0.0f, -cosf(yaw) };
    const float r[3] = { cosf(yaw), 0.0f, -sinf(yaw) };
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

/* ---- The avatar: the body drawn from you, not from the game's animation
 *
 * With vr_avatar on (and the arms: MOTION), the local player is drawn as a
 * VRChat avatar: the upper body (spine 9 and all on it: chest, neck, head,
 * arms) stands in its bind pose (the skeleton's own rest pose, an upright
 * A-pose: the inverse of each record's inverse bind), so attacks and Photon
 * Arts don't move it; the arms then reach for the controllers (arms_pose);
 * the whole body is turned about its root to face where you look (the gaze,
 * eased); the hips and legs keep the game's animation. The game doesn't
 * know: its facing (player +0x70), its nodes and its attacks are as they
 * were (the nodes are put back after each palette).
 *
 * The game turns the character to where it walks and plays its forward
 * walk; held R, it strafes and backs off facing ahead (user). So the legs
 * of a strafe are learned from the game's own: while R is held and you walk
 * left, right or back for 2.5-5 s, one gait cycle of the legs (nodes 1-8 in the
 * root's frame, one sample a game frame) is kept, and saved
 * (psp2i_vr_legs.bin); after that, walking that way from your gaze draws
 * those legs, at your speed, whatever the game plays. Forward is the game's
 * own run (and is learned too, for walking ahead while R strafes). With
 * vr_avatar_legs=still the legs stand in the bind pose instead. */
#define AV_LEG_N     8                         /* nodes 1-8: hips, pelvis, both legs */
#define AV_SPINE     9                         /* the upper body: 9-26 (all on 9) */
#define AV_UPPER_END 27
#define AV_REC_N     150                       /* 5 s of one direction at 30 game frames a second, 2.5 s at 60 */
#define AV_CLIP_MAX  75                        /* the longest gait cycle looked for (half the recording) */
#define AV_CLIP_MIN  8
#define AV_MOVE_MIN  0.02f                     /* world units a game frame: moving */
#define AV_TURN      0.2f                      /* of the way to the gaze a vblank */
#define AV_FADE      6                         /* game frames to cross-fade the legs */
#define AV_HYST      0.17f                     /* radians past a 45-degree border before the legs change */
enum { AV_FWD, AV_LEFT, AV_RIGHT, AV_BACK, AV_DIRS };
static const char *const AV_DIR_NAME[AV_DIRS] = { "forward", "left", "right", "back" };
typedef struct { int n; float speed; float m[AV_CLIP_MAX][AV_LEG_N][16]; } av_clip;
static av_clip  g_av_clip[AV_DIRS];
static int      g_av_learned[AV_DIRS];         /* this session */
static float    g_av_rec[AV_REC_N][AV_LEG_N][16], g_av_rec_speed[AV_REC_N];
static int      g_av_rec_n, g_av_rec_dir = -1;
static char     g_av_file[600];
static int      g_av_on, g_av_still;           /* vr_body_avatar */
static float    g_gaze[3];                     /* vr_body_gaze: view space */
static uint64_t g_gaze_vb, g_av_vb = ~0ull;
static float    g_av_yb, g_av_beta;            /* the body's world yaw, its turn in the model */
static int      g_av_yb_ok;
static float    g_av_pos[3], g_av_seen[AV_LEG_N][16];
static int      g_av_pos_ok, g_av_dir = AV_FWD;
static int      g_av_src = -1, g_av_src_prev = -1, g_av_fade;   /* the legs: -1 the game's, else a clip */
static float    g_av_phase, g_av_phase_prev, g_av_speed;
static float    g_av_e13[16];                  /* third person: the head's change, eye space (rigid pieces on it) */
static int      g_av_e13_ok;
static uint32_t g_av_bind_skel[4];             /* the bind pose per skeleton (the body comes with three) */
static float    g_av_bind[4][NODES_MAX][16];
#ifndef PSP2I_PROD
static uint64_t g_av_log;
#endif

static void mul44(const float *a, const float *b, float *o);
static int  inv_affine(const float *m, float *o);

void vr_body_avatar(int on, int legs_still) {
    if (on != g_av_on || legs_still != g_av_still)
        fprintf(stderr, "vr: avatar %s\n", on ? (legs_still ? "on (the legs stand still)" : "on (the legs walk)") : "off");
    g_av_on = on;
    g_av_still = legs_still;
    if (!on) psp_gpu_set_rigid_move(0, NULL, 0.0f, NULL);
}
void vr_body_gaze(const float fwd[3]) {
    memcpy(g_gaze, fwd, sizeof g_gaze);
    g_gaze_vb = psp_sched_vblank_count();
}

static void av_save(void) {
    FILE *f = g_av_file[0] ? fopen(g_av_file, "wb") : NULL;
    if (!f) { fprintf(stderr, "vr: avatar: cannot write %s\n", g_av_file); return; }
    const int hdr[2] = { 1, AV_LEG_N };
    fwrite("PSP2IVRL", 1, 8, f);
    fwrite(hdr, sizeof hdr, 1, f);
    for (int d = 0; d < AV_DIRS; d++) {
        fwrite(&g_av_clip[d].n, sizeof(int), 1, f);
        fwrite(&g_av_clip[d].speed, sizeof(float), 1, f);
        fwrite(g_av_clip[d].m, sizeof g_av_clip[d].m[0], (size_t)g_av_clip[d].n, f);
    }
    fclose(f);
}
void vr_body_legs_file(const char *path) {
    snprintf(g_av_file, sizeof g_av_file, "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[8];
    int hdr[2], ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PSP2IVRL", 8) && fread(hdr, sizeof hdr, 1, f) == 1 &&
                     hdr[0] == 1 && hdr[1] == AV_LEG_N;
    for (int d = 0; d < AV_DIRS && ok; d++) {
        av_clip *c = &g_av_clip[d];
        ok = fread(&c->n, sizeof(int), 1, f) == 1 && fread(&c->speed, sizeof(float), 1, f) == 1 && c->n >= 0 &&
             c->n <= AV_CLIP_MAX && fread(c->m, sizeof c->m[0], (size_t)c->n, f) == (size_t)c->n;
    }
    fclose(f);
    if (!ok) { memset(g_av_clip, 0, sizeof g_av_clip); fprintf(stderr, "vr: avatar: %s is not a legs file; ignored\n", path); return; }
    fprintf(stderr, "vr: avatar: legs from %s (forward %d, left %d, right %d, back %d frames)\n", path, g_av_clip[0].n,
            g_av_clip[1].n, g_av_clip[2].n, g_av_clip[3].n);
}

/* Which way a move goes, from an angle (0 ahead, + to the left); with
 * `cur`, it changes only past the border by AV_HYST. */
static int av_dir_of(float a, int cur) {
    const float q = 0.7853982f;
    if (cur >= 0) {
        const float c = cur == AV_FWD ? 0.0f : cur == AV_LEFT ? 2.0f * q : cur == AV_RIGHT ? -2.0f * q : 4.0f * q;
        if (fabsf(wrap_pi(a - c)) < q + AV_HYST) return cur;
    }
    return fabsf(a) <= q ? AV_FWD : fabsf(a) >= 3.0f * q ? AV_BACK : a > 0 ? AV_LEFT : AV_RIGHT;
}

/* The recording of one direction is full: one gait cycle out of it. */
static void av_cycle(int d) {
    float t5[AV_REC_N][3], t8[AV_REC_N][3], lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < AV_REC_N; i++) {
        memcpy(t5[i], &g_av_rec[i][4][12], sizeof t5[i]);             /* the ankles, 5 and 8 */
        memcpy(t8[i], &g_av_rec[i][7][12], sizeof t8[i]);
        const float s = (t5[i][0] - t8[i][0]) + (t5[i][2] - t8[i][2]);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    int best = 0;
    float best_e = 1e30f;
    for (int L = AV_CLIP_MIN; L <= AV_CLIP_MAX; L++) {
        float e = 0.0f;
        for (int i = AV_REC_N - L; i < AV_REC_N; i++)
            for (int k = 0; k < 3; k++) e += fabsf(t5[i][k] - t5[i - L][k]) + fabsf(t8[i][k] - t8[i - L][k]);
        e /= (float)L;
        if (e < best_e) { best_e = e; best = L; }
    }
    const float amp = hi - lo;
    if (!best || amp < 0.5f || best_e > 0.25f * amp) {
        fprintf(stderr, "vr: avatar: the %s walk wasn't steady (stride %.2f, best cycle %d frames off by %.2f): not kept\n",
                AV_DIR_NAME[d], (double)amp, best, (double)best_e);
        return;
    }
    av_clip *c = &g_av_clip[d];
    c->n = best;
    c->speed = 0.0f;
    for (int k = 0; k < best; k++) {
        memcpy(c->m[k], g_av_rec[AV_REC_N - best + k], sizeof c->m[k]);
        c->speed += g_av_rec_speed[AV_REC_N - best + k] / (float)best;
    }
    g_av_learned[d] = 1;
    fprintf(stderr, "vr: avatar: learned the %s walk (%d frames a cycle, off by %.2f, stride %.2f, %.3f a frame): saved\n",
            AV_DIR_NAME[d], best, (double)best_e, (double)amp, (double)c->speed);
    av_save();
}

/* One sample of the game's legs for the direction being learned: strafes
 * (left, right, back) with R held, forward without it. */
static void av_learn(uint32_t nodes, int r_held, int moving, int dir, float speed) {
    const int d = !moving ? -1 : dir == AV_FWD ? (r_held ? -1 : AV_FWD) : (r_held ? dir : -1);
    if (d != g_av_rec_dir) { g_av_rec_n = 0; g_av_rec_dir = d; }
    if (d < 0 || g_av_learned[d]) return;
    float a0[16], ai[16], nb[16];
    node_get(nodes, 0, a0);
    if (!inv_affine(a0, ai)) return;
    for (int b = 0; b < AV_LEG_N; b++) {
        node_get(nodes, 1 + b, nb);
        mul44(ai, nb, g_av_rec[g_av_rec_n][b]);
    }
    g_av_rec_speed[g_av_rec_n] = speed;
    if (++g_av_rec_n == AV_REC_N) { av_cycle(d); g_av_rec_n = 0; }
}

/* Once a vblank, at the body's first palette (its nodes as animated): the
 * body's turn to the gaze, the walk's direction from it, the legs' source
 * and phase, and the learning. */
static void avatar_frame(uint32_t model, uint32_t nodes, int n) {
    const uint64_t vb = psp_sched_vblank_count();
    if (vb == g_av_vb) return;
    g_av_vb = vb;
    g_av_live = 0;
    g_av_e13_ok = 0;
    const uint32_t pl = gamelog_local_player();
    float Mb[16], Mi[16];
    for (uint32_t i = 0; i < 16; i++) Mb[i] = psp_read_f32(model + 32u + 4u * i);
    const float fx = pl ? psp_read_f32(pl + 0x90u) : 0.0f, fz = pl ? psp_read_f32(pl + 0x98u) : 0.0f;
    const float fl = sqrtf(fx * fx + fz * fz);
    if (!g_av_on || !g_arms_on || !pl || n < AV_UPPER_END || vb - g_gaze_vb > 6 || fl < 1e-4f || !inv_affine(Mb, Mi)) {
        g_av_yb_ok = 0;
        psp_gpu_set_rigid_move(0, NULL, 0.0f, NULL);
        return;
    }
    /* The gaze in the model, level: its yaw there, and in the world (the
     * model turned as the game's facing). The body eases to it in the world,
     * so the game turning the character doesn't swing the drawn body. */
    float gm[3];
    for (int r = 0; r < 3; r++) gm[r] = Mi[r] * g_gaze[0] + Mi[4 + r] * g_gaze[1] + Mi[8 + r] * g_gaze[2];
    const float ym = atan2f(fx, fz);
    if (gm[0] * gm[0] + gm[2] * gm[2] > 1e-3f * (gm[0] * gm[0] + gm[1] * gm[1] + gm[2] * gm[2])) {   /* not straight down */
        const float yg = wrap_pi(atan2f(gm[0], gm[2]) + ym);
        g_av_yb = g_av_yb_ok ? wrap_pi(g_av_yb + AV_TURN * wrap_pi(yg - g_av_yb)) : yg;
        g_av_yb_ok = 1;
        g_av_lag = wrap_pi(yg - g_av_yb);
    }
    if (!g_av_yb_ok) return;
    g_av_beta = wrap_pi(g_av_yb - ym);
    /* A new game frame (the legs changed): the move since the last one. */
    int fresh = 0;
    for (int b = 0; b < AV_LEG_N && !fresh; b++) {
        float m[16];
        node_get(nodes, 1 + b, m);
        fresh = memcmp(m, g_av_seen[b], sizeof m) != 0;
    }
    if (fresh) {
        for (int b = 0; b < AV_LEG_N; b++) node_get(nodes, 1 + b, g_av_seen[b]);
        float p[3], v[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) p[k] = psp_read_f32(pl + 0x4B0u + 4u * (uint32_t)k);
        if (g_av_pos_ok) for (int k = 0; k < 3; k++) v[k] = p[k] - g_av_pos[k];
        memcpy(g_av_pos, p, sizeof p);
        g_av_pos_ok = 1;
        const float s = sqrtf(v[0] * v[0] + v[2] * v[2]);
        const int moving = s > AV_MOVE_MIN && s < 5.0f;               /* not a warp */
        /* Model axes: +z the facing, +x the character's left (y x z). */
        const float left = (v[0] * fz - v[2] * fx) / fl, ahead = (v[0] * fx + v[2] * fz) / fl;
        const float a_game = atan2f(left, ahead);
        const int game_dir = av_dir_of(a_game, -1);
        if (moving) g_av_dir = av_dir_of(wrap_pi(a_game - g_av_beta), g_av_dir);
        av_learn(nodes, psp_read32(pl + 0x214u) != 0, moving, game_dir, s);   /* +0x214: R held (ATT_STATE) */
        /* The legs: a learned walk when you walk that way from the body and
         * the game's legs don't (it turned to the walk, or R strafes). */
        int src = -1;
        if (moving && !g_av_still) {
            if (g_av_dir != AV_FWD && g_av_clip[g_av_dir].n) src = g_av_dir;
            else if (g_av_dir == AV_FWD && game_dir != AV_FWD && g_av_clip[AV_FWD].n) src = AV_FWD;
        }
        if (src != g_av_src) {
            g_av_src_prev = g_av_src;
            g_av_phase_prev = g_av_phase;
            g_av_src = src;
            g_av_phase = 0.0f;
            g_av_fade = AV_FADE;
        } else if (g_av_fade > 0) g_av_fade--;
        g_av_speed = s;
        for (int w = 0; w < 2; w++) {                                  /* the phases: at your speed */
            const int c = w ? g_av_src_prev : g_av_src;
            float *ph = w ? &g_av_phase_prev : &g_av_phase;
            if (c < 0 || !g_av_clip[c].n) continue;
            *ph += g_av_clip[c].speed > 1e-4f ? s / g_av_clip[c].speed : 1.0f;
            *ph -= (float)g_av_clip[c].n * floorf(*ph / (float)g_av_clip[c].n);
        }
    }
    g_av_live = 1;
#ifndef PSP2I_PROD
    if (vb - g_av_log >= 300) {
        g_av_log = vb;
        fprintf(stderr, "vr: avatar: body turned %+.0f deg from the game's facing (the gaze %+.0f ahead of it); moving %.3f a"
                        " frame %s of the body; legs %s%s; learned%s%s%s%s\n", (double)(g_av_beta * 57.29578f),
                (double)(g_av_lag * 57.29578f), (double)g_av_speed, AV_DIR_NAME[g_av_dir],
                g_av_still ? "standing" : g_av_src < 0 ? "the game's" : AV_DIR_NAME[g_av_src], g_av_src >= 0 ? " (learned)" : "",
                g_av_clip[0].n ? " forward" : "", g_av_clip[1].n ? " left" : "", g_av_clip[2].n ? " right" : "",
                g_av_clip[3].n ? " back" : "");
    }
#endif
}

/* The skeleton's bind pose (model space, unscaled: the inverse of each
 * record's inverse bind; flag 0x8 = none, the identity), cached. */
static const float (*av_bind(uint32_t skel, uint32_t recs, int n))[16] {
    int k = 0;
    while (k < 4 && g_av_bind_skel[k] && g_av_bind_skel[k] != skel) k++;
    if (k == 4) { k = 3; g_av_bind_skel[3] = 0; }
    if (g_av_bind_skel[k] != skel) {
        for (int b = 0; b < n; b++) {
            static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            const uint32_t r = recs + 144u * (uint32_t)b;
            float ib[16];
            for (uint32_t i = 0; i < 16; i++) ib[i] = psp_read_f32(r + 48u + 4u * i);
            if ((psp_read32(r) & 0x8u) || !inv_affine(ib, g_av_bind[k][b])) memcpy(g_av_bind[k][b], I, sizeof I);
            for (int c = 0; c < 3; c++) {                                 /* rotation only */
                const float l = len3(&g_av_bind[k][b][c * 4]);
                if (l > 1e-6f) for (int j = 0; j < 3; j++) g_av_bind[k][b][c * 4 + j] /= l;
            }
        }
        g_av_bind_skel[k] = skel;
    }
    return (const float (*)[16])g_av_bind[k];
}

/* Node b in its bind pose, on its posed parent: the rotation the bind has
 * against `ref` (its parent, or the root for the spine, so the hips' sway
 * doesn't tilt the chest), the animated node's scale, and the translation
 * from its parent by `off` (its offset in the parent's frame). */
static void av_rest_node(float (*P)[16], const float (*B)[16], const float *A_b, int b, int ref, int p, const float *off) {
    float Rr[9], Br[9], Bb[9], t[9], R[9];
    for (int c = 0; c < 3; c++) {
        const float l = len3(&P[ref][c * 4]);
        for (int j = 0; j < 3; j++) {
            Rr[j * 3 + c] = l > 1e-6f ? P[ref][c * 4 + j] / l : 0.0f;
            Br[j * 3 + c] = B[ref][c * 4 + j];
            Bb[j * 3 + c] = B[b][c * 4 + j];
        }
    }
    mat3_tmul(Br, Bb, t);                                              /* the bind's rotation against ref */
    mat3_mul(Rr, t, R);
    for (int c = 0; c < 3; c++) {
        const float s = len3(&A_b[c * 4]);
        for (int j = 0; j < 3; j++) P[b][c * 4 + j] = R[j * 3 + c] * s;
        P[b][c * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; j++) P[b][12 + j] = P[p][j] * off[0] + P[p][4 + j] * off[1] + P[p][8 + j] * off[2] + P[p][12 + j];
    P[b][15] = 1.0f;
}

/* A learned walk's legs at a phase (the samples blended), on the root. */
static void av_clip_at(const av_clip *c, float ph, const float *a0, int b, float *o) {
    const int i = (int)ph % c->n, j = (i + 1) % c->n;
    const float f = ph - floorf(ph);
    float m[16];
    for (int k = 0; k < 16; k++) m[k] = c->m[i][b][k] + f * (c->m[j][b][k] - c->m[i][b][k]);
    mul44(a0, m, o);
}

/* The avatar's pose into the nodes (they are saved: g_arms_saved). */
static void avatar_pose(uint32_t nodes, uint32_t skel, uint32_t recs, int n) {
    const float (*A)[16] = (const float (*)[16])g_arms_saved;
    const float (*B)[16] = av_bind(skel, recs, n);
    static float P[NODES_MAX][16];
    memcpy(P, A, sizeof P[0] * (size_t)n);
    /* The legs: the game's, a learned walk (cross-faded on a change), or
     * the bind pose (still). */
    for (int b = 1; b <= AV_LEG_N; b++) {
        const int p = (int16_t)psp_read16(recs + 144u * (uint32_t)b + 6u);
        if (p < 0 || p >= b) continue;
        if (g_av_still) {
            float off[3], Bi[16];
            if (!inv_affine(B[p], Bi)) continue;
            for (int j = 0; j < 3; j++) off[j] = Bi[j] * B[b][12] + Bi[4 + j] * B[b][13] + Bi[8 + j] * B[b][14] + Bi[12 + j];
            av_rest_node(P, B, A[b], b, p, p, off);
            continue;
        }
        float cur[16], old[16];
        if (g_av_src >= 0 && g_av_clip[g_av_src].n) av_clip_at(&g_av_clip[g_av_src], g_av_phase, A[0], b - 1, cur);
        else memcpy(cur, A[b], sizeof cur);
        if (g_av_fade > 0) {
            if (g_av_src_prev >= 0 && g_av_clip[g_av_src_prev].n)
                av_clip_at(&g_av_clip[g_av_src_prev], g_av_phase_prev, A[0], b - 1, old);
            else memcpy(old, A[b], sizeof old);
            const float w = (float)g_av_fade / (float)AV_FADE;          /* of the old */
            for (int k = 0; k < 16; k++) cur[k] += w * (old[k] - cur[k]);
        }
        memcpy(P[b], cur, sizeof cur);
    }
    /* The upper body in its bind pose, each node at its animated offset from
     * its parent (the bones keep their lengths). */
    for (int b = AV_SPINE; b < AV_UPPER_END && b < n; b++) {
        const int p = (int16_t)psp_read16(recs + 144u * (uint32_t)b + 6u);
        float Ai[16], off[3];
        if (p < 0 || p >= b || !inv_affine(A[p], Ai)) continue;
        for (int j = 0; j < 3; j++) off[j] = Ai[j] * A[b][12] + Ai[4 + j] * A[b][13] + Ai[8 + j] * A[b][14] + Ai[12 + j];
        av_rest_node(P, B, A[b], b, b == AV_SPINE ? 0 : p, p, off);
    }
    for (int b = 0; b < n; b++) node_put(nodes, b, P[b]);
    /* All of it turned to the gaze, about the root. */
    const float c = cosf(g_av_beta), s = sinf(g_av_beta);
    const float Ry[9] = { c, 0, s, 0, 1, 0, -s, 0, c };
    turn_below(nodes, recs, n, 0, Ry, &A[0][12]);
}

/* Poses both arms; returns 1 with the nodes saved for arms_restore. */
static int arms_pose(uint32_t model, uint32_t skel, int arms) {
    const int n = (int)psp_read32(skel + 40u);
    const uint32_t nodes = psp_read32(model + 100u), recs = psp_read32(skel + 48u);
    if (n < 23 || n > NODES_MAX || !user_ptr(nodes) || !user_ptr(recs)) return 0;
    for (int b = 0; b < n; b++) node_get(nodes, b, g_arms_saved[b]);
    g_arms_n_saved = n;
    const int av = arms && g_av_live && n >= AV_UPPER_END;
    if (av) avatar_pose(nodes, skel, recs, n);
    /* The body's turn in the model (the targets and the hands' rotation are
     * in the body's frame). */
    const float bc = av ? cosf(g_av_beta) : 1.0f, bs = av ? sinf(g_av_beta) : 0.0f;
    const float Rb[9] = { bc, 0, bs, 0, 1, 0, -bs, 0, bc };
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
        const float ob[3] = { -o[0], o[1], o[2] };
        float om[3];
        for (int k = 0; k < 3; k++) om[k] = Rb[k * 3] * ob[0] + Rb[k * 3 + 1] * ob[1] + Rb[k * 3 + 2] * ob[2];
        const float T0[3] = { S[0] + upm * om[0], S[1] + upm * om[1], S[2] + upm * om[2] };
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
        float C[9];
        mat3_mul(Rb, g_ctrl_rot[hd], C);                       /* the controller in the model */
        if (g_cal_req) {
            mat3_tmul(C, H, g_cal[hd]);                        /* C^T H: the hand in the controller's frame */
            g_cal_ok[hd] = 1;
        } else if (g_cal_ok[hd]) {
            float Ht[9];
            mat3_mul(C, g_cal[hd], Ht);                        /* where the hand should be: C x the offset */
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
static float    g_att_posed[NODES_MAX][16];    /* ... and the node as drawn (the avatar's fixed weapons) */
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
    int      grip_r, grip_told, new_told;      /* the grip is from R held (a gun's aim); dev logs said once */
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
/* Dev, with the avatar: the weapons fixed in the hand, every 2 s: drawn at
 * their grip, followed though far from the hand, and new matrices next to
 * an animated hand that nothing holds yet (a Photon Art's own copy?). */
static unsigned g_wd_fixed, g_wd_far, g_wd_new;
static uint64_t g_wd_log;
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
        memcpy(g_att_posed[b], now, sizeof now);
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
    att_rec *a = NULL, *old = &g_att[0];
    for (int i = 0; i < ATT_MAX && !a; i++) if (g_att[i].key == mat) a = &g_att[i];
    /* A weapon fixed in the hand (the avatar) stays followed however far the
     * game's animation throws it from the hand (Photon Arts). */
    const int far = near > ATT_NEAR * ATT_NEAR;
    if (far && !(a && g_av_live && a->grip_node >= 0)) return 0;
#ifndef PSP2I_PROD
    g_ad_near++;
    if (far) g_wd_far++;
#endif
    const uint64_t vb = psp_sched_vblank_count();
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
    /* With the avatar a weapon is fixed in the hand: once its grip is
     * known, it is drawn there whatever the game's animation does with it
     * (spins, throws and swings of attacks and Photon Arts: user, "stop the
     * animations for weapons that are held in hands too"). */
    const int fixed = g_av_live && a->grip_node >= 0;
    /* Let go only of what has left the hand for good (put away, dropped):
     * a gun coming up into the hand drifts for a few frames. */
    if (a->node[st] >= 0) a->off[st] = a->err[a->node[st]] > 4.0f * ATT_ERR_OK ? a->off[st] + 1 : 0;
    if (a->node[st] >= 0 && a->off[st] > 30 && !fixed) {
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
        if (!fixed) return 0;                                  /* use_L is the old node's: from the next frame */
        have = 0;
    }
    /* First person: a rigid piece at the head that no hand holds (the face
     * and head: they stayed with the body's node 13 and the container's
     * model hidden, which took only the hair and the accessories, user) is
     * drawn shrunk away. ATT_NODE[ATT_HEAD_I] is node 13. */
    if ((g_hide_head || g_av_e13_ok) && a->node[0] < 0 && a->node[1] < 0 && a->grip_node < 0) {
        mul44(Wi[ATT_HEAD_I], Mw, L);
        if (!g_hide_head && L[12] * L[12] + L[13] * L[13] + L[14] * L[14] < ATT_HEAD_NEAR * ATT_HEAD_NEAR) {
            /* Third person, the avatar: it goes with the drawn head. */
            float out[16];
            mul44(g_av_e13, Mw, out);
            for (uint32_t i = 0; i < 16; i++) { keep[i] = Mw[i]; psp_write_f32(mat + 4u * i, out[i]); }
            return 1;
        }
        if (g_hide_head && L[12] * L[12] + L[13] * L[13] + L[14] * L[14] < ATT_HEAD_NEAR * ATT_HEAD_NEAR) {
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
    /* With the avatar the grip is taken once, the first time it sits still
     * in the hand (with R again: a gun's aim), and kept. */
    int nb_i;
    if (a->node[st] >= 0 && have && a->err[a->node[st]] <= 4.0f * ATT_ERR_OK &&
        (!fixed || (st && a->err[a->node[st]] <= ATT_ERR_OK && !a->grip_r))) {
        nb_i = ATT_NODE[a->node[st]];
        if (st || (g_av_live && a->grip_node < 0 && a->err[a->node[st]] <= ATT_ERR_OK)) {
            memcpy(a->grip, use_L, sizeof use_L);
            a->grip_node = a->node[st];
            if (st) a->grip_r = 1;
            if (g_av_live && !a->grip_told) {
                a->grip_told = 1;
                fprintf(stderr, "vr: arms: the matrix at 0x%08X is fixed in the hand (node %d)\n", mat, ATT_NODE[a->grip_node]);
            }
        }
    } else if (fixed || (!st && a->node[0] < 0 && a->grip_node >= 0)) {
        nb_i = ATT_NODE[a->grip_node];
        memcpy(use_L, a->grip, sizeof use_L);
    } else {
#ifndef PSP2I_PROD
        if (g_av_live && best >= 0 && dist < ATT_HAND_NEAR) {
            g_wd_new++;
            if (!a->new_told) {
                a->new_told = 1;
                fprintf(stderr, "vr: arms: new matrix 0x%08X (obj 0x%08X) %.2f from node %d, not held yet (drift %.3f, seen %d"
                                " frames): drawn as the game has it\n", mat, g_ad_cur_obj, (double)dist, ATT_NODE[best],
                        (double)a->err[best], a->frames);
            }
        }
#endif
        return 0;
    }
    if (!g_att_moved[nb_i]) return 0;
    /* M' = Mb x (D x node) x L: the posed node, the same attachment. */
    const uint32_t nodes = psp_read32(body + 100u);
    float nb[16], np[16], t[16], out[16];
    node_get(nodes, nb_i, nb);
    /* A fixed weapon: on the hand as drawn, nothing of this frame's
     * animation (the weapon may be copied before the body's pose is redone:
     * D of the last pose x this frame's animated hand let the Photon Art's
     * hand motion through, user: "it still in hand but seems to try to move
     * to the animation sequence"). */
    if (fixed) memcpy(np, g_att_posed[nb_i], sizeof np);
    else mul44(g_att_d[nb_i], nb, np);
    mul44(Mb, np, t);
    mul44(t, use_L, out);
    for (uint32_t i = 0; i < 16; i++) { keep[i] = Mw[i]; psp_write_f32(mat + 4u * i, out[i]); }
#ifndef PSP2I_PROD
    if (fixed) g_wd_fixed++;
#endif
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
#ifndef PSP2I_PROD
    if (g_av_live && psp_sched_vblank_count() - g_wd_log >= 120) {
        g_wd_log = psp_sched_vblank_count();
        fprintf(stderr, "vr: arms: in 2 s: %u weapon draws at their fixed grip (%u of them far from the game's hand), %u draws"
                        " of matrices by a hand not held yet\n", g_wd_fixed, g_wd_far, g_wd_new);
        g_wd_fixed = g_wd_far = g_wd_new = 0;
    }
#endif
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
            if (g_arms_on) {
                avatar_frame(model, psp_read32(model + 100u), (int)psp_read32(skel + 40u));
                arms_frame();
            }
            const int arms = g_arms_on && (g_arms_ok[0] || g_arms_ok[1]);
            if ((arms || g_hide_head) && arms_pose(model, skel, arms)) {
                const uint32_t nodes = psp_read32(model + 100u);
                if (arms) att_keep_pose(nodes, g_arms_n_saved);    /* before the head: only the arms' changes */
                else g_att_dn = 0;
                if (g_hide_head && g_arms_n_saved > HEAD_NODE) {   /* before it is zeroed */
                    float hd[16];
                    node_get(nodes, HEAD_NODE, hd);
                    head_rigid_mark(model, g_arms_saved[HEAD_NODE], hd, arms && g_av_live ? g_av_beta : 0.0f);
                }
                if (arms && g_av_live && !g_hide_head && g_arms_n_saved > HEAD_NODE) {
                    /* Third person: what the game hangs on the animated head
                     * (the rigid head, hair pieces) goes where the avatar's
                     * head is: E = Mb x D13 x inv(Mb), eye space. */
                    float Mb[16], Mi[16], t[16], W[3];
                    for (uint32_t i = 0; i < 16; i++) Mb[i] = psp_read_f32(model + 32u + 4u * i);
                    if (inv_affine(Mb, Mi)) {
                        mul44(Mb, g_att_d[HEAD_NODE], t);
                        mul44(t, Mi, g_av_e13);
                        g_av_e13_ok = 1;
                        const float *a = g_arms_saved[HEAD_NODE];
                        for (int r = 0; r < 3; r++) W[r] = Mb[r] * a[12] + Mb[4 + r] * a[13] + Mb[8 + r] * a[14] + Mb[12 + r];
                        psp_gpu_set_rigid_move(1, W, HEAD_RIGID_R, g_av_e13);
                    }
                } else psp_gpu_set_rigid_move(0, NULL, 0.0f, NULL);
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
