/* The VR probe: see vr_probe.h. Runs on the game thread (the GE draws there,
 * and the vblank hook runs there), so it needs no locking. */
#include "vr_probe.h"
#include "../host/gamelog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>

#define MAX_VIEWS 16
#define MAX_SKIN  256
#define USER_LO   0x08800000u          /* user memory: where the game's objects live */
#define USER_HI   0x0A000000u

typedef struct { float view[12], proj[16]; int verts, draws; } view_rec;
typedef struct {
    float world[12], view[12], bone0[12];
    int nbones, nverts, draws;
    uint32_t vaddr, tex;
} skin_rec;
typedef struct {
    uint64_t flip;
    view_rec v[MAX_VIEWS]; int nv;
    skin_rec s[MAX_SKIN];  int ns;
    int draws2d, draws3d;
    int world_ident, world_other;      /* unskinned 3D draws by world matrix */
    float big_world[12]; int big_verts; /* the world matrix of the largest unskinned draw */
} frame_rec;

static frame_rec g_cur, g_last;
static int g_have_last;
static int g_report_due;
static int g_every = -1;
static char g_path[700];

/* ---- matrices (the GE's 4x3 column-major: out = R * in + t, R[i][j] = m[j*3+i]) */

static void cam_from_view(const float *m, float pos[3], float fwd[3]) {
    for (int j = 0; j < 3; j++) pos[j] = -(m[j * 3 + 0] * m[9] + m[j * 3 + 1] * m[10] + m[j * 3 + 2] * m[11]);
    fwd[0] = -m[2]; fwd[1] = -m[5]; fwd[2] = -m[8];      /* view -Z in world */
}

static void mul43_pt(const float *m, const float in[3], float out[3]) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

static int same(const float *a, const float *b, int n, float eps) {
    for (int i = 0; i < n; i++) if (fabsf(a[i] - b[i]) > eps) return 0;
    return 1;
}

/* ---- the observer ------------------------------------------------------------- */

static void observe(const psp_gpu_draw_info *d) {
    const uint64_t flips = psp_display_flips();
    if (flips != g_cur.flip) {
        if (g_cur.nv) { g_last = g_cur; g_have_last = 1; }
        g_cur.nv = g_cur.ns = g_cur.draws2d = g_cur.draws3d = 0;
        g_cur.world_ident = g_cur.world_other = g_cur.big_verts = 0;
        g_cur.flip = flips;
    }
    if (d->through) { g_cur.draws2d++; return; }
    g_cur.draws3d++;
    int k = 0;
    while (k < g_cur.nv && !same(g_cur.v[k].view, d->view, 12, 1e-5f)) k++;
    if (k == g_cur.nv && k < MAX_VIEWS) {
        memcpy(g_cur.v[k].view, d->view, sizeof g_cur.v[k].view);
        memcpy(g_cur.v[k].proj, d->proj, sizeof g_cur.v[k].proj);
        g_cur.v[k].verts = g_cur.v[k].draws = 0;
        g_cur.nv++;
    }
    if (k < MAX_VIEWS) { g_cur.v[k].verts += d->nverts; g_cur.v[k].draws++; }
    if (!d->bones) {
        static const float ident[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        if (same(d->world, ident, 12, 1e-5f)) g_cur.world_ident++; else g_cur.world_other++;
        if (d->nverts > g_cur.big_verts) { g_cur.big_verts = d->nverts; memcpy(g_cur.big_world, d->world, sizeof g_cur.big_world); }
        return;
    }
    /* One entry per model: draws with the same world matrix and first bone. */
    int s = 0;
    while (s < g_cur.ns && !(same(g_cur.s[s].world, d->world, 12, 1e-5f) && same(g_cur.s[s].bone0, d->bones, 12, 1e-5f))) s++;
    if (s == g_cur.ns) {
        if (s >= MAX_SKIN) return;
        skin_rec *r = &g_cur.s[s];
        memcpy(r->world, d->world, sizeof r->world);
        memcpy(r->view, d->view, sizeof r->view);
        memcpy(r->bone0, d->bones, sizeof r->bone0);
        r->nbones = d->nbones;
        r->nverts = r->draws = 0;
        r->vaddr = d->vaddr;
        r->tex = d->tex_addr;
        g_cur.ns++;
    }
    g_cur.s[s].nverts += d->nverts;
    g_cur.s[s].draws++;
}

/* The scene camera of a frame: the view matrix with the most vertices. */
static const view_rec *scene_view(const frame_rec *f) {
    const view_rec *best = NULL;
    for (int i = 0; i < f->nv; i++) if (!best || f->v[i].verts > best->verts) best = &f->v[i];
    return best;
}

int vr_probe_camera(float pos[3], float fwd[3], float *fovy_deg) {
    const view_rec *v = g_have_last ? scene_view(&g_last) : NULL;
    if (!v) return 0;
    cam_from_view(v->view, pos, fwd);
    if (fovy_deg) *fovy_deg = v->proj[5] != 0.0f ? 2.0f * atanf(1.0f / fabsf(v->proj[5])) * 57.29578f : 0.0f;
    return 1;
}

/* ---- guest RAM search --------------------------------------------------------- */

static const uint8_t *user_ram(void) { return (const uint8_t *)psp_mem_ptr(USER_LO, USER_HI - USER_LO); }

static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }

