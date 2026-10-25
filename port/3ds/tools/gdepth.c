/* gdepth: how deep a running psp2i.exe's main thread goes into its host stack,
 * and what fills it. The PSP threads run on fibers; SwitchToFiber keeps the
 * thread's TEB StackBase on the running fiber's stack, so StackBase - Rsp is
 * that fiber's depth. Samples every millisecond, writes the deepest point of
 * each second, and at every new record (each 256 KB deeper) walks the whole
 * stack and writes how many frames each function has there: a call chain
 * that never unwinds shows up as the functions repeated thousands of times.
 * (The 3DS's user_main ran out of its 4 MB host stack in town: 3DS_PROGRESS.md.)
 *   gdepth <pid> <duration_s> <out.txt>
 * Build: cl /O2 gdepth.c dbghelp.lib winmm.lib
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef NTSTATUS(NTAPI *query_thread_fn)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
typedef struct { NTSTATUS st; PVOID teb; CLIENT_ID cid; KAFFINITY aff; LONG prio, base; } thread_basic;

static HANDLE g_proc;

typedef struct { char *name; unsigned n; } row;
#define NROW 65536
static row g_rows[NROW];
static void count(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    h &= NROW - 1;
    for (;;) {
        if (!g_rows[h].name) { g_rows[h].name = _strdup(name); g_rows[h].n = 1; return; }
        if (!strcmp(g_rows[h].name, name)) { g_rows[h].n++; return; }
        h = (h + 1) & (NROW - 1);
    }
}
static int cmp(const void *a, const void *b) { return (int)((const row *)b)->n - (int)((const row *)a)->n; }

static const char *sym_name(DWORD64 a, char *buf, size_t n) {
    char sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    snprintf(buf, n, "%s", SymFromAddr(g_proc, a, &d, si) ? si->Name : "?");
    return buf;
}

/* the whole stack: frames per function, and the innermost / outermost few */
static void walk(FILE *f, HANDLE th, CONTEXT ctx) {
    for (int i = 0; i < NROW; i++) { free(g_rows[i].name); g_rows[i].name = NULL; }
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    static char last[64][128];
    int frames = 0;
    char nb[300];
    while (frames < 2000000 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_proc, th, &sf, &ctx, NULL,
                                           SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        if (!sf.AddrPC.Offset) break;
        sym_name(sf.AddrPC.Offset, nb, sizeof nb);
        if (frames < 24) fprintf(f, "  inner %2d %s\n", frames, nb);
        snprintf(last[frames & 63], sizeof last[0], "%s", nb);
        count(nb);
        frames++;
    }
    for (int i = frames > 24 ? frames - 24 : 24; i < frames; i++) fprintf(f, "  outer %7d %s\n", i, last[i & 63]);
    row *all = (row *)malloc(sizeof(row) * NROW);
    int n = 0;
    for (int i = 0; i < NROW; i++) if (g_rows[i].name) all[n++] = g_rows[i];
    qsort(all, (size_t)n, sizeof *all, cmp);
    fprintf(f, "  %d frames, %d functions; most frames:\n", frames, n);
    for (int i = 0; i < n && i < 40; i++) fprintf(f, "  %8u %s\n", all[i].n, all[i].name);
    free(all);
    fflush(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "gdepth <pid> <duration_s> <out.txt>\n"); return 2; }
    const DWORD pid = (DWORD)atoi(argv[1]);
    const int dur = atoi(argv[2]);
    FILE *f = fopen(argv[3], "w");
    if (!f) return 1;
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_proc) { fprintf(stderr, "OpenProcess failed %lu\n", GetLastError()); return 1; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    DWORD tid = 0; FILETIME best = { 0xFFFFFFFF, 0xFFFFFFFF };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (t && GetThreadTimes(t, &c, &e, &k, &u) && CompareFileTime(&c, &best) < 0) { best = c; tid = te.th32ThreadID; }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { fprintf(stderr, "OpenThread failed\n"); return 1; }
    query_thread_fn q = (query_thread_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    thread_basic tb;
    if (!q || q(th, (THREADINFOCLASS)0, &tb, sizeof tb, NULL) != 0) { fprintf(stderr, "no TEB\n"); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(g_proc, NULL, TRUE)) { fprintf(stderr, "SymInitialize failed\n"); return 1; }
    fprintf(stderr, "gdepth: pid %lu thread %lu\n", pid, tid);
    timeBeginPeriod(1);
    const ULONGLONG t0 = GetTickCount64(), end = t0 + (ULONGLONG)dur * 1000;
    ULONGLONG sec_end = t0 + 1000;
    DWORD64 sec_max = 0, rec = 0;
    while (GetTickCount64() < end) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) break;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        NT_TIB tib;
        SIZE_T got = 0;
        if (GetThreadContext(th, &ctx) && ReadProcessMemory(g_proc, tb.teb, &tib, sizeof tib, &got)) {
            const DWORD64 depth = (DWORD64)tib.StackBase - ctx.Rsp;
            if (depth < (64u << 20)) {                       /* else a fiber switch in progress */
                if (depth > sec_max) sec_max = depth;
                if (depth > rec + (256u << 10)) {
                    rec = depth;
                    fprintf(f, "record %.1f s: %llu KB (stack base %p)\n", (GetTickCount64() - t0) / 1000.0,
                            (unsigned long long)(depth >> 10), tib.StackBase);
                    walk(f, th, ctx);
                }
            }
        }
        ResumeThread(th);
        if (GetTickCount64() >= sec_end) {
            fprintf(f, "second %llu: deepest %llu KB\n", (unsigned long long)((sec_end - t0) / 1000),
                    (unsigned long long)(sec_max >> 10));
            fflush(f);
            sec_max = 0;
            sec_end += 1000;
        }
    }
    timeEndPeriod(1);
    fclose(f);
    return 0;
}
