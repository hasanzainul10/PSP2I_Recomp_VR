/* PSP2i 3DS bring-up check (T0).
 *
 * Not the game: a small app that proves the toolchain, the .3dsx and .cia
 * packaging and both screens work, and measures what T1 has to decide on:
 *   - how much APPLICATION memory this build actually gets (the game needs
 *     roughly 100-135 MB: 3DS_PLAN.md section 2), as .3dsx and as .cia;
 *   - whether the New 3DS speedup (804 MHz + L2) took;
 *   - how much work threads on core 1 and core 2 get done next to core 0.
 * The same lines go to the screens, to svcOutputDebugString (Azahar's log)
 * and to sdmc:/psp2i/report_3dsx.txt or report_cia.txt (read with ftpd). */
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

/* ---- cores ------------------------------------------------------------------
 * What matters for the game is not which core number a thread reports (the
 * first run on the console said 2 for a thread asked onto core 1) but how
 * much work it gets done while core 0 is busy too. So: count loop passes on
 * core 0 alone, then on core 0 and a worker on core N at the same time.
 *   worker/alone ~1.0  a parallel core of its own (core 2 expected)
 *   worker/alone ~0.3  core 1 under APT_SetAppCpuTimeLimit(30)
 *   worker/alone ~0    the worker never got a core of its own
 * main/alone stays ~1.0 when the worker isn't taking time from core 0. */

#define SPIN_TICKS (SYSCLOCK_ARM11 / 4)   /* 250 ms */

static volatile int spin_stop, spin_started;
static volatile u32 spin_count;
static volatile s32 spin_cpu = -1;

/* The work both sides count, the same per pass on either side: 4096
 * increments of a counter in memory, then one check. */
static void spin_block(volatile u32 *c)
{
    for (int k = 0; k < 4096; k++) (*c)++;
}

static void spin_worker(void *arg)
{
    (void)arg;
    spin_cpu = svcGetProcessorID();
    spin_started = 1;
    while (!spin_stop) spin_block(&spin_count);
}

static u32 spin_main(void)
{
    volatile u32 n = 0;
    u64 end = svcGetSystemTick() + SPIN_TICKS;
    while (svcGetSystemTick() < end) spin_block(&n);
    return n;
}

/* One worker on `core` while core 0 spins. Prints a report line. */
static void core_test(int core, u32 alone)
{
    spin_stop = spin_started = 0;
    spin_count = 0;
    spin_cpu = -1;
    Thread t = threadCreate(spin_worker, NULL, 4096, 0x30, core, false);
    if (!t) {
        say("core%d: threadCreate failed", core);
        return;
    }
    /* sleeping yields core 0, so a worker that landed there can start too */
    for (int i = 0; i < 100 && !spin_started; i++) svcSleepThread(1000000);
    if (!spin_started) {
        say("core%d: created, didn't start in 100 ms", core);
        spin_stop = 1;
        threadJoin(t, U64_MAX);
        threadFree(t);
        return;
    }
    u32 base = spin_count;
    u32 m = spin_main();
    u32 w = spin_count - base;
    spin_stop = 1;
    threadJoin(t, U64_MAX);
    threadFree(t);
    say("core%d: on cpu %ld, worker/alone %.2f, main/alone %.2f",
        core, (long)spin_cpu, (double)w / alone, (double)m / alone);
}

/* ---- clock ------------------------------------------------------------------
 * osSetSpeedupEnable fails silently when ptm:sysm can't be opened, so time
 * the same loop with the speedup off and on. The system tick always counts
 * at SYSCLOCK_ARM11 (268 MHz); a ratio of ~3 means 804 MHz works. (The first
 * console run assumed 2 cycles per pass and read 321 MHz: the cycles per
 * pass aren't known, the ratio doesn't need them.) */
static u64 loop_ticks(void)
{
    u32 i = 20000000;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(i) : : "cc");
    return svcGetSystemTick() - t0;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    /* one report per format, so a .3dsx run and a .cia run don't overwrite
     * each other (envIsHomebrew: started by the Homebrew Launcher) */
    const char *report_path = envIsHomebrew() ? "sdmc:/psp2i/report_3dsx.txt"
                                              : "sdmc:/psp2i/report_cia.txt";
    mkdir("sdmc:/psp2i", 0777);
    report = fopen(report_path, "w");

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(false);
    svcSleepThread(10000000);
    u64 slow = loop_ticks();
    osSetSpeedupEnable(true);
    svcSleepThread(10000000);
    u64 fast = loop_ticks();

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

    say("clock: speedup off %llu ticks, on %llu, ratio %.2f (3 = 804 MHz)",
        (unsigned long long)slow, (unsigned long long)fast, (double)slow / fast);

    u32 alone = spin_main();
    say("core0 alone: %lu passes in 250 ms", (unsigned long)alone);
    core_test(1, alone);
    core_test(2, alone);
    say("report: %s", report_path);
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