static int near_val(float a, float b) {
    const float eps = fmaxf(0.01f, fabsf(b) * 1e-4f);
    return fabsf(a - b) <= eps;
}

/* Addresses holding x, y, z as three consecutive floats. */
static void find_vec3(FILE *f, const char *what, const float v[3]) {
    const uint8_t *ram = user_ram();
    if (!ram) { fprintf(f, "  (guest RAM not mapped)\n"); return; }
    int n = 0;
    for (uint32_t off = 0; off + 12 <= USER_HI - USER_LO; off += 4) {
        if (!near_val(rdf(ram + off), v[0])) continue;
        if (!near_val(rdf(ram + off + 4), v[1]) || !near_val(rdf(ram + off + 8), v[2])) continue;
        if (n < 24) fprintf(f, "  %s at 0x%08X\n", what, USER_LO + off);
        n++;
    }
    fprintf(f, "  %s: %d place(s)%s\n", what, n, n > 24 ? " (first 24 shown)" : "");
}

/* Addresses holding the view matrix: 12 floats in a row, or a 4x4 with each
 * column padded to 4 floats (the VFPU's layout). */
static void find_matrix(FILE *f, const float *m) {
    const uint8_t *ram = user_ram();
    if (!ram) return;
    int n = 0;
    for (uint32_t off = 0; off + 64 <= USER_HI - USER_LO; off += 4) {
        if (!near_val(rdf(ram + off), m[0]) || !near_val(rdf(ram + off + 4), m[1]) || !near_val(rdf(ram + off + 8), m[2]))
            continue;
        int packed = 1, padded = 1;
        for (int i = 3; i < 12 && packed; i++) packed = near_val(rdf(ram + off + 4 * (uint32_t)i), m[i]);
        for (int c = 1; c < 4 && padded; c++)
            for (int r = 0; r < 3 && padded; r++) padded = near_val(rdf(ram + off + 16 * (uint32_t)c + 4 * (uint32_t)r), m[c * 3 + r]);
        if ((packed || padded) && n++ < 24)
            fprintf(f, "  view matrix (%s) at 0x%08X\n", packed ? "4x3 packed" : "4x4 padded", USER_LO + off);
    }
    fprintf(f, "  view matrix: %d place(s)\n", n);
}

/* ---- the camera matrix in RAM --------------------------------------------------
 * PSP2i hands the GE identity view and world matrices: the camera is folded
 * into the bone matrices (and the world matrices of unskinned draws) on the
 * CPU. So the camera has to be found in RAM: rigid 4x4 matrices (VFPU layout,
 * columns padded to 4 floats, the last row 0 0 0 1) that take a position the
 * local player object holds to where the player model is drawn (eye space).
 * Object world matrices are rigid too but don't line up with the player. */

