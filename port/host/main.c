/* psp2i — host for the recompiled Phantasy Star Portable 2 Infinity
 * (NPJH50332) module `psu`.
 *
 * What a PSP does between "load the executable" and "the game's code runs",
 * done here explicitly:
 *
 *   1. load the ELF's PT_LOAD segment at its link address;
 *   2. hand the rest of user memory (0x08800000-0x0A000000) above the module
 *      to the allocator -- the module's .bss runs to 0x08FA6744, so the
 *      allocator's built-in default would hand out the game's own globals;
 *   3. register every recompiled function for indirect calls;
 *   4. start module_start as the first thread, with argv[0] the boot path in
 *      guest memory, its $gp from the module info;
 *   5. run the scheduler, presenting the framebuffer every vblank.
 *
 * usage: psp2i [--root DIR] [--iso FILE | --extracted] [--eboot FILE] [--headless]
 *              [--seconds N] [--capture-every N] [--capture-dir DIR]
 *
 *   --root      directory holding flash/ (the PSP fonts), ms/ (memory stick)
 *               and, for an extracted disc, disc/; default: GameData next to
 *               the executable, else ./GameData
 *   --iso       the game's disc image (.iso), read directly; default: the first
 *               *.iso next to the executable, in the root, or in the root's
 *               parent folder
 *   --extracted use the extracted disc in <root>/disc/ even when an ISO is
 *               found (the default when there is none)
 *   --eboot     the decrypted EBOOT.BIN; default: EBOOT.BIN next to the
 *               executable, else the disc's PSP_GAME/SYSDIR/EBOOT.BIN
 *   --headless  no window
 *   --seconds   stop after N seconds of game time and print the report
 *   --capture-every / --capture-dir  dump the framebuffer as PPM every N vblanks
 *   --fps 30|60|120  initial frame rate through the game's own frame-rate API
 *               (120: the emulated display refreshes at 120 Hz; F7 runs the
 *               frame-rate audit, F6 the press audit, rateaudit.c)
 *               (framerate.c); changeable at run time in the settings menu
 *   --menu-key F1..F9|F11  the keyboard key that opens/closes the settings
 *               menu (default F1; the controller's Guide or R3 also do)
 *   --show-fps / --hide-fps  the measured-FPS counter at start (default shown)
 *   --rs-speed 1.0..2.0  right-stick camera sensitivity at start (default
 *               1.0; the settings menu changes it in 0.25 steps)
 *   --rstick X,Y@VBLANK[+N]  scripted right stick (PSP-style bytes), for tests
 *   --replay-input FILE      play back a PSP2I_INPUT_RECORD=FILE recording (same
 *               input on the same game frame; its fps unless --fps)
 *   --overlay-shot VBLANK  (repeatable) save the presented image, overlay
 *               included, as <capture-dir>/overlay_<vblank>.ppm (and with
 *               stereo on, each eye's picture as overlay_<vblank>_L/_R.ppm)
 *   --ge-dump VBLANK  what F12 does, at that vblank (frame and draws in dumps/)
 *   --stereo    (VR builds) draw the game once per eye without a headset, to
 *               check the stereo pictures with --overlay-shot
 *   --audio / --no-audio  sound through SDL3.dll (audio_sdl.c): on by default
 *               with a window, off headless; PSP2I_AUDIO_DUMP=file.wav records
 *   --sdl / --no-sdl  controllers through SDL3.dll (input_sdl.c): on by default
 *               with a window, off headless unless --sdl
 *   --resolution 960x544|1280x720|1920x1080|2560x1440|3840x2160|fullscreen
 *               window size for this run (default: psp2i_display.ini next to
 *               the exe, else 1920x1080); the settings menu changes and saves
 *               it, Alt+Enter toggles fullscreen. PSP2I_PRESENT=gdi skips the
 *               GPU presenter (present.c) and scales with GDI.
 *   --texture-packs DIR[;DIR...]  PPSSPP texture packs for this run, instead of
 *               psp2i_display.ini texture_packs= (texreplace.h); "" = none
 *   --render-scale N  internal resolution for this run, 1..6 (Direct3D 11),
 *               instead of psp2i_display.ini render_scale=
 *   --oracle    (trace builds) diff the function at ADDR against an interpreter
 *               the first time it runs; see host/oracle.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/vfpu.h>
#include <signal.h>
#include <time.h>
#include <psprecomp/render.h>
#include <psprecomp/net.h>

#include "recomp_funcs.h"
#include "input_sdl.h"
#include "audio_sdl.h"
#include "atrac_at3.h"
#include "h264_openh264.h"
#include "framerate.h"
#include "menu.h"
#include "font_win.h"
#include "web_overlay.h"
#include "camera.h"
#include "login.h"
#include "online.h"
#include "savecrypt.h"
#include "gamelog.h"
#include "functrace.h"
#include "rateaudit.h"
#include "prod.h"
#include "textedit.h"
#include "present.h"
#ifdef PSP2I_VR
#include "vr.h"
#endif
#include "vr_probe.h"
#if defined(PSP2I_VR) && !defined(PSP2I_PROD)
static void vrshot_request(void);        /* F11: a VR capture (below) */
#endif
#ifdef PSP2I_TEXREPLACE
#include "texreplace.h"
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <dbghelp.h>
#  include <mmsystem.h>
#  include <psapi.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define host_mkdir(p) mkdir(p, 0777)
#endif

#define USER_PARTITION_TOP 0x0A000000u
#define SCREEN_W 480
#define SCREEN_H 272

/* The window title: the game's name in the community build; dev builds add
 * their progress to it (see the vblank handler). */
#ifdef PSP2I_PROD
#define WINDOW_TITLE "Phantasy Star Portable 2 Infinity Recompiled"
#else
#define WINDOW_TITLE "PSP2i (recompiled)"
#endif

/* ---- ELF loading ---------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;
typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} Elf32_Phdr;
typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} Elf32_Shdr;
#pragma pack(pop)

typedef struct {
    uint32_t entry;
    uint32_t gp;
    uint32_t load_lo, load_hi;
    uint32_t stub_lo, stub_hi;   /* .sceStub.text: the import thunks */
    char     name[29];
} module_info;

void oracle_arm(uint32_t addr, uint32_t stub_lo, uint32_t stub_hi);
void oracle_arm_sweep(uint32_t stub_lo, uint32_t stub_hi);
void oracle_report(FILE *out);
void dump_arm(uint32_t addr, const char *path);
void args_arm(uint32_t addr, uint64_t flip);
void dump_on_bad_access(const char *path);
void ring_arm(void);
#ifdef _WIN32
int  d3d11_init(void);
void d3d11_report(FILE *out);
int  d3d11_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h);
void d3d11_set_scale(int s);
void d3d11_set_scale_y(int s);
void d3d11_set_quality(int aniso, float sharpen);
int  d3d11_tex_mips(void);
int  d3d11_sharpen_ready(void);
const uint32_t *d3d11_present_hires(int *w, int *h);
const uint32_t *d3d11_present_eye(int eye, int *w, int *h);
void d3d11_eye_share(int on);
void d3d11_eye_readback(int on);
void d3d11_set_mono_left(int on);
void *d3d11_present_eye_shared(int *w, int *h, uint32_t *tag);
uint32_t d3d11_present_eye_tag(void);
const uint32_t *d3d11_hud_picture(int *w, int *h);
#endif
#include "gl_ge.h"

/* The renderer (menu RENDERER, psp2i_display.ini renderer=, --renderer):
 * REN_D3D11 / REN_GL draw the GE on the GPU, REN_SOFTWARE is the reference. */
static int g_renderer = REN_D3D11;        /* what runs now */
static int g_gl_window;                   /* OpenGL presents into the window */

/* The displayed frame from the GPU renderer's asynchronous copy (1), keep the
 * last picture (2), or read VRAM (0). */
static int gpu_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    if (g_renderer == REN_GL) return gl_ge_present(addr, stride, fmt, out, w, h);
#ifdef _WIN32
    if (g_renderer == REN_D3D11) return d3d11_present(addr, stride, fmt, out, w, h);
#endif
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static int load_elf_buf(uint8_t *b, size_t len, const char *path, module_info *mi);

static int load_elf(const char *path, module_info *mi) {
    size_t len = 0;
    uint8_t *b = read_file(path, &len);
    if (!b) { fprintf(stderr, "psp2i: cannot read %s\n", path); return -1; }
    return load_elf_buf(b, len, path, mi);
}

/* The ELF in memory (taken and freed); `path` names it in messages. */
static int load_elf_buf(uint8_t *b, size_t len, const char *path, module_info *mi) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)b;
    if (len < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) != 0 || eh->machine != 8) {
        fprintf(stderr, "psp2i: %s is not a decrypted MIPS ELF (encrypted EBOOTs must be decrypted first)\n", path);
        free(b);
        return -1;
    }
    memset(mi, 0, sizeof *mi);
    mi->entry = eh->entry;
    mi->load_lo = 0xFFFFFFFFu;

    for (unsigned i = 0; i < eh->phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(b + eh->phoff + i * eh->phentsize);
        if (ph->type != 1) continue;
        if ((size_t)ph->offset + ph->filesz > len ||
            psp_mem_write_block(ph->vaddr, b + ph->offset, ph->filesz) != 0) {
            fprintf(stderr, "psp2i: cannot place segment at 0x%08X\n", ph->vaddr);
            free(b);
            return -1;
        }
        /* .bss: the memory map starts zeroed, but be explicit. */
        for (uint32_t a = ph->vaddr + ph->filesz; a < ph->vaddr + ph->memsz; a++) psp_write8(a, 0);
        if (ph->vaddr < mi->load_lo) mi->load_lo = ph->vaddr;
        if (ph->vaddr + ph->memsz > mi->load_hi) mi->load_hi = ph->vaddr + ph->memsz;
        printf("PT_LOAD 0x%08X..0x%08X (file %u bytes)\n", ph->vaddr, ph->vaddr + ph->memsz, ph->filesz);
    }

    /* $gp and the module name come from .rodata.sceModuleInfo:
     * u16 attribute, u8 version[2], char name[28], u32 gp, ... */
    if (eh->shoff && eh->shstrndx < eh->shnum) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(b + eh->shoff);
        const char *strtab = (const char *)(b + sh[eh->shstrndx].offset);
        for (unsigned i = 0; i < eh->shnum; i++) {
            if (!strcmp(strtab + sh[i].name, ".sceStub.text")) {
                mi->stub_lo = sh[i].addr;
                mi->stub_hi = sh[i].addr + sh[i].size;
            }
            if (strcmp(strtab + sh[i].name, ".rodata.sceModuleInfo") != 0) continue;
            const uint8_t *m = b + sh[i].offset;
            memcpy(mi->name, m + 4, 28);
            memcpy(&mi->gp, m + 32, 4);
        }
    }
    free(b);
    if (!mi->gp) { fprintf(stderr, "psp2i: no module info, cannot find $gp\n"); return -1; }
    return 0;
}

/* ---- hooks the generated code calls ------------------------------------- */

void psp_syscall(uint32_t id) {
    /* Firmware calls go through the import thunks, never a raw syscall, so
     * reaching one means control is in data being executed as code. */
    fprintf(stderr, "psp2i: raw syscall 0x%05X reached\n", id);
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}

/* An instruction the recompiler could not translate. Reported per address
 * with a count -- once a site is known, repeating it hides new ones. */
#define UNIMPL_SITES 256
static struct { uint32_t addr; uint64_t hits; const char *what; } g_unimpl[UNIMPL_SITES];
static int g_unimpl_n;

void psp_unimplemented(uint32_t addr, const char *what) {
    for (int i = 0; i < g_unimpl_n; i++)
        if (g_unimpl[i].addr == addr) { g_unimpl[i].hits++; return; }
    if (g_unimpl_n < UNIMPL_SITES) {
        g_unimpl[g_unimpl_n].addr = addr;
        g_unimpl[g_unimpl_n].what = what;
        g_unimpl[g_unimpl_n].hits = 1;
        g_unimpl_n++;
    }
    fprintf(stderr, "psp2i: untranslated instruction at 0x%08X: %s\n", addr, what ? what : "?");
}

/* ---- presentation --------------------------------------------------------- */

static uint32_t g_rgba[SCREEN_W * SCREEN_H];   /* 0xAARRGGBB for GDI: the frame plus the overlay */
static uint32_t g_frame[SCREEN_W * SCREEN_H];  /* the game's frame alone (grab_frame, d3d11_present) */
static int      g_have_frame;

/* Internal resolution (psp2i_display.ini render_scale=, --render-scale; D3D11
 * only, from the next start): the game drawn at S x 480x272. g_frame and
 * g_rgba stay 480x272 (captures, F12, the overlay, GDI); g_hi_frame is the
 * renderer's S-times picture and g_hi_rgba that picture with the overlay,
 * for the window (present.c) and the headset. */
static int       g_hi_s = 1;                /* the scale in use: 1 = off */
static int       g_hi_sy = 1;               /* ... down, for the VR eye pictures (g_hi_s.. 12) */
static uint32_t *g_hi_frame, *g_hi_rgba;
static int       g_hi_valid;                /* g_hi_frame holds the frame g_frame holds */

/* Convert the current framebuffer. Returns 0 if the game has not set one. */
static int grab_frame(void) {
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    if (!addr) return 0;
    const int bpp = fmt == 3 ? 4 : 2;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            uint32_t p = bpp == 4 ? psp_read32(at) : psp_read16(at);
            uint32_t r, g, b;
            switch (fmt) {
            case 0:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x3F) * 255 / 63; b = ((p >> 11) & 0x1F) * 255 / 31; break;
            case 1:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x1F) * 255 / 31; b = ((p >> 10) & 0x1F) * 255 / 31; break;
            case 2:  r = (p & 0xF) * 17; g = ((p >> 4) & 0xF) * 17; b = ((p >> 8) & 0xF) * 17; break;
            default: r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF; break;
            }
            g_frame[y * SCREEN_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return 1;
}

static void ctrl_commit(void);
static int g_dump_key;                     /* F12 pressed: capture on the next vblank */

/* The settings menu and the FPS counter (menu.c), drawn over the presented
 * image -- the same for either renderer, and never in the game's VRAM, so
 * F12 dumps and --capture-every frames stay the game's own. */
static menu_state g_menu;
static fps_meter  g_fpsm;
static uint32_t   g_host_held, g_host_latched;  /* MI_* from keys the game never sees */
static uint32_t   g_keys_latched;          /* PSP bits of keys pressed since the last vblank */
static char       g_menu_key_name[8] = "F1";

/* The PSN sign-in screen (login.c), opened when the game asks to sign in;
 * while open it takes all input. */
static login_state g_login;
static uint32_t    g_login_keys;          /* MI_* from navigation keys while a text screen is open */
/* The settings menu's SEGA SERVER editor (textedit.c). */
static textedit    g_sega_edit;
static int         g_edit_adhoc;            /* the editor is open for: 0 SEGA SERVER, 1 ADHOC SERVER, 2 MODERN SERVER */
static int         g_edit_enter;          /* Enter pressed in the editor: save */

static int text_ui_open(void) { return g_login.state != LOGIN_CLOSED || g_sega_edit.open; }

static void draw_overlay(void) {
    menu_image img = { g_rgba, SCREEN_W, SCREEN_H };
    if (g_menu.show_fps) fps_draw(&g_fpsm, &img);
#ifndef PSP2I_PROD
    {   /* F7's frame-rate audit, while it runs */
        const char *ra = rateaudit_status();
        if (ra[0]) menu_text(&img, 4, SCREEN_H - 22, ra, 0xFFD040u);
    }
#endif
    const char *st = online_status_line();
    if (st) {
        char line[96];
        snprintf(line, sizeof line, "%.78s", st);
        const int w = menu_text_width(line);
        for (int y = 2; y < 13; y++) for (int x = SCREEN_W - w - 7; x < SCREEN_W - 2; x++)
            if (x >= 0) g_rgba[y * SCREEN_W + x] = 0xFF000000u | ((g_rgba[y * SCREEN_W + x] >> 2) & 0x003F3F3Fu);
        menu_text(&img, SCREEN_W - w - 4, 4, line, 0xFFFFFF);
    }
    login_draw(&g_login, &img);
    menu_draw(&g_menu, &img, framerate_game_fps(), g_menu_key_name);
    textedit_draw(&g_sega_edit, &img);
}

/* What the window shows: the latest game frame with the overlay on top.
 * With a render scale, the S-times picture too: the overlay is drawn at
 * 480x272 as always, and each pixel it changed covers its S x S block. */
static void compose(void) {
    memcpy(g_rgba, g_frame, sizeof g_rgba);
    draw_overlay();
    if (g_hi_s <= 1 || !g_hi_rgba) return;
    const int S = g_hi_s, W = SCREEN_W * S;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            const int i = y * SCREEN_W + x;
            const int flat = !g_hi_valid || g_rgba[i] != g_frame[i];
            for (int k = 0; k < S; k++) {
                uint32_t *dst = g_hi_rgba + (size_t)(y * S + k) * W + (size_t)x * S;
                if (flat) for (int j = 0; j < S; j++) dst[j] = g_rgba[i];
                else memcpy(dst, g_hi_frame + (size_t)(y * S + k) * W + (size_t)x * S, (size_t)S * 4);
            }
        }
}

#ifdef _WIN32
static HWND g_wnd;
static int g_alt_enter;                     /* Alt+Enter pressed; handled with the menu */
static uint32_t g_keys;
static WPARAM g_menu_vk = VK_F1;
static ULONGLONG g_close_at;       /* when the window was closed (GetTickCount64); 0 = not closed */

/* Keys for the menu only: the hotkey, and Escape/Backspace as Back. */
static uint32_t host_key(WPARAM vk) {
    if (vk == g_menu_vk) return MI_TOGGLE;
    if (vk == VK_ESCAPE || vk == VK_BACK) return MI_BACK;
    return 0;
}

static int g_swap_oc;                       /* Circle <-> Cross (menu CIRCLE/CROSS, swap_circle_cross=) */
static float g_rs_saved;                    /* menu CONTROLS > SENSITIVITY (rs_speed=; 0: not saved yet) */
static int g_rs_on = 1;                     /* menu CONTROLS > RIGHT STICK (right_stick=on/off) */
static int g_test_ticks, g_test_logic;      /* menu TESTS (test_tick_pacing= / test_30hz_logic=; framerate.c) */

static uint32_t key_bit(WPARAM vk) {
    if (g_swap_oc && (vk == 'X' || vk == 'Z')) vk = vk == 'X' ? 'Z' : 'X';
    switch (vk) {
    case VK_UP:     return 0x0010;
    case VK_RIGHT:  return 0x0020;
    case VK_DOWN:   return 0x0040;
    case VK_LEFT:   return 0x0080;
    case 'Q':       return 0x0100;   /* L */
    case 'W':       return 0x0200;   /* R */
    case 'S':       return 0x1000;   /* triangle */
    case 'X':       return 0x2000;   /* circle */
    case 'Z':       return 0x4000;   /* cross */
    case 'A':       return 0x8000;   /* square */
    case VK_RETURN: return 0x0008;   /* start */
    case VK_SPACE:  return 0x0001;   /* select */
    default:        return 0;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CHAR:
        /* Text for the sign-in screen (letters do not act as PSP buttons then). */
        if (g_sega_edit.open) { if (wp > 32 && wp < 127) textedit_type(&g_sega_edit, (char)wp); }
        else if (g_login.state != LOGIN_CLOSED && wp >= 32 && wp < 127) login_type(&g_login, (char)wp);
        return 0;
    case WM_KEYDOWN: {
        const int first = !(lp & (1 << 30));                     /* not an auto-repeat */
#ifndef PSP2I_PROD                                     /* test keys: not in the community build */
        if (wp == VK_F12 && first) g_dump_key = 1;
        if (wp == VK_F9 && first && g_menu_vk != VK_F9) { functrace_toggle(); return 0; }   /* function-activity recorder */
        if (wp == VK_F7 && first && g_menu_vk != VK_F7) { rateaudit_start(); return 0; }    /* frame-rate audit */
        if (wp == VK_F6 && first && g_menu_vk != VK_F6) { rateaudit_start_press(); return 0; }   /* press audit */
        if (wp == VK_F8 && first && g_menu_vk != VK_F8) { vr_probe_request(); return 0; }    /* VR probe report (vr_probe.h) */
#ifdef PSP2I_VR
        if (wp == VK_F11 && first && g_menu_vk != VK_F11) { vrshot_request(); return 0; }   /* VR capture: eyes + report */
#endif
#endif
        if (text_ui_open() && wp != g_menu_vk) {
            uint32_t k = 0;
            switch (wp) {
            case VK_UP: k = MI_UP; break;
            case VK_DOWN: case VK_TAB: k = MI_DOWN; break;
            case VK_LEFT: k = MI_LEFT; break;
            case VK_RIGHT: k = MI_RIGHT; break;
            case VK_RETURN: if (g_sega_edit.open) g_edit_enter = 1; else k = MI_CONFIRM; break;
            case VK_ESCAPE: k = MI_BACK; break;
            case VK_BACK: if (g_sega_edit.open) textedit_backspace(&g_sega_edit); else login_backspace(&g_login); break;
            default: break;
            }
            g_login_keys |= k;
            return 0;
        }
        const uint32_t hk = host_key(wp);
        if (hk) { g_host_held |= hk; if (first) g_host_latched |= hk; return 0; }
        if (first) g_keys_latched |= key_bit(wp);
        g_keys |= key_bit(wp);  ctrl_commit(); return 0;
    }
    case WM_KEYUP:
        switch (wp) {
        case VK_UP: g_login_keys &= ~MI_UP; break;
        case VK_DOWN: case VK_TAB: g_login_keys &= ~MI_DOWN; break;
        case VK_LEFT: g_login_keys &= ~MI_LEFT; break;
        case VK_RIGHT: g_login_keys &= ~MI_RIGHT; break;
        case VK_RETURN: g_login_keys &= ~MI_CONFIRM; break;
        case VK_ESCAPE: g_login_keys &= ~MI_BACK; break;
        default: break;
        }
        g_host_held &= ~host_key(wp); g_keys &= ~key_bit(wp); ctrl_commit(); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {               /* Alt+Enter */
            if (!(lp & (1 << 30))) g_alt_enter = 1;
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == '\r') return 0;                                 /* no beep for Alt+Enter */
        break;
    case WM_SIZE:    present_resized(); web_overlay_resize(); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;                                 /* WM_PAINT covers it all */
    case WM_CLOSE:
        /* Gone at once: the game stops at its next check and the clean-up
         * (report, devices, libraries) runs after, out of sight and silent. */
        web_overlay_close();                   /* a web page open over the game (LoadExec, below) */
        g_close_at = GetTickCount64();
        ShowWindow(h, SW_HIDE);
        audio_set_master(0.0f);
        psp_request_exit();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (present_active() || g_gl_window) { EndPaint(h, &ps); return 0; }   /* the GPU shows it */
        RECT rc;
        GetClientRect(h, &rc);
        /* Aspect kept, black bars (GDI fallback; present.c does this on the GPU). */
        int x, y, w, hh;
        present_fit(rc.right, rc.bottom, SCREEN_W, SCREEN_H, &x, &y, &w, &hh);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;
        if (x > 0) {
            SetRect(&bar, 0, 0, x, rc.bottom); FillRect(dc, &bar, black);
            SetRect(&bar, x + w, 0, rc.right, rc.bottom); FillRect(dc, &bar, black);
        }
        if (y > 0) {
            SetRect(&bar, 0, 0, rc.right, y); FillRect(dc, &bar, black);
            SetRect(&bar, 0, y + hh, rc.right, rc.bottom); FillRect(dc, &bar, black);
        }
        SetStretchBltMode(dc, COLORONCOLOR);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = SCREEN_W;
        bi.bmiHeader.biHeight = -SCREEN_H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, x, y, w, hh, 0, 0, SCREEN_W, SCREEN_H,
                      g_rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* Size the window for a RES_ mode (menu.h): a framed window with that client
 * size, shrunk (aspect kept) to fit the monitor's work area, or borderless
 * covering the whole monitor for RES_FULLSCREEN. */
static void window_apply_res(int mode) {
    if (!g_wnd) return;
    if (IsZoomed(g_wnd) || IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
    int w, h;
    if (!menu_res_size(mode, &w, &h)) {
        const RECT m = mi.rcMonitor;
        SetWindowLongPtrA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        const RECT wa = mi.rcWork;
        RECT r = { 0, 0, w, h };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        const int fw = (r.right - r.left) - w, fh = (r.bottom - r.top) - h;   /* frame */
        const int aw = (wa.right - wa.left) - fw, ah = (wa.bottom - wa.top) - fh;
        if (w > aw || h > ah) {
            int x, y;
            present_fit(aw, ah, w, h, &x, &y, &w, &h);
            fprintf(stderr, "window: %s does not fit this monitor; using %dx%d\n", menu_res_name(mode), w, h);
        }
        SetWindowLongPtrA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_NOTOPMOST,
                     wa.left + ((wa.right - wa.left) - (w + fw)) / 2, wa.top + ((wa.bottom - wa.top) - (h + fh)) / 2,
                     w + fw, h + fh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    present_resized();
}

static void window_open(int res) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "psp2i";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_OWNDC;                                     /* OpenGL keeps the window's DC */
    /* The exe's icon (resource 1, asset/icon.ico; compiled into the prod
     * build): large for the taskbar and Alt+Tab, small for the title bar.
     * Without the resource both are NULL and Windows' default stays. */
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    RegisterClassA(&wc);
    g_wnd = CreateWindowA("psp2i", WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, SCREEN_W * 2, SCREEN_H * 2,
                          NULL, NULL, wc.hInstance, NULL);
    {
        HICON sm_icon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (wc.hIcon) SendMessageA(g_wnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
        if (sm_icon) SendMessageA(g_wnd, WM_SETICON, ICON_SMALL, (LPARAM)sm_icon);
    }
    window_apply_res(res);
    const char *e = getenv("PSP2I_PRESENT");
    if (e && !strcmp(e, "gdi")) fprintf(stderr, "present: PSP2I_PRESENT=gdi, scaling with GDI\n");
    else if (g_renderer != REN_GL) present_init(g_wnd);      /* OpenGL presents itself (window_show) */
}

/* The composed frame into the window on the GPU: 0, or nonzero for GDI. */
static int window_show(void) {
    if (g_gl_window) {
        RECT rc;
        GetClientRect(g_wnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return 0;      /* minimised */
        int x, y, w, h;
        present_fit(rc.right, rc.bottom, SCREEN_W, SCREEN_H, &x, &y, &w, &h);
        if (gl_blit_frame(g_rgba, SCREEN_W, SCREEN_H, x, y, w, h, rc.right, rc.bottom) != 0) return -1;
        gl_platform_swap();
        return 0;
    }
    if (g_hi_s > 1 && g_hi_rgba && present_active()) return present_frame(g_hi_rgba, SCREEN_W * g_hi_s, SCREEN_H * g_hi_s);
    return present_active() ? present_frame(g_rgba, SCREEN_W, SCREEN_H) : -1;
}

static void window_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}
#endif

/* ---- the per-vblank hook --------------------------------------------------- */

static int      g_headless;
static double   g_seconds;
static uint32_t g_capture_every;
static char     g_capture_dir[512] = "captures";
#define MAX_OVERLAY_SHOT 16
static uint64_t g_overlay_shot[MAX_OVERLAY_SHOT];   /* --overlay-shot */
static int      g_noverlay_shot;

/* Scripted input for headless runs: --press BUTTON@VBLANK[+FRAMES] holds a
 * button from that vblank for FRAMES vblanks (default 6). Repeatable. */
#define MAX_PRESS 128
static struct { uint32_t bits; uint64_t at, len; } g_press[MAX_PRESS];
static int g_npress;
static uint32_t g_script_bits;
#define PRESS_MENU 0x80000000u             /* --press menu@...: the settings-menu hotkey, never the game's */

static int add_press(const char *spec) {
    static const struct { const char *name; uint32_t bit; } B[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l", 0x0100 }, { "r", 0x0200 },
        { "triangle", 0x1000 }, { "circle", 0x2000 }, { "cross", 0x4000 }, { "square", 0x8000 },
        { "menu", PRESS_MENU },
    };
    char name[32];
    unsigned long long at = 0, len = 6;
    if (g_npress >= MAX_PRESS || sscanf(spec, "%31[a-z]@%llu+%llu", name, &at, &len) < 2) return -1;
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        if (strcmp(name, B[i].name)) continue;
        g_press[g_npress].bits = B[i].bit;
        g_press[g_npress].at = at;
        g_press[g_npress].len = len;
        g_npress++;
        return 0;
    }
    return -1;
}

/* --stick X,Y@VBLANK[+FRAMES]: hold the analog stick at PSP values X,Y
 * (0..255, 128 = centre) -- scripted analog input for headless tests. */
#define MAX_STICK 32
static struct { uint8_t x, y; uint64_t at, len; } g_stick[MAX_STICK];
static int g_nstick;
static int g_script_stick = -1;            /* index held now, -1 none */
static struct { uint8_t x, y; uint64_t at, len; } g_rstick[MAX_STICK];   /* --rstick */
static int g_nrstick;

static int add_stick(const char *spec) {
    unsigned x, y;
    unsigned long long at = 0, len = 6;
    if (g_nstick >= MAX_STICK || sscanf(spec, "%u,%u@%llu+%llu", &x, &y, &at, &len) < 3 || x > 255 || y > 255)
        return -1;
    g_stick[g_nstick].x = (uint8_t)x; g_stick[g_nstick].y = (uint8_t)y;
    g_stick[g_nstick].at = at; g_stick[g_nstick].len = len;
    g_nstick++;
    return 0;
}

/* ---- input recording and replay -------------------------------------------------
 * PSP2I_INPUT_RECORD=FILE records what the game reads -- buttons and left stick
 * after the menu filter, and the camera stick -- whenever it changes, keyed by
 * game frame (display flip; the game reads input once per frame), plus the
 * frame-rate setting:
 *   fps 60                     at the start, and after every menu change
 *   F <flip> <buttons> <lx> <ly>   buttons in hex, sticks 0..255 (128 centre)
 *   R <flip> <rx> <ry>
 * --replay-input FILE plays such a file back: the same input on the same game
 * frame, so a route a player walked once can be re-run headless (fps from the
 * file unless --fps is given). */
static FILE    *g_rec;
static uint32_t g_rec_bits = 0xFFFFFFFFu;
static uint8_t  g_rec_ax, g_rec_ay, g_rec_rx = 128, g_rec_ry = 128;

static void rec_open(const char *path, int fps) {
    g_rec = fopen(path, "w");
    if (!g_rec) { fprintf(stderr, "input: cannot write the recording %s\n", path); return; }
    fprintf(g_rec, "# psp2i input recording (see --replay-input)\nfps %d\n", fps);
    fflush(g_rec);
    fprintf(stderr, "input: recording to %s\n", path);
}

static void rec_pad(uint32_t bits, uint8_t ax, uint8_t ay) {
    if (!g_rec || (bits == g_rec_bits && ax == g_rec_ax && ay == g_rec_ay)) return;
    g_rec_bits = bits; g_rec_ax = ax; g_rec_ay = ay;
    fprintf(g_rec, "F %llu %04X %u %u\n", (unsigned long long)psp_display_flips(), bits, ax, ay);
    fflush(g_rec);
}

static void rec_cam(uint8_t rx, uint8_t ry) {
    if (!g_rec || (rx == g_rec_rx && ry == g_rec_ry)) return;
    g_rec_rx = rx; g_rec_ry = ry;
    fprintf(g_rec, "R %llu %u %u\n", (unsigned long long)psp_display_flips(), rx, ry);
    fflush(g_rec);
}

static void rec_fps(int fps) {
    if (!g_rec) return;
    fprintf(g_rec, "fps %d\n", fps);
    fflush(g_rec);
}

typedef struct { uint64_t flip; char kind; uint32_t a; uint8_t x, y; } rep_event;
static rep_event *g_rep;
static int      g_nrep, g_rep_pos, g_rep_on;
static uint32_t g_rep_bits;
static uint8_t  g_rep_ax = 128, g_rep_ay = 128, g_rep_rx = 128, g_rep_ry = 128;

/* Load a recording; returns its first fps (0 if none), -1 on error. */
static int replay_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "input: cannot read the recording %s\n", path); return -1; }
    char line[128];
    int cap = 0, first_fps = 0;
    uint64_t last_flip = 0;
    while (fgets(line, sizeof line, f)) {
        rep_event e;
        memset(&e, 0, sizeof e);
        unsigned long long fl;
        unsigned a, x, y;
        int fps;
        if (line[0] == 'F' && sscanf(line + 1, "%llu %x %u %u", &fl, &a, &x, &y) == 4) { e.kind = 'F'; e.flip = fl; e.a = a; e.x = (uint8_t)x; e.y = (uint8_t)y; }
        else if (line[0] == 'R' && sscanf(line + 1, "%llu %u %u", &fl, &x, &y) == 3) { e.kind = 'R'; e.flip = fl; e.x = (uint8_t)x; e.y = (uint8_t)y; }
        else if (sscanf(line, "fps %d", &fps) == 1) { if (!first_fps && !g_nrep) { first_fps = fps; continue; } e.kind = 'S'; e.flip = last_flip; e.a = (uint32_t)fps; }
        else continue;
        last_flip = e.flip;
        if (g_nrep == cap) {
            cap = cap ? cap * 2 : 1024;
            rep_event *n = (rep_event *)realloc(g_rep, sizeof *g_rep * (size_t)cap);
            if (!n) { fclose(f); return -1; }
            g_rep = n;
        }
        g_rep[g_nrep++] = e;
    }
    fclose(f);
    g_rep_on = 1;
    fprintf(stderr, "input: replaying %s (%d events, to game frame %llu)\n", path, g_nrep,
            g_nrep ? (unsigned long long)g_rep[g_nrep - 1].flip : 0ull);
    return first_fps;
}

