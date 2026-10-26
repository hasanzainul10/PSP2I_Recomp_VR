/* The on-console profiler (PSP2I_PROFILE builds; 3DS_PROGRESS.md T4): which
 * recompiled game functions the field's ~100 ms of game code a frame goes to.
 *
 * port/3ds/prof_game.h keeps psp2i_prof_fn on the function the game's thread
 * is in (a firmware call, callbacks included: its import thunk). A thread on core 2, which nothing
 * else uses, reads it 1000 times a second into a ring; the game's thread
 * drains the ring once a second (time_report) and every 10 s appends that
 * window's top functions to sdmc:/psp2i/profile.txt, with the window's frames
 * and game ms/s so a window can be matched to a scene, and rewrites
 * profile_total.txt with the whole run's. Addresses are PSP addresses: a body
 * entered at a second entry point counts under that address. */
#ifdef PSP2I_PROFILE

#include "3ds_prof.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>

#define BASE "sdmc:/psp2i"

volatile uint32_t psp2i_prof_fn, psp2i_prof_imp;   /* function; firmware call in progress */

#define RING 8192                       /* 8 s of samples: drained every second */
static uint32_t g_ring[RING];
static volatile uint32_t g_head;        /* written by the sampler only */
static uint32_t g_tail;                 /* the game's thread only */

#define NSLOT 32768                     /* the game has 88,317 functions; few run */
typedef struct { uint32_t addr, total, win; } slot;
static slot g_slot[NSLOT];
static uint32_t g_used, g_lost;

static void sampler(void *arg) {
    (void)arg;
    for (;;) {
        svcSleepThread(1000000);        /* 1 ms */
        const uint32_t h = g_head;
        const uint32_t imp = psp2i_prof_imp;
        g_ring[h & (RING - 1)] = imp ? imp : psp2i_prof_fn;
        __sync_synchronize();           /* the sample before the index */
        g_head = h + 1;
    }
}

int psp2i_prof_start(void) {
    /* above the game's priority, so a sample isn't late; alone on core 2 */
    Thread t = threadCreate(sampler, NULL, 4096, 0x18, 2, true);
    fprintf(stderr, "prof: sampler %s\n", t ? "on core 2, 1 kHz" : "could not start");
    return t ? 0 : -1;
}

static void count(uint32_t a) {
    unsigned h = (unsigned)((a >> 2) * 2654435761u) & (NSLOT - 1);
    while (g_slot[h].addr != a) {
        if (!g_slot[h].addr) {
            if (g_used >= NSLOT * 3 / 4) { g_lost++; return; }
            g_slot[h].addr = a;
            g_used++;
            break;
        }
        h = (h + 1) & (NSLOT - 1);
    }
    g_slot[h].total++;
    g_slot[h].win++;
}

static int by_win(const void *x, const void *y) {
    const slot *a = *(slot *const *)x, *b = *(slot *const *)y;
    return a->win < b->win ? 1 : a->win > b->win ? -1 : 0;
}
static int by_total(const void *x, const void *y) {
    const slot *a = *(slot *const *)x, *b = *(slot *const *)y;
    return a->total < b->total ? 1 : a->total > b->total ? -1 : 0;
}

static slot *g_sorted[NSLOT];

void psp2i_prof_second(double game_ms, uint64_t frames, double draws_per_frame) {
    static int secs, window;
    static uint64_t win_frames;
    static double win_game, win_draws;
    const uint32_t h = g_head;
    __sync_synchronize();
    if (h - g_tail > RING) g_tail = h - RING;      /* (not expected: drained every second) */
    for (; g_tail != h; g_tail++) count(g_ring[g_tail & (RING - 1)]);
    win_frames += frames;
    win_game += game_ms;
    win_draws += draws_per_frame;
    if (++secs % 10) return;

    int n = 0;
    uint32_t win_n = 0, total_n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (g_slot[i].addr) {
            g_sorted[n++] = &g_slot[i];
            win_n += g_slot[i].win;
            total_n += g_slot[i].total;
        }
    qsort(g_sorted, (size_t)n, sizeof g_sorted[0], by_win);
    FILE *f = fopen(BASE "/profile.txt", "a");
    if (f) {
        fprintf(f, "window %d (s %d-%d): %lu samples, %llu frames, game %.0f ms/s, %.0f draws/frame\n", window,
                secs - 10, secs, (unsigned long)win_n, (unsigned long long)win_frames, win_game / 10.0,
                win_draws / 10.0);
        for (int i = 0; i < n && i < 40 && g_sorted[i]->win; i++)
            fprintf(f, "  %5.1f%% %4lu  %08lX\n", 100.0 * g_sorted[i]->win / (win_n ? win_n : 1),
                    (unsigned long)g_sorted[i]->win, (unsigned long)g_sorted[i]->addr);
        fclose(f);
    }
    fprintf(stderr, "prof: window %d written (%lu samples, %d functions, %lu lost)\n", window,
            (unsigned long)win_n, n, (unsigned long)g_lost);
    qsort(g_sorted, (size_t)n, sizeof g_sorted[0], by_total);
    f = fopen(BASE "/profile_total.txt", "w");
    if (f) {
        fprintf(f, "whole run, %d s: %lu samples, %d functions\n", secs, (unsigned long)total_n, n);
        for (int i = 0; i < n && i < 1000; i++)
            fprintf(f, "%5.2f%% %6lu  %08lX\n", 100.0 * g_sorted[i]->total / (total_n ? total_n : 1),
                    (unsigned long)g_sorted[i]->total, (unsigned long)g_sorted[i]->addr);
        fclose(f);
    }
    for (int i = 0; i < n; i++) g_sorted[i]->win = 0;
    window++;
    win_frames = 0;
    win_game = win_draws = 0;
}

#endif
