/* PSP2i 3DS bring-up check (T0).
 *
 * Not the game: a small app that proves the toolchain, the .3dsx and .cia
 * packaging and both screens work, and measures what T1 has to decide on:
 *   - how much APPLICATION memory this build actually gets (the game needs
 *     roughly 100-135 MB: 3DS_PLAN.md section 2), as .3dsx and as .cia;
 *   - whether the New 3DS speedup (804 MHz + L2) took;
 *   - whether a thread can run on core 1 and on core 2.
 * The same lines go to the screens, to svcOutputDebugString (Azahar's log)
 * and to sdmc:/psp2i/report.txt (read back with ftpd). */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
    svcOutputDebugString(line, strlen(line));
    if (report) { fprintf(report, "%s\n", line); fflush(report); }
}

static volatile int core_ran[4];

static void core_probe(void *arg)
{
    int core = (int)(intptr_t)arg;
    s32 actual = svcGetProcessorID();
    core_ran[core] = 1 + actual;   /* 0 = never ran */
}

/* Starts a short thread on `core`; returns the core it ran on, or -1. */
static int try_core(int core)
{
    Thread t = threadCreate(core_probe, (void *)(intptr_t)core, 4096, 0x30, core, false);
    if (!t) return -1;
    threadJoin(t, 1000000000ULL);
    threadFree(t);
    return core_ran[core] ? core_ran[core] - 1 : -1;
}

/* The clock, measured: a dependent loop of known length against the system
 * tick (always SYSCLOCK_ARM11, 268 MHz), since osSetSpeedupEnable fails
 * silently when ptm:sysm can't be opened. Each iteration is a subs + bne,
 * about 2 cycles on the ARM11 once in cache. */
static int cpu_mhz(void)
{
    const u32 n = 20000000;
    u32 i = n;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(i) : : "cc");
    u64 dt = svcGetSystemTick() - t0;
    return (int)((2.0 * n * SYSCLOCK_ARM11) / ((double)dt * 1e6) + 0.5);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    mkdir("sdmc:/psp2i", 0777);
    report = fopen("sdmc:/psp2i/report.txt", "w");

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);

    /* core 1 needs a CPU time limit from APT first; core 2 needs the
     * exheader flag (the .cia's CanAccessCore2), so a .3dsx should fail it */
    APT_SetAppCpuTimeLimit(30);

    consoleSelect(&top);
    printf("\x1b[2;2HPSP2i 3DS bring-up (T0)\n");
    printf("\x1b[4;2HTop screen: the game will go here.\n");
    printf("\x1b[5;2HBottom screen: the HUD will go here.\n");
    printf("\x1b[8;2H3D slider:\n");
    printf("\x1b[28;2HSTART: exit\n");

    say("PSP2i 3DS bring-up");
    say("new3ds=%d envhb=%d", n3ds, envIsHomebrew());
    say("app_mem=%lu KiB (sys %lu, base %lu)",
        (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
        (unsigned long)(osGetMemRegionSize(MEMREGION_SYSTEM) >> 10),
        (unsigned long)(osGetMemRegionSize(MEMREGION_BASE) >> 10));
    /* what libctru took for the heaps at start: the most the game can malloc
     * (osGetMemRegionFree read 4 GB in Azahar, so it isn't used) */
    say("heap=%lu KiB linear=%lu KiB (linear free %lu KiB)",
        (unsigned long)(__ctru_heap_size >> 10),
        (unsigned long)(__ctru_linear_heap_size >> 10),
        (unsigned long)(linearSpaceFree() >> 10));

    say("cpu ~%d MHz (268 = no speedup, 804 = speedup)", cpu_mhz());
    say("core1=%d core2=%d", try_core(1), try_core(2));
    say("report: sdmc:/psp2i/report.txt");
    if (report) { fclose(report); report = NULL; }

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        consoleSelect(&top);
        printf("\x1b[8;13H%3d%%  ", (int)(osGet3DSliderState() * 100.0f + 0.5f));
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