static void ctrl_commit(void);
static void replay_step(void) {
    if (!g_rep_on) return;
    const uint64_t f = psp_display_flips();
    int pad = 0;
    while (g_rep_pos < g_nrep && g_rep[g_rep_pos].flip <= f) {
        const rep_event *e = &g_rep[g_rep_pos++];
        if (e->kind == 'F') { g_rep_bits = e->a; g_rep_ax = e->x; g_rep_ay = e->y; pad = 1; }
        else if (e->kind == 'R') { g_rep_rx = e->x; g_rep_ry = e->y; }
        else if (e->kind == 'S') { framerate_set((int)e->a); g_menu.fps = (int)e->a; fprintf(stderr, "input: replay sets %u fps\n", e->a); }
    }
    if (pad) ctrl_commit();
    if (g_rep_pos == g_nrep && g_nrep) {
        fprintf(stderr, "input: replay finished at game frame %llu (vblank %llu)\n",
                (unsigned long long)f, (unsigned long long)psp_sched_vblank_count());
        g_nrep = 0;                                       /* report once; the last state stays */
    }
}

/* --dump-ram-at VBLANK PATH (repeatable): guest RAM as a raw file. */
#define MAX_RAMDUMP 8
static struct { uint64_t at; char path[512]; } g_ramdump[MAX_RAMDUMP];
static int g_nramdump;

static void dump_ram_now(uint64_t vb) {
    for (int i = 0; i < g_nramdump; i++) {
        if (g_ramdump[i].at != vb) continue;
        FILE *f = fopen(g_ramdump[i].path, "wb");
        if (f) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, f); fclose(f); }
        fprintf(stderr, "ram: dumped at vblank %llu -> %s\n", (unsigned long long)vb, g_ramdump[i].path);
    }
}

/* --log-floats ADDR COUNT EVERY: print COUNT floats at guest ADDR every
 * EVERY vblanks (e.g. a player position while testing movement). */
static uint32_t g_logf_addr, g_logf_count, g_logf_every;

static void apply_script(uint64_t vb) {
    replay_step();
    if (g_nramdump) dump_ram_now(vb);
    if (g_logf_every && vb % g_logf_every == 0) {
        fprintf(stderr, "floats: vblank %llu flip %llu", (unsigned long long)vb, (unsigned long long)psp_display_flips());
        for (uint32_t i = 0; i < g_logf_count; i++) {
            uint32_t w = psp_read32(g_logf_addr + i * 4);
            float v;
            memcpy(&v, &w, 4);
            fprintf(stderr, " %.3f", v);
        }
        fprintf(stderr, "\n");
    }
    if (g_nstick) {
        int k = -1;
        for (int i = 0; i < g_nstick; i++)
            if (vb >= g_stick[i].at && vb < g_stick[i].at + g_stick[i].len) k = i;
        if (k != g_script_stick) {
            g_script_stick = k;
            if (k >= 0) fprintf(stderr, "input: stick %u,%u at vblank %llu\n", g_stick[k].x, g_stick[k].y, (unsigned long long)vb);
            else fprintf(stderr, "input: stick centred at vblank %llu\n", (unsigned long long)vb);
            ctrl_commit();
        }
    }
    if (!g_npress) return;
    uint32_t bits = 0;
    for (int i = 0; i < g_npress; i++)
        if (vb >= g_press[i].at && vb < g_press[i].at + g_press[i].len) bits |= g_press[i].bits;
    if (bits != g_script_bits) {
        fprintf(stderr, "input: buttons 0x%04X at vblank %llu\n", bits, (unsigned long long)vb);
        g_script_bits = bits;
        ctrl_commit();
    }
}

/* What the game reads: keyboard, SDL controllers and the --press script
 * combined. Buttons are OR-ed; the analog stick comes from the controllers
 * (centred when none is deflected). */
static int      g_sdl_on;
static uint32_t g_pad_bits;
static uint8_t  g_pad_ax = 128, g_pad_ay = 128;

/* Game units per metre: a first estimate (the orbit camera sits ~55-60 units
 * behind the player, about 5.5 m at this scale); vr_world_scale= adjusts it. */
#define VR_WORLD_SCALE_DEFAULT 10.0f

/* VR (vr/vr.h): the theater screen and the motion controllers as a pad.
 * Saved in psp2i_display.ini (vr=, vr_distance=, vr_width=); --vr / --no-vr
 * for one run. The controllers add to the SDL pad: buttons OR together, and
 * the VR sticks win unless only the SDL one is pushed. */
static int      g_vr_on;                    /* the saved choice (menu VR) */
static int      g_vr_dist_dm = 25, g_vr_width_dm = 30;
/* Phase 2, stereo (vr_stereo=, vr_world_scale=): with Direct3D 11 the game is
 * drawn again for each eye at the GE (psp_gpu_set_stereo, d3d11.c's eye
 * copies) and the screen becomes a stereo window. The world scale is game
 * units per metre: it sets the IPD and the convergence distance (the
 * screen's distance) in game units. */
static int      g_vr_stereo = 1;
static float    g_vr_world_scale = VR_WORLD_SCALE_DEFAULT;
/* Phase 3, head tracking (vr_head=, vr_level=): with stereo, each eye is
 * drawn from the head (the game camera plus the head's pose since recenter)
 * and the headset shows the pictures as a projection layer at the game's
 * FOV, instead of the window. vr_level takes the game camera's pitch and
 * roll out, so the horizon stays level with the real floor. */
static int      g_vr_head = 1, g_vr_level = 1;
/* vr_fov_scale=, vr_fov_scale_y=: with head tracking the eyes see that many
 * times the game's view across and up (its FOV tangents). The picture keeps
 * the PSP's shape, so a taller scale makes its pixels taller: the game's
 * 480:272 at 1.6 across would be only ~67 degrees tall.
 * vr_cull_scale=: the game's frustum cull (0x08D7D63C) is built that much
 * wider, so what the wider view and the head turns show is drawn. */
#define VR_FOV_SCALE_DEFAULT  1.6f
#define VR_FOV_SCALE_Y_DEFAULT 2.6f
#define VR_CULL_SCALE_DEFAULT 8.0f
static float    g_vr_fov = VR_FOV_SCALE_DEFAULT, g_vr_fov_y = VR_FOV_SCALE_Y_DEFAULT, g_vr_cull = VR_CULL_SCALE_DEFAULT;
/* vr_fov_auto=on: the fov scales follow the headset's own eye view
 * (vr_view_tan) plus 5%, so the eye pictures always fill it. At 1.6 / 2.6
 * (99 x 94 degrees) the PS VR2's wider view showed the pictures' edge, moving
 * with the head: "a transparent rectangle overhead", dark or light where an
 * area tints the screen (user, 2026-10-23). The picture's rows are set at the
 * start from the saved scales, so a change is fully sharp from the next start. */
static int      g_vr_fov_auto = 1;
/* The HUD panel (as PSUEx-VR does it): the HUD pass's HUD and menus, drawn
 * as the game drew them into their own picture (d3d11.c draw_hud), on a flat
 * panel vr_hud_distance= metres ahead, a little below eye level, as wide as
 * vr_hud_size= of the game's view there, turning only to follow the heading
 * once the head has looked more than vr_hud_follow= degrees away (vr.h). */
#define VR_HUD_SIZE_DEFAULT     0.7f
/* Which HUD-pass sheets go on the panel, by their clip w (PSP2i draws every
 * HUD-pass sheet at a fixed depth per layer: GE dumps and VR captures,
 * ADDRESS_MAP.md "The HUD pass"): menus 3-71 (start menu, shops, mission
 * banners, the top-left panel), the field HUD 84-85.99 (HP panel, palette,
 * prompts), the minimap's frame 86.85-87.05 and its 3D map (2300 and more),
 * and the menu background layer 92 (the white sheet behind dialogs such as
 * "Abandon mission?": VR capture 2026-10-22). Everything else stays in the
 * world: the place and enemy labels (86, 88), the lock-on reticle (90).
 * Unknown layers stay in the world. Any sheet covering more than a fifth of
 * the screen goes on the panel too: NPC portraits share the labels' layer
 * (86: the shop's portrait, GE dump 2026-10-22), and labels are small. */
static int psp2i_hud_panel(float w, float frac) {
    const int layer = w < 80.0f || (w >= 84.0f && w < 85.995f) || (w >= 86.85f && w <= 87.05f) || (w >= 91.5f && w <= 92.5f) ||
                      w > 1000.0f;
#ifndef PSP2I_PROD
    /* Dev: the big sheets that go on the panel only by their size (user: "the
     * flat appears while moving"; a full-screen effect there would put a flat
     * copy of the game in front of them). Each layer and size once. */
    if (!layer && frac > 0.2f) {
        static int seen[64], n;
        const int key = (int)lroundf(w * 10.0f) * 16 + (int)(frac * 10.0f);
        int k = 0;
        while (k < n && seen[k] != key) k++;
        if (k == n && n < 64) {
            seen[n++] = key;
            fprintf(stderr, "vr: a big HUD-pass sheet: w %.2f, %.0f%% of the screen (vblank %llu): %s\n", (double)w,
                    (double)(frac * 100.0f), (unsigned long long)psp_sched_vblank_count(),
                    w < 89.0f ? "on the panel" : "in the world");
        }
    }
#endif
    /* By size only at the labels' depth (the portraits, w 86): big sheets
     * deeper than that are things in the world the game draws in its HUD
     * pass (w 90-220, 20-209% of the screen, depth changing as they move:
     * VR log 2026-10-23), which on the panel put a flat sheet in front of
     * the user while moving. */
    return (frac > 0.2f && w < 89.0f) || layer;
}
/* Screen-space effects (hit sparks, techniques: small orthographic draws in
 * the scene pass) are placed this far along the camera's ray: about where
 * the player and what they hit are (the orbit camera is 55-60 units back). */
#define PSP2I_FX_DIST           55.0f
/* First person: the effects that far beyond the player instead (game units,
 * ~1.8 m): at the player's own distance they were at the eyes and cut away
 * (user: "some techniques become invisible" since first person). */
#define PSP2I_FP_FX_AHEAD       6.0f
/* vr_menu_screen=on: PSP2i's menu scenes (the continue page, mission select,
 * loading screens: small 3D dioramas with their own camera) are shown as
 * the game draws them on the flat screen, not from the head. They are told
 * from the field by the camera the frame is drawn with (VR captures,
 * 2026-10-22): the field's projection has x scale 1.3684 (tan 0.7308) and is
 * centred, and its view is turned; mission select has x scale 0.9816, the
 * continue page an off-centre projection (PROJ[8] -0.4) and an unturned view
 * (identity rotation), like the loading screens. */
#define PSP2I_FIELD_PROJ_X      1.3684f
#define PSP2I_PROJ_MATRIX_G     0x08FA97A0u
static int      g_vr_menu_screen = 1;
static int      g_menu_scene = -1;          /* the last frame was a menu scene (logged on change) */
/* The menu scene is a loading transition (an unturned view with the field's
 * centred projection, between areas): the headset keeps the last 3D picture
 * up to 5 s through it instead of 1 s (user: "the flat screen blips still
 * occur"). Real menus are off-centre or much wider. */
static int      g_menu_loading;
/* P4 first person (vr_view=first, vr_fp_eye=, vr_fp_scale=): with head
 * tracking the eyes sit in the local player's head instead of at the game
 * camera, rendering only (the camera, and what other players see, stay the
 * game's). The head is placed from the player's position (player +0x4B0, the
 * feet), vr_fp_eye game units up along world up. Measured 2026-10-22 (the
 * GE probe, standing in town): the model's top is 5.8 units above the
 * position, the hips 3.05, so a ~1.7 m character makes about 3.4 units per
 * metre; vr_fp_scale is that, for the head's movement and the IPD in first
 * person (vr_world_scale stays third person's). The view's heading is the
 * game camera's (it follows behind the player; the right stick turns it). */
#define VR_FP_EYE_DEFAULT   5.3f
#define VR_FP_SCALE_DEFAULT 3.4f
/* vr_fp_forward=: the eyes that many game units ahead of the head along the
 * gaze, so the player's own head is always behind them (user, first run: "I
 * am inside the head"). 1.2 is ~35 cm: past a head's radius (~0.4) even
 * when the run animation leans the head forward (the user asked for more
 * than 0.6 for running). */
#define VR_FP_FORWARD_DEFAULT 1.2f
/* vr_fp_near=: in first person the eyes' 3D scene is clipped this many game
 * units in front of them (psp_gpu_stereo.near_w): the run animation still
 * threw the head ahead of the eyes at 1.2 (user: "running, still see back of
 * head"), and a clip plane hides it whatever the animation does. 1.0 is
 * ~30 cm. */
#define VR_FP_NEAR_DEFAULT 1.0f
static int      g_vr_first;
static float    g_vr_fp_eye = VR_FP_EYE_DEFAULT, g_vr_fp_scale = VR_FP_SCALE_DEFAULT, g_vr_fp_fwd = VR_FP_FORWARD_DEFAULT;
static float    g_vr_fp_near = VR_FP_NEAR_DEFAULT;
static float g_fp_head_off[3];               /* the head's centre across from the feet, world axes (head_frame's probe) */
static int   g_fp_head_ok;
static int   g_fp_cam;                       /* the last field camera was applied at the head */
static float g_fp_cam_hr[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   /* ... turned as the head was then (level frame) */
/* First person's own heading (the angle of the view's level backward axis,
 * atan2(x, z) in the world), so the view doesn't swing when the game camera
 * trails round behind the player: it turns with the game camera only while
 * the right stick turns it, and takes the game's again after a cut or reset
 * (a jump of over FP_YAW_CUT radians), at recenter and when first person
 * starts. g_fp_game_yaw is the game camera's own (eye - target). The left
 * stick is turned by (this heading + the head's yaw - the game's), so it
 * walks where the head looks (ctrl_commit). */
#define FP_YAW_CUT 0.5f
static float g_fp_yaw, g_fp_game_yaw;
static int   g_fp_yaw_ok;
static int   g_fp_stick_on, g_fp_stick_in[2], g_fp_stick_out[2];   /* the last turned stick (the dev walk log) */
/* vr_fp_nudge=: in first person, an attack (Square, Triangle) pressed while
 * the character faces more than FP_NUDGE_MIN away from where the head looks
 * is held back while the stick turns it to the head first (fp_nudge). */
static int   g_vr_fp_nudge = 1;
/* P5 motion controls (vr_input=pad|motion, engine menu VR > INPUT MODE): with
 * MOTION the headset shows the controllers' hands (vr.h vr_hands_get), and
 * each 3D frame places them in the game world (head_frame: through the same
 * level frame and scale as the eyes), for the arms, swings and aim to come
 * (VR_PLAN P5). The controllers' buttons stay the PSP pad either way. */
static int   g_vr_motion;
typedef struct {
    int   ok;
    float p[3];          /* the grip, world */
    float axes[9];       /* its axes in the world, column-major (x, y, z) */
    float aim_p[3], aim_d[3];   /* the pointer ray, world */
} game_hand;
static game_hand g_hand_w[2];
/* ... and its projection that much wider across / up (1 = the game's): the
 * eyes' view plus VR_FP_VIEW_MARGIN, so the game culls nothing the eyes see,
 * even as they turn before the next frame (user: "when I look up the bottom
 * is missing, when I look down the top: make the radius bigger"). */
#define VR_FP_VIEW_MARGIN 1.25f
static float g_fp_kx = 1.0f, g_fp_ky = 1.0f;
static float g_fp_p0, g_fp_p5;               /* the field camera's own projection x / y scales (before widening) */
/* The eyes' view scales for the frame being drawn (psp_gpu_stereo.fov_scale):
 * against the GE's projection, which is the widened one in first person. */
static float g_eye_fs_x = VR_FOV_SCALE_DEFAULT, g_eye_fs_y = VR_FOV_SCALE_Y_DEFAULT, g_eye_kyx = 1.0f;
static float g_ge_kx = 1.0f;                 /* the GE's projection's widening measured this frame (1 = none) */
/* VR image quality (vr_aniso=, vr_sharpen=; d3d11_set_quality, from the
 * next start): anisotropic filtering with mipmaps for the game's textures,
 * and a contrast-adaptive sharpening pass on the eye pictures. Only with VR
 * on: the window's game is drawn as before otherwise. */
#define VR_ANISO_DEFAULT   16
#define VR_SHARPEN_DEFAULT 0.5f
static int      g_vr_aniso = VR_ANISO_DEFAULT;
static float    g_vr_sharpen = VR_SHARPEN_DEFAULT;
#define VR_HUD_DIST_INI_DEFAULT   2.5f          /* = vr.h VR_HUD_DISTANCE_DEFAULT (vr.h is VR builds only) */
#define VR_HUD_FOLLOW_INI_DEFAULT 35.0f         /* = vr.h VR_HUD_FOLLOW_DEFAULT */
static float    g_vr_hud_dist = VR_HUD_DIST_INI_DEFAULT, g_vr_hud_size = VR_HUD_SIZE_DEFAULT;
static float    g_vr_hud_follow = VR_HUD_FOLLOW_INI_DEFAULT;
static int      g_head_on;                  /* the eyes follow the head now */
static int      g_fake_head;                /* PSP2I_VR_FAKE_HEAD=yaw,pitch (dev): a fixed head turn without a headset */
static float    g_fake_yaw, g_fake_pitch;   /* degrees */
static int      g_stereo_arg;               /* --stereo: the eyes without a headset (dev) */
static int      g_stereo_on;                /* the eyes are drawn now */
static const uint32_t *g_eye_px[2];         /* the latest eye pictures (d3d11.c), or NULL */
static int      g_eye_w, g_eye_h;
static int      g_eye_new;                  /* g_eye_px changed this vblank */
/* The headset takes the eyes on the GPU (d3d11_eye_share) unless sharing
 * failed; the overlay then goes as its own layer (vr_submit_overlay). */
static int      g_overlay_up;               /* the last compose() drew something */
static uint32_t g_ovl_px[SCREEN_W * SCREEN_H], g_ovl_sent[SCREEN_W * SCREEN_H];
static int      g_ovl_sent_valid;
static void    *g_eye_shared;               /* this vblank's shared eye texture, or NULL */
static int      g_eye_shared_w, g_eye_shared_h;
static uint32_t g_eye_shared_tag, g_eye_px_tag;   /* the head pose (vr_head.id) the eyes were drawn with, 0 = none */
static uint64_t g_eye_shared_vb;            /* the last vblank one was sent */
static uint32_t *g_eye_rgba[2];             /* ... with the overlay, for the headset */
static int      g_vr_live;                  /* vr_start ran */
static uint32_t g_vr_host;                  /* VR_HOST_* held */
static uint8_t  g_vr_rx = 128, g_vr_ry = 128;
static char     g_exe_dir[512];

static int stick_off_centre(uint8_t x, uint8_t y) { return x < 96 || x > 160 || y < 96 || y > 160; }

#ifdef PSP2I_VR
static void vr_apply(void) {
    /* The swapchain matches the render scale, so the headset gets every pixel. */
    /* The HUD panel: vr_hud_size of the game's view width (fovy 45 at 480:272) at its distance. */
    const vr_config cfg = { g_vr_dist_dm / 10.0f, g_vr_width_dm / 10.0f, g_hi_s > 1 ? g_hi_s : VR_SCALE_DEFAULT,
                            g_vr_stereo && g_renderer == REN_D3D11, g_vr_hud_dist, VR_HUD_DROP_DEFAULT,
                            2.0f * g_vr_hud_dist * 0.7311f * g_vr_hud_size, g_vr_hud_follow,
                            g_hi_s > 1 ? g_hi_sy : 0, g_vr_motion };
    if (g_vr_on && !g_vr_live && !g_headless) {
        vr_set_swap_circle_cross(g_swap_oc);
        g_vr_live = vr_start(g_exe_dir, &cfg) == 0;
    } else if (!g_vr_on && g_vr_live) {
        /* No more eyes to the headset from this frame on: the next present
         * came before the stereo update turned sharing off, and crashed in
         * share_eyes once the runtime was gone (VR off in the menu, crash
         * 2026-10-23). */
        if (g_renderer == REN_D3D11) d3d11_eye_share(0);
        vr_stop();
        g_vr_live = 0;
        g_vr_host = 0;
        g_vr_rx = g_vr_ry = 128;
    } else if (g_vr_live) {
        vr_configure(&cfg);
    }
}

/* Column-major 3x3 (m[c * 3 + r]) of the rotation q (x, y, z, w). */
static void quat_mat3(const float *q, float *m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[3] = 2 * (x * y - z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + x * w);
    m[6] = 2 * (x * z + y * w);     m[7] = 2 * (y * z - x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

static void mat3_mul(const float *a, const float *b, float *out) {   /* out = a * b */
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) out[c * 3 + r] = a[r] * b[c * 3] + a[3 + r] * b[c * 3 + 1] + a[6 + r] * b[c * 3 + 2];
}

/* vr_level: the game camera with only its heading, as axes in the game
 * camera's own view space (columns x, y, z). World up seen from the camera
 * is the view matrix's second column (0x08FA98A0, the view the frame is
 * drawn with: ADDRESS_MAP.md). Identity when that is not a usable view. */
#define PSP2I_VIEW_MATRIX 0x08FA98A0u
static void level_axes(float *m) {
    static const float ID[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    memcpy(m, ID, sizeof ID);
    float u[3] = { psp_read_f32(PSP2I_VIEW_MATRIX + 16), psp_read_f32(PSP2I_VIEW_MATRIX + 20),
                   psp_read_f32(PSP2I_VIEW_MATRIX + 24) };
    const float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul > 0.5f && ul < 2.0f)) return;
    for (int k = 0; k < 3; k++) u[k] /= ul;
    /* Forward (-z) with the up part taken out: the heading. */
    float f[3] = { u[2] * u[0], u[2] * u[1], -1.0f + u[2] * u[2] };
    const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-3f) return;                                     /* looking straight up or down */
    const float z[3] = { -f[0] / fl, -f[1] / fl, -f[2] / fl };
    const float x[3] = { u[1] * z[2] - u[2] * z[1], u[2] * z[0] - u[0] * z[2], u[0] * z[1] - u[1] * z[0] };
    for (int k = 0; k < 3; k++) { m[k] = x[k]; m[3 + k] = u[k]; m[6 + k] = z[k]; }
}

