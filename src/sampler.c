/*
 * DBG: RECOMP_SAMPLE=1 profiles every thread of the process by sampling.
 *
 * About once a millisecond each thread is suspended just long enough to read
 * its instruction pointer. Nothing is allocated or resolved while a thread is
 * stopped (it may hold the heap or the dbghelp lock); names are looked up at
 * report time. Every 10 s the threads that were not just waiting get their
 * hottest functions printed to stderr as [SAMPLE] lines.
 *
 * RECOMP_SAMPLE_CALLERS=name also reports, for samples inside that function,
 * the first return address on the stack that lies in the .exe -- which
 * caller is paying for a library routine such as strchr.
 */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 64
#define MAX_SAMPLES (4u << 20)
#define REPORT_MS   10000
#define TOP         14

typedef struct { DWORD tid; HANDLE h; } Thr;
typedef struct { unsigned short t; DWORD64 ip; } Sample;
typedef struct { unsigned short t; DWORD64 fn, maxdisp; char name[64]; unsigned n; } Hit;

static Thr s_thr[MAX_THREADS];
static int s_nthr;
static Sample *s_buf;
static unsigned s_n;
static DWORD64 s_watch_lo, s_watch_hi, s_exe_lo, s_exe_hi;
static Sample s_callers[1u << 16];
static unsigned s_ncallers;

static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        int i;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
            continue;
        for (i = 0; i < s_nthr && s_thr[i].tid != te.th32ThreadID; i++)
            ;
        if (i < s_nthr || s_nthr == MAX_THREADS)
            continue;
        s_thr[s_nthr].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                     FALSE, te.th32ThreadID);
        if (s_thr[s_nthr].h) {
            s_thr[s_nthr].tid = te.th32ThreadID;
            s_nthr++;
        }
    }
    CloseHandle(snap);
}

static int cmp_sample(const void *a, const void *b)
{
    const Sample *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->ip < y->ip ? -1 : x->ip > y->ip;
}

static int cmp_hit(const void *a, const void *b)
{
    const Hit *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    if (x->fn != y->fn) return x->fn < y->fn ? -1 : 1;
    return 0;
}

