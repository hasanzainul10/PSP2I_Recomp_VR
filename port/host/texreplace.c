/* Texture packs in PPSSPP's format; see texreplace.h.
 *
 * A pack's [hashes] line "AAAAAAAACCCCCCCCDDDDDDDD = dir/file.png" is the key
 * PPSSPP's TextureReplacer looks textures up by: A the address, C the low
 * half of its texture cache key (size register ^ CLUT hash), D the hash of
 * the texture bytes. psp_gpu_pack_key computes the same three for the
 * current draw; this file maps them to PNGs, decodes those with stb_image
 * and keeps the decoded pixels, up to a budget, for the backends to upload
 * (d3d11.c, gl_ge.c). Loading is synchronous: a big PNG costs its decode
 * time on the frame that first draws it. */

#include "texreplace.h"

#include <psprecomp/render.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define MAX_PACKS   8
#define CACHE_BYTES (768ull << 20)   /* decoded PNGs kept in memory (LRU beyond this) */

typedef struct { char dir[600]; int ignore_addr; } pack;

typedef struct {
    char    *path;                   /* as the textures.ini names it, '/'-separated */
    int      pack;                   /* the pack that named it: looked for there, then in earlier packs */
    int      state;                  /* 0 not loaded, 1 pixels in memory, -1 unreadable */
    int      w, h;
    uint32_t *px;
    uint64_t last_use;
} pfile;

/* (cachekey, hash) -> file; file -1 = listed with no file (PPSSPP: "don't replace"). */
typedef struct { uint64_t ck; uint32_t h; int32_t file; int used; } pkey;

static pack     g_packs[MAX_PACKS];
static int      g_npacks;
static int      g_ignore_addr;       /* every pack has ignoreAddress = true */
static pfile   *g_files;
static int      g_nfiles, g_capfiles;
static pkey    *g_keys;
static uint32_t g_capkeys, g_nkeys;  /* g_capkeys: a power of two */
static int32_t *g_path_index;        /* open addressing over g_files by (pack, path) */
static uint32_t g_cap_path;
static uint64_t g_cached_bytes, g_use;
static int      g_verbose;           /* PSP2I_TEXREPLACE_LOG=1: every load and every miss */
static int      g_logged;

static uint64_t n_find, n_hit, n_miss, n_loaded, n_failed;
static double   g_decode_ms;

/* ---- tables --------------------------------------------------------------------------- */

static uint32_t key_slot(uint64_t ck, uint32_t h, uint32_t cap) {
    uint64_t x = (ck ^ ((uint64_t)h << 29) ^ h) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 32) & (cap - 1);
}

static pkey *key_find(uint64_t ck, uint32_t h) {
    if (!g_capkeys) return NULL;
    for (uint32_t s = key_slot(ck, h, g_capkeys); g_keys[s].used; s = (s + 1) & (g_capkeys - 1))
        if (g_keys[s].ck == ck && g_keys[s].h == h) return &g_keys[s];
    return NULL;
}

static void key_put(uint64_t ck, uint32_t h, int32_t file) {
    if ((g_nkeys + 1) * 2 > g_capkeys) {
        uint32_t cap = g_capkeys ? g_capkeys * 2 : 4096;
        pkey *nk = (pkey *)calloc(cap, sizeof *nk);
        if (!nk) return;
        for (uint32_t i = 0; i < g_capkeys; i++) {
            if (!g_keys[i].used) continue;
            uint32_t s = key_slot(g_keys[i].ck, g_keys[i].h, cap);
            while (nk[s].used) s = (s + 1) & (cap - 1);
            nk[s] = g_keys[i];
        }
        free(g_keys);
        g_keys = nk; g_capkeys = cap;
    }
    pkey *k = key_find(ck, h);
    if (!k) {
        uint32_t s = key_slot(ck, h, g_capkeys);
        while (g_keys[s].used) s = (s + 1) & (g_capkeys - 1);
        k = &g_keys[s];
        k->used = 1; k->ck = ck; k->h = h;
        g_nkeys++;
    }
    k->file = file;                  /* a later line or pack wins, as in PPSSPP */
}