#define MAX_CAND 16
typedef struct { uint32_t addr, pl_off; int layout; float dist, eye[3], fwd[3], t[3]; } cam_cand;

/* 1 = column-major (translation in m[12..14]), 2 = row-major (stored
 * transposed; m comes back column-major), 0 = not a rigid transform. */
static int rigid44(const uint8_t *p, float m[16]) {
    memcpy(m, p, 16 * sizeof(float));
    for (int i = 0; i < 16; i++) if (!(fabsf(m[i]) < 1e5f)) return 0;      /* also rejects NaN */
    if (m[15] != 1.0f) return 0;
    int layout = 1;
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f) {
        if (m[12] != 0.0f || m[13] != 0.0f || m[14] != 0.0f) return 0;
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++) { const float t = m[i * 4 + j]; m[i * 4 + j] = m[j * 4 + i]; m[j * 4 + i] = t; }
        layout = 2;
    }
    for (int a = 0; a < 3; a++)
        for (int b = a; b < 3; b++) {
            const float d = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
            if (fabsf(d - (a == b ? 1.0f : 0.0f)) > 2e-3f) return 0;
        }
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (det < 0.5f) return 0;
    if (fabsf(m[0] - 1.0f) < 1e-4f && fabsf(m[5] - 1.0f) < 1e-4f && fabsf(m[10] - 1.0f) < 1e-4f) return 0;   /* identity rotation */
    return layout;
}

static void find_camera(FILE *f, const float player_eye[3]) {
    const uint8_t *ram = user_ram();
    const uint32_t pl = gamelog_local_player();
    if (!ram) return;
    if (!pl || pl < USER_LO || pl + 0x2000 > USER_HI) {
        fprintf(f, "camera matrix search: no local player object yet\n");
        return;
    }
    const uint8_t *po = ram + (pl - USER_LO);
    cam_cand c[MAX_CAND];
    int nc = 0, nrigid = 0;
    for (uint32_t off = 0; off + 64 <= USER_HI - USER_LO; off += 16) {
        float m[16];
        const int layout = rigid44(ram + off, m);
        if (!layout) continue;
        nrigid++;
        float best = 1e30f;
        uint32_t best_off = 0;
        for (uint32_t k = 0; k + 12 <= 0x2000; k += 4) {
            const float x = rdf(po + k), y = rdf(po + k + 4), z = rdf(po + k + 8);
            if (!(fabsf(x) < 1e5f && fabsf(y) < 1e5f && fabsf(z) < 1e5f) || (x == 0 && y == 0 && z == 0)) continue;
            const float q[3] = { m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                                 m[2] * x + m[6] * y + m[10] * z + m[14] };
            const float dx = q[0] - player_eye[0], dy = q[1] - player_eye[1], dz = q[2] - player_eye[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best) { best = d; best_off = k; }
        }
        if (best > 10.0f) continue;
        int slot = nc < MAX_CAND ? nc++ : -1;
        if (slot < 0) {                                   /* full: replace the worst if this is better */
            slot = 0;
            for (int i = 1; i < MAX_CAND; i++) if (c[i].dist > c[slot].dist) slot = i;
            if (c[slot].dist <= best) continue;
        }
        cam_cand *e = &c[slot];
        e->addr = USER_LO + off;
        e->pl_off = best_off;
        e->dist = best;
        e->layout = layout;
        for (int j = 0; j < 3; j++) {                     /* eye = -R^T t; forward = -(row 2 of R) */
            e->t[j] = m[12 + j];
            e->eye[j] = -(m[j * 4] * m[12] + m[j * 4 + 1] * m[13] + m[j * 4 + 2] * m[14]);
            e->fwd[j] = -m[j * 4 + 2];
        }
    }
    for (int i = 1; i < nc; i++)                          /* sort by distance */
        for (int k = i; k > 0 && c[k].dist < c[k - 1].dist; k--) { cam_cand t = c[k]; c[k] = c[k - 1]; c[k - 1] = t; }
    fprintf(f, "camera matrix search: %d rigid 4x4 matrices in RAM, %d map a float triple of the local player "
               "0x%08X to within 10 of model #0 (best first):\n", nrigid, nc, pl);
    for (int i = 0; i < nc; i++)
        fprintf(f, "  0x%08X %s: player +0x%03X off by %.3f; eye (%.3f, %.3f, %.3f) fwd (%.3f, %.3f, %.3f) t (%.3f, %.3f, %.3f)\n",
                c[i].addr, c[i].layout == 1 ? "col-major" : "row-major", c[i].pl_off, (double)c[i].dist, (double)c[i].eye[0], (double)c[i].eye[1], (double)c[i].eye[2],
                (double)c[i].fwd[0], (double)c[i].fwd[1], (double)c[i].fwd[2], (double)c[i].t[0], (double)c[i].t[1], (double)c[i].t[2]);
}