static int cmp_count(const void *a, const void *b)
{
    const Hit *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int is_wait(const char *name)
{
    return !strncmp(name, "Nt", 2) || !strncmp(name, "Zw", 2) || strstr(name, "Wait") || strstr(name, "Sleep") ||
           strstr(name, "Delay");
}

static int report_all;

static void report(void)
{
    char sbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
    Hit *hits;
    unsigned nh = 0, i, j;

    qsort(s_buf, s_n, sizeof *s_buf, cmp_sample);
    hits = (Hit *)calloc(s_n + 1, sizeof *hits);
    if (!hits)
        return;
    /* One name lookup per distinct address. */
    for (i = 0; i < s_n; i = j) {
        DWORD64 disp = 0;
        for (j = i; j < s_n && s_buf[j].t == s_buf[i].t && s_buf[j].ip == s_buf[i].ip; j++)
            ;
        memset(sbuf, 0, sizeof sbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        hits[nh].t = s_buf[i].t;
        hits[nh].n = j - i;
        if (SymFromAddr(GetCurrentProcess(), s_buf[i].ip, &disp, sym)) {
            hits[nh].fn = sym->Address;
            hits[nh].maxdisp = disp;
            snprintf(hits[nh].name, sizeof hits[nh].name, "%s", sym->Name);
        } else {
            hits[nh].fn = s_buf[i].ip & ~0xFFFull;
            snprintf(hits[nh].name, sizeof hits[nh].name, "?%llX", (unsigned long long)s_buf[i].ip);
        }
        nh++;
    }
    /* Merge addresses of the same function. */
    qsort(hits, nh, sizeof *hits, cmp_hit);
    for (i = 0, j = 0; i < nh; i++) {
        if (j && hits[j - 1].t == hits[i].t && hits[j - 1].fn == hits[i].fn) {
            hits[j - 1].n += hits[i].n;
            if (hits[i].maxdisp > hits[j - 1].maxdisp)
                hits[j - 1].maxdisp = hits[i].maxdisp;
        }
        else
            hits[j++] = hits[i];
    }
    nh = j;
    /* Per thread: skip threads that mostly wait. */
    for (i = 0; i < nh; i = j) {
        unsigned total = 0, busy = 0, k;
        for (j = i; j < nh && hits[j].t == hits[i].t; j++) {
            total += hits[j].n;
            if (!is_wait(hits[j].name))
                busy += hits[j].n;
        }
        if (busy * 10 < total && !report_all)
            continue;
        qsort(hits + i, j - i, sizeof *hits, cmp_count);
        fprintf(stderr, "[SAMPLE] thread %u: %u samples, %u%% busy\n",
                report_all ? (unsigned)hits[i].t : (unsigned)s_thr[hits[i].t].tid, total,
                busy * 100 / (total ? total : 1));
        for (k = i; k < j && k < i + TOP; k++)
            fprintf(stderr, "[SAMPLE]   %5.1f%%  %s (%llX +%llX)\n", hits[k].n * 100.0 / total, hits[k].name,
                    (unsigned long long)hits[k].fn, (unsigned long long)hits[k].maxdisp);
    }
    free(hits);
    fflush(stderr);
}

static DWORD WINAPI sampler(LPVOID arg)
{
    DWORD last = GetTickCount(), lastthr = 0;
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        int i;
        if (GetTickCount() - lastthr > 1000) {
            refresh_threads();
            lastthr = GetTickCount();
        }
        for (i = 0; i < s_nthr && s_n < MAX_SAMPLES; i++) {
            CONTEXT c;
            if (SuspendThread(s_thr[i].h) == (DWORD)-1)
                continue;
            c.ContextFlags = s_watch_lo ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(s_thr[i].h, &c)) {
                s_buf[s_n].t = (unsigned short)i;
                s_buf[s_n].ip = c.Rip;
                s_n++;
                if (c.Rip >= s_watch_lo && c.Rip < s_watch_hi) {
                    /* Unwind properly (scanning the stack for code addresses
                     * picks up stale ones) and keep the first four frames
                     * inside the .exe, innermost first. */
                    int depth = 0;
                    for (int f = 0; f < 16 && depth < 4 && c.Rip && s_ncallers < sizeof s_callers / sizeof *s_callers; f++) {
                        DWORD64 base = 0, frame = 0;
                        PVOID hd = NULL;
                        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                        if (f && c.Rip >= s_exe_lo && c.Rip < s_exe_hi) {
                            s_callers[s_ncallers].t = (unsigned short)depth++;
                            s_callers[s_ncallers].ip = c.Rip;
                            s_ncallers++;
                        }
                        if (!rf) {                        /* leaf: return address on top */
                            c.Rip = *(const DWORD64 *)c.Rsp;
                            c.Rsp += 8;
                        } else {
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &frame, NULL);
                        }
                    }
                }
            }
            ResumeThread(s_thr[i].h);
        }
        if (GetTickCount() - last >= REPORT_MS) {
            report();
            s_n = 0;
            if (s_ncallers) {
                Sample *b = s_buf;
                fprintf(stderr, "[SAMPLE] callers of the watched function (thread = stack depth):\n");
                memcpy(s_buf, s_callers, s_ncallers * sizeof *s_buf);
                s_n = s_ncallers;
                report_all = 1;
                report();
                report_all = 0;
                s_buf = b;
                s_n = s_ncallers = 0;
            }
            last = GetTickCount();
        }
        Sleep(1);
    }
    return 0;
}

void sampler_start(void)
{
    const char *w = getenv("RECOMP_SAMPLE_CALLERS");
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi)) {
        s_exe_lo = (DWORD64)(uintptr_t)mi.lpBaseOfDll;
        s_exe_hi = s_exe_lo + mi.SizeOfImage;
    }
    if (w && *w) {
        char sbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
        memset(sbuf, 0, sizeof sbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromName(GetCurrentProcess(), w, sym)) {
            s_watch_lo = sym->Address;
            s_watch_hi = sym->Address + (sym->Size ? sym->Size : 256);
        }
        fprintf(stderr, "[SAMPLE] watching %s at %llX-%llX\n", w, (unsigned long long)s_watch_lo,
                (unsigned long long)s_watch_hi);
    }
    s_buf = (Sample *)malloc(MAX_SAMPLES * sizeof *s_buf);
    if (s_buf)
        CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
}