/* The view (inverse of the camera [q | p]) into m, column-major 4x4. */
static void view_of(const float *q, const float *p, float *m) {
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) m[c * 4 + r] = q[r * 3 + c];
    for (int r = 0; r < 3; r++) m[12 + r] = -(q[r * 3] * p[0] + q[r * 3 + 1] * p[1] + q[r * 3 + 2] * p[2]);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

/* P5: the hands in the game world for this frame. A hand relative to the
 * recentred head goes where the eyes' head would be at that offset: base
 * (the head's origin in the game camera's view space) + k * lev * p, then
 * out of the view space through the view matrix the frame is drawn with
 * (its rotation transposed). Dev builds log them every ~2 s against the
 * player's feet, the check that the scale and frame are right (a hand held
 * at the hip in first person: ~3 units up from the feet, at 3.4 a metre). */
static void hands_frame(const float *base, const float *lev, float k) {
    vr_hand vh[2];
    if (!g_vr_motion || !g_vr_live || !vr_hands_get(vh)) {
        g_hand_w[0].ok = g_hand_w[1].ok = 0;
        return;
    }
    float v[16];
    for (int i = 0; i < 16; i++) v[i] = psp_read_f32(PSP2I_VIEW_MATRIX + 4u * (uint32_t)i);
    for (int hd = 0; hd < 2; hd++) {
        game_hand *o = &g_hand_w[hd];
        o->ok = vh[hd].ok;
        if (!o->ok) continue;
        float pv[2][3], hr[9], ar[9], gq[9], aq[9];
        for (int r = 0; r < 3; r++) {
            pv[0][r] = base[r] + k * (lev[r] * vh[hd].p[0] + lev[3 + r] * vh[hd].p[1] + lev[6 + r] * vh[hd].p[2]);
            pv[1][r] = base[r] + k * (lev[r] * vh[hd].aim_p[0] + lev[3 + r] * vh[hd].aim_p[1] + lev[6 + r] * vh[hd].aim_p[2]);
        }
        quat_mat3(vh[hd].q, hr);
        quat_mat3(vh[hd].aim_q, ar);
        mat3_mul(lev, hr, gq);                                   /* axes in the camera's view space */
        mat3_mul(lev, ar, aq);
        for (int c = 0; c < 3; c++) {                            /* view -> world: R^T (x - t) */
            o->p[c] = v[c * 4] * (pv[0][0] - v[12]) + v[c * 4 + 1] * (pv[0][1] - v[13]) + v[c * 4 + 2] * (pv[0][2] - v[14]);
            o->aim_p[c] = v[c * 4] * (pv[1][0] - v[12]) + v[c * 4 + 1] * (pv[1][1] - v[13]) + v[c * 4 + 2] * (pv[1][2] - v[14]);
            o->aim_d[c] = -(v[c * 4] * aq[6] + v[c * 4 + 1] * aq[7] + v[c * 4 + 2] * aq[8]);   /* the ray is -z */
            for (int a = 0; a < 3; a++)
                o->axes[a * 3 + c] = v[c * 4] * gq[a * 3] + v[c * 4 + 1] * gq[a * 3 + 1] + v[c * 4 + 2] * gq[a * 3 + 2];
        }
    }
#ifndef PSP2I_PROD
    static uint64_t last;
    const uint64_t vb = psp_sched_vblank_count();
    if (vb - last >= 120) {
        last = vb;
        const uint32_t pl = gamelog_local_player();
        float f[3] = { 0, 0, 0 };
        if (pl) for (int i = 0; i < 3; i++) f[i] = psp_read_f32(pl + 0x4B0u + 4u * (uint32_t)i);
        for (int hd = 0; hd < 2; hd++) {
            const game_hand *o = &g_hand_w[hd];
            if (!o->ok) { fprintf(stderr, "vr: hand %s: not tracked\n", hd ? "right" : "left"); continue; }
            const float dx = o->p[0] - f[0], dz = o->p[2] - f[2];
            fprintf(stderr, "vr: hand %s: world %.1f %.1f %.1f; from the feet %.2f across, %.2f up (%s person, %.1f units/m);"
                            " aim %.2f %.2f %.2f\n", hd ? "right" : "left", (double)o->p[0], (double)o->p[1], (double)o->p[2],
                    (double)sqrtf(dx * dx + dz * dz), (double)(o->p[1] - f[1]), g_vr_first ? "first" : "third", (double)k,
                    (double)o->aim_d[0], (double)o->aim_d[1], (double)o->aim_d[2]);
        }
    }
#endif
}

/* psp_gpu_stereo.on_frame: at the first 3D draw of each game frame, each
 * eye's view from the newest head pose. The game camera stays where the game
 * put it (nothing in the game changes); each eye is a camera on top of it at
 * the head's orientation and position since recenter, in game units (the
 * world scale), so every draw of the frame is seen from the head. */
static void head_frame(psp_gpu_stereo *s) {
    vr_head h;
    memset(&h, 0, sizeof h);
    h.q[3] = 1.0f;
    h.eye[0][0] = -0.5f * VR_IPD_DEFAULT; h.eye[1][0] = 0.5f * VR_IPD_DEFAULT;
    const int live = g_vr_live && vr_head_get(&h);
#ifndef PSP2I_PROD
    if (!live && g_fake_head) {                                 /* yaw about y, then pitch about x */
        const float y = g_fake_yaw * 0.00872665f, x = g_fake_pitch * 0.00872665f;   /* half angles, radians */
        h.q[0] = cosf(y) * sinf(x); h.q[1] = sinf(y) * cosf(x); h.q[2] = -sinf(y) * sinf(x); h.q[3] = cosf(y) * cosf(x);
    }
#endif
    float lev[9], hr[9], q[9];
    if (g_vr_first && g_fp_cam) {
        /* The game camera already looks as the head did when it was applied
         * (hook_apply_camera): the level frame in its view is that turn
         * undone, so only the head's movement since then is added. */
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) lev[c * 3 + r] = g_fp_cam_hr[r * 3 + c];
    }
    else if (g_vr_level) level_axes(lev);
    else { static const float ID[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(lev, ID, sizeof ID); }
    quat_mat3(h.q, hr);
    mat3_mul(lev, hr, q);                                       /* the head's axes in the game camera's view space */
    /* First person: the head's origin is the player's head, seen from the
     * game camera: the feet (player +0x4B0 through the view matrix), plus
     * vr_fp_eye along world up, plus where the model's head really is
     * across (psp_gpu_stereo_probe, measured each frame: the run animation
     * throws it forward, user: "still see head"), plus vr_fp_forward along
     * the gaze. The measured height is not used, so the eyes don't bob with
     * the run. Else the head's origin is the game camera itself. */
    static float prev_v[16];
    float base[3] = { 0, 0, 0 };
    float fx_dist = PSP2I_FX_DIST;
    int first = 0;
    if (g_vr_first) {
        const uint32_t pl = gamelog_local_player();
        float v[16], w[3], c[3];
        for (int i = 0; i < 16; i++) v[i] = psp_read_f32(PSP2I_VIEW_MATRIX + 4u * (uint32_t)i);
        if (pl) for (int i = 0; i < 3; i++) w[i] = psp_read_f32(pl + 0x4B0u + 4u * (uint32_t)i);
        for (int r = 0; r < 3 && pl; r++) c[r] = v[r] * w[0] + v[4 + r] * w[1] + v[8 + r] * w[2] + v[12 + r];
        const float ul = sqrtf(v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
        /* A sane position: in front of the camera, within the orbit's reach. */
        if (pl && ul > 0.5f && ul < 2.0f && (c[2] < 0.0f || g_fp_cam) && c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 200.0f * 200.0f) {
            /* The last frame's head, from its own camera into world axes
             * (the view's rotation transposed), level. */
            float pr[6];
            if (psp_gpu_stereo_probe_result(pr) && pr[2] > 20.0f && (pr[3] != 0 || pr[4] != 0 || pr[5] != 0)) {
                float hw[3];
                for (int cc = 0; cc < 3; cc++)
                    hw[cc] = prev_v[cc * 4] * pr[3] + prev_v[cc * 4 + 1] * pr[4] + prev_v[cc * 4 + 2] * pr[5];
                hw[1] = 0.0f;                                   /* +Y is up: only across */
                if (hw[0] * hw[0] + hw[2] * hw[2] < 3.0f * 3.0f) {
                    /* A fifth of the way each frame (~0.2 s to follow at 30 fps):
                     * half was shaky with the run's sway (user, for comfort). */
                    for (int cc = 0; cc < 3; cc += 2) g_fp_head_off[cc] = g_fp_head_ok ? g_fp_head_off[cc] + 0.2f * (hw[cc] - g_fp_head_off[cc]) : hw[cc];
                    g_fp_head_ok = 1;
                }
            }
            /* This frame's: within 3 units of the feet's vertical, the head
             * from 1 unit below the eye height up (ceilings above 8 left out). */
            const float un[3] = { v[4] / ul, v[5] / ul, v[6] / ul };
            psp_gpu_stereo_probe(c, un, 3.0f, 8.0f, g_vr_fp_eye - 1.0f, 1);
            memcpy(prev_v, v, sizeof prev_v);
            float hv[3];                                        /* the head's offset, in this view */
            for (int r = 0; r < 3; r++) hv[r] = g_fp_head_ok ? v[r] * g_fp_head_off[0] + v[8 + r] * g_fp_head_off[2] : 0.0f;
            for (int r = 0; r < 3; r++)
                base[r] = c[r] + g_vr_fp_eye * un[r] + hv[r] - g_vr_fp_fwd * q[6 + r];   /* ahead: -z */
            fx_dist = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) + PSP2I_FP_FX_AHEAD;
            first = 1;
        }
    }
    if (!first) g_fp_head_ok = 0;
    static int was_first = -1;
    if (g_vr_first && first != was_first)
        fprintf(stderr, "vr: first person %s\n", first ? "on (the eyes in the player's head)" : "waiting for the player");
    was_first = g_vr_first ? first : -1;
    const float k = first ? g_vr_fp_scale : g_vr_world_scale;
    {   /* the eyes' view against the GE's projection: measured, as the GE
         * may draw with the game's own projection though the cull saw the
         * widened one (2026-10-23: it does; assuming it was widened took
         * every first-person frame for a menu scene) */
        float kx = 1.0f, ky = 1.0f;
        if (first && g_fp_cam && g_fp_p0 > 0 && g_fp_p5 > 0 && fabsf(s->proj[0]) > 1e-3f && fabsf(s->proj[5]) > 1e-3f) {
            /* Only the projection hook_set_projection wrote counts as widened:
             * mission select's own (x 0.98) was taken for a widened field
             * projection and shown in 3D (2026-10-23). */
            const float wx = g_fp_p0 / g_fp_kx, wy = g_fp_p5 / g_fp_ky;
            if (fabsf(fabsf(s->proj[0]) - wx) < 0.05f * wx && fabsf(fabsf(s->proj[5]) - wy) < 0.05f * wy) {
                kx = g_fp_kx;
                ky = g_fp_ky;
            }
        }
        g_ge_kx = kx;
        const float fx = g_vr_fov > 1.0f ? g_vr_fov : 1.0f, fy = g_vr_fov_y > 1.0f ? g_vr_fov_y : 1.0f;
        g_eye_fs_x = fx / kx;
        g_eye_fs_y = fy / ky;
        g_eye_kyx = ky / kx;
        s->fov_scale = g_eye_fs_x;
        s->fov_scale_y = g_eye_fs_y;
    }
    s->near_w = first ? g_vr_fp_near : 0.0f;                    /* the player's own head, clipped away */
    s->fx_dist = fx_dist;
    for (int e = 0; e < 2; e++) {
        float p[3];
        for (int r = 0; r < 3; r++)
            p[r] = base[r] + k * (lev[r] * h.p[0] + lev[3 + r] * h.p[1] + lev[6 + r] * h.p[2] +
                                  q[r] * h.eye[e][0] + q[3 + r] * h.eye[e][1] + q[6 + r] * h.eye[e][2]);
        view_of(q, p, s->eye_view[e]);
    }
    hands_frame(base, lev, k);
    s->tag = live ? h.id : 0;
    /* Menu scenes: as the game draws them, on the flat screen (tag 0). */
    int menu = 0;
    if (g_vr_menu_screen) {
        float v[16], pr[16];
        for (int i = 0; i < 16; i++) {
            v[i] = psp_read_f32(PSP2I_VIEW_MATRIX + 4u * (uint32_t)i);
            pr[i] = s->proj[i];                                 /* what the GE draws with (RAM may hold another camera's) */
        }
        const int unturned = fabsf(v[0] - 1) < 1e-4f && fabsf(v[5] - 1) < 1e-4f && fabsf(v[10] - 1) < 1e-4f &&
                             fabsf(v[1]) < 1e-4f && fabsf(v[2]) < 1e-4f && fabsf(v[4]) < 1e-4f &&
                             fabsf(v[6]) < 1e-4f && fabsf(v[8]) < 1e-4f && fabsf(v[9]) < 1e-4f;
        const int off_centre = fabsf(pr[8]) > 1e-3f || fabsf(pr[9]) > 1e-3f;
        /* Another FOV only counts when it's far from the field's: mission
         * select is 0.98 (wider), close-up cutscenes 5.67. A boss fight's
         * camera was 1.30-1.34 and the whole fight went flat (user) when
         * this allowed 3%. */
        const float px = pr[0] * g_ge_kx;                     /* the game's own x scale (GE widening undone) */
        const int other_fov = px < 1.15f || px > 2.5f;
        menu = unturned || off_centre || other_fov;
        g_menu_loading = menu && unturned && !off_centre && !other_fov;
        if (menu != g_menu_scene)
            fprintf(stderr, "vr: %s (view %s, projection x %.4f%s)\n",
                    menu ? "a menu scene: on the flat screen" : "a 3D scene: from the head",
                    unturned ? "unturned" : "turned", (double)pr[0], off_centre ? ", off-centre" : "");
        g_menu_scene = menu;
    }
    s->flat_frame = menu;
    if (menu) s->tag = 0;
#ifndef PSP2I_PROD
    /* First person (dev): every ~4 s, where the measured head is across from
     * the feet (world axes, game units). */
    static unsigned n_fp;
    if (first && ++n_fp % 120 == 0)
        fprintf(stderr, "vr: fp head: %s, %.2f / %.2f across from the feet (world x / z)\n",
                g_fp_head_ok ? "measured" : "not measured", (double)g_fp_head_off[0], (double)g_fp_head_off[2]);
#endif
}

#ifndef PSP2I_PROD
/* VR captures (dev): F11, or every PSP2I_VR_SHOT_EVERY=N seconds. For one
 * frame the eyes also come back through the CPU (still shared on the GPU
 * with the headset: through the CPU only, that frame showed flat), and
 * <capture dir>/vrshot_<vblank>_L.ppm / _R.ppm get the pictures the headset
 * gets; vrshot_<vblank>.txt the head pose, the HUD frame, the settings and
 * psprecomp's trace of that frame (psp_gpu_stereo_trace: each draw that isn't
 * a moved 3D scene draw, with what the eyes do with it). */
static volatile LONG g_vrshot_req;
static uint64_t g_vrshot_every;             /* vblanks; 0 = only F11 */
static uint64_t g_vrshot_vb, g_vrshot_last; /* the capture in progress (0 = none), the last one */
static FILE    *g_vrshot_txt;
static int      g_vrshot_noshare;           /* the eyes also read back to the CPU until saved */
static char     g_vrshot_eyes[160];         /* the report's last line, once the eyes are saved */
static void vrshot_request(void) { InterlockedExchange(&g_vrshot_req, 1); }

