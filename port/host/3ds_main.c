/* PSP2i on New 3DS: the host (3DS_PLAN.md).
 *
 * The game from the SD card, drawn on the PICA200 by c3d_ge.c (T3) and
 * shown on the top screen each vblank; the 3DS's buttons as the PSP's; the
 * bottom screen shows the log's last lines and the frame rate. No sound and
 * no stereo yet (T4, T5). With sdmc:/psp2i/software present, the runtime's
 * software rasterizer draws instead and its frame is copied by the CPU (T2).
 *
 * Files, all under sdmc:/psp2i/:
 *
 *   game.iso          the game's disc image (or any one *.iso there)
 *   EBOOT.BIN         optional: a decrypted EBOOT, when the disc's is encrypted
 *   GameData/flash/   the PSP fonts (copied from the PC build's GameData/flash)
 *   GameData/ms/      the memory stick: saves, DLC (created on first save)
 *   psp2i_log.txt     the log of the last run (stderr)
 *   report_game.txt   memory at start-up and each thread's stack use at exit
 *   software          (any content) draw with the software rasterizer
 *
 * Buttons: A Circle, B Cross, Y Square, X Triangle, L/ZL L, R/ZR R, the
 * Circle Pad the analog stick, the C-stick the camera (camera.c), START,
 * SELECT, the D-pad. HOME suspends or quits as for any title. */
#include <3ds.h>
#include <citro3d.h>
#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"

#include "c3d_ge.h"
#include "camera.h"
#include "savecrypt.h"

#define BASE "sdmc:/psp2i"
#define SCREEN_W 480
#define SCREEN_H 272
#define TOP_W 400
#define TOP_H 240
#define USER_PARTITION_TOP 0x0A000000u

void psp_recomp_register(void);          /* the generated code (port/gen) */

extern u32 __ctru_heap_size, __ctru_linear_heap_size;

static PrintConsole g_bottom;
static FILE *g_report;
static int g_gpu;                        /* c3d_ge.c draws (else the software rasterizer) */

/* A line to the bottom screen, the log and the report. */
int psp2i_diag_on;

void psp2i_diag(const char *fmt, ...)
{
    if (!psp2i_diag_on) return;
    FILE *f = fopen(BASE "/diag.txt", "a");
    if (!f) return;
    fprintf(f, "%10.3f ", (double)svcGetSystemTick() / SYSCLOCK_ARM11);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    consoleSelect(&g_bottom);
    printf("%s\n", line);
    fprintf(stderr, "3ds: %s\n", line);
    if (g_report) { fprintf(g_report, "%s\n", line); fflush(g_report); }
    psp2i_diag("say: %s", line);
}

static unsigned long used_kib(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned long)(mi.uordblks >> 10);
}

/* The error stays on the bottom screen until HOME closes the app. */
static int fail(const char *msg)
{
    say("ERROR: %s", msg);
    fflush(stderr);
    while (aptMainLoop()) gspWaitForVBlank();
    return 1;
}

/* ---- hooks the generated code calls (as in sdl_main.c) --------------------- */

void psp_syscall(uint32_t id) {
    fprintf(stderr, "psp2i: raw syscall 0x%05lX reached\n", (unsigned long)id);
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}

void psp_unimplemented(uint32_t addr, const char *what) {
    static uint32_t seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return;
    if (n < 256) seen[n++] = addr;
    fprintf(stderr, "psp2i: untranslated instruction at 0x%08lX: %s\n", (unsigned long)addr, what ? what : "?");
}

/* ---- the game's ELF (as in sdl_main.c) --------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;
typedef struct __attribute__((packed)) { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Elf32_Phdr;
typedef struct __attribute__((packed)) { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; } Elf32_Shdr;

typedef struct { uint32_t entry, gp, load_hi; char name[29]; } module_info;

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

/* Place the PT_LOAD segments; $gp and the name from .rodata.sceModuleInfo.
 * (The ELF's bytes are taken and freed.) -2 if it is not a decrypted ELF. */
