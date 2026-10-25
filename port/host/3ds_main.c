/* PSP2i on New 3DS: the host (3DS_PLAN.md).
 *
 * T1 (compile and fit): this links the whole game -- the psprecomp runtime
 * and every recompiled function, through psp_recomp_register -- and checks
 * that it fits: what the code left of the heap, then what the guest RAM
 * (psp_mem_init) and the dispatch tables (psp_recomp_register) take. It
 * doesn't start the game yet; that is T2 (files, input, the scheduler).
 * Results go to both screens and sdmc:/psp2i/report_game.txt. */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

void psp_recomp_register(void);          /* the generated code (port/gen) */

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

/* ---- the T1 fit check ------------------------------------------------------- */

extern u32 __ctru_heap_size, __ctru_linear_heap_size;

static PrintConsole top, bottom;
static FILE *report;

static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    consoleSelect(&bottom);
    printf("%s\n", line);
    fprintf(stderr, "3ds: %s\n", line);
    if (report) { fprintf(report, "%s\n", line); fflush(report); }
}

/* Heap in use so far, in KiB (newlib's allocator; the heap itself is
 * __ctru_heap_size, set at start from what the code left). */
static unsigned long used_kib(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned long)(mi.uordblks >> 10);
}

/* The largest single block malloc can still give, in KiB (binary search).
 * The pointer goes through a volatile: GCC drops a malloc+free pair whose
 * result isn't used and treats it as successful (the first console run read
 * the whole heap as free with 38 MB of it in use). */
static unsigned long largest_kib(void)
{
    size_t lo = 0, hi = __ctru_heap_size;
    while (hi - lo > 64 * 1024) {
        size_t mid = lo + (hi - lo) / 2;
        void *volatile p = malloc(mid);
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    return (unsigned long)(lo >> 10);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    osSetSpeedupEnable(true);

    mkdir("sdmc:/psp2i", 0777);
    report = fopen("sdmc:/psp2i/report_game.txt", "w");

    consoleSelect(&top);
    printf("\x1b[2;2HPSP2i Recomp for New 3DS (T1: fit check)\n");
    printf("\x1b[28;2HSTART: exit\n");

    say("PSP2i 3DS T1 fit check (%s)", envIsHomebrew() ? "3dsx" : "cia");
    say("app_mem=%lu KiB", (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10));
    say("heap=%lu KiB linear=%lu KiB (after the code)",
        (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(__ctru_linear_heap_size >> 10));
    say("start: used %lu KiB, largest block %lu KiB", used_kib(), largest_kib());

    if (psp_mem_init() != 0) {
        say("psp_mem_init FAILED (guest RAM 32 MB + VRAM 2 MB)");
    } else {
        psp_cpu_reset();
        say("guest RAM: used %lu KiB, largest block %lu KiB", used_kib(), largest_kib());
        u64 t0 = svcGetSystemTick();
        psp_recomp_register();
        u64 dt = svcGetSystemTick() - t0;
        say("register: %.0f ms; used %lu KiB, largest block %lu KiB",
            dt * 1000.0 / SYSCLOCK_ARM11, used_kib(), largest_kib());
        /* heap - used is the budget; "largest block" checks it on the console
         * (Azahar doesn't enforce the heap's end, so it reads the whole heap) */
        say("free for caches, textures, audio: %lu KiB heap (largest %lu) + %lu KiB linear",
            (unsigned long)(__ctru_heap_size >> 10) - used_kib(), largest_kib(),
            (unsigned long)(linearSpaceFree() >> 10));
    }
    if (report) { fclose(report); report = NULL; }

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    psp_mem_free();
    gfxExit();
    return 0;
}