static uint32_t path_hash(int pk, const char *p) {
    uint32_t h = 2166136261u ^ (uint32_t)pk;
    for (; *p; p++) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    return h;
}

static void path_index_add(int i, uint32_t cap, int32_t *tab) {
    uint32_t s = path_hash(g_files[i].pack, g_files[i].path) & (cap - 1);
    while (tab[s] >= 0) s = (s + 1) & (cap - 1);
    tab[s] = i;
}

/* The file entry for `path` named by pack `pk` (one per PNG, however many keys use it). */
static int file_get(int pk, const char *path) {
    if (g_cap_path) {
        for (uint32_t s = path_hash(pk, path) & (g_cap_path - 1); g_path_index[s] >= 0; s = (s + 1) & (g_cap_path - 1)) {
            const pfile *f = &g_files[g_path_index[s]];
            if (f->pack == pk && !strcmp(f->path, path)) return g_path_index[s];
        }
    }
    if (g_nfiles == g_capfiles) {
        int cap = g_capfiles ? g_capfiles * 2 : 1024;
        pfile *nf = (pfile *)realloc(g_files, (size_t)cap * sizeof *nf);
        if (!nf) return -1;
        g_files = nf; g_capfiles = cap;
    }
    if ((uint32_t)(g_nfiles + 1) * 2 > g_cap_path) {
        uint32_t cap = g_cap_path ? g_cap_path * 2 : 2048;
        int32_t *tab = (int32_t *)malloc(cap * sizeof *tab);
        if (!tab) return -1;
        memset(tab, 0xFF, cap * sizeof *tab);
        for (int i = 0; i < g_nfiles; i++) path_index_add(i, cap, tab);
        free(g_path_index);
        g_path_index = tab; g_cap_path = cap;
    }
    pfile *f = &g_files[g_nfiles];
    memset(f, 0, sizeof *f);
    f->path = (char *)malloc(strlen(path) + 1);
    if (!f->path) return -1;
    strcpy(f->path, path);
    f->pack = pk;
    path_index_add(g_nfiles, g_cap_path, g_path_index);
    return g_nfiles++;
}

/* ---- textures.ini --------------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* sscanf(key, "%16llx%8x_%d"): up to 16 hex digits of cache key, up to 8 of
 * data hash (0 if absent: a wildcard), then an optional mip level. */
static int parse_key(const char *k, uint64_t *ck, uint32_t *h, int *level) {
    int n = 0;
    *ck = 0; *h = 0; *level = 0;
    for (; n < 16 && isxdigit((unsigned char)*k); n++, k++)
        *ck = *ck << 4 | (uint64_t)(isdigit((unsigned char)*k) ? *k - '0' : (tolower((unsigned char)*k) - 'a' + 10));
    if (!n) return 0;
    for (int m = 0; m < 8 && isxdigit((unsigned char)*k); m++, k++)
        *h = *h << 4 | (uint32_t)(isdigit((unsigned char)*k) ? *k - '0' : (tolower((unsigned char)*k) - 'a' + 10));
    if (*k == '_') *level = atoi(k + 1);
    return 1;
}

