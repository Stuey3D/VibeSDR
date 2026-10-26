/* ★ Windows stand-in for <sys/resource.h>: getrusage() from GetProcessTimes / GetThreadTimes (the
 *   benchmark's CPU measure) and the peak working set as ru_maxrss (KB, as on Linux). Thread priority is
 *   NOT here: vibe_thread.h has a Windows ladder of its own, so setpriority() is deliberately absent. */
#pragma once
#include "vibe_win_compat.h"
#include <psapi.h>
#define RUSAGE_SELF   0
#define RUSAGE_THREAD 1
struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock,
         ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
static inline void vibe_ft_to_tv_(const FILETIME* f, struct timeval* tv) {
    unsigned long long t = ((unsigned long long)f->dwHighDateTime << 32) | f->dwLowDateTime;  /* 100 ns */
    tv->tv_sec = (long)(t / 10000000ULL);
    tv->tv_usec = (long)((t % 10000000ULL) / 10);
}
static inline int getrusage(int who, struct rusage* u) {
    FILETIME c, e, k, us;
    memset(u, 0, sizeof *u);
    BOOL ok = who == RUSAGE_THREAD ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &us)
                                   : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us);
    if (!ok) { errno = EINVAL; return -1; }
    vibe_ft_to_tv_(&us, &u->ru_utime);
    vibe_ft_to_tv_(&k, &u->ru_stime);
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) u->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
    return 0;
}