static int load_elf_buf(uint8_t *b, size_t len, module_info *mi) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)b;
    if (len < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) != 0 || eh->machine != 8) { free(b); return -2; }
    memset(mi, 0, sizeof *mi);
    mi->entry = eh->entry;
    for (unsigned i = 0; i < eh->phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(b + eh->phoff + i * eh->phentsize);
        if (ph->type != 1) continue;
        if ((size_t)ph->offset + ph->filesz > len || psp_mem_write_block(ph->vaddr, b + ph->offset, ph->filesz) != 0) { free(b); return -2; }
        for (uint32_t a = ph->vaddr + ph->filesz; a < ph->vaddr + ph->memsz; a++) psp_write8(a, 0);
        if (ph->vaddr + ph->memsz > mi->load_hi) mi->load_hi = ph->vaddr + ph->memsz;
    }
    if (eh->shoff && eh->shstrndx < eh->shnum) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(b + eh->shoff);
        const char *strtab = (const char *)(b + sh[eh->shstrndx].offset);
        for (unsigned i = 0; i < eh->shnum; i++) {
            if (strcmp(strtab + sh[i].name, ".rodata.sceModuleInfo") != 0) continue;
            memcpy(mi->name, b + sh[i].offset + 4, 28);
            memcpy(&mi->gp, b + sh[i].offset + 32, 4);
        }
    }
    free(b);
    return mi->gp ? 0 : -2;
}

/* sdmc:/psp2i/game.iso, else the first *.iso in sdmc:/psp2i. */
static int open_disc(char *path, size_t cap) {
    snprintf(path, cap, BASE "/game.iso");
    if (psp_io_open_disc_image(path) == 0) return 0;
    DIR *d = opendir(BASE);
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".iso") == 0) {
            snprintf(path, cap, BASE "/%s", e->d_name);
            if (psp_io_open_disc_image(path) == 0) { rc = 0; break; }
        }
    }
    closedir(d);
    return rc;
}

/* The game's EBOOT: sdmc:/psp2i/EBOOT.BIN if there, else the disc's. */
static uint8_t *find_eboot(size_t *len) {
    uint8_t *b = read_file(BASE "/EBOOT.BIN", len);
    if (b) { say("EBOOT: " BASE "/EBOOT.BIN"); return b; }
    uint32_t l = 0;
    b = psp_io_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &l);
    *len = l;
    return b;
}

/* ---- the picture: emulated VRAM to the top screen ----------------------------- */

/* The top screen is 400x240 and stored rotated: 240-pixel columns, the
 * first at the left, each from the bottom up, 3 bytes (B, G, R) a pixel.
 * The PSP's 480x272 is scaled to it by picking pixels (nearest). */
static uint16_t g_sx[TOP_W], g_sy[TOP_H];