static int load_pack(const char *dir) {
    if (g_npacks == MAX_PACKS) { fprintf(stderr, "texreplace: more than %d packs; %s ignored\n", MAX_PACKS, dir); return 0; }
    char ini[700];
    snprintf(ini, sizeof ini, "%s/textures.ini", dir);
    FILE *f = fopen(ini, "rb");
    if (!f) { fprintf(stderr, "texreplace: no textures.ini in %s; pack ignored\n", dir); return 0; }

    /* Options come first in every pack, but read them all before taking keys. */
    const int pk = g_npacks;
    pack *p = &g_packs[pk];
    snprintf(p->dir, sizeof p->dir, "%s", dir);
    p->ignore_addr = 0;
    char line[1200], section[64] = "";
    char hashname[32] = "quick";                       /* PPSSPP's default when hash= is absent */
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;
        if (*s == '[') { snprintf(section, sizeof section, "%s", s); continue; }
        if (strcmp(section, "[options]")) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(k, "hash")) snprintf(hashname, sizeof hashname, "%s", v);
        else if (!strcmp(k, "ignoreAddress")) p->ignore_addr = !strcmp(v, "true") || !strcmp(v, "1");
        else if (!strcmp(k, "reduceHash") && (!strcmp(v, "true") || !strcmp(v, "1")))
            fprintf(stderr, "texreplace: %s: reduceHash is not supported; its reduced keys will not match\n", dir);
    }
    for (char *c = hashname; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (strcmp(hashname, "xxh64")) {
        fprintf(stderr, "texreplace: %s: hash = %s is not supported (only xxh64); pack ignored\n", dir, hashname);
        fclose(f);
        return 0;
    }

    rewind(f);
    section[0] = '\0';
    int n = 0, skipped_levels = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            snprintf(section, sizeof section, "%s", s);
            if (strcmp(section, "[options]") && strcmp(section, "[hashes]"))
                fprintf(stderr, "texreplace: %s: section %s is not supported; ignored\n", dir, section);
            continue;
        }
        if (strcmp(section, "[hashes]")) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        uint64_t ck; uint32_t h; int level;
        if (!parse_key(k, &ck, &h, &level)) continue;
        if (level != 0) { skipped_levels++; continue; }
        for (char *c = v; *c; c++) if (*c == '\\') *c = '/';
        if (strstr(v, "../")) continue;               /* PPSSPP refuses paths leaving the pack too */
        char *bar = strchr(v, '|');                    /* "level0|level1|...": level 0 only */
        if (bar) *bar = '\0';
        const int file = *v ? file_get(pk, v) : -1;
        if (*v && file < 0) continue;
        key_put(ck, h, file);
        n++;
    }
    fclose(f);
    g_npacks++;
    fprintf(stderr, "texreplace: %s: %d textures%s%s\n", dir, n, p->ignore_addr ? " (ignoreAddress)" : "",
            skipped_levels ? " (mip levels above 0 ignored)" : "");
    return n;
}

int texreplace_init(const char *packs, const char *base_dir) {
    g_verbose = getenv("PSP2I_TEXREPLACE_LOG") != NULL;
    int total = 0;
    while (packs && *packs) {
        const char *e = strchr(packs, ';');
        size_t len = e ? (size_t)(e - packs) : strlen(packs);
        char item[600], dir[600];
        if (len >= sizeof item) len = sizeof item - 1;
        memcpy(item, packs, len);
        item[len] = '\0';
        char *d = trim(item);
        size_t dl = strlen(d);
        while (dl && (d[dl - 1] == '/' || d[dl - 1] == '\\')) d[--dl] = '\0';
        if (*d) {
            const int absolute = d[0] == '/' || d[0] == '\\' || (d[0] && d[1] == ':');
            if (absolute || !base_dir) snprintf(dir, sizeof dir, "%s", d);
            else snprintf(dir, sizeof dir, "%s/%s", base_dir, d);
            total += load_pack(dir);
        }
        packs = e ? e + 1 : NULL;
    }
    g_ignore_addr = g_npacks > 0;
    for (int i = 0; i < g_npacks; i++) if (!g_packs[i].ignore_addr) g_ignore_addr = 0;
    if (g_npacks)
        fprintf(stderr, "texreplace: %d pack%s, %u keys, %d PNGs\n", g_npacks, g_npacks > 1 ? "s" : "", g_nkeys, g_nfiles);
    return total;
}

int texreplace_on(void) { return g_nkeys != 0; }

/* ---- lookup --------------------------------------------------------------------------- */

/* TextureReplacer::LookupWildcard: the exact key, then the keys a pack can
 * write with parts left out -- CLUT key only, no address, data hash only.
 * With ignoreAddress the address is never part of the key. */
