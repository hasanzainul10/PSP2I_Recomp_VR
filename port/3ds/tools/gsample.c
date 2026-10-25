/* gsample: sample a running psp2i.exe's main thread and charge each sample to
 * the innermost recompiled game function on its stack (psp_body_* / psp_func_*),
 * so a game function's own code plus the runtime helpers it calls directly
 * (psp_read32, psp_write32, ...) count as its time.
 *   gsample <pid> <delay_s> <duration_s> <out.txt>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSLOT (1 << 20)
typedef struct { DWORD64 addr; char *name; } symc;
static symc g_sc[NSLOT];
static HANDLE g_proc;

static const char *sym_of(DWORD64 a) {
    unsigned h = (unsigned)((a >> 2) * 2654435761u) & (NSLOT - 1);
    for (;;) {
        if (g_sc[h].addr == a) return g_sc[h].name;
        if (!g_sc[h].addr) break;
        h = (h + 1) & (NSLOT - 1);
    }
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    const char *n = SymFromAddr(g_proc, a, &d, si) ? si->Name : "?";
    g_sc[h].addr = a;
    g_sc[h].name = _strdup(n);
    return g_sc[h].name;
}

typedef struct { char *name; unsigned n; } row;
#define NROW 65536
static row g_rows[NROW];
static int g_nrows;
static void count(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    h &= NROW - 1;
    for (;;) {
        if (!g_rows[h].name) { g_rows[h].name = _strdup(name); g_rows[h].n = 1; g_nrows++; return; }
        if (!strcmp(g_rows[h].name, name)) { g_rows[h].n++; return; }
        h = (h + 1) & (NROW - 1);
    }
}
static int cmp(const void *a, const void *b) { return (int)((const row *)b)->n - (int)((const row *)a)->n; }

static int is_game(const char *n) { return !strncmp(n, "psp_body_", 9) || !strncmp(n, "psp_func_", 9); }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "gsample <pid> <delay_s> <duration_s> <out.txt>\n"); return 2; }
    const DWORD pid = (DWORD)atoi(argv[1]);
    const int delay = atoi(argv[2]), dur = atoi(argv[3]);
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_proc) { fprintf(stderr, "OpenProcess failed %lu\n", GetLastError()); return 1; }
    /* the main thread: the one created first */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    DWORD tid = 0; FILETIME best = { 0xFFFFFFFF, 0xFFFFFFFF };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (th && GetThreadTimes(th, &c, &e, &k, &u) && CompareFileTime(&c, &best) < 0) { best = c; tid = te.th32ThreadID; }
        if (th) CloseHandle(th);
    }
    CloseHandle(snap);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { fprintf(stderr, "OpenThread failed\n"); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(g_proc, NULL, TRUE)) { fprintf(stderr, "SymInitialize failed\n"); return 1; }
    fprintf(stderr, "gsample: pid %lu thread %lu; waiting %d s\n", pid, tid, delay);
    Sleep((DWORD)delay * 1000);
    timeBeginPeriod(1);
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)dur * 1000;
    unsigned total = 0, game = 0;
    while (GetTickCount64() < end) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) break;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
            STACKFRAME64 sf;
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            const char *leaf = NULL, *hit = NULL, *below = NULL;
            for (int d = 0; d < 64; d++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_proc, th, &sf, &ctx, NULL,
                                 SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
                if (!sf.AddrPC.Offset) break;
                const char *n = sym_of(sf.AddrPC.Offset);
                if (!leaf) leaf = n;
                if (is_game(n)) { hit = n; break; }
                below = n;                         /* what the game function called */
            }
            total++;
            /* the game function's own: its code, or a memory helper it called */
            const int mem = !below || !strncmp(below, "psp_read", 8) || !strncmp(below, "psp_write", 9) ||
                            !strcmp(below, "psp_mem_ptr") || !strcmp(below, "note_write") ||
                            !strcmp(below, "note_read") || !strncmp(below, "psp_scc", 7);
            char b[300];
            if (hit && mem) { game++; count(hit); }
            else if (hit) { snprintf(b, sizeof b, "(called) %s", below); count(b); }
            else { snprintf(b, sizeof b, "(not game) %s", leaf ? leaf : "?"); count(b); }
        }
        ResumeThread(th);
    }
    timeEndPeriod(1);
    row *all = (row *)malloc(sizeof(row) * (size_t)g_nrows);
    int n = 0;
    for (int i = 0; i < NROW; i++) if (g_rows[i].name) all[n++] = g_rows[i];
    qsort(all, (size_t)n, sizeof *all, cmp);
    FILE *f = fopen(argv[4], "w");
    if (!f) return 1;
    fprintf(f, "# %u samples, %u in game functions (%.1f%%)\n", total, game, 100.0 * game / (total ? total : 1));
    for (int i = 0; i < n; i++) fprintf(f, "%u %.3f %s\n", all[i].n, 100.0 * all[i].n / (total ? total : 1), all[i].name);
    fclose(f);
    fprintf(stderr, "gsample: %u samples, %u in game functions, %d names\n", total, game, n);
    return 0;
}