static void vrshot_ppm(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int k = 0; k < w * h; k++) {
        const uint8_t rgb[3] = { (uint8_t)(px[k] >> 16), (uint8_t)(px[k] >> 8), (uint8_t)px[k] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
#endif

/* The game builds its cull planes (view space, from the projection) in
 * 0x08D7D63C, called by SetProjection and the viewport setter
 * (ADDRESS_MAP.md). With head tracking it runs with the projection's x and y
 * scales divided by vr_cull_scale, then the projection is put back: the
 * planes are wider and nothing else changes. Orthographic projections are
 * left alone. */
#define FN_FRUSTUM_PLANES 0x08D7D63Cu
#define PSP2I_PROJ_MATRIX 0x08FA97A0u
#define PSP2I_PROJ_ORTHO  0x08FA9830u   /* 1 = the current projection is orthographic */
static uint64_t n_cull_wide;
/* VR head tracking (vr_cull_all=on): the game's frustum tests of its
 * objects' bounding shapes (0x08D7CAEC and its neighbours, ADDRESS_MAP.md
 * "The frustum cull": 16 = outside, 2 = inside) all answer "inside", so
 * everything the game has loaded is drawn. The cull is the game camera's,
 * and in first person that camera is ~16 m behind the player: looking back
 * showed an empty world (user), which widening the side planes can't fix
 * (nothing behind a camera is in any view through it). Only what this
 * game draws changes; the GPU clips what is behind the eyes. */
static int      g_vr_cull_all = 1;
/* vr_two_sided=on (head tracking): the 3D scene drawn without back-face
 * culling (psp_gpu_stereo.two_sided): "angles not viewable in the original
 * game are not rendered" (user): one-sided walls and roofs seen from behind. */
static int      g_vr_two_sided = 1;
static uint64_t n_cull_all;
static void hook_cull_test(void (*original)(void)) {
    if (!g_head_on || !g_vr_cull_all) { original(); return; }
    psp_cpu.r[2] = 2;                                           /* inside */
    if (n_cull_all++ == 0) fprintf(stderr, "vr: every object drawn (the game's frustum tests answer inside)\n");
}


/* First person: the game's field camera is applied with its view at the
 * player's head (ApplyCamera 0x08AFD474: its view matrix, cam+0x10, goes to
 * the global 0x08FA98A0, then the projection and the cull planes). The game
 * draws and culls from where the eyes are: the town's shop tops were missing
 * in first person while the camera stood ~16 m back and above ("unless I move
 * closer, or in third person lower": the game picks what to draw by the
 * camera's place). The heading is the game camera's, level; the camera
 * object's eye, target and movement vectors are left alone (the orbit reads
 * its eye back each frame, and moving follows the camera's heading). The
 * field camera is the one with the field's projection (x scale 1.15..2.5). */
static uint64_t n_fp_cam;
#define FN_APPLY_CAMERA 0x08AFD474u
#ifndef PSP2I_PROD
/* Dev: each ApplyCamera of a frame and the global projection when the
 * scene walk starts (hook_node_vis), logged every ~5 s: is the widened
 * projection the one the game culls with? */
static char     g_apply_log[512];
static int      g_apply_log_n, g_apply_log_print, g_node_seen;
static uint64_t g_apply_log_flip;
#endif

#ifndef PSP2I_PROD
/* Dev: PSP2I_PL_TRACE=FILE writes, once per game frame with the field
 * camera, the pad as sent, the headings and 4 KB of the local player object
 * plus 512 bytes of the camera object, for offline searches (the player's
 * facing, attack and lock-on state: compare frames, don't guess). */
static uint32_t g_sent_bits;
static uint8_t  g_sent_ax = 128, g_sent_ay = 128;
#define PL_TRACE_PL  0x1000u
#define PL_TRACE_CAM 0x200u
static void pl_trace(uint32_t cam, uint32_t pl) {
    static FILE *f;
    static int tried;
    static uint64_t last_flip, bytes;
    if (!tried) {
        tried = 1;
        const char *p = getenv("PSP2I_PL_TRACE");
        if (p && *p && (f = fopen(p, "wb")) != NULL) fprintf(stderr, "vr: player trace to %s\n", p);
    }
    const uint64_t fl = psp_display_flips();
    if (!f || !pl || fl == last_flip || bytes > (200u << 20)) return;
    last_flip = fl;
    struct {
        uint32_t magic, flip, pl, cam, bits;
        uint8_t ax, ay, in_ax, in_ay;
        uint32_t first, fp_cam;
        float fp_yaw, game_yaw, head_yaw;
    } r;
    memset(&r, 0, sizeof r);
    r.magic = 0x52544C50u; r.flip = (uint32_t)fl; r.pl = pl; r.cam = cam; r.bits = g_sent_bits;
    r.ax = g_sent_ax; r.ay = g_sent_ay; r.in_ax = g_pad_ax; r.in_ay = g_pad_ay;
    r.first = (uint32_t)g_vr_first; r.fp_cam = (uint32_t)g_fp_cam;
    r.fp_yaw = g_fp_yaw; r.game_yaw = g_fp_game_yaw;
#ifdef PSP2I_VR
    vr_head hd;
    if (g_vr_live && vr_head_get(&hd)) { float hr[9]; quat_mat3(hd.q, hr); r.head_yaw = atan2f(hr[6], hr[8]); }
#endif
    const void *pp = psp_mem_ptr(pl, PL_TRACE_PL), *cp = psp_mem_ptr(cam, PL_TRACE_CAM);
    if (!pp || !cp) return;
    fwrite(&r, sizeof r, 1, f);
    fwrite(pp, PL_TRACE_PL, 1, f);
    fwrite(cp, PL_TRACE_CAM, 1, f);
    bytes += sizeof r + PL_TRACE_PL + PL_TRACE_CAM;
    if (fl % 300 == 0) fflush(f);
}
#endif

static void hook_apply_camera(void (*original)(void)) {
    const uint32_t cam = psp_cpu.r[4], pl = gamelog_local_player();
    const float px = cam ? psp_read_f32(cam + 0x90u) : 0.0f;
#ifndef PSP2I_PROD
    {
        const uint64_t fl = psp_display_flips();
        if (fl != g_apply_log_flip) {
            if (g_apply_log_print && g_apply_log_n) fprintf(stderr, "vr: applies:%s\n", g_apply_log);
            g_apply_log_print = psp_sched_vblank_count() % 300 < 4;
            g_apply_log_flip = fl; g_apply_log_n = 0; g_apply_log[0] = '\0'; g_node_seen = 0;
        }
        size_t L = strlen(g_apply_log);
        if (L < sizeof g_apply_log - 64)
            snprintf(g_apply_log + L, sizeof g_apply_log - L, " [cam %08X x %.3f]", cam, (double)px);
        g_apply_log_n++;
    }
#endif
    const int field = px > 1.15f && px < 2.5f;      /* other cameras (3 applies a frame) leave g_fp_cam alone */
    if (!g_head_on || !g_vr_first) g_fp_cam = g_fp_yaw_ok = 0;
    if (!field) { original(); return; }
#ifndef PSP2I_PROD
    pl_trace(cam, pl);
#endif
    g_fp_cam = 0;
    if (g_head_on && g_vr_first && pl) {
        /* The game camera's backward axis, level, from its eye and target
         * (+0xFC, +0x108: the game's own, never written here). */
        float bz[3];
        for (int i = 0; i < 3; i++) bz[i] = psp_read_f32(cam + 0xFCu + 4u * (uint32_t)i) - psp_read_f32(cam + 0x108u + 4u * (uint32_t)i);
        if (bz[0] * bz[0] + bz[2] * bz[2] > 1e-6f) {
            const float gy = atan2f(bz[0], bz[2]);
            float dg = gy - g_fp_game_yaw;
            dg -= 6.2831853f * floorf((dg + 3.1415927f) / 6.2831853f);
            if (!g_fp_yaw_ok || fabsf(dg) > FP_YAW_CUT) g_fp_yaw = gy;   /* a start, cut or camera reset */
            else if (camera_x() != 0.0f) g_fp_yaw += dg;                  /* the right stick turning */
            g_fp_game_yaw = gy;
            g_fp_yaw_ok = 1;
        }
        /* The view's backward axis in the world, level: first person's heading. */
        float z[3] = { sinf(g_fp_yaw), 0.0f, cosf(g_fp_yaw) };
        if (g_fp_yaw_ok) {
            const float lx[3] = { z[2], 0.0f, -z[0] }, ly[3] = { 0.0f, 1.0f, 0.0f }, lz[3] = { z[0], 0.0f, z[2] };   /* x = up cross z */
            /* Turned as the head is (yaw, pitch, roll on the level heading),
             * so what the eyes look at is in front of the game camera: in
             * first person the game left out pieces behind a level camera
             * (the town's centre roof hub overhead: 27 draws of one texture
             * in third person, none in first; GE dumps 2026-10-23). */
            float hr[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
#ifdef PSP2I_VR
            vr_head hd;
            memset(&hd, 0, sizeof hd);
            if (g_vr_live && vr_head_get(&hd)) quat_mat3(hd.q, hr);
#endif
            memcpy(g_fp_cam_hr, hr, sizeof hr);
            float x[3], y[3];
            for (int i = 0; i < 3; i++) {                   /* the head's axes in the world: L * hr */
                x[i] = lx[i] * hr[0] + ly[i] * hr[1] + lz[i] * hr[2];
                y[i] = lx[i] * hr[3] + ly[i] * hr[4] + lz[i] * hr[5];
                z[i] = lx[i] * hr[6] + ly[i] * hr[7] + lz[i] * hr[8];
            }
            float h[3];
            for (int i = 0; i < 3; i++) h[i] = psp_read_f32(pl + 0x4B0u + 4u * (uint32_t)i);
            h[1] += g_vr_fp_eye;
            if (g_fp_head_ok) { h[0] += g_fp_head_off[0]; h[2] += g_fp_head_off[2]; }
            const float *rows[3] = { x, y, z };
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) psp_write_f32(cam + 0x10u + 4u * (uint32_t)(c * 4 + r), rows[r][c]);
                psp_write_f32(cam + 0x10u + 4u * (uint32_t)(12 + r), -(rows[r][0] * h[0] + rows[r][1] * h[1] + rows[r][2] * h[2]));
            }
            g_fp_cam = 1;
            if (n_fp_cam++ == 0) fprintf(stderr, "vr: first person: the game camera at the player's head\n");
#ifndef PSP2I_PROD
            {   /* Every 2 s while walking: the headings (degrees, backward axis) and
                 * where the player went, to check the walk follows the head:
                 * moved = view + head when the stick is pushed straight ahead. */
                static float lp[3];
                static uint64_t n_walk;
                const float mx = h[0] - lp[0], mz = h[2] - lp[2];
                if (g_fp_stick_on && mx * mx + mz * mz > 1e-4f && n_walk++ % 60 == 0)
                    fprintf(stderr, "vr: fp walk: game %.0f view %.0f head %+.0f moved %.0f; stick %d,%d -> %d,%d\n",
                            (double)(g_fp_game_yaw * 57.29578f), (double)(g_fp_yaw * 57.29578f),
                            (double)(atan2f(hr[6], hr[8]) * 57.29578f), (double)(atan2f(-mx, -mz) * 57.29578f),
                            g_fp_stick_in[0] - 128, g_fp_stick_in[1] - 128, g_fp_stick_out[0] - 128, g_fp_stick_out[1] - 128);
                memcpy(lp, h, sizeof lp);
            }
#endif
        }
    }
    original();
}

/* SetProjection (0x08D7DA78): in first person with the camera at the head, a
 * field-like perspective projection (x scale 1.15..2.5) is made wider after
 * it is set, by the eyes' view times VR_FP_VIEW_MARGIN, so everything the
 * game culls with sees past the edges of the eyes' view. Widening it after
 * ApplyCamera didn't hold (the scene walk still saw x 1.368: the projection
 * is set again before the walk), so it is done wherever it is set. The cull
 * planes (built inside, from the narrow one) are wider by vr_cull_scale. */
#define FN_SET_PROJECTION 0x08D7DA78u
static uint64_t n_proj_wide;
static void hook_set_projection(void (*original)(void)) {
    original();
    if (!g_head_on || !g_vr_first || !g_fp_cam || psp_read32(PSP2I_PROJ_ORTHO) == 1) return;
    const float p0 = psp_read_f32(PSP2I_PROJ_MATRIX), p5 = psp_read_f32(PSP2I_PROJ_MATRIX + 20);
    if (!(fabsf(p0) > 1.15f && fabsf(p0) < 2.5f)) return;
    const float fx = g_vr_fov > 1.0f ? g_vr_fov : 1.0f, fy = g_vr_fov_y > 1.0f ? g_vr_fov_y : 1.0f;
    g_fp_kx = fx * VR_FP_VIEW_MARGIN;
    g_fp_ky = fy * VR_FP_VIEW_MARGIN;
    g_fp_p0 = fabsf(p0); g_fp_p5 = fabsf(p5);
    psp_write_f32(PSP2I_PROJ_MATRIX, p0 / g_fp_kx);
    psp_write_f32(PSP2I_PROJ_MATRIX + 20, p5 / g_fp_ky);
    if (n_proj_wide++ == 0) fprintf(stderr, "vr: first person: the game's projection %.1fx / %.1fx wider\n", (double)g_fp_kx, (double)g_fp_ky);
}

#ifndef PSP2I_PROD
/* Dev, what the game leaves out of the frame: 0x08D7D270(flags[], desc[],
 * i, ...) decides scene node i's visibility; flags[i] bit 0x1 = hidden.
 * Counted per reason and logged every ~5 s with the view in use, to compare
 * first and third person at the same place: already hidden or skipped
 * (flags 0x401 before), the description's 0x10, its 0x20, else the frustum
 * (or another) test. Desc entries are 144 bytes. */
#define FN_NODE_VIS 0x08D7D270u
static uint64_t n_node[6];                  /* visited, visible, pre-hidden, desc 0x10, desc 0x20, other */
static void hook_node_vis(void (*original)(void)) {
    if (!g_node_seen && g_apply_log_print) {
        g_node_seen = 1;
        size_t L = strlen(g_apply_log);
        if (L < sizeof g_apply_log - 64)
            snprintf(g_apply_log + L, sizeof g_apply_log - L, " {scene walk: global proj x %.3f y %.3f, wide %d}",
                     (double)psp_read_f32(PSP2I_PROJ_MATRIX), (double)psp_read_f32(PSP2I_PROJ_MATRIX + 20), g_fp_cam);
    }
    const uint32_t fa = psp_cpu.r[4] + psp_cpu.r[6] * 4u, da = psp_cpu.r[5] + psp_cpu.r[6] * 144u;
    const uint32_t before = psp_read32(fa), desc = psp_read32(da);
    original();
    const uint32_t after = psp_read32(fa);
    n_node[0]++;
    if (before & 0x401) n_node[2]++;
    else if (!(after & 1)) n_node[1]++;
    else if (desc & 0x10) n_node[3]++;
    else if (desc & 0x20) n_node[4]++;
    else n_node[5]++;
    static uint64_t last_vb;
    const uint64_t vb = psp_sched_vblank_count();
    if (vb - last_vb >= 300) {
        last_vb = vb;
        fprintf(stderr, "vr: nodes (%s person): %llu visited, %llu visible; hidden: %llu already, %llu desc 0x10, %llu desc 0x20,"
                        " %llu by a test\n", g_vr_first ? "first" : "third",
                (unsigned long long)n_node[0], (unsigned long long)n_node[1], (unsigned long long)n_node[2],
                (unsigned long long)n_node[3], (unsigned long long)n_node[4], (unsigned long long)n_node[5]);
        memset(n_node, 0, sizeof n_node);
    }
}
#endif

static void hook_frustum_planes(void (*original)(void)) {
    const float k = g_head_on ? g_vr_cull : 1.0f;
    if (k <= 1.0f || psp_read32(PSP2I_PROJ_ORTHO) == 1) { original(); return; }
    const float p0 = psp_read_f32(PSP2I_PROJ_MATRIX), p5 = psp_read_f32(PSP2I_PROJ_MATRIX + 20);
    psp_write_f32(PSP2I_PROJ_MATRIX, p0 / k);
    psp_write_f32(PSP2I_PROJ_MATRIX + 20, p5 / k);
    original();
    psp_write_f32(PSP2I_PROJ_MATRIX, p0);
    psp_write_f32(PSP2I_PROJ_MATRIX + 20, p5);
    if (n_cull_wide++ == 0) fprintf(stderr, "vr: cull planes %.1fx wider than the game's\n", (double)k);
}

/* Once per vblank: the eyes are drawn while the headset shows the game (or
 * --stereo), with Direct3D 11 only (the backend with eye copies). */
static void display_save(int res);            /* below */
static int  g_res_saved;
/* vr_fov_auto: the scales from the headset's eye view (the field's FOV:
 * PSP2I_FIELD_PROJ_X across, 480:272), saved when they change. */
static void fov_auto_update(void) {
    float tx, ty;
    if (!g_vr_fov_auto || !g_vr_live || !vr_view_tan(&tx, &ty)) return;
    const float gx = 1.0f / PSP2I_FIELD_PROJ_X, gy = gx * (float)SCREEN_H / (float)SCREEN_W;
    float fx = tx * 1.05f / gx, fy = ty * 1.05f / gy;
    fx = fx < 1.0f ? 1.0f : fx > 3.0f ? 3.0f : fx;
    fy = fy < 1.0f ? 1.0f : fy > 4.0f ? 4.0f : fy;
    if (fabsf(fx - g_vr_fov) < 0.05f && fabsf(fy - g_vr_fov_y) < 0.05f) return;
    g_vr_fov = fx; g_vr_fov_y = fy;
    fprintf(stderr, "vr: fov scales %.2f / %.2f (the headset's view; the eye pictures' rows follow at the next start)\n",
            (double)fx, (double)fy);
    if (!g_headless) display_save(g_res_saved);
}

static void stereo_update(void) {
    fov_auto_update();
    /* Through short drops out of RUNNING too: the session left VISIBLE /
     * FOCUSED for moments while the user still saw the game in 3D, and each
     * drop turned the 3D off. Off after 3 s without RUNNING. */
    static uint64_t running_vb;
    const uint64_t vb = psp_sched_vblank_count();
    if (g_vr_live && vr_status() == VR_RUNNING) running_vb = vb ? vb : 1;
    const int recent = running_vb && vb - running_vb < 180;
    const int on = g_renderer == REN_D3D11 &&
                   (g_stereo_arg || (g_vr_live && g_vr_stereo && recent));
    const int head = on && ((g_vr_live && g_vr_head) || g_fake_head);
    psp_gpu_stereo st;
    memset(&st, 0, sizeof st);
    if (on) {
        st.on = 1;
        st.half_ipd = 0.5f * vr_ipd() * g_vr_world_scale;
        st.converge = g_vr_dist_dm / 10.0f * g_vr_world_scale;   /* the screen's distance: no disparity there */
        st.flat_after_clear = 1;                                  /* PSP2i draws its HUD in 3D after a second clear */
        st.mono_is_left = 1;                                      /* two pictures, not three: the window shows the left eye */
        st.eye_views = head;                                      /* head tracking: head_frame sets each eye per frame */
        st.two_sided = head && g_vr_two_sided;
        st.on_frame = head ? head_frame : NULL;
        st.fov_scale = head ? g_vr_fov : 1.0f;
        st.fov_scale_y = head ? g_vr_fov_y : 1.0f;
        st.hud_panel = psp2i_hud_panel;                           /* HUD and menus to the panel (d3d11.c draw_hud) */
        st.fx_dist = PSP2I_FX_DIST;
    }
    if (on != g_stereo_on) fprintf(stderr, "vr: stereo %s\n", on ? "on (each eye drawn at the GE)" : "off");
    if (head != g_head_on)
        fprintf(stderr, "vr: head tracking %s (view %.1fx / %.1fx the game's across / up, cull %.1fx)\n",
                head ? (g_vr_level ? "on, horizon levelled" : "on") : "off", (double)g_vr_fov, (double)g_vr_fov_y,
                (double)g_vr_cull);
    g_head_on = head;
    psp_gpu_set_stereo(&st);
    d3d11_set_mono_left(st.mono_is_left);
    d3d11_eye_share(on && g_vr_live && !vr_share_failed());
    g_stereo_on = on;
}

#ifndef PSP2I_PROD
/* The capture folder with its parents (a missing parent lost every capture:
 * "cannot write", 2026-10-23). */
static void capture_dir_make(void) {
    char p[sizeof g_capture_dir];
    snprintf(p, sizeof p, "%s", g_capture_dir);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { const char k = *c; *c = '\0'; CreateDirectoryA(p, NULL); *c = k; }
    CreateDirectoryA(p, NULL);
}

/* Each vblank, after the eye pictures are fetched: start a capture, save its
 * pictures, close its report when psprecomp's trace is done. */
static void vrshot_vblank(uint64_t vb) {
    d3d11_eye_readback(g_vrshot_noshare);
    if (g_vrshot_every && g_stereo_on && vb - g_vrshot_last >= g_vrshot_every) vrshot_request();
    if (!g_vrshot_vb && InterlockedExchange(&g_vrshot_req, 0)) {
        if (!g_stereo_on) { fprintf(stderr, "vr: capture: no stereo now\n"); return; }
        char path[700];
        capture_dir_make();
        snprintf(path, sizeof path, "%s/vrshot_%06llu.txt", g_capture_dir, (unsigned long long)vb);
        g_vrshot_txt = fopen(path, "w");
        if (!g_vrshot_txt) { fprintf(stderr, "vr: capture: cannot write %s\n", path); return; }
        g_vrshot_vb = g_vrshot_last = vb;
        g_vrshot_noshare = 1;
        vr_head h;
        memset(&h, 0, sizeof h);
        const int live = g_vr_live && vr_head_get(&h);
        FILE *f = g_vrshot_txt;
        fprintf(f, "vrshot vblank %llu, flip %llu\n", (unsigned long long)vb, (unsigned long long)psp_display_flips());
        fprintf(f, "head %s: id %u q %.4f %.4f %.4f %.4f p %.3f %.3f %.3f m; eyes %.4f,%.4f,%.4f / %.4f,%.4f,%.4f\n",
                live ? "tracked" : "none", h.id, (double)h.q[0], (double)h.q[1], (double)h.q[2], (double)h.q[3],
                (double)h.p[0], (double)h.p[1], (double)h.p[2], (double)h.eye[0][0], (double)h.eye[0][1],
                (double)h.eye[0][2], (double)h.eye[1][0], (double)h.eye[1][1], (double)h.eye[1][2]);
        fprintf(f, "settings: head %d level %d fov %.2f/%.2f cull %.1f hud %.1f m size %.2f world scale %.1f; game tan_x %.4f\n",
                g_head_on, g_vr_level, (double)g_vr_fov, (double)g_vr_fov_y, (double)g_vr_cull, (double)g_vr_hud_dist,
                (double)g_vr_hud_size, (double)g_vr_world_scale, (double)psp_gpu_stereo_tan_half_fov());
        fprintf(f, "game view 0x08FA98A0:");
        for (int k = 0; k < 16; k++) fprintf(f, " %.3f", (double)psp_read_f32(PSP2I_VIEW_MATRIX + 4u * (uint32_t)k));
        fprintf(f, "\n");
        psp_gpu_stereo_trace(f);
        fprintf(stderr, "vr: capture %llu started\n", (unsigned long long)vb);
        return;
    }
    if (!g_vrshot_vb) return;
    if (g_vrshot_noshare && g_eye_new && g_eye_px[0] && g_eye_px[1]) {
        char path[700];
        for (int e = 0; e < 2; e++) {
            snprintf(path, sizeof path, "%s/vrshot_%06llu_%c.ppm", g_capture_dir, (unsigned long long)g_vrshot_vb, e ? 'R' : 'L');
            vrshot_ppm(path, g_eye_px[e], g_eye_w, g_eye_h);
        }
        int hw = 0, hh = 0;
        const uint32_t *hud = g_renderer == REN_D3D11 ? d3d11_hud_picture(&hw, &hh) : NULL;   /* the HUD panel's picture */
        if (hud) {
            snprintf(path, sizeof path, "%s/vrshot_%06llu_H.ppm", g_capture_dir, (unsigned long long)g_vrshot_vb);
            vrshot_ppm(path, hud, hw, hh);
        }
        snprintf(g_vrshot_eyes, sizeof g_vrshot_eyes, "eyes: tag %u, %dx%d, saved at vblank %llu\n", g_eye_px_tag,
                 g_eye_w, g_eye_h, (unsigned long long)vb);
        g_vrshot_noshare = 0;
    }
    if (vb - g_vrshot_vb > 300) {                               /* 5 s: give up on what is missing */
        if (g_vrshot_noshare) fprintf(stderr, "vr: capture: no eye pictures came back\n");
        g_vrshot_noshare = 0;
        if (psp_gpu_stereo_tracing()) psp_gpu_stereo_trace(NULL);
    }
    if (!g_vrshot_noshare && !psp_gpu_stereo_tracing()) {
        if (g_vrshot_txt) {
            fputs(g_vrshot_eyes[0] ? g_vrshot_eyes : "eyes: none came back\n", g_vrshot_txt);
            fclose(g_vrshot_txt);
        }
        g_vrshot_eyes[0] = '\0';
        g_vrshot_txt = NULL;
        fprintf(stderr, "vr: capture %llu -> %s/vrshot_%06llu*\n", (unsigned long long)g_vrshot_vb, g_capture_dir,
                (unsigned long long)g_vrshot_vb);
        g_vrshot_vb = 0;
    }
}
#endif

/* The eye pictures' half-FOV tangents: the game's, times vr_fov_scale(_y)
 * for pictures drawn from the head (tag != 0); vertical 0 = from the shape. */
static float eye_tan(uint32_t tag) {
    return psp_gpu_stereo_tan_half_fov() * (tag ? g_eye_fs_x : 1.0f);
}
static float eye_tan_y(uint32_t tag) {   /* the GE's y scale over x is 480:272 times ky/kx (first person's widening) */
    return tag ? psp_gpu_stereo_tan_half_fov() * (float)SCREEN_H / (float)SCREEN_W * g_eye_kyx * g_eye_fs_y : 0.0f;
}

/* The eye pictures with the overlay (menus, FPS) on top, as compose() does
 * for the S-times picture: each overlay pixel covers its block. */
static void compose_eyes(void) {
    const int S = g_eye_w / SCREEN_W, SY = g_eye_h / SCREEN_H, W = g_eye_w;
    for (int e = 0; e < 2; e++) {
        if (!g_eye_rgba[e]) g_eye_rgba[e] = (uint32_t *)malloc((size_t)SCREEN_W * 6 * SCREEN_H * 12 * 4);
        if (!g_eye_rgba[e]) return;
        for (int y = 0; y < SCREEN_H; y++)
            for (int x = 0; x < SCREEN_W; x++) {
                const int i = y * SCREEN_W + x;
                const int flat = g_rgba[i] != g_frame[i];
                for (int k = 0; k < SY; k++) {
                    uint32_t *dst = g_eye_rgba[e] + (size_t)(y * SY + k) * W + (size_t)x * S;
                    if (flat) for (int j = 0; j < S; j++) dst[j] = g_rgba[i];
                    else memcpy(dst, g_eye_px[e] + (size_t)(y * SY + k) * W + (size_t)x * S, (size_t)S * 4);
                }
            }
    }
}
#endif

#ifdef PSP2I_VR
/* First person: the attack nudge (VR_PLAN F1). After walking left, right or
 * back the character faces where it walked, and an attack goes that way, not
 * where the head looks. From a player trace (2026-10-23, ADDRESS_MAP): the
 * facing is player +0x70's rotation (its forward row x, z at +0x90, +0x98);
 * the stick turns it 20 degrees a game frame, from 1-3 frames after the push,
 * and letting the stick go mid-turn snaps it to the stick's heading a frame
 * later; a stick pushed during an attack's start doesn't turn it. So the
 * attack is held back: the stick points ahead (the walk turn below aims it at
 * the head) until it turns that way, is let go for FP_NUDGE_GAP frames, then
 * the attack is pressed. Not while R is held or its lock-on (player +0x214)
 * is set: the lock-on picks the target. Only what this player presses. */
#define FP_NUDGE_BUTTONS 0x9000u    /* Square, Triangle */
#define FP_NUDGE_MIN     0.5236f    /* 30 degrees between the facing and the view */
#define FP_NUDGE_HOLD    6          /* game frames of stick at most */
#define FP_NUDGE_GAP     2          /* game frames of centred stick: the snap */
#define FP_NUDGE_PRESS   2          /* game frames the attack is pressed at least */
#define FP_NUDGE_COMBO   30         /* game frames after an attack: no nudge (combos, Just Attacks keep their timing) */
enum { NUDGE_OFF, NUDGE_TURN, NUDGE_GAP, NUDGE_PRESS };
static int      g_nudge;
static uint32_t g_nudge_btn, g_nudge_prev;
static uint64_t g_nudge_flip, g_nudge_flip0, g_nudge_turned, g_attack_flip;
static float    g_nudge_err0;

static float wrap_pi(float a) { return a - 6.2831853f * floorf((a + 3.1415927f) / 6.2831853f); }

/* The local player's facing, atan2(x, z) of its forward axis. */
static int fp_face(uint32_t pl, float *a) {
    const float x = psp_read_f32(pl + 0x90u), z = psp_read_f32(pl + 0x98u);
    if (!(x * x + z * z > 0.25f)) return 0;      /* not a rotation (or NaN) */
    *a = atan2f(x, z);
    return 1;
}

static void fp_nudge(uint32_t *bits, uint8_t *ax, uint8_t *ay) {
    const uint32_t raw = *bits, down = raw & ~g_nudge_prev & FP_NUDGE_BUTTONS;
    g_nudge_prev = raw;
    const uint32_t pl = gamelog_local_player();
    vr_head hd;
    if (!g_vr_fp_nudge || !g_head_on || !g_vr_first || !g_fp_cam || !g_fp_yaw_ok || g_rep_on || g_menu.open || !pl ||
        !g_vr_live || !vr_head_get(&hd)) {
        if (g_nudge && g_nudge < NUDGE_PRESS) *bits |= g_nudge_btn;   /* the held-back attack still goes */
        g_nudge = NUDGE_OFF;
        return;
    }
    float hr[9], face;
    quat_mat3(hd.q, hr);
    const float look = g_fp_yaw + atan2f(hr[6], hr[8]) + 3.1415927f;   /* g_fp_yaw is the backward axis */
    const int have = fp_face(pl, &face);
    const float err = have ? wrap_pi(look - face) : 0.0f;
    const uint64_t fl = psp_display_flips(), last = g_attack_flip;
    if (down) g_attack_flip = fl;
    switch (g_nudge) {
    case NUDGE_OFF:
        if (!down || !have || fabsf(err) <= FP_NUDGE_MIN || (last && fl - last < FP_NUDGE_COMBO)) return;
        if ((raw & 0x0200u) || psp_read32(pl + 0x214u) != 0) {
#ifndef PSP2I_PROD
            fprintf(stderr, "vr: fp nudge: none, locked on (facing %+.0f from the view)\n", (double)(err * 57.29578f));
#endif
            return;
        }
        g_nudge = NUDGE_TURN;
        g_nudge_btn = down;
        g_nudge_flip = g_nudge_flip0 = fl;
        g_nudge_err0 = err;
        /* fall through */
    case NUDGE_TURN:
        *bits &= ~g_nudge_btn;
        /* At least 2 frames of this stick (the game takes the heading 1-3
         * frames after the push) and the facing nearer the view: a facing
         * still turning from the player's own stick doesn't count, its snap
         * would go the old way (headset run 2026-10-23: walking back). */
        if ((fl - g_nudge_flip0 >= 2 && have && fabsf(err) < fabsf(g_nudge_err0) - 0.26f) || fabsf(err) <= FP_NUDGE_MIN ||
            fl - g_nudge_flip0 >= FP_NUDGE_HOLD) {
            g_nudge = NUDGE_GAP;              /* turning the head's way: the game has the heading */
            g_nudge_flip = fl;
            g_nudge_turned = fl - g_nudge_flip0;
            *ax = *ay = 128;
        } else {
            *ax = 128; *ay = 1;               /* straight ahead, turned to the head below */
        }
        return;
    case NUDGE_GAP:
        *bits &= ~g_nudge_btn;
        *ax = *ay = 128;
        if (fl - g_nudge_flip < FP_NUDGE_GAP) return;
        g_nudge = NUDGE_PRESS;
        g_nudge_flip = fl;
#ifndef PSP2I_PROD
        fprintf(stderr, "vr: fp nudge: facing %+.0f from the view, stick %llu frames, then %+.0f; attack 0x%04X sent\n",
                (double)(g_nudge_err0 * 57.29578f), (unsigned long long)g_nudge_turned,
                (double)(err * 57.29578f), g_nudge_btn);
#endif
        /* fall through */
    case NUDGE_PRESS:
        *bits |= g_nudge_btn;                 /* pressed even if already let go */
        if (fl - g_nudge_flip >= FP_NUDGE_PRESS) g_nudge = NUDGE_OFF;
        return;
    }
}
#endif

static void ctrl_commit(void) {
    uint32_t bits = g_script_bits | g_pad_bits;
#ifdef _WIN32
    bits |= g_keys;
#endif
    uint8_t ax = g_pad_ax, ay = g_pad_ay;
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    if (g_rep_on) { bits |= g_rep_bits; ax = g_rep_ax; ay = g_rep_ay; }
    /* The settings menu takes all input while open (menu_filter_game). */
    if (g_login.state != LOGIN_CLOSED || web_overlay_active()) {
        /* The sign-in screen (or a web page over the game: the PS Store)
         * has the input; buttons still held when it closes stay hidden from
         * the game until released. */
        g_menu.game_mask |= bits & ~PRESS_MENU;
        bits = 0;
        ax = ay = 128;
    }
    bits = menu_filter_game(&g_menu, bits & ~PRESS_MENU);
    if (g_menu.open) ax = ay = 128;
#ifdef PSP2I_VR
    /* First person: the stick walks where the head looks. The game moves
     * relative to its camera (g_fp_game_yaw); the head looks along first
     * person's heading plus its own yaw, so the stick is turned by the
     * difference. Only the stick changes (what this player presses). */
    g_fp_stick_on = 0;
    vr_head hd;
    fp_nudge(&bits, &ax, &ay);
    if (g_head_on && g_vr_first && g_fp_cam && g_fp_yaw_ok && !g_rep_on && (ax != 128 || ay != 128) &&
        g_vr_live && vr_head_get(&hd)) {
        float hr[9];
        quat_mat3(hd.q, hr);
        const float d = g_fp_yaw + atan2f(hr[6], hr[8]) - g_fp_game_yaw, cd = cosf(d), sd = sinf(d);
        const float sx = (float)ax - 128.0f, sf = 128.0f - (float)ay;      /* right, ahead */
        float rx = sx * cd - sf * sd, rf = sx * sd + sf * cd;
        const float big = fmaxf(fabsf(rx), fabsf(rf));
        if (big > 127.0f) { rx *= 127.0f / big; rf *= 127.0f / big; }    /* the same direction, inside the pad's square */
        g_fp_stick_in[0] = ax; g_fp_stick_in[1] = ay;
        ax = (uint8_t)lroundf(128.0f + rx);
        ay = (uint8_t)lroundf(128.0f - rf);
        g_fp_stick_out[0] = ax; g_fp_stick_out[1] = ay;
        g_fp_stick_on = 1;
    }
#endif
    if (!g_rep_on) rec_pad(bits, ax, ay);
#ifndef PSP2I_PROD
    g_sent_bits = bits; g_sent_ax = ax; g_sent_ay = ay;
#endif
    psp_ctrl_set(bits, ax, ay);
}

/* PSP buttons as menu inputs (menu_inputs_from_psp: Circle accepts, Cross
 * goes back, as in the game), plus the scripted hotkey. */
static uint32_t menu_inputs(uint32_t psp, uint8_t ax, uint8_t ay) {
    return menu_inputs_from_psp(psp, ax, ay) | ((psp & PRESS_MENU) ? MI_TOGGLE : 0);
}

/* The window size (menu RESOLUTION), kept in psp2i_display.ini next to the exe:
 *   resolution=1920x1080      (or 960x544 ... 3840x2160, fullscreen) */
static char g_display_ini[600];
static int  g_last_windowed = RES_DEFAULT;  /* where Alt+Enter returns from fullscreen */

static int g_res_saved = RES_DEFAULT;       /* what psp2i_display.ini holds */
static int g_ren_saved = REN_D3D11;
static int g_render_scale = 1;              /* render_scale=: internal resolution 1..6 (D3D11; 5, 6 for VR) */
static int g_render_scale_arg;              /* --render-scale for one run; 0 = the saved one */
static int g_vol_saved[VOL_N] = { VOL_STEPS, VOL_STEPS, VOL_STEPS };
static int g_att_saved = ATT_DEFAULT;
static int g_openh264 = 1;                  /* fetch Cisco's OpenH264 for movies (h264_openh264.c) */
static char g_font_key[260] = "system";     /* the game's text font (font_win.h): system, psp, or a file in fonts/ */
static char g_texpacks[1024];               /* texture pack folders, ';'-separated (texreplace.h); empty = off */

/* "80" -> 8 tenths (0..100, rounded to 10%). */
static int vol_parse(const char *s) {
    const int v = atoi(s);
    return v < 0 ? 0 : v > 100 ? VOL_STEPS : (v + 5) / 10;
}

static int display_load(void) {
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "r") : NULL;
    if (!f) return -1;
    char line[1100];                        /* texture_packs= can be long */
    int res = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "resolution=", 11)) res = menu_res_parse(line + 11);
        else if (!strncmp(line, "renderer=", 9)) { const int r = menu_ren_parse(line + 9); if (r >= 0) g_ren_saved = r; }
        else if (!strncmp(line, "render_scale=", 13)) { const int r = atoi(line + 13); g_render_scale = r < 1 ? 1 : r > 6 ? 6 : r; }
        else if (!strncmp(line, "master_volume=", 14)) g_vol_saved[VOL_MASTER] = vol_parse(line + 14);
        else if (!strncmp(line, "music_volume=", 13))  g_vol_saved[VOL_MUSIC] = vol_parse(line + 13);
        else if (!strncmp(line, "sfx_volume=", 11))    g_vol_saved[VOL_SFX] = vol_parse(line + 11);
        else if (!strncmp(line, "attenuation=", 12))   g_att_saved = !strncmp(line + 12, "off", 3) ? ATT_OFF : menu_att_parse(atoi(line + 12));
        else if (!strncmp(line, "openh264=", 9))       g_openh264 = strncmp(line + 9, "off", 3) != 0;
        else if (!strncmp(line, "swap_circle_cross=", 18)) g_swap_oc = !strncmp(line + 18, "on", 2);
        else if (!strncmp(line, "rs_speed=", 9))       g_rs_saved = (float)atof(line + 9);
        else if (!strncmp(line, "right_stick=", 12))   g_rs_on = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "test_tick_pacing=", 17)) g_test_ticks = !strncmp(line + 17, "on", 2);
        else if (!strncmp(line, "test_30hz_logic=", 16))  g_test_logic = !strncmp(line + 16, "on", 2);
        else if (!strncmp(line, "vr=", 3))             g_vr_on = !strncmp(line + 3, "on", 2);
        else if (!strncmp(line, "vr_stereo=", 10))     g_vr_stereo = strncmp(line + 10, "off", 3) != 0;
        else if (!strncmp(line, "vr_head=", 8))        g_vr_head = strncmp(line + 8, "off", 3) != 0;
        else if (!strncmp(line, "vr_level=", 9))       g_vr_level = strncmp(line + 9, "off", 3) != 0;
        else if (!strncmp(line, "vr_fov_auto=", 12)) g_vr_fov_auto = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "vr_fov_scale=", 13)) {
            const float k = (float)atof(line + 13);
            if (k >= 1.0f && k <= 3.0f) g_vr_fov = k;
        }
        else if (!strncmp(line, "vr_hud_distance=", 16)) {
            const float d = (float)atof(line + 16);
            if (d >= 0.5f && d <= 20.0f) g_vr_hud_dist = d;
        }
        else if (!strncmp(line, "vr_menu_screen=", 15)) g_vr_menu_screen = strncmp(line + 15, "off", 3) != 0;
        else if (!strncmp(line, "vr_hud_follow=", 14)) {
            const float d = (float)atof(line + 14);
            if (d >= 0.0f && d <= 90.0f) g_vr_hud_follow = d;
        }
        else if (!strncmp(line, "vr_hud_size=", 12)) {
            const float k = (float)atof(line + 12);
            if (k >= 0.2f && k <= 1.0f) g_vr_hud_size = k;
        }
        else if (!strncmp(line, "vr_fov_scale_y=", 15)) {
            const float k = (float)atof(line + 15);
            if (k >= 1.0f && k <= 4.0f) g_vr_fov_y = k;
        }
        else if (!strncmp(line, "vr_cull_scale=", 14)) {
            const float k = (float)atof(line + 14);
            if (k >= 1.0f && k <= 50.0f) g_vr_cull = k;
        }
        else if (!strncmp(line, "vr_world_scale=", 15)) {
            const float k = (float)atof(line + 15);
            if (k >= 1.0f && k <= 100.0f) g_vr_world_scale = k;
        }
        else if (!strncmp(line, "vr_view=", 8))        g_vr_first = !strncmp(line + 8, "first", 5);
        else if (!strncmp(line, "vr_fp_eye=", 10)) {
            const float k = (float)atof(line + 10);
            if (k >= 1.0f && k <= 20.0f) g_vr_fp_eye = k;
        }
        else if (!strncmp(line, "vr_cull_all=", 12)) g_vr_cull_all = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "vr_two_sided=", 13)) g_vr_two_sided = strncmp(line + 13, "off", 3) != 0;
        else if (!strncmp(line, "vr_aniso=", 9)) {
            const int k = atoi(line + 9);
            if (k >= 0 && k <= 16) g_vr_aniso = k;
        }
        else if (!strncmp(line, "vr_sharpen=", 11)) {
            const float k = (float)atof(line + 11);
            if (k >= 0.0f && k <= 1.0f) g_vr_sharpen = k;
        }
        else if (!strncmp(line, "vr_fp_near=", 11)) {
            const float k = (float)atof(line + 11);
            if (k >= 0.0f && k <= 5.0f) g_vr_fp_near = k;
        }
        else if (!strncmp(line, "vr_fp_forward=", 14)) {
            const float k = (float)atof(line + 14);
            if (k >= 0.0f && k <= 3.0f) g_vr_fp_fwd = k;
        }
        else if (!strncmp(line, "vr_fp_nudge=", 12)) g_vr_fp_nudge = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "vr_input=", 9))     g_vr_motion = !strncmp(line + 9, "motion", 6);
        else if (!strncmp(line, "vr_fp_scale=", 12)) {
            const float k = (float)atof(line + 12);
            if (k >= 0.5f && k <= 20.0f) g_vr_fp_scale = k;
        }
        else if (!strncmp(line, "vr_distance=", 12) || !strncmp(line, "vr_width=", 9)) {
            const int dist = line[3] == 'd';
            int dm = (int)(atof(line + (dist ? 12 : 9)) * 10.0 + 0.5);
            dm = (dm + VR_STEP_DM / 2) / VR_STEP_DM * VR_STEP_DM;   /* to the menu's 0.5 m steps */
            const int lo = dist ? VR_DIST_MIN_DM : VR_WIDTH_MIN_DM, hi = dist ? VR_DIST_MAX_DM : VR_WIDTH_MAX_DM;
            dm = dm < lo ? lo : dm > hi ? hi : dm;
            if (dist) g_vr_dist_dm = dm; else g_vr_width_dm = dm;
        }
        else if (!strncmp(line, "texture_packs=", 14)) {
            snprintf(g_texpacks, sizeof g_texpacks, "%s", line + 14);
            g_texpacks[strcspn(g_texpacks, "\r\n")] = '\0';
        }
        else if (!strncmp(line, "font=", 5)) {
            snprintf(g_font_key, sizeof g_font_key, "%s", line + 5);
            g_font_key[strcspn(g_font_key, "\r\n")] = '\0';
        }
    }
    fclose(f);
    if (res >= 0) g_res_saved = res;
    return res;
}

