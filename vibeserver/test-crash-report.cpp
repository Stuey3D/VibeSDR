// test-crash-report.cpp — the fatal-signal record (crash_report.h) says what the journal needs, and the
// process still dies of the SAME signal afterwards (so systemd's status=11/SEGV and any core survive).
// Linux only: forks a child that faults on a NAMED thread, captures its stderr, checks both.
#include "crash_report.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__linux__)
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

static std::string runChild(int which, int& status) {
    int fd[2]; if (pipe(fd) != 0) return "";
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        vibecrash::install("test-1.2.3");
        std::thread t([which] {
            prctl(PR_SET_NAME, "vibe-dab-msc", 0, 0, 0);
            if (which == 0) { volatile int* p = nullptr; *p = 42; }        // SIGSEGV
            else            { std::abort(); }                               // SIGABRT (glibc's heap checks)
        });
        t.join();
        _exit(0);
    }
    close(fd[1]);
    std::string out; char b[4096]; ssize_t r;
    while ((r = read(fd[0], b, sizeof b)) > 0) out.append(b, size_t(r));
    close(fd[0]);
    waitpid(pid, &status, 0);
    return out;
}

int main() {
    int st = 0;
    std::string out = runChild(0, st);
    std::fputs(out.c_str(), stdout);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "a segfault still ends the process with SIGSEGV");
    check(out.find("VibeServer CRASH: signal 11") != std::string::npos, "the signal is named");
    check(out.find("thread 'vibe-dab-msc'") != std::string::npos, "the faulting thread is named");
    check(out.find("fault-addr 0x0") != std::string::npos, "the fault address is given");
    check(out.find("version test-1.2.3") != std::string::npos, "the version is given");
    check(out.find(" pc 0x") != std::string::npos && out.find("(exe+0x") != std::string::npos, "the PC is given as an exe offset");
    check(out.find("end of report") != std::string::npos, "the report is complete");
    out = runChild(1, st);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "an abort still ends the process with SIGABRT");
    check(out.find("VibeServer CRASH: signal 6") != std::string::npos, "an abort is reported too");
    std::printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
#else
int main() { std::puts("SKIP (Linux only)"); return 0; }
#endif