/* ---- the report ----------------------------------------------------------------- */

static void print_m43(FILE *f, const char *name, const float *m) {
    fprintf(f, "  %s:", name);
    for (int i = 0; i < 12; i++) fprintf(f, " %.4f", (double)m[i]);
    fprintf(f, "\n");
}

typedef struct { int idx; float perp, along; float pos[3]; } ranked;
static int cmp_ranked(const void *a, const void *b) {
    const float x = ((const ranked *)a)->perp, y = ((const ranked *)b)->perp;
    return x < y ? -1 : x > y;
}

static void report(void) {
    FILE *f = g_path[0] ? fopen(g_path, "a") : NULL;
    if (!f) { fprintf(stderr, "vr-probe: cannot write %s\n", g_path); return; }
    fprintf(f, "==== vr-probe at vblank %llu, flip %llu ====\n",
            (unsigned long long)psp_sched_vblank_count(), (unsigned long long)psp_display_flips());
    const view_rec *sv = g_have_last ? scene_view(&g_last) : NULL;
    if (!sv) { fprintf(f, "no 3D frame seen yet\n\n"); fclose(f); return; }
    fprintf(f, "frame: %d 3D draws, %d 2D draws, %d view matrices, %d skinned models\n",
            g_last.draws3d, g_last.draws2d, g_last.nv, g_last.ns);
    for (int i = 0; i < g_last.nv; i++) {
        float p[3], d[3];
        cam_from_view(g_last.v[i].view, p, d);
        fprintf(f, "  view %d%s: %d draws, %d verts, eye (%.3f, %.3f, %.3f) fwd (%.3f, %.3f, %.3f)\n", i,
                &g_last.v[i] == sv ? " (scene)" : "", g_last.v[i].draws, g_last.v[i].verts,
                (double)p[0], (double)p[1], (double)p[2], (double)d[0], (double)d[1], (double)d[2]);
    }
    float cam[3], fwd[3], fovy;
    vr_probe_camera(cam, fwd, &fovy);
    const float aspect = sv->proj[0] != 0.0f ? sv->proj[5] / sv->proj[0] : 0.0f;
    fprintf(f, "scene camera: eye (%.4f, %.4f, %.4f) forward (%.4f, %.4f, %.4f) fovy %.2f deg, aspect %.3f\n",
            (double)cam[0], (double)cam[1], (double)cam[2], (double)fwd[0], (double)fwd[1], (double)fwd[2],
            (double)fovy, (double)aspect);
    print_m43(f, "view", sv->view);
    fprintf(f, "  unskinned draws: %d with an identity world matrix, %d with another\n", g_last.world_ident, g_last.world_other);
    if (g_last.big_verts) print_m43(f, "world of the largest unskinned draw", g_last.big_world);
    fprintf(f, "  proj:");
    for (int i = 0; i < 16; i++) fprintf(f, " %.4f", (double)sv->proj[i]);
    fprintf(f, "\n");
    find_vec3(f, "camera position", cam);
    find_matrix(f, sv->view);

    /* Skinned models seen by the scene camera, nearest to its line of sight. */
    ranked r[MAX_SKIN];
    int n = 0;
    for (int i = 0; i < g_last.ns; i++) {
        const skin_rec *s = &g_last.s[i];
        if (!same(s->view, sv->view, 12, 1e-5f)) continue;
        const float zero[3] = { 0, 0, 0 };
        float local[3], p[3];
        mul43_pt(s->bone0, zero, local);                 /* the first bone's origin, in model space */
        mul43_pt(s->world, local, p);
        const float d[3] = { p[0] - cam[0], p[1] - cam[1], p[2] - cam[2] };
        const float t = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        const float q[3] = { d[0] - t * fwd[0], d[1] - t * fwd[1], d[2] - t * fwd[2] };
        r[n].idx = i;
        r[n].along = t;
        r[n].perp = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) + (t < 0 ? 1e6f : 0.0f);
        memcpy(r[n].pos, p, sizeof p);
        n++;
    }
    qsort(r, (size_t)n, sizeof r[0], cmp_ranked);
    fprintf(f, "skinned models by distance from the line of sight (nearest first):\n");
    for (int k = 0; k < n && k < 8; k++) {
        const skin_rec *s = &g_last.s[r[k].idx];
        fprintf(f, " #%d: %.3f off the line, %.3f ahead; draws %d, verts %d, %d weights, vertices 0x%08X, texture 0x%08X\n",
                k, (double)(r[k].perp >= 1e6f ? r[k].perp - 1e6f : r[k].perp), (double)r[k].along,
                s->draws, s->nverts, s->nbones, s->vaddr, s->tex);
        fprintf(f, "  world translation (%.4f, %.4f, %.4f); first bone origin in world (%.4f, %.4f, %.4f)\n",
                (double)s->world[9], (double)s->world[10], (double)s->world[11],
                (double)r[k].pos[0], (double)r[k].pos[1], (double)r[k].pos[2]);
        if (k < 3) {
            find_vec3(f, "world translation", &s->world[9]);
            if (!same(&s->world[9], r[k].pos, 3, 1e-3f)) find_vec3(f, "bone origin", r[k].pos);
        }
    }
    if (n > 0) find_camera(f, r[0].pos);
    fprintf(f, "\n");
    fclose(f);
    fprintf(stderr, "vr-probe: report written to %s\n", g_path);
}