static void display_save(int res) {
    g_res_saved = res;
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "w") : NULL;
    if (!f) { fprintf(stderr, "display: cannot write %s\n", g_display_ini); return; }
    fprintf(f, "; psp2i display settings (the settings menu changes this)\n");
    fprintf(f, "resolution=%s\n", menu_res_name(res));
    fprintf(f, "; renderer: d3d11 or opengl (used from the next start)\n");
    fprintf(f, "renderer=%s\n", menu_ren_key(g_ren_saved));
    fprintf(f, "; internal resolution, Direct3D 11 only: 1 = the PSP's 480x272, 2..6 = that many times sharper (used from the next start;"
               " 5 and 6 cost a lot, for VR: engine menu VR > RESOLUTION)\n");
    fprintf(f, "render_scale=%d\n", g_render_scale);
    fprintf(f, "; volumes 0..100 (%%); attenuation while other programs play sound: off, -6db, -12db, -20db\n");
    fprintf(f, "master_volume=%d\nmusic_volume=%d\nsfx_volume=%d\n",
            g_vol_saved[VOL_MASTER] * 10, g_vol_saved[VOL_MUSIC] * 10, g_vol_saved[VOL_SFX] * 10);
    if (g_att_saved == ATT_OFF) fprintf(f, "attenuation=off\n");
    else fprintf(f, "attenuation=%ddb\n", menu_att_db(g_att_saved));
    fprintf(f, "; movies: on = download Cisco's OpenH264 decoder on first start (licenses/OpenH264-NOTICE.txt), off = black movies\n");
    fprintf(f, "openh264=%s\n", g_openh264 ? "on" : "off");
    fprintf(f, "; the game's text font: system (Windows' font), psp (flash/font, if there), or a .ttf/.otf/.ttc in the fonts folder\n");
    fprintf(f, "font=%s\n", g_font_key);
    fprintf(f, "; on = Circle and Cross exchanged (keys X/Z and controllers)\n");
    fprintf(f, "swap_circle_cross=%s\n", g_swap_oc ? "on" : "off");
    fprintf(f, "; the right stick: on = it turns the camera, off = it does nothing; rs_speed 1.00..2.00\n");
    fprintf(f, "right_stick=%s\nrs_speed=%.2f\n", g_rs_on ? "on" : "off", (double)menu_rs_speed(&g_menu));
    fprintf(f, "; menu TESTS at 60 / 120 fps (off by default): the game's tick counters paced as at 30; frame-counted logic at 30 Hz\n");
    fprintf(f, "test_tick_pacing=%s\ntest_30hz_logic=%s\n", g_test_ticks ? "on" : "off", g_test_logic ? "on" : "off");
    fprintf(f, "; VR (OpenXR): on = the game on a screen in the headset; its distance and width in metres\n");
    fprintf(f, "vr=%s\nvr_distance=%d.%d\nvr_width=%d.%d\n", g_vr_on ? "on" : "off",
            g_vr_dist_dm / 10, g_vr_dist_dm % 10, g_vr_width_dm / 10, g_vr_width_dm % 10);
    fprintf(f, "; VR stereo (Direct3D 11): on = a 3D window into the game (as wide as its view), off = the flat screen;"
               " world scale = game units per metre (sets the depth)\n");
    fprintf(f, "vr_stereo=%s\nvr_world_scale=%.1f\n", g_vr_stereo ? "on" : "off", (double)g_vr_world_scale);
    fprintf(f, "; VR head tracking (with stereo): on = you see the game from your head (at the game's field of view), off = the 3D window;"
               " level: on = the horizon stays level (the game camera's tilt taken out)\n");
    fprintf(f, "vr_head=%s\nvr_level=%s\n", g_vr_head ? "on" : "off", g_vr_level ? "on" : "off");
    fprintf(f, "; with head tracking: fov scale = the view as that many times the game's across (1.0..3.0) and up (_y, 1.0..4.0;"
               " wider = less sharp); cull scale = the game's object culling that much wider (1 = the game's)\n");
    fprintf(f, "; with head tracking: fov auto = on: the fov scales follow the headset's own view (no edge of the picture in"
               " sight), off = the fov scales below as set\n");
    fprintf(f, "vr_fov_auto=%s\n", g_vr_fov_auto ? "on" : "off");
    fprintf(f, "vr_fov_scale=%.1f\nvr_fov_scale_y=%.1f\nvr_cull_scale=%.1f\n", (double)g_vr_fov, (double)g_vr_fov_y,
            (double)g_vr_cull);
    fprintf(f, "; with head tracking: cull all = on: every object the game has loaded is drawn (nothing missing behind you"
               " or far away; costs some speed), off = the game's own culling (widened by cull scale)\n");
    fprintf(f, "vr_cull_all=%s\n", g_vr_cull_all ? "on" : "off");
    fprintf(f, "; with head tracking: two sided = on: both sides of the 3D scene's faces drawn (walls and roofs the PSP camera"
               " never saw from behind stay solid), off = the game's back-face culling\n");
    fprintf(f, "vr_two_sided=%s\n", g_vr_two_sided ? "on" : "off");
    fprintf(f, "; with head tracking: the HUD and menus on a panel this many metres away, this size of the game's view (0.2..1.0);"
               " follow = how many degrees you can look away before it glides back in front of you (0 = it stays where recenter put it)\n");
    fprintf(f, "vr_hud_distance=%.1f\nvr_hud_size=%.2f\nvr_hud_follow=%.0f\n", (double)g_vr_hud_dist, (double)g_vr_hud_size,
            (double)g_vr_hud_follow);
    fprintf(f, "; with head tracking: view first = you are in your character's head (gamepad: L3 twice quickly toggles), third = behind it;"
               " fp eye = the eye height in game units (the character is ~5.8 tall); fp forward = the eyes this far ahead of the"
               " head along your gaze (so your own head stays behind you); fp near = nothing drawn closer than this to your eyes"
               " (hides your own head when running); fp scale = game units per metre in first person\n");
    fprintf(f, "vr_view=%s\nvr_fp_eye=%.1f\nvr_fp_forward=%.1f\nvr_fp_near=%.1f\nvr_fp_scale=%.1f\n", g_vr_first ? "first" : "third",
            (double)g_vr_fp_eye, (double)g_vr_fp_fwd, (double)g_vr_fp_near, (double)g_vr_fp_scale);
    fprintf(f, "; first person: fp nudge = on: an attack (Square, Triangle) first turns your character to where you look"
               " (not while R locks on), off = the attack goes the way the character faces\n");
    fprintf(f, "vr_fp_nudge=%s\n", g_vr_fp_nudge ? "on" : "off");
    fprintf(f, "; VR input: pad = a gamepad (or the VR controllers as one), motion = the VR controllers' hands shown"
               " where they are (motion controls build on it; the buttons stay the pad's)\n");
    fprintf(f, "vr_input=%s\n", g_vr_motion ? "motion" : "pad");
    fprintf(f, "; VR image quality (from the next start, VR only): aniso = texture filtering 0 (off) or 2..16;"
               " sharpen = sharpening of the headset's pictures 0.0 (off) .. 1.0\n");
    fprintf(f, "vr_aniso=%d\nvr_sharpen=%.2f\n", g_vr_aniso, (double)g_vr_sharpen);
    fprintf(f, "; menu screen: on = the game's menu scenes (continue, mission select, loading) on a flat screen instead of in 3D around you\n");
    fprintf(f, "vr_menu_screen=%s\n", g_vr_menu_screen ? "on" : "off");
    fprintf(f, "; PPSSPP texture packs: folders holding a textures.ini, ';'-separated, later ones win (relative to the exe); empty = off\n");
    fprintf(f, "texture_packs=%s\n", g_texpacks);
    fclose(f);
}

static void display_set(int res) {
    if (res != RES_FULLSCREEN) g_last_windowed = res;
    if (g_headless) { fprintf(stderr, "menu: resolution %s (headless: not saved)\n", menu_res_name(res)); return; }
#ifdef _WIN32
    window_apply_res(res);
#endif
    display_save(res);
    fprintf(stderr, "menu: resolution %s (saved)\n", menu_res_name(res));
}

/* Volumes and attenuation to where they act: the device output (master and
 * attenuation, audio_sdl.c), the music decoder and the sound-effect mixer. */
static void audio_apply(void) {
    audio_set_master((float)g_vol_saved[VOL_MASTER] / VOL_STEPS);
    atrac_at3_set_gain((float)g_vol_saved[VOL_MUSIC] / VOL_STEPS);
    psp_sas_set_gain((float)g_vol_saved[VOL_SFX] / VOL_STEPS);
    audio_set_attenuation_db(menu_att_db(g_att_saved));
}

/* Once per vblank: the menu reads the raw input (the game's is filtered). */
static void menu_step(void) {
    uint32_t psp = g_script_bits | g_pad_bits, latched = g_host_latched;
#ifdef _WIN32
    psp |= g_keys;
    latched |= menu_inputs(g_keys_latched, 128, 128);
#endif
    g_keys_latched = 0;
    g_host_latched = 0;
#ifdef _WIN32
    if (g_alt_enter) {
        g_alt_enter = 0;
        g_menu.res = g_menu.res == RES_FULLSCREEN ? g_last_windowed : RES_FULLSCREEN;
        display_set(g_menu.res);
    }
#endif
    uint8_t ax = g_pad_ax, ay = g_pad_ay;
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    uint32_t held = menu_inputs(psp, ax, ay) | g_host_held;
    if (input_sdl_host_buttons() & (INPUT_HOST_GUIDE | INPUT_HOST_RSTICK)) held |= MI_TOGGLE;
#ifdef PSP2I_VR
    if (g_vr_host & VR_HOST_MENU) held |= MI_TOGGLE;
    snprintf(g_menu.vr_status, sizeof g_menu.vr_status, "%s", g_vr_live ? vr_status_name(vr_status()) : "");
    /* WORLD SIZE: the scale of the view in use, as a percentage of life
     * size (VR_FP_SCALE_DEFAULT game units per metre = 100%). */
    g_menu.vr_first = g_vr_first;
    g_menu.vr_world_pct = (int)lroundf(VR_FP_SCALE_DEFAULT * 100.0f / (g_vr_first ? g_vr_fp_scale : g_vr_world_scale));
    g_menu.vr_hud_dm = (int)lroundf(g_vr_hud_dist * 10.0f);
    g_menu.vr_res = g_render_scale;                     /* RESOLUTION: the saved render scale */
    g_menu.vr_res_now = g_hi_s;
    g_menu.vr_sharpen = (int)lroundf(g_vr_sharpen * VR_SHARPEN_STEPS);
    g_menu.vr_filter = menu_vr_filter_of(g_vr_aniso);
    g_menu.vr_motion = g_vr_motion;
    /* RESTART: sharpening needs a start in VR (its shader), mipmaps a start
     * with filtering on (they're made when textures are first loaded). */
    g_menu.vr_quality_restart = 0;
    if (g_renderer == REN_D3D11 && g_vr_sharpen > 0 && !d3d11_sharpen_ready()) g_menu.vr_quality_restart |= VR_RESTART_SHARPEN;
    if (g_renderer == REN_D3D11 && (g_vr_aniso > 0) != d3d11_tex_mips()) g_menu.vr_quality_restart |= VR_RESTART_FILTER;
#endif

    snprintf(g_menu.sega, sizeof g_menu.sega, "%s", online_sega_redirect());
    snprintf(g_menu.adhoc, sizeof g_menu.adhoc, "%s", online_adhoc_server());
    snprintf(g_menu.modern, sizeof g_menu.modern, "%s", online_modern_server());
    g_menu.adhoc_mode = online_adhoc_mode();
    if (g_sega_edit.open) {
        /* The text editor (SEGA SERVER or ADHOC SERVER) has the input (pad
         * buttons and navigation keys; typed characters arrive through WM_CHAR). */
        const uint32_t pad = g_script_bits | g_pad_bits;
        int r = textedit_update(&g_sega_edit, menu_inputs_from_psp(pad & 0xFFFF, ax, ay) | g_login_keys, pad);
        if (g_edit_enter) { g_edit_enter = 0; g_sega_edit.open = 0; r = TE_SAVE; }
        if (r == TE_SAVE && g_edit_adhoc == 2) {
            online_set_modern_server(g_sega_edit.text);
            fprintf(stderr, "menu: modern server %s\n", online_modern_server());
        } else if (r == TE_SAVE && g_edit_adhoc) {
            online_set_adhoc_server(g_sega_edit.text);
            fprintf(stderr, "menu: ad hoc server %s\n", online_adhoc_server());
        } else if (r == TE_SAVE) {
            online_set_sega_redirect(g_sega_edit.text);
            fprintf(stderr, "menu: SEGA server redirect %s\n", online_sega_redirect()[0] ? online_sega_redirect() : "off");
        }
        menu_update(&g_menu, 0, 0);       /* keep its press detection in step */
        g_menu.prev = held;               /* no stray press when the editor closes */
        return;
    }

    const int fx = menu_update(&g_menu, held, latched);
    if (fx & MFX_EDIT_SEGA) {
        char hint[96];
        snprintf(hint, sizeof hint, "%.22s -> HOST[:PORT], EMPTY = OFF", online_sega_host());
        textedit_open(&g_sega_edit, "SEGA SERVER REDIRECT", hint, online_sega_redirect());
        g_edit_adhoc = 0;
    }
    if (fx & MFX_EDIT_ADHOC) {
        textedit_open(&g_sega_edit, "ADHOC SERVER", "HOST[:PORT] (DEFAULT PORT 27312)", online_adhoc_server());
        g_edit_adhoc = 1;
    }
    if (fx & MFX_EDIT_MODERN) {
        textedit_open(&g_sega_edit, "MODERN SERVER", "HOST, OR THE HOSTING PLAYER (IPV4 OR IPV6)", online_modern_server());
        g_edit_adhoc = 2;
    }
    if (fx & MFX_OPENED) {                    /* what the others type to join, if this game hosts modern */
        char v4[64], v6[64];
        psp_adhoc_host_addresses(v4, sizeof v4, v6, sizeof v6);
        const char *ms = online_modern_server();
        if (!ms[0] || !strcmp(ms, "host"))
            snprintf(g_menu.share, sizeof g_menu.share, "%s%s%s", v6[0] ? v6 : v4, v6[0] && v4[0] ? " / " : "", v6[0] ? v4 : "");
        else g_menu.share[0] = '\0';
    }
    if (fx & MFX_ADHOC_MODE) {
        online_set_adhoc_mode(g_menu.adhoc_mode);
        fprintf(stderr, "menu: ad hoc connection %d (0 PPSSPP direct, 1 PPSSPP relay, 2 modern)\n", g_menu.adhoc_mode);
    }
    if (fx & MFX_FPS_CHANGED) {
        framerate_set(g_menu.fps);
        rec_fps(g_menu.fps);
        fprintf(stderr, "menu: frame rate %d fps\n", g_menu.fps);
    }
    if (fx & MFX_RES_CHANGED) display_set(g_menu.res);
    if (fx & MFX_AUDIO) {
        for (int k = 0; k < VOL_N; k++) g_vol_saved[k] = g_menu.vol[k];
        g_att_saved = g_menu.att;
        audio_apply();
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: volume master %d%%, music %d%%, effects %d%%; attenuation %d dB\n",
                g_vol_saved[VOL_MASTER] * 10, g_vol_saved[VOL_MUSIC] * 10, g_vol_saved[VOL_SFX] * 10, menu_att_db(g_att_saved));
    }
    if (fx & MFX_RENDERER) {
        g_ren_saved = g_menu.renderer;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: renderer %s (saved; used from the next start)\n", menu_ren_key(g_menu.renderer));
    }
    if (fx & MFX_SWAP_OC) {
        g_swap_oc = g_menu.swap_oc;
        input_sdl_set_swap_circle_cross(g_swap_oc);
#ifdef PSP2I_VR
        vr_set_swap_circle_cross(g_swap_oc);
#endif
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: Circle/Cross %s (saved)\n", g_swap_oc ? "swapped" : "normal");
    }