static void present(void) {
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    const int bpp = fmt == 3 ? 4 : 2;
    const uint8_t *src = addr ? (const uint8_t *)psp_mem_ptr(addr, stride * SCREEN_H * (uint32_t)bpp) : NULL;
    if (!src) {
        memset(fb, 0, TOP_W * TOP_H * 3);
    } else {
        for (int x = 0; x < TOP_W; x++) {
            u8 *col = fb + x * TOP_H * 3;
            for (int y = 0; y < TOP_H; y++) {
                const uint8_t *s = src + ((uint32_t)g_sy[y] * stride + g_sx[x]) * (uint32_t)bpp;
                uint32_t r, g, b;
                if (bpp == 4) { r = s[0]; g = s[1]; b = s[2]; }
                else {
                    const uint32_t p = (uint32_t)s[0] | (uint32_t)s[1] << 8;
                    switch (fmt) {
                    case 0:  r = (p & 0x1F) << 3; g = ((p >> 5) & 0x3F) << 2; b = ((p >> 11) & 0x1F) << 3; break;
                    case 1:  r = (p & 0x1F) << 3; g = ((p >> 5) & 0x1F) << 3; b = ((p >> 10) & 0x1F) << 3; break;
                    default: r = (p & 0xF) * 17; g = ((p >> 4) & 0xF) * 17; b = ((p >> 8) & 0xF) * 17; break;
                    }
                }
                u8 *d = col + (TOP_H - 1 - y) * 3;
                d[0] = (u8)b; d[1] = (u8)g; d[2] = (u8)r;
            }
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

/* ---- input --------------------------------------------------------------------- */

#define P_SELECT   0x0001u
#define P_START    0x0008u
#define P_UP       0x0010u
#define P_RIGHT    0x0020u
#define P_DOWN     0x0040u
#define P_LEFT     0x0080u
#define P_L        0x0100u
#define P_R        0x0200u
#define P_TRIANGLE 0x1000u
#define P_CIRCLE   0x2000u
#define P_CROSS    0x4000u
#define P_SQUARE   0x8000u

/* A 3DS stick reading (about -156..156, up positive) as a PSP stick byte
 * (0..255, 128 centre, up = 0). */
static uint8_t stick_byte(int v, int flip) {
    if (flip) v = -v;
    int b = 128 + v * 128 / 150;
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

/* sdmc:/psp2i/input: presses played at fixed times, for testing without
 * hands on the pad (Azahar or the console). One per line:
 *   <second> <BUTTON[+BUTTON...]> <duration>      e.g.  20.5 CROSS 0.3
 *   <second> STICK <x> <y> <duration>             PSP stick bytes, 128 = centre
 * seconds from boot; buttons CROSS CIRCLE SQUARE TRIANGLE START SELECT L R
 * UP DOWN LEFT RIGHT. They add to the real pad; no file, no change. */
typedef struct { float at, dur; uint32_t bits; int ax, ay; int told; } scripted_press;
static scripted_press g_script[256];
static int g_nscript;
static u64 g_boot_tick;

static void script_load(void) {
    static const struct { const char *name; uint32_t bit; } NAMES[] = {
        { "CROSS", P_CROSS }, { "CIRCLE", P_CIRCLE }, { "SQUARE", P_SQUARE }, { "TRIANGLE", P_TRIANGLE },
        { "START", P_START }, { "SELECT", P_SELECT }, { "L", P_L }, { "R", P_R },
        { "UP", P_UP }, { "DOWN", P_DOWN }, { "LEFT", P_LEFT }, { "RIGHT", P_RIGHT },
    };
    FILE *f = fopen(BASE "/input", "r");
    if (!f) return;
    char line[128];
    while (g_nscript < 256 && fgets(line, sizeof line, f)) {
        scripted_press p = { 0 };
        char what[64];
        p.ax = p.ay = -1;
        if (sscanf(line, "%f %63s", &p.at, what) != 2) continue;
        if (!strcmp(what, "STICK")) {
            if (sscanf(line, "%*f %*s %d %d %f", &p.ax, &p.ay, &p.dur) != 3) continue;
        } else {
            if (sscanf(line, "%*f %*s %f", &p.dur) != 1) continue;
            for (char *tok = strtok(what, "+"); tok; tok = strtok(NULL, "+"))
                for (size_t k = 0; k < sizeof NAMES / sizeof NAMES[0]; k++)
                    if (!strcmp(tok, NAMES[k].name)) p.bits |= NAMES[k].bit;
            if (!p.bits) continue;
        }
        g_script[g_nscript++] = p;
    }
    fclose(f);
    if (g_nscript) say("input: %d scripted presses from " BASE "/input", g_nscript);
}

static void read_pad(void) {
    static const struct { u32 key; uint32_t bit; } MAP[] = {
        { KEY_A, P_CIRCLE }, { KEY_B, P_CROSS }, { KEY_Y, P_SQUARE }, { KEY_X, P_TRIANGLE },
        { KEY_L, P_L }, { KEY_ZL, P_L }, { KEY_R, P_R }, { KEY_ZR, P_R },
        { KEY_START, P_START }, { KEY_SELECT, P_SELECT },
        { KEY_DUP, P_UP }, { KEY_DDOWN, P_DOWN }, { KEY_DLEFT, P_LEFT }, { KEY_DRIGHT, P_RIGHT },
    };
    hidScanInput();
    const u32 held = hidKeysHeld();
    uint32_t bits = 0;
    for (size_t k = 0; k < sizeof MAP / sizeof MAP[0]; k++) if (held & MAP[k].key) bits |= MAP[k].bit;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    uint8_t ax = stick_byte(cp.dx, 0), ay = stick_byte(cp.dy, 1);
    if (g_nscript) {
        const float now = (float)((double)(svcGetSystemTick() - g_boot_tick) / SYSCLOCK_ARM11);
        for (int k = 0; k < g_nscript; k++) {
            scripted_press *p = &g_script[k];
            if (now < p->at || now >= p->at + p->dur) continue;
            bits |= p->bits;
            if (p->ax >= 0) { ax = (uint8_t)p->ax; ay = (uint8_t)p->ay; }
            if (!p->told) { p->told = 1; fprintf(stderr, "input: %.1f s press %04lX stick %d,%d\n", now, (unsigned long)p->bits, p->ax, p->ay); }
        }
    }
    psp_ctrl_set(bits, ax, ay);
    camera_set_stick(stick_byte(cs.dx, 0), stick_byte(cs.dy, 1));
}

/* ---- once per vblank, on the scheduler's clock --------------------------------- */

static u64 g_stat_tick;
static uint64_t g_stat_flips;

static uint64_t ticks_now(void) { return svcGetSystemTick(); }

/* Where the last second went (ms in a second, each counted once) and the
 * target traffic per frame: the bottom screen's lines 2-4 and the log. */
static void time_report(double secs, uint64_t frames) {
    static c3d_ge_stats was;
    static uint64_t prim_was, idle_was;
    c3d_ge_stats s;
    c3d_ge_stats_get(&s);
    const uint64_t prim = psp_gpu_prim_ticks(), idle_us = psp_sched_idle_us();
    const double ms = 1000.0 / SYSCLOCK_ARM11 / secs, f = frames ? (double)frames : 1.0;
    const double draw = (double)(s.draw - was.draw) * ms, wait = (double)(s.wait - was.wait) * ms,
                 rb = (double)(s.readback - was.readback) * ms, up = (double)(s.upload - was.upload) * ms,
                 tex = (double)(s.texture - was.texture) * ms, pres = (double)(s.present - was.present) * ms;
    /* prims include the backend's draw and what it calls (waits, transfers) */
    const double backend = draw + wait + rb + up + tex;
    double ge = (double)(prim - prim_was) * ms - backend;
    if (ge < 0) ge = 0;
    /* idle: no PSP thread runnable (waiting for the next vblank) */
    const double idle = (double)(idle_us - idle_was) / 1000.0 / secs;
    double game = 1000.0 - idle - ge - backend - pres;
    if (game < 0) game = 0;
    /* 40 columns: ms/s on two lines, then per frame */
    printf("game %3.0f ge %3.0f draw %3.0f wait %3.0f  \n"
           "rb %3.0f up %3.0f tex %3.0f pr %3.0f idle %3.0f\n"
           "/f: ev %4.1f rb %4.1f up %4.1f fin %4.1f  \n",
           game, ge, draw, wait, rb, up, tex, pres, idle,
           (double)(s.evictions - was.evictions) / f, (double)(s.readbacks - was.readbacks) / f,
           (double)(s.uploads - was.uploads) / f, (double)(s.finishes - was.finishes) / f);
    fprintf(stderr, "time: %llu frames; ms/s game %.0f ge %.0f draw %.0f wait %.0f readback %.0f upload %.0f "
                    "texture %.0f present %.0f idle %.0f; per frame %.1f draws, %.1f evictions, %.1f readbacks, "
                    "%.1f uploads, %.1f finishes\n",
            (unsigned long long)frames, game, ge, draw, wait, rb, up, tex, pres, idle,
            (double)(s.draws - was.draws) / f, (double)(s.evictions - was.evictions) / f,
            (double)(s.readbacks - was.readbacks) / f, (double)(s.uploads - was.uploads) / f,
            (double)(s.finishes - was.finishes) / f);
    psp2i_diag("time: %llu frames; game %.0f ge %.0f draw %.0f wait %.0f idle %.0f; io %llu KB, ge cmds %llu",
               (unsigned long long)frames, game, ge, draw, wait, idle,
               (unsigned long long)(psp_io_bytes_read() >> 10), (unsigned long long)psp_ge_command_count());
    was = s;
    prim_was = prim;
    idle_was = idle_us;

    /* ge by kind of draw: the vertex loop's ms/s and vertices per frame
     * (w skin weights, l lights, p powf, t through, g texgen, m morph) */
    psp_gpu_vkind vk[5];
    const int nvk = psp_gpu_vkinds_take(vk, 5);
    char line[320];
    int at = snprintf(line, sizeof line, "vtx:");
    for (int i = 0; i < nvk && at < (int)sizeof line - 48; i++) {
        const uint32_t k = vk[i].kind;
        at += snprintf(line + at, sizeof line - (size_t)at, " [w%u l%u%s%s%s%s %.0f ms %.0f v/f %.0f d/f]",
                       (unsigned)PSP_VK_WEIGHTS(k), (unsigned)PSP_VK_LIGHTS(k), (k & PSP_VK_POW) ? " p" : "",
                       (k & PSP_VK_THROUGH) ? " t" : "", (k & PSP_VK_TEXGEN) ? " g" : "",
                       (k & PSP_VK_MORPH) ? " m" : "", (double)vk[i].ticks * ms,
                       (double)vk[i].verts / f, (double)vk[i].draws / f);
    }
    fprintf(stderr, "%s\n", line);
}

static void on_vblank(void) {
    static int nv;
    if (psp2i_diag_on && nv < 10)
        psp2i_diag("vblank %d: vblanks %llu flips %llu io %llu KB ge cmds %llu", nv,
                   (unsigned long long)psp_sched_vblank_count(), (unsigned long long)psp_display_flips(),
                   (unsigned long long)(psp_io_bytes_read() >> 10), (unsigned long long)psp_ge_command_count());
    nv++;
    if (!aptMainLoop()) { psp_request_exit(); return; }   /* HOME > Close */
    read_pad();
    if (g_gpu) {
        uint32_t addr, stride, fmt;
        psp_display_get(&addr, &stride, &fmt);
        if (nv <= 10) psp2i_diag("present %d: fb %08lX stride %lu fmt %lu", nv, (unsigned long)addr,
                                 (unsigned long)stride, (unsigned long)fmt);
        c3d_ge_present(addr, stride, (int)fmt);
        if (nv <= 10) psp2i_diag("present %d done", nv);
    } else present();
    const u64 now = svcGetSystemTick();
    if (now - g_stat_tick >= SYSCLOCK_ARM11) {            /* once a second */
        const uint64_t flips = psp_display_flips();
        consoleSelect(&g_bottom);
        /* lines 1-4, the cursor kept where the log is */
        printf("\x1b[s\x1b[1;1H%.1f fps  heap %lu KiB used   \n",
               (double)(flips - g_stat_flips) * SYSCLOCK_ARM11 / (double)(now - g_stat_tick), used_kib());
        if (g_gpu) time_report((double)(now - g_stat_tick) / SYSCLOCK_ARM11, flips - g_stat_flips);
        printf("\x1b[u");
        g_stat_flips = flips;
        g_stat_tick = now;
        /* sdmc:/psp2i/dumpframes holds seconds ("6 9 12"): every draw of the
         * frame at each of them goes to dump_<s>.txt (psp_gpu_dump_next_frame) */
        static int secs, ndump = -1, dump_at[16];
        ++secs;
        if (ndump < 0) {
            ndump = 0;
            FILE *f = fopen(BASE "/dumpframes", "r");
            if (f) { while (ndump < 16 && fscanf(f, "%d", &dump_at[ndump]) == 1) ndump++; fclose(f); }
        }
        int dump_now = 0;
        for (int k = 0; k < ndump; k++) dump_now |= dump_at[k] == secs;
        if (dump_now) {
            char path[64];
            snprintf(path, sizeof path, BASE "/dump_%02d.txt", secs);
            psp_gpu_dump_next_frame(path);
        }
        /* sdmc:/psp2i/shots holds seconds too: the displayed picture at each
         * goes to shot_<s>.ppm (480x272), as the PC build's --overlay-shot */
        static int nshot = -1, shot_at[32];
        if (nshot < 0) {
            nshot = 0;
            FILE *f = fopen(BASE "/shots", "r");
            if (f) { while (nshot < 32 && fscanf(f, "%d", &shot_at[nshot]) == 1) nshot++; fclose(f); }
        }
        for (int k = 0; k < nshot; k++)
            if (shot_at[k] == secs) {
                uint32_t addr, stride, fmt;
                psp_display_get(&addr, &stride, &fmt);
                char path[64];
                snprintf(path, sizeof path, BASE "/shot_%03d.ppm", secs);
                if (g_gpu) c3d_ge_capture(addr, stride, (int)fmt, path);
            }
        if (secs % 10 == 0) {
            psp_sched_stack_report(stderr);   /* each thread's peak, to size the stacks */
            if (g_gpu) c3d_ge_report(stderr);
        }
        gfxFlushBuffers();                    /* the bottom screen's text */
        fflush(stderr);
    }
}

/* ---- start-up ------------------------------------------------------------------- */

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &g_bottom);
    osSetSpeedupEnable(true);
    mkdir(BASE, 0777);
    {
        struct stat ds;
        psp2i_diag_on = stat(BASE "/diag", &ds) == 0;
        psp2i_diag("diag: start");
    }
    /* the log: buffered (the SD card is slow), flushed once a second */
    if (freopen(BASE "/psp2i_log.txt", "w", stderr)) setvbuf(stderr, NULL, _IOFBF, 64 * 1024);
    g_report = fopen(BASE "/report_game.txt", "w");
    printf("\x1b[6;1H");                                    /* lines 1-4: the frame rate, time_report */

    say("PSP2i 3DS T2 (%s)", envIsHomebrew() ? "3dsx" : "cia");
    say("heap %lu KiB after the code, linear %lu KiB",
        (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(__ctru_linear_heap_size >> 10));

    struct stat sw;
    if (stat(BASE "/software", &sw) != 0) {
        /* 1 MB of GPU commands: ~340 bytes a draw, 1,600 draws in busy town scenes */
        if (C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4) && c3d_ge_init() == 0) g_gpu = 1;
        else say("citro3d could not start: drawing in software");
    }
    say("drawing: %s", g_gpu ? "PICA200 (citro3d)" : "software");
    g_boot_tick = svcGetSystemTick();
    script_load();
    for (int x = 0; x < TOP_W; x++) g_sx[x] = (uint16_t)(x * SCREEN_W / TOP_W);
    for (int y = 0; y < TOP_H; y++) g_sy[y] = (uint16_t)(y * SCREEN_H / TOP_H);

    if (psp_mem_init() != 0) return fail("out of memory for the PSP's RAM");
    psp_cpu_reset();
    char iso[300];
    if (open_disc(iso, sizeof iso) != 0)
        return fail("no game: put the ISO at " BASE "/game.iso");
    say("disc: %s", iso);
    size_t elf_len = 0;
    uint8_t *elf = find_eboot(&elf_len);
    if (!elf) return fail("the disc has no PSP_GAME/SYSDIR/EBOOT.BIN");
    if (elf_len < 4 || memcmp(elf, "\x7F" "ELF", 4) != 0)
        return fail("the disc's EBOOT is encrypted: put a decrypted EBOOT.BIN in " BASE);
    module_info mi;
    if (load_elf_buf(elf, elf_len, &mi) != 0) return fail("the EBOOT could not be loaded");
    say("module '%s' entry 0x%08lX", mi.name, (unsigned long)mi.entry);

    struct stat st;
    if (stat(BASE "/GameData/flash/font", &st) != 0) say("no fonts in " BASE "/GameData/flash/font: text may not show");
    mkdir(BASE "/GameData", 0777);
    psp_hle_init();
    psp_io_set_root(BASE "/GameData");
    /* PSP savedata encryption: saves interchangeable with the PC, a PSP and PPSSPP */
    static const psp_savedata_crypto SAVE_CRYPTO = { savecrypt_decrypt, savecrypt_encrypt, savecrypt_sfo_hash };
    psp_savedata_set_crypto(&SAVE_CRYPTO);
    psp_sysmem_set_heap(mi.load_hi, USER_PARTITION_TOP);
    psp_recomp_register();
    camera_init(1.0f);
    psp_gpu_set_clock(ticks_now);                         /* the GE's share of time_report */
    psp_sched_set_vblank_hook(on_vblank);
    say("ready: heap %lu KiB used of %lu", used_kib(), (unsigned long)(__ctru_heap_size >> 10));

    static const char boot_path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    const uint32_t argp = psp_sysmem_alloc(0x100, 1);
    psp_mem_write_block(argp, boot_path, (uint32_t)sizeof boot_path);
    g_stat_tick = svcGetSystemTick();
    psp2i_diag("psp_sched_run");
    const int rc = psp_sched_run(mi.entry, (uint32_t)sizeof boot_path, argp, 0x20, 0x40000, mi.gp);

    say("the game ended (%d); heap %lu KiB used", rc, used_kib());
    if (g_report) psp_sched_stack_report(g_report);
    psp_sched_stack_report(stderr);
    if (g_gpu) c3d_ge_report(stderr);
    if (g_report) fclose(g_report);
    fflush(stderr);
    psp_mem_free();
    gfxExit();
    return rc;
}