/* ---- the host's side ------------------------------------------------------------- */

void vr_probe_init(const char *report_path) {
    snprintf(g_path, sizeof g_path, "%s", report_path ? report_path : "vr_probe.txt");
    memset(&g_cur, 0, sizeof g_cur);
    g_cur.flip = (uint64_t)-1;
    psp_gpu_set_draw_observer(observe);
}

void vr_probe_request(void) {
    g_report_due = 1;
    /* PSP2I_VR_PROBE_WATCH=ADDR: (re)arm the write watch on F8, so it starts
     * where the scene is (the first 32 writes get a stack in game_log.txt)
     * rather than at boot, when clears use up the hits. */
    const char *w = getenv("PSP2I_VR_PROBE_WATCH");
    if (w && *w) {
        const uint32_t a = (uint32_t)strtoul(w, NULL, 0);
        psp_mem_watch_write(a);
        fprintf(stderr, "vr-probe: write watch armed on 0x%08X\n", a);
    }
}

void vr_probe_vblank(void) {
    if (g_report_due) { g_report_due = 0; report(); }
    if (g_every < 0) { const char *e = getenv("PSP2I_VR_PROBE_EVERY"); g_every = e ? atoi(e) : 0; }
    static uint64_t last_flip;
    const uint64_t flips = psp_display_flips();
    if (g_every > 0 && flips != last_flip && flips % (uint64_t)g_every == 0) {
        last_flip = flips;
        float p[3], d[3], fov;
        if (vr_probe_camera(p, d, &fov))
            fprintf(stderr, "vr-probe: flip %llu eye (%.3f, %.3f, %.3f) fwd (%.3f, %.3f, %.3f) fovy %.1f\n",
                    (unsigned long long)flips, (double)p[0], (double)p[1], (double)p[2],
                    (double)d[0], (double)d[1], (double)d[2], (double)fov);
    }
}