#ifdef PSP2I_VR
    if (fx & MFX_VR) {
        g_vr_on = g_menu.vr_on;
        g_vr_dist_dm = g_menu.vr_dist_dm;
        g_vr_width_dm = g_menu.vr_width_dm;
        g_vr_hud_dist = (float)g_menu.vr_hud_dm / 10.0f;
        g_render_scale = g_menu.vr_res;                 /* from the next start */
        g_vr_sharpen = (float)g_menu.vr_sharpen / (float)VR_SHARPEN_STEPS;
        g_vr_aniso = menu_vr_filter_aniso(g_menu.vr_filter);
        if (g_renderer == REN_D3D11) d3d11_set_quality(g_vr_aniso, g_vr_sharpen);
        if (g_menu.vr_motion != g_vr_motion) {
            g_vr_motion = g_menu.vr_motion;
            if (g_vr_motion && g_vr_live) vr_haptic(-1, 0.5f, 0.15f);   /* a buzz: the controllers are the hands now */
            fprintf(stderr, "menu: VR input %s\n", g_vr_motion ? "motion (the hands shown)" : "pad");
        }
        if (g_menu.vr_world_pct > 0) {                   /* the view in use: life size * 100 / percent */
            const float k = VR_FP_SCALE_DEFAULT * 100.0f / (float)g_menu.vr_world_pct;
            if (g_vr_first) g_vr_fp_scale = k; else g_vr_world_scale = k;
        }
        vr_apply();
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: VR %s, screen %d.%d m away, %d.%d m wide, HUD %.1f m, world %d%% (%s person) (saved)\n",
                g_vr_on ? "on" : "off", g_vr_dist_dm / 10, g_vr_dist_dm % 10, g_vr_width_dm / 10, g_vr_width_dm % 10,
                (double)g_vr_hud_dist, g_menu.vr_world_pct, g_vr_first ? "first" : "third");
    }
    if (fx & MFX_VR_RECENTER) { vr_recenter(); g_fp_yaw_ok = 0; }   /* first person faces the game camera's way again */
#endif
    if (fx & MFX_FONT) {
        g_menu.font = font_win_select(g_menu.font);
        snprintf(g_menu.font_name, sizeof g_menu.font_name, "%s", font_win_name(g_menu.font));
        snprintf(g_font_key, sizeof g_font_key, "%s", font_win_key(g_menu.font));
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: font %s (saved; text drawn from now on)\n", g_font_key);
    }
    if (fx & MFX_SHOW_FPS) fprintf(stderr, "menu: FPS counter %s\n", g_menu.show_fps ? "on" : "off");
    if (fx & MFX_TESTS) {
        g_test_ticks = g_menu.test_ticks;
        g_test_logic = g_menu.test_logic;
        framerate_set_tests(g_test_ticks, g_test_logic);
        if (!g_headless) display_save(g_res_saved);
    }
    if (fx & MFX_RS_ON) {
        g_rs_on = g_menu.rs_on;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: right stick %s (saved)\n", g_rs_on ? "turns the camera" : "off");
    }
    if (fx & MFX_RS_SPEED) {
        camera_set_speed(menu_rs_speed(&g_menu));
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: right-stick sensitivity %.2fx\n", (double)menu_rs_speed(&g_menu));
    }
    if (fx & (MFX_OPENED | MFX_CLOSED)) {
        fprintf(stderr, "menu: %s at vblank %llu\n", (fx & MFX_OPENED) ? "opened" : "closed",
                (unsigned long long)psp_sched_vblank_count());
        ctrl_commit();                     /* take or give back the game's input now */
    }
}

/* The right stick for the camera (camera.c): SDL, or --rstick in scripts;
 * centred while the settings menu is open or the stick is turned off there. */
static void update_camera_stick(uint64_t vb) {
    uint8_t rx = 128, ry = 128;
    if (g_sdl_on) input_sdl_right_stick(&rx, &ry);
    if (g_vr_live && (stick_off_centre(g_vr_rx, g_vr_ry) || !stick_off_centre(rx, ry))) { rx = g_vr_rx; ry = g_vr_ry; }   /* the right VR controller */
    for (int i = 0; i < g_nrstick; i++)
        if (vb >= g_rstick[i].at && vb < g_rstick[i].at + g_rstick[i].len) { rx = g_rstick[i].x; ry = g_rstick[i].y; }
    if (g_rep_on) { rx = g_rep_rx; ry = g_rep_ry; }
    if (g_menu.open || !g_menu.rs_on) rx = ry = 128;      /* the menu's RIGHT STICK off: no camera */
    if (!g_rep_on) rec_cam(rx, ry);
    camera_set_stick(rx, ry);
}

static void poll_controllers(void) {
    if (!g_sdl_on && !g_vr_live) return;
    uint32_t b = 0;
    uint8_t ax = 128, ay = 128;
    if (g_sdl_on) input_sdl_poll(&b, &ax, &ay);
#ifdef PSP2I_VR
    if (g_vr_live) {
        vr_pad vp;
        vr_pad_get(&vp);                      /* released (centred) while the headset has no focus */
        b |= vp.buttons;
        if (stick_off_centre(vp.ax, vp.ay) || !stick_off_centre(ax, ay)) { ax = vp.ax; ay = vp.ay; }
        g_vr_host = vp.host;
        g_vr_rx = vp.rx; g_vr_ry = vp.ry;
#ifndef PSP2I_PROD
        if (vp.host & VR_HOST_SHOT) vrshot_request();   /* left grip + left stick click: like F11 */
#endif
    }
    /* A gamepad in VR (no motion controllers needed): L3 tapped recenters,
     * L3 held a second saves a VR capture (dev builds) instead. L3 is no
     * PSP button, so the game never sees it. */
    if (g_vr_live && g_sdl_on) {
        static ULONGLONG down;                  /* when L3 went down; 0 = up */
        static int held_done;
        const int l3 = (input_sdl_host_buttons() & INPUT_HOST_LSTICK) != 0;
        const ULONGLONG now = GetTickCount64();
        if (l3 && !down) { down = now; held_done = 0; }
        if (l3 && !held_done && now - down >= 1000) {
            held_done = 1;
#ifndef PSP2I_PROD
            fprintf(stderr, "vr: gamepad L3 held: a capture and a GE dump\n");
            vrshot_request();
            g_dump_key = 1;                     /* and F12's GE dump: every draw the game submits */
#endif
        }
        if (!l3 && down) {
            static ULONGLONG last_tap;              /* a second tap within 0.4 s: first / third person */
            if (!held_done && last_tap && now - last_tap < 400) {
                g_vr_first = !g_vr_first;
                fprintf(stderr, "vr: gamepad L3 twice: %s person\n", g_vr_first ? "first" : "third");
                if (!g_headless) display_save(g_res_saved);
                last_tap = 0;
            } else if (!held_done) {
                fprintf(stderr, "vr: gamepad L3: recenter\n");
                vr_recenter();
                g_fp_yaw_ok = 0;
                last_tap = now;
            }
            down = 0;
        }
    }
#endif
    if (b != g_pad_bits || ax != g_pad_ax || ay != g_pad_ay || g_fp_stick_on
#ifdef PSP2I_VR
        || g_nudge                                                                  /* the attack nudge runs by frames */
#endif
        ) {   /* first person's turned stick follows the head */
        g_pad_bits = b; g_pad_ax = ax; g_pad_ay = ay;
        ctrl_commit();
    }
}

/* Frame-rate measurement: the game's frames are its framebuffer flips, sampled
 * over windows of 60 vblanks (about one second of host time). */
#define FPS_MAX_WIN 4096
static double   g_fps_win[FPS_MAX_WIN];
static int      g_fps_n;
static uint64_t g_fps_last_flips, g_fps_last_us;
static int      g_fps_log = -1;

/* Once a minute, one "perf:" line in online_log.txt: frame rate, frame busy
 * times, GE and idle share and the process's memory over that minute -- so
 * slowdowns that only come in long sessions show what grew with them. */
static void perf_minute(double fps) {
    static double sum, mn = 1e9;
    static int n;
    static uint64_t last_fb[6], last_ge, last_idle, last_us;
    sum += fps; n++;
    if (fps < mn) mn = fps;
    if (n < 60) return;
    const uint64_t now = psp_sched_now_us(), ge = psp_ge_host_us(), idle = psp_sched_idle_us();
    uint64_t fb[6];
    psp_display_frame_busy(fb);
    const double span = last_us ? (double)(now - last_us) : (double)now;
    double mb = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) mb = (double)pmc.PagefileUsage / (1024.0 * 1024.0);
#endif
    online_log("perf: %.1f min, fps avg %.1f min %.1f; frames <16.7ms %llu, <25 %llu, <33 %llu, <50 %llu, 50+ %llu; "
               "GE %.1f%%, idle %.1f%%; memory %.0f MB",
               now / 60e6, sum / n, mn,
               (unsigned long long)(fb[0] + fb[1] - last_fb[0] - last_fb[1]), (unsigned long long)(fb[2] - last_fb[2]),
               (unsigned long long)(fb[3] - last_fb[3]), (unsigned long long)(fb[4] - last_fb[4]),
               (unsigned long long)(fb[5] - last_fb[5]),
               span > 0 ? 100.0 * (double)(ge - last_ge) / span : 0.0,
               span > 0 ? 100.0 * (double)(idle - last_idle) / span : 0.0, mb);
    memcpy(last_fb, fb, sizeof fb);
    last_ge = ge; last_idle = idle; last_us = now;
    sum = 0; n = 0; mn = 1e9;
}

static void sample_fps(uint64_t vb) {
    if (vb % 60) return;
    uint64_t now = psp_sched_now_us(), flips = psp_display_flips();
    if (g_fps_last_us) {
        double fps = (double)(flips - g_fps_last_flips) * 1e6 / (double)(now - g_fps_last_us);
        if (g_fps_n < FPS_MAX_WIN) g_fps_win[g_fps_n++] = fps;
        if (g_fps_log < 0) g_fps_log = getenv("PSP2I_FPS_LOG") != NULL;
        if (g_fps_log) fprintf(stderr, "fps: %5.1f at vblank %llu (counter shows %.1f)\n", fps,
                               (unsigned long long)vb, fps_meter_value(&g_fpsm));
        if (!g_headless) perf_minute(fps);
    }
    g_fps_last_flips = flips;
    g_fps_last_us = now;
}

static void report_fps(void) {
    /* Skip the first 5 windows (boot, logos) when summarising steady state. */
    int from = g_fps_n > 10 ? 5 : 0, n = 0, below = 0;
    double sum = 0, mn = 1e9;
    for (int i = from; i < g_fps_n; i++) {
        sum += g_fps_win[i]; n++;
        if (g_fps_win[i] < mn) mn = g_fps_win[i];
        if (g_fps_win[i] < 29.0) below++;
    }
    if (!n) return;
    double run = psp_sched_now_us() / 1e6;
    fprintf(stderr, "  frame rate          avg %.1f fps, min %.1f fps over %d 1-s windows (%d below 29)\n",
            sum / n, mn, n, below);
    {
        uint64_t sp[4];
        psp_display_flip_spacing(sp);
        fprintf(stderr, "  flip spacing        1 vblank: %llu, 2: %llu, 3: %llu, 4+: %llu\n",
                (unsigned long long)sp[0], (unsigned long long)sp[1], (unsigned long long)sp[2], (unsigned long long)sp[3]);
        uint64_t fb[6];
        psp_display_frame_busy(fb);
        fprintf(stderr, "  frame busy time     <8ms %llu, <16.7 %llu, <25 %llu, <33.3 %llu, <50 %llu, 50+ %llu\n",
                (unsigned long long)fb[0], (unsigned long long)fb[1], (unsigned long long)fb[2],
                (unsigned long long)fb[3], (unsigned long long)fb[4], (unsigned long long)fb[5]);
    }
    /* For PSP2I_GE_DUMP_FLIP / --watch-from-flip, which count flips. */
    fprintf(stderr, "  display flips       %llu at vblank %llu\n",
            (unsigned long long)psp_display_flips(), (unsigned long long)psp_sched_vblank_count());
    fprintf(stderr, "  host time          GE lists %.1f%%, of which rasterizing %.1f%%; idle %.1f%%\n",
            100.0 * psp_ge_host_us() / 1e6 / run, 100.0 * psp_gpu_host_us() / 1e6 / run,
            100.0 * psp_sched_idle_us() / 1e6 / run);
}

static uint32_t g_watch_late_addr;
static uint64_t g_watch_late_flip;
/* --find-word VALUE VBLANK: list every RAM word equal to VALUE at that vblank
 * (to find where the game keeps a pointer before watching it). */
static uint32_t g_find_value;
static uint64_t g_find_vblank;
static void find_word(void) {
    int n = 0;
    for (uint32_t off = 0; off + 4 <= PSP_RAM_SIZE && n < 64; off += 4) {
        uint32_t w;
        memcpy(&w, psp_mem.ram + off, 4);
        if (w == g_find_value) {
            fprintf(stderr, "find-word: 0x%08X at 0x%08X\n", w, PSP_RAM_BASE + off);
            n++;
        }
    }
    fprintf(stderr, "find-word: %d match(es) for 0x%08X at vblank %llu\n", n, g_find_value,
            (unsigned long long)psp_sched_vblank_count());
}

static void exe_dir(char *out, size_t cap);

static int dir_exists(const char *p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return 1;
#endif
}

/* The first *.iso next to the exe, in the root, or in the root's parent
 * (FinalBuild keeps the root at ..\GameData, the image beside it). */
static void find_iso(const char *dir, const char *root, char *out, size_t cap) {
#ifdef _WIN32
    char parent[700];
    snprintf(parent, sizeof parent, "%s/..", root);
    const char *const where[3] = { dir, root, parent };
    for (int i = 0; i < 3; i++) {
        char pat[760];
        snprintf(pat, sizeof pat, "%s\\*.iso", where[i]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        snprintf(out, cap, "%s\\%s", where[i], fd.cFileName);
        return;
    }
#else
    (void)dir; (void)root; (void)out; (void)cap;
#endif
}

/* F12: the displayed frame as dumps/frame_<vblank>.ppm and every draw of the
 * next frame as dumps/draws_<vblank>.txt, next to the exe. */
static uint64_t g_ge_dump_vb;              /* --ge-dump VBLANK: as F12 at that vblank */

static void take_dump(uint64_t vb) {
    char dir[600], path[700];
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/dumps", dir);
    host_mkdir(path);
    snprintf(path, sizeof path, "%s/dumps/frame_%06llu.ppm", dir, (unsigned long long)vb);
    psp_display_capture(path);
    fprintf(stderr, "dump: F12 at vblank %llu -> %s\n", (unsigned long long)vb, path);
    snprintf(path, sizeof path, "%s/dumps/draws_%06llu.txt", dir, (unsigned long long)vb);
    psp_gpu_dump_next_frame(path);
}

/* --profile-from / --profile-to VBLANK: sample only inside that window.
 * Never before the first vblank: the call-stack unwinder takes the loader's
 * function-table lock, and suspending the game thread while it loads a DLL
 * (D3D, SDL, FFmpeg all load before the scheduler starts) deadlocks. */
static volatile LONG g_prof_on = 0;
static uint64_t g_prof_from, g_prof_to;

static void on_vblank(void) {
    uint64_t vb = psp_sched_vblank_count();
    g_prof_on = vb >= g_prof_from && (!g_prof_to || vb < g_prof_to);
    if (g_ge_dump_vb && vb == g_ge_dump_vb) g_dump_key = 1;
    if (g_dump_key) { g_dump_key = 0; take_dump(vb); }
#ifndef PSP2I_PROD
    rateaudit_vblank();
    vr_probe_vblank();
#endif
#ifdef PSP2I_VR
    stereo_update();
#endif
    if (g_find_vblank && vb == g_find_vblank) find_word();
    if (g_watch_late_addr && psp_display_flips() >= g_watch_late_flip) {
        /* --watch-from-flip: arm the write watch only once the game is there. */
        psp_mem_watch_write(g_watch_late_addr);
        g_watch_late_addr = 0;
    }
    apply_script(vb);
    poll_controllers();
    menu_step();
    update_camera_stick(vb);
    online_poll();
    if (g_login.state != LOGIN_CLOSED) {
        static int was_open;
        if (!was_open) {
            /* Headless tests only: pre-fill the fields (never logged). */
            const char *u = getenv("PSP2I_TEST_LOGIN_USER"), *pw = getenv("PSP2I_TEST_LOGIN_PASS");
            if (u) snprintf(g_login.user, sizeof g_login.user, "%s", u);
            if (pw) snprintf(g_login.pass, sizeof g_login.pass, "%s", pw);
            if (u && pw) g_login.sel = LOGIN_BTN_SIGNIN;
        }
        was_open = 1;
        uint32_t psp = g_script_bits | g_pad_bits;
        uint8_t ax = g_pad_ax, ay = g_pad_ay;
        if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
        const uint32_t held = menu_inputs_from_psp(psp & 0xFFFF, ax, ay) | g_login_keys;
        switch (login_update(&g_login, held, psp)) {
        case LOGIN_ACT_SUBMIT:   online_login_submit(); break;
        case LOGIN_ACT_CANCEL:   online_login_cancel(); break;
        case LOGIN_ACT_CLOSE_OK: online_login_closed_ok(); break;
        default: break;
        }
        if (g_login.state == LOGIN_CLOSED) { was_open = 0; ctrl_commit(); }
    }
    fps_meter_sample(&g_fpsm, psp_sched_now_us(), psp_display_flips());
    sample_fps(vb);
    /* The picture for the window. With D3D11 and the display buffer ahead of
     * VRAM it comes from an asynchronous copy (d3d11_present: no stall, at
     * most a vblank or two late); otherwise from VRAM. Done headless too, so
     * benchmarks pay what a windowed run pays. Captures and F12 read VRAM,
     * which psp_mem_ptr's access hook keeps exact. */
    int have_frame = 0;
    {
        uint32_t addr, stride, fmt;
        psp_display_get(&addr, &stride, &fmt);
#ifdef _WIN32
        const int p = addr ? gpu_present(addr, stride, (int)fmt, g_frame, SCREEN_W, SCREEN_H) : 0;
        if (p == 1) have_frame = 1;
        if (p == 1 && g_hi_s > 1) {                      /* the S-times picture of the same frame */
            int hw, hh;
            const uint32_t *hi = d3d11_present_hires(&hw, &hh);
            g_hi_valid = hi && hw == SCREEN_W * g_hi_s && hh == SCREEN_H * g_hi_s;
            if (g_hi_valid) memcpy(g_hi_frame, hi, (size_t)hw * hh * 4);
        }
        if (p == 0) g_hi_valid = 0;                      /* from VRAM: 480x272 only */
#ifdef PSP2I_VR
        /* The eye pictures stay until the next game frame replaces them (a
         * frame shows for 2+ vblanks; d3d11_present then returns 2). A
         * picture from VRAM (p 0: movies) has no eyes. */
        g_eye_new = 0;
        g_eye_shared = g_stereo_on && g_renderer == REN_D3D11
                     ? d3d11_present_eye_shared(&g_eye_shared_w, &g_eye_shared_h, &g_eye_shared_tag) : NULL;
        if (!g_stereo_on || p == 0) g_eye_px[0] = g_eye_px[1] = NULL;
        if (p == 1 && g_stereo_on && g_renderer == REN_D3D11) {
            int w1, h1, w2, h2;
            const uint32_t *l = d3d11_present_eye(0, &w1, &h1), *r = d3d11_present_eye(1, &w2, &h2);
            g_eye_px[0] = g_eye_px[1] = NULL;
            if (l && r && w1 == w2 && h1 == h2 && w1 % SCREEN_W == 0 && w1 <= SCREEN_W * 6) {
                g_eye_px[0] = l; g_eye_px[1] = r; g_eye_w = w1; g_eye_h = h1;
                g_eye_px_tag = d3d11_present_eye_tag();
                g_eye_new = 1;
            }
        }
#ifndef PSP2I_PROD
        vrshot_vblank(vb);
#endif
#endif
        if (p == 1 && getenv("PSP2I_PRESENT_VERIFY")) {
            /* The asynchronous picture against VRAM made current (the access
             * hook reads the same target back): they must match exactly. */
            static uint32_t keep[SCREEN_W * SCREEN_H];
            static uint64_t frames, bad_frames, bad_px;
            memcpy(keep, g_frame, sizeof keep);
            grab_frame();
            uint64_t bad = 0;
            for (int i = 0; i < SCREEN_W * SCREEN_H; i++) bad += keep[i] != g_frame[i];
            frames++; if (bad) { bad_frames++; bad_px += bad; }
            if (frames % 600 == 0 || (bad && bad_frames <= 5))
                fprintf(stderr, "present-verify: %llu frames, %llu differ (%llu pixels)\n",
                        (unsigned long long)frames, (unsigned long long)bad_frames, (unsigned long long)bad_px);
        }
        else if (p == 0 && !g_headless) have_frame = grab_frame();
#endif
    }
#ifdef _WIN32
    if (!g_headless) {
        if (have_frame) g_have_frame = 1;
        if (g_have_frame) {
            compose();
#ifdef PSP2I_VR
            g_overlay_up = memcmp(g_rgba, g_frame, sizeof g_rgba) != 0;
            /* Stereo: each eye's own picture, for each new game frame, and
             * every vblank while the overlay (engine menu) is up so it stays
             * live. On the GPU when it can be (no overlay). */
            /* Head tracking: a picture drawn without the head (tag 0: a menu
             * scene, or a frame of the game's loading transitions) goes on the
             * flat screen only once that has lasted a second. Until then the
             * headset keeps the last 3D picture at its pose (user: "the flat
             * game sometimes appears floating on top of VR for a short
             * moment": the area changes' loading frames). */
            static uint64_t tracked_vb;                  /* the last picture drawn from the head */
            if (g_vr_live && g_eye_shared && g_eye_shared_tag) tracked_vb = vb;
            if (g_vr_live && !g_eye_shared && g_eye_px[0] && g_eye_px_tag) tracked_vb = vb;
            const int hold_flat = tracked_vb && vb - tracked_vb < (g_menu_loading ? 300u : 60u);
            if (g_vr_live && g_eye_shared) {
                if (g_eye_shared_tag || !hold_flat)
                    vr_submit_stereo_shared(g_eye_shared, g_eye_shared_w, g_eye_shared_h, eye_tan(g_eye_shared_tag),
                                            eye_tan_y(g_eye_shared_tag), g_eye_shared_tag);
                g_eye_shared_vb = vb;
            }
            if (g_vr_live && g_stereo_on && vb - g_eye_shared_vb < 8) {
                /* The overlay as its own layer: the pixels compose() changed. */
                for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
                    g_ovl_px[i] = g_rgba[i] != g_frame[i] ? g_rgba[i] | 0xFF000000u : 0;
                if (!g_ovl_sent_valid || memcmp(g_ovl_px, g_ovl_sent, sizeof g_ovl_px) != 0) {
                    vr_submit_overlay(g_ovl_px);
                    memcpy(g_ovl_sent, g_ovl_px, sizeof g_ovl_sent);
                    g_ovl_sent_valid = 1;
                }
            } else if (g_ovl_sent_valid) {               /* the CPU pictures carry it again */
                vr_submit_overlay(NULL);
                g_ovl_sent_valid = 0;
            }
            if (g_vr_live && !g_eye_shared && g_eye_px[0]) {
                if (g_eye_new || g_overlay_up) {
                    compose_eyes();
                    if (g_eye_rgba[0] && g_eye_rgba[1] && (g_eye_px_tag || !hold_flat))
                        vr_submit_stereo(g_eye_rgba[0], g_eye_rgba[1], g_eye_w, g_eye_h, eye_tan(g_eye_px_tag),
                                         eye_tan_y(g_eye_px_tag), g_eye_px_tag);
                }
            } else if (g_vr_live && have_frame && !(g_stereo_on && vb - g_eye_shared_vb < 8) && !hold_flat) {
                /* the same picture, in the headset (not between shared stereo frames) */
                if (g_hi_s > 1) vr_submit_frame(g_hi_rgba, SCREEN_W * g_hi_s, SCREEN_H * g_hi_s);
                else vr_submit_frame(g_rgba, SCREEN_W, SCREEN_H);
            }
#endif
            if (window_show() != 0) InvalidateRect(g_wnd, NULL, FALSE);
        }
#ifndef PSP2I_PROD
        if ((vb % 30) == 0) {                                /* dev builds: progress in the title */
            char title[160];
            snprintf(title, sizeof title, "PSP2i (recompiled) - vblank %llu, GE cmds %llu",
                     (unsigned long long)vb, (unsigned long long)psp_ge_command_count());
            SetWindowTextA(g_wnd, title);
        }
#endif
        window_pump();
    }
#endif
    for (int i = 0; i < g_noverlay_shot; i++) {
        if (g_overlay_shot[i] != vb || !grab_frame()) continue;
        compose();
        char path[700];
        snprintf(path, sizeof path, "%s/overlay_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
            for (int k = 0; k < SCREEN_W * SCREEN_H; k++) {
                const uint8_t rgb[3] = { (uint8_t)(g_rgba[k] >> 16), (uint8_t)(g_rgba[k] >> 8), (uint8_t)g_rgba[k] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            fprintf(stderr, "overlay-shot: vblank %llu -> %s\n", (unsigned long long)vb, path);
        }
        if (g_hi_s > 1 && g_hi_valid) {                  /* the renderer's S-times picture (no overlay) */
            const int W = SCREEN_W * g_hi_s, H = SCREEN_H * g_hi_s;
            snprintf(path, sizeof path, "%s/overlay_%06llu_x%d.ppm", g_capture_dir, (unsigned long long)vb, g_hi_s);
            if ((f = fopen(path, "wb")) != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int k = 0; k < W * H; k++) {
                    const uint8_t rgb[3] = { (uint8_t)(g_hi_frame[k] >> 16), (uint8_t)(g_hi_frame[k] >> 8), (uint8_t)g_hi_frame[k] };
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
                fprintf(stderr, "overlay-shot: vblank %llu -> %s\n", (unsigned long long)vb, path);
            }
        }
#ifdef PSP2I_VR
        for (int e = 0; e < 2 && g_eye_px[0] && g_eye_px[1]; e++) {   /* stereo: each eye (no overlay) */
            snprintf(path, sizeof path, "%s/overlay_%06llu_%c.ppm", g_capture_dir, (unsigned long long)vb, e ? 'R' : 'L');
            if ((f = fopen(path, "wb")) == NULL) continue;
            fprintf(f, "P6\n%d %d\n255\n", g_eye_w, g_eye_h);
            for (int k = 0; k < g_eye_w * g_eye_h; k++) {
                const uint32_t c = g_eye_px[e][k];
                const uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            fprintf(stderr, "overlay-shot: vblank %llu -> %s\n", (unsigned long long)vb, path);
        }
#endif
    }
    if (g_capture_every && vb % g_capture_every == 0) {
        char path[700];
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
        psp_display_capture(path);
        /* PSP2I_CAPTURE_FB=ADDR: also capture a 512-wide 8888 buffer at ADDR
         * (e.g. a game's off-screen scene target) as fb_<vblank>.ppm. */
        static const char *fbenv = (const char *)1;
        if (fbenv == (const char *)1) fbenv = getenv("PSP2I_CAPTURE_FB");
        if (fbenv) {
            const uint32_t a = (uint32_t)strtoul(fbenv, NULL, 0);
            const psp_gpu_backend *be = psp_gpu_get_backend();
            if (be) be->sync_vram(a, 272u * 512u * 4u);
            snprintf(path, sizeof path, "%s/fb_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n480 272\n255\n");
                for (uint32_t y = 0; y < 272; y++)
                    for (uint32_t x = 0; x < 480; x++) {
                        uint32_t p = psp_read32(a + (y * 512u + x) * 4u);
                        uint8_t rgb[3] = { (uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16) };
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
            }
        }
    }
    if (g_seconds > 0 && psp_sched_now_us() >= (uint64_t)(g_seconds * 1e6)) psp_request_exit();
}

/* ---- report ---------------------------------------------------------------- */

#ifdef _WIN32
static void prof_report(void);
#endif
static void report(void) {
    fprintf(stderr, "\n==== psp2i report ====\n");
    fprintf(stderr, "  run time            %.2f s\n", psp_sched_now_us() / 1e6);
    report_fps();
    camera_report(stderr);
#ifdef _WIN32
    d3d11_report(stderr);
#endif
    gl_ge_report(stderr);
#ifdef PSP2I_TEXREPLACE
    texreplace_report(stderr);
#endif
#ifdef _WIN32
    prof_report();
#endif
    fprintf(stderr, "  vblanks             %llu\n", (unsigned long long)psp_sched_vblank_count());
    fprintf(stderr, "  framebuffer         0x%08X\n", psp_display_framebuffer());
    fprintf(stderr, "  GE commands         %llu\n", (unsigned long long)psp_ge_command_count());
    fprintf(stderr, "  GE vertices         %llu\n", (unsigned long long)psp_ge_vertex_count());
    fprintf(stderr, "  GE pixels written   %llu\n", (unsigned long long)psp_ge_pixels());
    fprintf(stderr, "  bytes read (sceIo)  %llu\n", (unsigned long long)psp_io_bytes_read());
    fprintf(stderr, "  audio buffers       %llu\n", (unsigned long long)psp_audio_blocks());
    fprintf(stderr, "  bad memory accesses %llu\n", (unsigned long long)psp_mem_bad_access);
    fprintf(stderr, "  VFPU traps          %llu\n", (unsigned long long)psp_vfpu_trap_count());
    fprintf(stderr, "  untranslated sites  %d\n", g_unimpl_n);
    for (int i = 0; i < g_unimpl_n && i < 40; i++)
        fprintf(stderr, "    0x%08X %-10s x%llu\n", g_unimpl[i].addr, g_unimpl[i].what,
                (unsigned long long)g_unimpl[i].hits);
    oracle_report(stderr);
    psp_hle_dump_calls(stderr, 30);
    psp_ge_dump_stats(stderr);
    psp_hle_dump_unimplemented(stderr);
    psp_sched_dump(stderr);
}

/* ---- sampling profiler (--profile) -------------------------------------------------
 *
 * A background thread suspends the game thread about every millisecond and
 * records its instruction pointer; at exit the samples are resolved to
 * function and source line through the PDB. Coarse, but it measures where the
 * time actually goes without instrumenting anything. */
#ifdef _WIN32
#define PROF_SLOTS (1 << 16)
static struct { DWORD64 ip; uint32_t n; } g_prof[PROF_SLOTS];
static volatile LONG g_prof_run;
static uint64_t g_prof_total;
static HANDLE g_prof_target;

/* Call stacks (x64 unwind data; the thread is suspended). Recompiled code
 * calls recompiled code directly, so the host stack is the game's call stack
 * too: "inclusive" counts every function on the stack once per sample (a
 * game function's whole subtree), and "outside the exe" attributes samples
 * that land in kernel waits, the D3D driver or the CRT to the nearest caller
 * in the exe -- which is what turns "ZwWaitForSingleObject 18%" into a
 * function of ours. */
#define PROF_DEPTH 96
typedef struct { DWORD64 ip; uint32_t n; } prof_slot;
static prof_slot g_prof_incl[PROF_SLOTS], g_prof_wait[PROF_SLOTS];
/* Leaf function and its caller, as (leaf ip, caller ip) pairs: who calls the
 * hot leaves (a CRT math routine says little without its caller). */
static struct { DWORD64 leaf, caller, caller2; uint32_t n; } g_prof_pair[PROF_SLOTS];

static void prof_pair(DWORD64 leaf, DWORD64 caller, DWORD64 caller2) {
    uint32_t h = (uint32_t)(((leaf >> 2) ^ (caller * 31) ^ (caller2 * 131)) * 2654435761u) & (PROF_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (g_prof_pair[h].leaf == leaf && g_prof_pair[h].caller == caller && g_prof_pair[h].caller2 == caller2) { g_prof_pair[h].n++; return; }
        if (!g_prof_pair[h].leaf) {
            g_prof_pair[h].leaf = leaf; g_prof_pair[h].caller = caller; g_prof_pair[h].caller2 = caller2;
            g_prof_pair[h].n = 1; return;
        }
    }
}
static DWORD64 g_exe_lo, g_exe_hi;

static void prof_count(DWORD64 ip, prof_slot *t) {
    uint32_t h = (uint32_t)((ip >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (t[h].ip == ip) { t[h].n++; return; }
        if (!t[h].ip) { t[h].ip = ip; t[h].n = 1; return; }
    }
}

static void prof_stack(CONTEXT *c) {
    DWORD64 seen[PROF_DEPTH];
    int nseen = 0, outside = c->Rip < g_exe_lo || c->Rip >= g_exe_hi;
    const DWORD64 leaf = c->Rip;
    DWORD64 caller1 = 0;
    for (int d = 0; d < PROF_DEPTH && c->Rip; d++) {
        const DWORD64 ip = c->Rip;
        if (ip >= g_exe_lo && ip < g_exe_hi) {
            if (outside) { prof_count(ip, g_prof_wait); outside = 0; }
            int dup = 0;
            for (int k = 0; k < nseen && !dup; k++) dup = seen[k] == ip;
            if (!dup) { seen[nseen++] = ip; prof_count(ip, g_prof_incl); }
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ip, &base, NULL);
        if (!fe) {                                 /* leaf: return address on top */
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        } else {
            void *hd = NULL;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ip, fe, c, &hd, &est, NULL);
        }
        if (d == 0 && c->Rip != ip) caller1 = c->Rip;
        if (d == 1 && caller1) prof_pair(leaf, caller1, c->Rip != ip ? c->Rip : 0);
        if (c->Rip == ip) break;
    }
}

static DWORD WINAPI prof_thread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (g_prof_run) {
        Sleep(1);
        if (!g_prof_on) continue;
        if (SuspendThread(g_prof_target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_prof_target, &ctx)) {
            DWORD64 ip = ctx.Rip;
            uint32_t h = (uint32_t)((ip >> 2) * 2654435761u) & (PROF_SLOTS - 1);
            for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof[h].ip == ip) { g_prof[h].n++; break; }
                if (!g_prof[h].ip) { g_prof[h].ip = ip; g_prof[h].n = 1; break; }
            }
            g_prof_total++;
            prof_stack(&ctx);
        }
        ResumeThread(g_prof_target);
    }
    timeEndPeriod(1);
    return 0;
}

static void prof_start(void) {
    {
        const uint8_t *b = (const uint8_t *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        g_exe_lo = (DWORD64)(uintptr_t)b;
        g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_prof_run = 1;
    CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
}

typedef struct { char name[160]; uint32_t n; } prof_row;
static int prof_cmp(const void *a, const void *b) {
    return (int)((const prof_row *)b)->n - (int)((const prof_row *)a)->n;
}

static void prof_report(void) {
    if (!g_prof_run) return;
    g_prof_run = 0;
    Sleep(5);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    static prof_row fn[8192], ln[8192];
    int nfn = 0, nln = 0;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof[i].ip) continue;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        char name[160] = "?", line[160] = "?";
        if (SymFromAddr(proc, g_prof[i].ip, &d, si)) snprintf(name, sizeof name, "%s", si->Name);
        IMAGEHLP_LINE64 L;
        DWORD dl = 0;
        memset(&L, 0, sizeof L);
        L.SizeOfStruct = sizeof L;
        if (SymGetLineFromAddr64(proc, g_prof[i].ip, &dl, &L)) {
            const char *f = strrchr(L.FileName, '\\');
            snprintf(line, sizeof line, "%s:%lu (%s)", f ? f + 1 : L.FileName, (unsigned long)L.LineNumber, name);
        } else snprintf(line, sizeof line, "%s", name);
        int k;
        for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
        if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
        if (k < 8192) fn[k].n += g_prof[i].n;
        for (k = 0; k < nln && strcmp(ln[k].name, line); k++) {}
        if (k == nln && nln < 8192) { snprintf(ln[nln].name, sizeof ln[nln].name, "%s", line); ln[nln++].n = 0; }
        if (k < 8192) ln[k].n += g_prof[i].n;
    }
    qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
    qsort(ln, (size_t)nln, sizeof ln[0], prof_cmp);
    fprintf(stderr, "  profile: %llu samples\n  top functions:\n", (unsigned long long)g_prof_total);
    const char *topenv = getenv("PSP2I_PROF_TOP");              /* list length, default 25 */
    const int top = topenv && atoi(topenv) > 0 ? atoi(topenv) : 25;
    for (int i = 0; i < nfn && i < top; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
    fprintf(stderr, "  top lines:\n");
    for (int i = 0; i < nln && i < 40; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * ln[i].n / (double)g_prof_total, ln[i].name);

    /* The stack tables, by function. */
    for (int pass = 0; pass < 2; pass++) {
        const prof_slot *t = pass ? g_prof_wait : g_prof_incl;
        nfn = 0;
        for (int i = 0; i < PROF_SLOTS; i++) {
            if (!t[i].ip) continue;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 d = 0;
            char name[160] = "?";
            if (SymFromAddr(proc, t[i].ip, &d, si)) snprintf(name, sizeof name, "%s", si->Name);
            int k;
            for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
            if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
            if (k < 8192) fn[k].n += t[i].n;
        }
        qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
        fprintf(stderr, pass ? "  outside the exe (waits, driver, CRT), by nearest caller in the exe:\n"
                             : "  inclusive (function and everything it calls):\n");
        for (int i = 0; i < nfn && i < (pass ? 25 : 3 * top); i++)
            fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
    }

    /* Leaf <- caller, by function names. */
    nfn = 0;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof_pair[i].leaf) continue;
        char names[3][160] = { "?", "?", "?" };
        for (int w = 0; w < 3; w++) {
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 d = 0;
            const DWORD64 a = w == 0 ? g_prof_pair[i].leaf : w == 1 ? g_prof_pair[i].caller : g_prof_pair[i].caller2;
            if (a && SymFromAddr(proc, a, &d, si)) snprintf(names[w], 160, "%s", si->Name);
        }
        char name[160];
        snprintf(name, sizeof name, "%.48s <- %.48s <- %.48s", names[0], names[1], names[2]);
        int k;
        for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
        if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
        if (k < 8192) fn[k].n += g_prof_pair[i].n;
    }
    qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
    fprintf(stderr, "  leaf <- caller:\n");
    for (int i = 0; i < nfn && i < 40; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
}
#endif

/* The game functions on the host stack, innermost first, as PSP addresses:
 * recompiled functions call each other as C functions, so the host stack is
 * the game's call stack. "08B35014 < 08A88584 < ...". For logs that say
 * where in the game something happens (online.c's HTTP log). */
#ifdef _WIN32
/* Symbols for these logs, without stalling the game: DbgHelp is initialised
 * and the exe's 100 MB PDB loaded on a background thread at start
 * (sym_preload); until then stacks are simply left out. Each return address
 * is symbolized once and remembered (the same few repeat endlessly), and
 * DbgHelp, which is single-threaded, is only used under g_sym_lock. Looking
 * up every frame of every HTTP / NP log line -- the PDB loading on the first
 * -- was the game-thread stutter on SEGA server traffic. */
static volatile LONG     g_sym_ready;
static CRITICAL_SECTION  g_sym_lock;

static DWORD WINAPI sym_preload(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_sym_lock);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    SymFromAddr(proc, (DWORD64)(uintptr_t)&sym_preload, &d, si);   /* loads the exe's PDB now */
    LeaveCriticalSection(&g_sym_lock);
    InterlockedExchange(&g_sym_ready, 1);
    return 0;
}