static const pkey *lookup(uint64_t cachekey, uint32_t hash) {
    const uint64_t lo = cachekey & 0xFFFFFFFFull, hi = cachekey & ~0xFFFFFFFFull;
    if (g_ignore_addr) cachekey = lo;
    const pkey *k;
    if ((k = key_find(cachekey, hash))) return k;
    if ((k = key_find(lo, 0))) return k;
    if (!g_ignore_addr && (k = key_find(cachekey, 0))) return k;
    if ((k = key_find(lo, hash))) return k;
    if (!g_ignore_addr && (k = key_find(hi, hash))) return k;
    if (!g_ignore_addr && (k = key_find(hi, 0))) return k;
    return key_find(0, hash);
}

uint32_t texreplace_find(void) {
    if (!g_nkeys) return 0;
    uint32_t addr, clut_key, hashes[2];
    const int nh = psp_gpu_pack_key(&addr, &clut_key, hashes);
    n_find++;
    const uint64_t ck = (uint64_t)addr << 32 | clut_key;
    for (int i = 0; i < nh; i++) {
        const pkey *k = lookup(ck, hashes[i]);
        if (!k) continue;
        if (k->file < 0 || g_files[k->file].state < 0) break;     /* listed as "don't replace", or unreadable */
        n_hit++;
        return (uint32_t)k->file + 1;
    }
    n_miss++;
    if (g_verbose) fprintf(stderr, "texreplace: miss %08X%08X%08X\n", g_ignore_addr ? 0u : addr, clut_key, hashes[nh - 1]);
    return 0;
}

/* Drop the least recently used decoded PNGs until `need` more bytes fit. */
static void cache_trim(uint64_t need, const pfile *keep) {
    while (g_cached_bytes + need > CACHE_BYTES) {
        pfile *old = NULL;
        for (int i = 0; i < g_nfiles; i++) {
            pfile *f = &g_files[i];
            if (f->state == 1 && f != keep && (!old || f->last_use < old->last_use)) old = f;
        }
        if (!old) return;
        g_cached_bytes -= (uint64_t)old->w * old->h * 4;
        stbi_image_free(old->px);
        old->px = NULL;
        old->state = 0;
    }
}

const uint32_t *texreplace_pixels(uint32_t id, int *w, int *h) {
    if (!id || id > (uint32_t)g_nfiles) return NULL;
    pfile *f = &g_files[id - 1];
    if (f->state < 0) return NULL;
    if (f->state == 0) {
        const clock_t t0 = clock();
        int iw = 0, ih = 0, comp;
        stbi_uc *px = NULL;
        /* The pack that named it first, then the packs before it. */
        char path[1300];
        for (int pk = f->pack; pk >= 0 && !px; pk--) {
            snprintf(path, sizeof path, "%s/%s", g_packs[pk].dir, f->path);
            px = stbi_load(path, &iw, &ih, &comp, 4);
        }
        g_decode_ms += (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        if (!px) {
            f->state = -1;
            n_failed++;
            fprintf(stderr, "texreplace: cannot read %s (%s); the original texture is drawn\n", f->path, stbi_failure_reason());
            return NULL;
        }
        cache_trim((uint64_t)iw * ih * 4, f);
        f->px = (uint32_t *)px; f->w = iw; f->h = ih; f->state = 1;
        g_cached_bytes += (uint64_t)iw * ih * 4;
        n_loaded++;
        if (g_verbose || g_logged < 16) {
            g_logged++;
            fprintf(stderr, "texreplace: loaded %s (%dx%d)\n", f->path, iw, ih);
        }
    }
    f->last_use = ++g_use;
    *w = f->w; *h = f->h;
    return f->px;
}

void texreplace_report(FILE *out) {
    if (!g_npacks) return;
    fprintf(out, "    texture packs: %llu lookups, %llu replaced, %llu not in a pack; %llu PNG loads (%llu unreadable, %.0f ms decoding), %.0f MB cached\n",
            (unsigned long long)n_find, (unsigned long long)n_hit, (unsigned long long)n_miss,
            (unsigned long long)n_loaded, (unsigned long long)n_failed, g_decode_ms,
            (double)g_cached_bytes / (1024.0 * 1024.0));
}
