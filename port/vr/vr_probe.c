/* The VR probe: see vr_probe.h. Runs on the game thread (the GE draws there,
 * and the vblank hook runs there), so it needs no locking. */
#include "vr_probe.h"

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
    if (!d->bones) return;
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

void vr_probe_request(void) { g_report_due = 1; }

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