static void sym_start(void) {
    InitializeCriticalSection(&g_sym_lock);
    HANDLE t = CreateThread(NULL, 0, sym_preload, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* A return address -> 1 and *fn (a recompiled game function), 2 (the guest
 * thread's bottom: stop), 0 (anything else). Remembered per address. */
#define SYM_CACHE 8192
static struct { uintptr_t pc; uint32_t fn; uint8_t kind, used; } g_sym_cache[SYM_CACHE];

static int sym_kind(HANDLE proc, uintptr_t pc, uint32_t *fn) {
    uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) & (SYM_CACHE - 1);
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (SYM_CACHE - 1)) {
        if (!g_sym_cache[h].used) break;
        if (g_sym_cache[h].pc == pc) { *fn = g_sym_cache[h].fn; return g_sym_cache[h].kind; }
    }
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    int kind = 0;
    unsigned a = 0;
    if (SymFromAddr(proc, (DWORD64)pc, &d, si)) {
        if (!strcmp(si->Name, "thread_body")) kind = 2;
        else if (sscanf(si->Name, "psp_body_%8X", &a) == 1 || sscanf(si->Name, "psp_func_%8X", &a) == 1) kind = 1;
    }
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (SYM_CACHE - 1))
        if (!g_sym_cache[h].used) {
            g_sym_cache[h].pc = pc; g_sym_cache[h].fn = a; g_sym_cache[h].kind = (uint8_t)kind; g_sym_cache[h].used = 1;
            break;
        }
    *fn = a;
    return kind;
}

int game_stack(char *out, size_t cap, int max_frames) {
    out[0] = '\0';
    if (!InterlockedCompareExchange(&g_sym_ready, 0, 0)) return 0;   /* symbols still loading: no stack */
    HANDLE proc = GetCurrentProcess();
    void *frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, NULL);
    size_t used = 0;
    int k = 0;
    uint32_t last = 0, seen[48];
    EnterCriticalSection(&g_sym_lock);
    for (USHORT i = 0; i < n && k < max_frames; i++) {
        uint32_t a;
        const int kind = sym_kind(proc, (uintptr_t)frames[i], &a);
        if (kind == 2) break;
        if (kind == 1 && a != last) {
            /* each function once: a state machine re-entering itself would
             * otherwise fill the chain and hide its callers */
            int dup = 0;
            for (int j = 0; j < k && !dup; j++) dup = seen[j] == a;
            last = a;
            if (dup) continue;
            if (k < 48) seen[k] = a;
            used += (size_t)snprintf(out + used, used < cap ? cap - used : 0, "%s%08X", k ? " < " : "", a);
            k++;
        }
    }
    LeaveCriticalSection(&g_sym_lock);
    return k;
}

/* --watch hits in a release build: name the recompiled functions on the host
 * stack (the host stack is the game's call stack), innermost first. */
/* The recompiled functions among `frames`, innermost first, as " name name ...". */
static void stack_names(void *const *frames, USHORT n, char *line, size_t cap) {
    HANDLE proc = GetCurrentProcess();
    size_t len = 0;
    line[0] = '\0';
    if (!InterlockedCompareExchange(&g_sym_ready, 0, 0)) { snprintf(line, cap, " (symbols still loading)"); return; }
    EnterCriticalSection(&g_sym_lock);
    for (USHORT i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &d, si)) {
            if (!strncmp(si->Name, "psp_", 4) && strncmp(si->Name, "psp_write", 9) && strncmp(si->Name, "psp_read", 8) &&
                len + strlen(si->Name) + 2 < cap)
                len += (size_t)snprintf(line + len, cap - len, " %s", si->Name);
            if (!strcmp(si->Name, "thread_body")) break;
        }
    }
    LeaveCriticalSection(&g_sym_lock);
}

/* Read-watch hits (psp_mem_watch_read): each distinct call stack once, with a
 * count every 600th repeat, so a reader that runs every frame stays readable. */
static void read_watch_hit(uint32_t watched, uint32_t addr) {
    static struct { uint64_t key; uint32_t hits; } seen[64];
    static int nseen, busy;
    if (busy) return;                      /* our own psp_read32 below reads the watched word */
    void *frames[16];
    const USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    uint64_t key = watched;
    for (USHORT i = 0; i < n; i++) key = key * 1000003u ^ (uint64_t)(uintptr_t)frames[i];
    int k = 0;
    while (k < nseen && seen[k].key != key) k++;
    if (k < nseen) {
        if (++seen[k].hits % 600 == 0)
            gamelog_note("[rwatch] 0x%08X: stack #%d read %u times", watched, k, seen[k].hits);
        return;
    }
    if (nseen == 64) return;
    seen[k].key = key; seen[k].hits = 1; nseen++;
    busy = 1;
    char line[900];
    stack_names(frames, n, line, sizeof line);
    gamelog_note("[rwatch] 0x%08X read (at 0x%08X, value %u) at vblank %llu, stack #%d:%s", watched, addr,
                 psp_read32(watched), (unsigned long long)psp_sched_vblank_count(), k, line);
    busy = 0;
}

/* Unmapped accesses (psp_mem_set_bad_hook): each distinct call stack once in
 * game_log.txt, with the address, so a null-pointer read in game code shows
 * which code does it. */
static void bad_access_hit(uint32_t addr, int write, int width) {
    static struct { uint64_t key; uint32_t hits; } seen[48];
    static int nseen;
    void *frames[16];
    const USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    uint64_t key = (uint64_t)write;
    for (USHORT i = 0; i < n; i++) key = key * 1000003u ^ (uint64_t)(uintptr_t)frames[i];
    int k = 0;
    while (k < nseen && seen[k].key != key) k++;
    if (k < nseen) {
        if (++seen[k].hits % 1000 == 0) gamelog_note("[bad] stack #%d: %u accesses", k, seen[k].hits);
        return;
    }
    if (nseen == 48) return;
    seen[k].key = key; seen[k].hits = 1; nseen++;
    char line[900];
    stack_names(frames, n, line, sizeof line);
    gamelog_note("[bad] %s%d at 0x%08X at vblank %llu, stack #%d:%s", write ? "write" : "read", width * 8, addr,
                 (unsigned long long)psp_sched_vblank_count(), k, line);
}

static void watch_backtrace(uint32_t addr, uint32_t value) {
    void *frames[24];
    const USHORT n = CaptureStackBackTrace(1, 24, frames, NULL);
    char line[900];
    stack_names(frames, n, line, sizeof line);
    fprintf(stderr, "    stack:%s\n", line);
    /* also in the logs the user sends back */
    gamelog_note("[watch] write 0x%08X to 0x%08X at vblank %llu; stack:%s", value, addr,
                 (unsigned long long)psp_sched_vblank_count(), line);
}
#endif

/* ---- crash reporting ---------------------------------------------------------- */

#ifdef _WIN32
#include <dbghelp.h>

/* A host-side fault (access violation, stack overflow) is a bug in the
 * runtime or the generated code. Report where -- host function, PSP state,
 * thread table -- before the process dies, so it can be found. */
static char g_crash_path[700];          /* logs/crash_log.txt next to the exe */
static DWORD g_crash_tid;               /* the faulting thread */

/* The faulting thread's own call stack, innermost first: for a stack overflow
 * (endless recursion) the first few dozen frames show the cycle. */
static void crash_walk_stack(HANDLE proc, CONTEXT ctx) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, g_crash_tid);
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rsp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "  call stack (innermost first):\n");
    for (int i = 0; i < 60; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th ? th : GetCurrentThread(), &sf, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !sf.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const unsigned long long rva = sf.AddrPC.Offset - (DWORD64)(uintptr_t)GetModuleHandleA(NULL);
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si))
            fprintf(stderr, "    #%-2d %s+0x%llx  (exe+0x%llx)\n", i, si->Name, (unsigned long long)disp, rva);
        else
            fprintf(stderr, "    #%-2d exe+0x%llx\n", i, rva);
    }
    if (th) CloseHandle(th);
}

static void crash_report(EXCEPTION_POINTERS *ep) {
    /* The report also goes to crash_log.txt: run.bat shows stderr only in the
     * console, which closes with the game. */
    if (g_crash_path[0]) {
        fprintf(stderr, "\n==== host exception: report written to %s ====\n", g_crash_path);
        if (freopen(g_crash_path, "a", stderr)) {
            time_t now = time(NULL);
            fprintf(stderr, "\n==== crash %s", ctime(&now));
        }
    }
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "\n==== host exception 0x%08lX at %p ====\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "  %s of %p\n", er->ExceptionInformation[0] ? "write" : "read",
                (void *)er->ExceptionInformation[1]);
    HANDLE proc = GetCurrentProcess();
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "  exe base %p, fault at exe+0x%llx\n", (void *)base,
            (unsigned long long)((uintptr_t)er->ExceptionAddress - base));
    /* The game-stack logger may already have initialised the symbol handler
     * (a second SymInitialize fails): use it either way. */
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)er->ExceptionAddress, &disp, si))
            fprintf(stderr, "  in %s+0x%llx\n", si->Name, (unsigned long long)disp);
        crash_walk_stack(proc, *ep->ContextRecord);
    }
    fprintf(stderr, "  last recompiled function entered: 0x%08X\n", psp_trace_last());
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X\n",
                psp_reg_names[i], psp_cpu.r[i], psp_reg_names[i + 1], psp_cpu.r[i + 1],
                psp_reg_names[i + 2], psp_cpu.r[i + 2], psp_reg_names[i + 3], psp_cpu.r[i + 3]);
    psp_sched_dump(stderr);
    fflush(stderr);
}

/* crash_dump.dmp next to crash_log.txt: the faulting thread's stack and the
 * memory it points at, for a debugger. Written first -- heap corruption may
 * leave too little working to finish the text report. */
static void crash_minidump(EXCEPTION_POINTERS *ep) {
    if (!g_crash_path[0]) return;
    char path[720];
    snprintf(path, sizeof path, "%s", g_crash_path);
    char *s = strrchr(path, '/');
    snprintf(s ? s + 1 : path, sizeof path - (size_t)(s ? s + 1 - path : 0), "crash_dump.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = g_crash_tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                      &mei, NULL, NULL);
    CloseHandle(f);
}

static DWORD WINAPI crash_report_thread(LPVOID p) { crash_report((EXCEPTION_POINTERS *)p); return 0; }

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static int once;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    g_crash_tid = GetCurrentThreadId();
    crash_minidump(ep);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        /* No stack is left to report on (the handler itself used to die silently):
         * do it on a fresh thread while this one waits. */
        HANDLE t = CreateThread(NULL, 1u << 20, crash_report_thread, ep, 0, NULL);
        if (t) { WaitForSingleObject(t, 60000); CloseHandle(t); }
    } else {
        crash_report(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Failures the unhandled-exception filter never sees, seen in multiplayer
 * (2026-10-10, Windows event log, no crash_log.txt): heap corruption
 * (0xC0000374, raised from ntdll and then fatal) and the C runtime's fast
 * fail (0xC0000409 in ucrtbase: an invalid parameter, or abort). The first
 * reaches a vectored handler; the others call ours before failing. */
static LONG WINAPI on_vectored(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374u || code == 0xC0000409u) on_crash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_from_here(DWORD code, const char *what) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof er);
    er.ExceptionCode = code;
    er.ExceptionAddress = (void *)(uintptr_t)ctx.Rip;
    EXCEPTION_POINTERS ep = { &er, &ctx };
    fprintf(stderr, "psp2i: %s\n", what);
    on_crash(&ep);
}

static void on_invalid_parameter(const wchar_t *expr, const wchar_t *fn, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)expr; (void)fn; (void)file; (void)line; (void)r;
    crash_from_here(0xC0000417u, "the C runtime was given an invalid parameter");
}

static void on_abort_signal(int sig) {
    (void)sig;
    crash_from_here(0xC0000409u, "abort()");
}

static void crash_handlers_install(void) {
    SetUnhandledExceptionFilter(on_crash);
    AddVectoredExceptionHandler(1, on_vectored);
    _set_invalid_parameter_handler(on_invalid_parameter);
    signal(SIGABRT, on_abort_signal);
}
#endif

/* ---- sceKernelLoadExec: "open website" ------------------------------------------
 * The game replaces itself with another program. PSP2i does it for one thing:
 * disc0:/PSP_GAME/USRDIR/module/browser.prx, which opened its website in the
 * PSP browser and then loaded the game again. Here the one window shows
 * whatever runs: the page (web_overlay.cpp; website_url= in
 * psp2i_online.ini) while it is open, then the game booting again in place
 * (main's reboot loop) -- as the PSP did. The game has already torn itself
 * down when it calls this, so there is nothing to return to. */
static int g_reboot;                  /* the run ended in LoadExec: boot the game again */

static void loadexec_host(const char *path, uint32_t argsize, uint32_t argp) {
    (void)argp;
    online_log("game: sceKernelLoadExec(%s, %u argument bytes)", path, argsize);
#ifdef _WIN32
    if (g_headless || !g_wnd) return;                   /* nothing to show; the game ends */
    const size_t n = strlen(path);
    if (n >= 11 && !_stricmp(path + n - 11, "browser.prx")) {
        audio_set_master(0.0f);                         /* the game is gone; no stuck sound under the page */
        web_overlay_show(g_wnd, online_website_url(), NULL);
        if (psp_exit_requested()) return;               /* the window was closed while the page was up */
    }
    /* browser.prx's own last step (and any other program): the game again */
    g_reboot = 1;
#else
    (void)path;
#endif
}

/* The game's PS Store (psp_np_set_store): on a PSP the firmware's store, drawn
 * over the game while it stays in the PSN lobby. Here: store_url= of
 * psp2i_online.ini ({title} = the NP title ID) over the running game
 * (web_overlay_open), the game's input held back meanwhile (ctrl path). */
static int store_open(const char *title_id) {
#ifdef _WIN32
    const char *url = online_store_url();
    if (!url[0] || g_headless || !g_wnd) return -1;     /* none configured: the dialog just finishes */
    char full[1024];
    const char *t = strstr(url, "{title}");
    if (t) snprintf(full, sizeof full, "%.*s%s%s", (int)(t - url), url, title_id, t + 7);
    else snprintf(full, sizeof full, "%s", url);
    online_log("game: PS Store -> %s (over the game)", full);
    return web_overlay_open(g_wnd, full);
#else
    (void)title_id;
    return -1;
#endif
}
static int store_showing(void) { return web_overlay_poll(); }

/* ---- main ------------------------------------------------------------------ */

static void exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *s = strrchr(out, '\\');
    if (s) *s = '\0';
#else
    snprintf(out, cap, ".");
#endif
}

static int exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* The guest side of a boot: fresh memory and CPU, the EBOOT (eboot, else the
 * opened disc image's), the firmware state, the recompiled functions, and the
 * boot path module_start receives (*argp, its length *arglen). Run at start
 * and again when the game reboots in place (g_reboot); the window, renderer,
 * sound, menu and online state are the host's and stay. 0 on success. */
static const char BOOT_PATH[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";

static int boot_guest(const char *eboot, const char *iso, const char *root, const char *ms_dir,
                      module_info *mi, uint32_t *argp) {
    if (!psp_mem.ram) {
        if (psp_mem_init() != 0) { fprintf(stderr, "psp2i: psp_mem_init failed\n"); return 1; }
    } else {                                  /* a reboot: the same buffers (the host may hold pointers), cleared */
        memset(psp_mem.ram, 0, PSP_RAM_SIZE);
        memset(psp_mem.vram, 0, PSP_VRAM_SIZE);
        memset(psp_mem.scratch, 0, PSP_SCRATCH_SIZE);
    }
    psp_cpu_reset();
    if (eboot[0]) {
        if (load_elf(eboot, mi) != 0) return 1;
    } else {
        uint32_t len = 0;
        uint8_t *b = psp_io_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &len);
        if (!b) { fprintf(stderr, "psp2i: %s has no PSP_GAME/SYSDIR/EBOOT.BIN\n", iso); return 1; }
        if (load_elf_buf(b, len, "the disc image's EBOOT.BIN", mi) != 0) return 1;
    }
    printf("module '%s': entry 0x%08X, gp 0x%08X, image 0x%08X..0x%08X\n",
           mi->name, mi->entry, mi->gp, mi->load_lo, mi->load_hi);

    psp_hle_init();
    psp_io_set_root(root);
    {
        const char *e = getenv("PSP2I_VFPU_NOINLINE");     /* A/B: runtime VFPU calls only */
        if (e && *e == '1') { psp_vfpu_force_runtime(1); printf("vfpu: inline operations disabled (PSP2I_VFPU_NOINLINE=1)\n"); }
    }
    if (ms_dir[0]) { psp_io_set_ms_root(ms_dir); printf("ms:    %s\n", ms_dir); }
    psp_sysmem_set_heap(mi->load_hi, USER_PARTITION_TOP);
    psp_recomp_register();

    /* argv[0]: the path the kernel booted the module from, copied into guest
     * memory; module_start receives (length including NUL, pointer). */
    *argp = psp_sysmem_alloc(0x100, 1);
    psp_mem_write_block(*argp, BOOT_PATH, (uint32_t)sizeof BOOT_PATH);
    return 0;
}

int main(int argc, char **argv) {
    char dir[512], root[600] = "", eboot[700] = "", ms_dir[600] = "";   /* --ms: memory stick elsewhere (tests) */
    char iso[700] = "";                    /* --iso, or found: the disc image */
    int extracted = 0;                     /* --extracted: the disc folder even with an ISO about */
    uint32_t oracle_addr = 0;
    const char *renderer = NULL;            /* --renderer; else psp2i_display.ini */
    int vr_arg = -1;                        /* --vr / --no-vr; else psp2i_display.ini */
    int res_arg = -1;                       /* --resolution; else psp2i_display.ini */
    const char *texpacks_arg = NULL;        /* --texture-packs; else psp2i_display.ini */
    int oracle_all = 0;
    int want_sdl = -1;                     /* -1: default (with a window) */
    int want_audio = -1;
    int fps = 30, fps_arg = 0;
    const char *replay_path = NULL;
    int show_fps = 1;                      /* the FPS counter: shown unless --hide-fps */
    float rs_speed = 1.0f;                 /* right-stick camera sensitivity */
    int rs_arg = 0;                        /* --rs-speed given: it wins over the saved one */
    uint32_t dump_addr = 0, args_addr = 0;
    uint64_t args_flip = 0;
    const char *dump_path = NULL;
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    log_path(g_crash_path, sizeof g_crash_path, dir, "crash_log.txt");   /* logs/crash_log.txt; the dump goes beside it */
#endif
#ifdef PSP2I_PROD
    /* Community build: no command line (see prod.c) except --console, which
     * shows the console window with the engine's messages. The exe is a
     * window-only program there, so without it no console appears. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--console") && AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            SetConsoleTitleA(WINDOW_TITLE " - console");
        }
    argc = 1;
#endif

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--root") && i + 1 < argc)          snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--eboot") && i + 1 < argc)         snprintf(eboot, sizeof eboot, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--iso") && i + 1 < argc)           snprintf(iso, sizeof iso, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--extracted"))                     extracted = 1;
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc)            snprintf(ms_dir, sizeof ms_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--headless"))                      g_headless = 1;
        else if (!strcmp(argv[i], "--sdl"))                           want_sdl = 1;
        else if (!strcmp(argv[i], "--no-sdl"))                        want_sdl = 0;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)           { fps = atoi(argv[++i]); fps_arg = 1; }
        else if (!strcmp(argv[i], "--replay-input") && i + 1 < argc)  replay_path = argv[++i];
        else if (!strcmp(argv[i], "--show-fps"))                      show_fps = 1;
        else if (!strcmp(argv[i], "--rs-speed") && i + 1 < argc)      { rs_speed = (float)atof(argv[++i]); rs_arg = 1; }
        else if (!strcmp(argv[i], "--rstick") && i + 1 < argc && g_nrstick < MAX_STICK) {
            unsigned x, y; unsigned long long at = 0, len = 6;
            if (sscanf(argv[++i], "%u,%u@%llu+%llu", &x, &y, &at, &len) < 3 || x > 255 || y > 255) {
                fprintf(stderr, "bad --rstick %s (want e.g. 255,128@4000+60)\n", argv[i]); return 1;
            }
            g_rstick[g_nrstick].x = (uint8_t)x; g_rstick[g_nrstick].y = (uint8_t)y;
            g_rstick[g_nrstick].at = at; g_rstick[g_nrstick].len = len;
            g_nrstick++;
        }
        else if (!strcmp(argv[i], "--hide-fps"))                      show_fps = 0;
        else if (!strcmp(argv[i], "--overlay-shot") && i + 1 < argc && g_noverlay_shot < MAX_OVERLAY_SHOT)
            g_overlay_shot[g_noverlay_shot++] = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--menu-key") && i + 1 < argc) {
            const char *k = argv[++i];
            const int n = (k[0] == 'F' || k[0] == 'f') ? atoi(k + 1) : 0;
            /* F10 is a system key (menu bar) and F12 takes dumps. */
            if (n < 1 || n > 11 || n == 10) { fprintf(stderr, "bad --menu-key %s (want F1..F9 or F11)\n", k); return 1; }
            snprintf(g_menu_key_name, sizeof g_menu_key_name, "F%d", n);
#ifdef _WIN32
            g_menu_vk = (WPARAM)(VK_F1 + n - 1);
#endif
        }
        else if (!strcmp(argv[i], "--vr"))                            vr_arg = 1;
        else if (!strcmp(argv[i], "--no-vr"))                         vr_arg = 0;
#ifdef PSP2I_VR
        else if (!strcmp(argv[i], "--stereo"))                        g_stereo_arg = 1;
#endif
        else if (!strcmp(argv[i], "--audio"))                         want_audio = 1;
        else if (!strcmp(argv[i], "--no-audio"))                      want_audio = 0;
        else if (!strcmp(argv[i], "--log-floats") && i + 3 < argc) {
            g_logf_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_logf_count = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            g_logf_every = (uint32_t)strtoul(argv[i + 3], NULL, 0);
            if (g_logf_count > 16) g_logf_count = 16;
            i += 3;
        }
        else if (!strcmp(argv[i], "--stick") && i + 1 < argc) {
            if (add_stick(argv[++i]) != 0) { fprintf(stderr, "bad --stick %s (want e.g. 255,128@4000+60)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--dump-ram-at") && i + 2 < argc && g_nramdump < MAX_RAMDUMP) {
            g_ramdump[g_nramdump].at = strtoull(argv[i + 1], NULL, 0);
            snprintf(g_ramdump[g_nramdump].path, sizeof g_ramdump[g_nramdump].path, "%s", argv[i + 2]);
            g_nramdump++;
            i += 2;
        }
        else if (!strcmp(argv[i], "--find-word") && i + 2 < argc) {
            g_find_value = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_find_vblank = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--watch-from-flip") && i + 2 < argc) {
            g_watch_late_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_watch_late_flip = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc)      renderer = argv[++i];
        else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc)  g_render_scale_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--texture-packs") && i + 1 < argc) texpacks_arg = argv[++i];
        else if (!strcmp(argv[i], "--resolution") && i + 1 < argc) {
            res_arg = menu_res_parse(argv[++i]);
            if (res_arg < 0) { fprintf(stderr, "bad --resolution %s (want e.g. 1920x1080 or fullscreen)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (add_press(argv[++i]) != 0) { fprintf(stderr, "bad --press %s (want e.g. cross@600+6)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--oracle-all"))                    oracle_all = 1;
#ifdef _WIN32
        else if (!strcmp(argv[i], "--profile"))                       prof_start();
        else if (!strcmp(argv[i], "--profile-from") && i + 1 < argc)  g_prof_from = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--profile-to") && i + 1 < argc)    g_prof_to = strtoull(argv[++i], NULL, 0);
#endif
        else if (!strcmp(argv[i], "--label-ring"))                    ring_arm();
        else if (!strcmp(argv[i], "--dump-on-bad") && i + 1 < argc)   dump_on_bad_access(argv[++i]);
        else if (!strcmp(argv[i], "--ge-dump") && i + 1 < argc)      g_ge_dump_vb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump-at") && i + 2 < argc)       { dump_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0); dump_path = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--args-from-flip") && i + 2 < argc) {
            args_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            args_flip = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--oracle") && i + 1 < argc)        oracle_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc)         psp_mem_watch_write((uint32_t)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)       g_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--capture-every") && i + 1 < argc) g_capture_every = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-dir") && i + 1 < argc)   snprintf(g_capture_dir, sizeof g_capture_dir, "%s", argv[++i]);
        else { fprintf(stderr, "usage: %s [--root DIR] [--iso FILE | --extracted] [--eboot FILE] [--headless] [--seconds N] "
                               "[--capture-every N] [--capture-dir DIR]\n", argv[0]); return 2; }
    }
#ifdef PSP2I_PROD
    if (prod_check(dir, root, sizeof root, eboot, sizeof eboot, iso, sizeof iso) != 0) return 1;
#endif
    if (!root[0]) {
        snprintf(root, sizeof root, "%s/GameData", dir);
        if (!dir_exists(root)) snprintf(root, sizeof root, "GameData");
    }
    if (!iso[0] && !extracted) find_iso(dir, root, iso, sizeof iso);
    if (extracted) iso[0] = '\0';
    if (!eboot[0]) {
        snprintf(eboot, sizeof eboot, "%s/EBOOT.BIN", dir);
        if (!exists(eboot)) {
            if (iso[0]) eboot[0] = '\0';                     /* the image's own */
            else snprintf(eboot, sizeof eboot, "%s/disc/PSP_GAME/SYSDIR/EBOOT.BIN", root);
        }
    }
    if (g_capture_every || g_noverlay_shot) host_mkdir(g_capture_dir);
    {
        char ms[700];
        if (ms_dir[0]) {
            host_mkdir(ms_dir);
            snprintf(ms, sizeof ms, "%s/PSP", ms_dir);          host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/PSP/SAVEDATA", ms_dir); host_mkdir(ms);
        } else {
            snprintf(ms, sizeof ms, "%s/ms", root);          host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/ms/PSP", root);      host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/ms/PSP/SAVEDATA", root); host_mkdir(ms);
        }
    }

#ifdef _WIN32
    crash_handlers_install();
    /* The scheduler sleeps until the next vblank or timeout with Sleep(ms - 1)
     * and spins the rest (threadman.c host_sleep_us), which assumes the 1 ms
     * timer resolution. At Windows' default (15.6 ms) one Sleep(1) can take a
     * whole vblank: frames that finished in time were shown a vblank late,
     * costing about 20 fps in the lobby at 60 fps. Only --profile set this
     * before, which is why profiled runs looked faster. */
    timeBeginPeriod(1);
    sym_start();                          /* symbols for the logs' game stacks, loaded in the background */
#ifndef PSP2I_PROD                        /* test hooks (--watch, the HP probe's read watch, bad accesses) */
    psp_mem_set_watch_hook(watch_backtrace);
    psp_mem_set_read_watch_hook(read_watch_hit);
    psp_mem_set_bad_hook(bad_access_hit);
#endif
    psp_set_loadexec_hook(loadexec_host);
    psp_np_set_store(store_open, store_showing);
#endif
    printf("root:  %s\ndisc:  %s\neboot: %s\n", root, iso[0] ? iso : "extracted (root/disc)",
           eboot[0] ? eboot : "from the disc image");

    module_info mi;
    if (iso[0] && psp_io_open_disc_image(iso) != 0) {
        fprintf(stderr, "psp2i: %s is not a PSP disc image (.iso)\n", iso);
        return 1;
    }
    uint32_t argp = 0;
    if (boot_guest(eboot, iso, root, ms_dir, &mi, &argp) != 0) return 1;
    if (dump_addr) dump_arm(dump_addr, dump_path);
    if (args_addr) args_arm(args_addr, args_flip);   /* entry hooks fire in trace builds only */
    if (oracle_addr || oracle_all) {
#ifndef PSPRECOMP_TRACE
        fprintf(stderr, "psp2i: --oracle needs a trace build (-DPSP2I_TRACE=ON); ignored\n");
#else
        if (oracle_addr) oracle_arm(oracle_addr, mi.stub_lo, mi.stub_hi);
        if (oracle_all)  oracle_arm_sweep(mi.stub_lo, mi.stub_hi);
#endif
    }

    snprintf(g_display_ini, sizeof g_display_ini, "%s/psp2i_display.ini", dir);
    int res = display_load();
    if (res_arg >= 0) res = res_arg;
    if (res < 0) res = RES_DEFAULT;
    if (res != RES_FULLSCREEN) g_last_windowed = res;
    /* The renderer: --renderer for this run, else the saved choice. GPU
     * rendering by default; the software rasterizer is the fallback and the
     * reference (--renderer software). */
    g_renderer = renderer ? menu_ren_parse(renderer) : g_ren_saved;
    if (renderer && !strcmp(renderer, "software")) g_renderer = REN_SOFTWARE;
    if (g_renderer < 0) { fprintf(stderr, "psp2i: unknown renderer '%s'; using d3d11\n", renderer); g_renderer = REN_D3D11; }
    if (g_renderer == REN_GL && (gl_platform_init() != 0 || gl_ge_init() != 0)) {
        fprintf(stderr, "psp2i: OpenGL unavailable; falling back to Direct3D 11\n");
        g_renderer = REN_D3D11;
    }
#ifdef _WIN32
    if (!g_headless) window_open(res);
    if (g_renderer == REN_GL && g_wnd) {
        g_gl_window = gl_platform_attach(g_wnd) == 0;
        if (!g_gl_window) present_init(g_wnd);
    }
    if (g_renderer == REN_D3D11) {
        int sc = g_render_scale_arg ? g_render_scale_arg : g_render_scale;
        sc = sc < 1 ? 1 : sc > 6 ? 6 : sc;                 /* 5 and 6: for VR (the eyes cover the headset's wide view) */
        d3d11_set_scale(sc);
#ifdef PSP2I_VR
        /* VR head tracking: the eyes see vr_fov_scale times the game's view
         * across but vr_fov_scale_y times up and down, so the same rows
         * would make pixels about 1.7x taller than wide in the headset (the
         * blur and pixelation up and down). Rows for the same pixels per
         * degree both ways. The window and VRAM still get S x S. */
        if (sc > 1 && (vr_arg >= 0 ? vr_arg : g_vr_on) && g_vr_stereo && g_vr_head) {
            const float tx = 1.0f / PSP2I_FIELD_PROJ_X, ty = tx * (float)SCREEN_H / (float)SCREEN_W;
            const float fx = g_vr_fov > 1.0f ? g_vr_fov : 1.0f, fy = g_vr_fov_y > 1.0f ? g_vr_fov_y : 1.0f;
            const float k = ((float)SCREEN_W / (float)SCREEN_H) * atanf(fy * ty) / atanf(fx * tx);
            int sy = (int)lroundf((float)sc * k);
            sy = sy < sc ? sc : sy > 12 ? 12 : sy;
            d3d11_set_scale_y(sy);
            g_hi_sy = sy;
        }
        if ((vr_arg >= 0 ? vr_arg : g_vr_on) && g_vr_stereo) {
            d3d11_set_quality(g_vr_aniso, g_vr_sharpen);
            fprintf(stderr, "d3d11: VR image quality: texture filtering %s, sharpening %.2f\n",
                    g_vr_aniso ? "anisotropic with mipmaps" : "as the game's", (double)g_vr_sharpen);
        }
#endif
        if (d3d11_init() != 0) {
            fprintf(stderr, "psp2i: falling back to the software renderer\n");
            g_renderer = REN_SOFTWARE;
        } else if (sc > 1) {
            g_hi_frame = (uint32_t *)calloc((size_t)SCREEN_W * sc * SCREEN_H * sc, 4);
            g_hi_rgba = (uint32_t *)calloc((size_t)SCREEN_W * sc * SCREEN_H * sc, 4);
            if (g_hi_frame && g_hi_rgba) g_hi_s = sc;
            fprintf(stderr, "d3d11: internal resolution %dx (%dx%d)\n", sc, SCREEN_W * sc, SCREEN_H * sc);
            if (g_hi_sy > sc)
                fprintf(stderr, "d3d11: VR eye pictures %dx%d (%dx down, for square pixels in the headset)\n",
                        SCREEN_W * sc, SCREEN_H * g_hi_sy, g_hi_sy);
        }
    }
#endif
    fprintf(stderr, "renderer: %s\n", g_renderer == REN_GL ? "OpenGL" : g_renderer == REN_D3D11 ? "Direct3D 11" : "software");
    if (want_sdl == 1 || (want_sdl < 0 && !g_headless)) g_sdl_on = input_sdl_init() == 0;
    audio_init(want_audio == 1 || (want_audio < 0 && !g_headless));
    atrac_at3_init(dir);           /* ATRAC music (psp2i_atrac.dll); silent without it */
    h264_openh264_init(dir, g_openh264);   /* movies; black (with sound) until it is ready */
    {
        const char *packs = texpacks_arg ? texpacks_arg : g_texpacks;
#ifdef PSP2I_TEXREPLACE
        texreplace_init(packs, dir);       /* none listed: off, textures drawn as before */
#else
        if (*packs) fprintf(stderr, "texreplace: this build has no PNG decoder (stb_image); texture_packs ignored\n");
#endif
    }
    if (replay_path) {                         /* a recorded route: its fps unless --fps */
        const int rf = replay_load(replay_path);
        if (rf < 0) return 1;
        if (rf > 0 && !fps_arg) fps = rf;
    }
    {
        const char *rp = getenv("PSP2I_INPUT_RECORD");
        if (rp && *rp && !replay_path) rec_open(rp, fps);
    }
    menu_init(&g_menu, fps, show_fps);
    g_menu.res = res;
    g_menu.renderer = g_ren_saved;
    g_menu.renderer_now = g_renderer;
    for (int k = 0; k < VOL_N; k++) g_menu.vol[k] = g_vol_saved[k];
    g_menu.att = g_att_saved;
    g_menu.swap_oc = g_swap_oc;
    input_sdl_set_swap_circle_cross(g_swap_oc);
    audio_apply();
    {   /* the game's text font: the saved choice, else Windows' own */
        char fonts_dir[700], pgf_dir[700];
        snprintf(fonts_dir, sizeof fonts_dir, "%s/fonts", dir);
        snprintf(pgf_dir, sizeof pgf_dir, "%s/flash/font", root);
#ifdef _WIN32
        CreateDirectoryA(fonts_dir, NULL);     /* where extra fonts go */
#endif
        g_menu.font_n = font_win_init(fonts_dir, pgf_dir);
        int fi = font_win_find(g_font_key);
        if (fi < 0) { fprintf(stderr, "font: \"%s\" not found; using the system font\n", g_font_key); fi = 0; }
        g_menu.font = font_win_select(fi);
        snprintf(g_menu.font_name, sizeof g_menu.font_name, "%s", font_win_name(g_menu.font));
    }
    fps_meter_init(&g_fpsm);
    framerate_init(fps);
    menu_set_rs_speed(&g_menu, !rs_arg && g_rs_saved > 0.0f ? g_rs_saved : rs_speed);
    g_menu.rs_on = g_rs_on;
    g_menu.test_ticks = g_test_ticks;
    g_menu.test_logic = g_test_logic;
#ifdef PSP2I_PROD
    g_menu.hidden |= MENU_BIT(MENU_TESTS);              /* the frame-rate tests are for development builds */
    g_test_ticks = g_test_logic = 0;
#endif
    framerate_set_tests(g_test_ticks, g_test_logic);
    camera_init(menu_rs_speed(&g_menu));
#ifdef PSP2I_VR
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", dir);
    if (vr_arg >= 0) g_vr_on = vr_arg;                  /* for this run; the menu saves its own choice */
    g_menu.hidden &= ~MENU_BIT(MENU_VR);
    g_menu.vr_on = g_vr_on;
    g_menu.vr_dist_dm = g_vr_dist_dm;
    g_menu.vr_width_dm = g_vr_width_dm;
    psp_hook_set(FN_FRUSTUM_PLANES, hook_frustum_planes);   /* a no-op unless head tracking is on */
    psp_hook_set(FN_APPLY_CAMERA, hook_apply_camera);       /* a no-op unless first person is on */
    psp_hook_set(FN_SET_PROJECTION, hook_set_projection);   /* a no-op unless first person is on */
#ifndef PSP2I_PROD
    psp_hook_set(FN_NODE_VIS, hook_node_vis);               /* counts only */
#endif
    {
        static const uint32_t CULL_TESTS[] = { 0x08D7C934u, 0x08D7CAECu, 0x08D7CD88u, 0x08D7CE6Cu, 0x08D7CF90u };
        for (int i = 0; i < (int)(sizeof CULL_TESTS / sizeof CULL_TESTS[0]); i++) psp_hook_set(CULL_TESTS[i], hook_cull_test);
    }
#ifndef PSP2I_PROD
    {   /* PSP2I_VR_FAKE_HEAD=yaw,pitch (degrees; with --stereo): the eyes drawn from a turned head, no headset */
        const char *e = getenv("PSP2I_VR_FAKE_HEAD");
        if (e && sscanf(e, "%f,%f", &g_fake_yaw, &g_fake_pitch) >= 1) {
            g_fake_head = 1;
            fprintf(stderr, "vr: fake head turn: yaw %.1f, pitch %.1f degrees\n", (double)g_fake_yaw, (double)g_fake_pitch);
        }
        const char *se = getenv("PSP2I_VR_SHOT_EVERY");          /* seconds between VR captures */
        if (se && atof(se) > 0) {
            g_vrshot_every = (uint64_t)(atof(se) * 60.0);
            fprintf(stderr, "vr: a capture every %.0f s while stereo is on (%s/vrshot_*)\n", atof(se), g_capture_dir);
        }
    }
#endif
    vr_apply();
#else
    (void)vr_arg;
#endif
    /* PSP savedata encryption: saves interchangeable with a PSP / PPSSPP */
    static const psp_savedata_crypto SAVE_CRYPTO = { savecrypt_decrypt, savecrypt_encrypt, savecrypt_sfo_hash };
    psp_savedata_set_crypto(&SAVE_CRYPTO);
    online_init(dir, &g_login);
    gamelog_init(dir);
    functrace_init(dir);
#ifndef PSP2I_PROD
    rateaudit_init(dir);                  /* the vblank-use census, and F7's frame-rate audit */
    {
        char probe[700];
        log_path(probe, sizeof probe, dir, "vr_probe.txt");
        vr_probe_init(probe);             /* F8: the camera and the characters, for the VR work */
    }
#endif
    psp_sched_set_vblank_hook(on_vblank);

    int rc;
    for (;;) {
        printf("starting module_start at 0x%08X\n", mi.entry);
        fflush(stdout);
        rc = psp_sched_run(mi.entry, (uint32_t)sizeof BOOT_PATH, argp, 0x20, 0x40000, mi.gp);
        if (!g_reboot || g_close_at) break;
        /* LoadExec (the website): the game again, in the same window */
        g_reboot = 0;
        online_log("game: booting the game again (after the website)");
        if (boot_guest(eboot, iso, root, ms_dir, &mi, &argp) != 0) { rc = 1; break; }
        audio_apply();                          /* the volume muted while the page was up */
    }

#ifdef _WIN32
    /* Closing: how long each step took (a slow one shows in online_log.txt). */
    const ULONGLONG t_stop = GetTickCount64();
    if (g_close_at) online_log("exit: window closed; the game stopped after %llu ms", t_stop - g_close_at);
#endif
    report();
#ifdef _WIN32
    timeEndPeriod(1);
    const ULONGLONG t_report = GetTickCount64();
#endif
    /* Sound first: input_sdl_shutdown ends with SDL_Quit, which frees the
     * audio streams too; destroying them after that freed them twice
     * (heap corruption 0xC0000374 on nearly every exit, and the crash
     * dump it wrote was the pause after closing the window). */
#ifdef PSP2I_VR
    if (g_vr_live) vr_stop();
#endif
    audio_shutdown();
    if (g_sdl_on) input_sdl_shutdown();
#ifdef _WIN32
    const ULONGLONG t_devices = GetTickCount64();
#endif
    psp_mem_free();
#ifdef _WIN32
    if (g_close_at)
        online_log("exit: report %llu ms, controllers and sound %llu ms, memory %llu ms",
                   t_report - t_stop, t_devices - t_report, GetTickCount64() - t_devices);
    /* What is left (Windows unloading the libraries, their threads) only
     * delays the process ending; the logs are written, so end it here. */
    fflush(NULL);
    if (g_close_at) TerminateProcess(GetCurrentProcess(), (UINT)rc);
#endif
    return rc;
}
