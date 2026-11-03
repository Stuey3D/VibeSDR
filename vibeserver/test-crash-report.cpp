// test-crash-report.cpp — the fatal-signal record (crash_report.h) says what the journal needs, and the
// process still dies of the SAME signal afterwards (so systemd's status=11/SEGV and any core survive).
// Linux only: forks a child that faults on a NAMED thread, captures its stderr, checks both.
// ★★ 2026-11-03 (a USRP clone's calloc fault on a corrupt heap): two more children —
//    · a CORRUPTED HEAP and then calloc() from a thread named 'vibe-usrp', the shape of that crash: the stack scan must
//      print (it now comes before backtrace(), which can die on such a heap);
//    · a fault near the TOP of a thread stack with an unreadable page right above it: the scan must stop at that page
//      and the report must still finish (the old scan read straight into it and died mid-report).
#include "crash_report.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

/* ★ The heap-corruption child: fill this thread's tcache for 64-byte chunks, free one more into the fastbin, overwrite
 *  that free chunk's forward pointer (glibc's safe-linking applied, so it passes the alignment check) with an address
 *  that is not mapped — then calloc(1, 56) until the allocator follows it. calloc(1, 56) is exactly what the tester's
 *  box faulted in (rdi 0x1, rsi 0x38). Whichever check catches it first, SIGSEGV or glibc's abort, the report prints. */
__attribute__((noinline)) static void corruptHeapThenCalloc() {
    void* hole = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(hole, 4096);
    const uintptr_t target = uintptr_t(hole) + 0x100;
    void* p[8];
    for (auto& q : p) q = std::malloc(56);
    for (int i = 0; i < 7; i++) std::free(p[i]);
    std::free(p[7]);
    *static_cast<volatile uintptr_t*>(p[7]) = (uintptr_t(p[7]) >> 12) ^ target;
    for (int i = 0; i < 32; i++) { void* volatile c = std::calloc(1, 56); (void)c; }
    volatile int* z = nullptr; *z = 1;   // ★ an allocator that noticed nothing: fault anyway — the scan is what is tested
}

static void* smallStackFault(void*) {
    prctl(PR_SET_NAME, "vibe-small", 0, 0, 0);
    volatile int* p = nullptr; *p = 7;
    return nullptr;
}

static std::string runChild(int which, int& status) {
    int fd[2]; if (pipe(fd) != 0) return "";
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        vibecrash::install("test-1.2.3");
        if (which == 3) {
            /* ★ A stack we place ourselves, with a PROT_NONE page directly ABOVE its top — the scan walks upward from
             *  sp, so this is the page it would run into. PTHREAD_STACK_MIN is 128 KB on arm64, so 256 KB. */
            const size_t sz = 256 * 1024, pg = 4096;
            char* m = static_cast<char*>(mmap(nullptr, sz + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            mprotect(m + sz, pg, PROT_NONE);
            pthread_attr_t a; pthread_attr_init(&a);
            pthread_attr_setstack(&a, m, sz);
            pthread_t t; pthread_create(&t, &a, smallStackFault, nullptr);
            pthread_join(t, nullptr);
            _exit(0);
        }
        std::thread t([which] {
            prctl(PR_SET_NAME, which == 2 ? "vibe-usrp" : "vibe-dab-msc", 0, 0, 0);
            if (which == 0) { volatile int* p = nullptr; *p = 42; }        // SIGSEGV
            else if (which == 1) { std::abort(); }                          // SIGABRT (glibc's heap checks)
            else corruptHeapThenCalloc();
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

static size_t count(const std::string& s, const char* what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
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
    check(out.find("stack scan") != std::string::npos && out.find("stack scan") < out.find("backtrace ("),
          "the stack scan is printed BEFORE backtrace()");
    out = runChild(1, st);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "an abort still ends the process with SIGABRT");
    check(out.find("VibeServer CRASH: signal 6") != std::string::npos, "an abort is reported too");

    std::puts("-- a corrupted heap, then calloc(1, 56) on 'vibe-usrp'");
    out = runChild(2, st);
    std::fputs(out.c_str(), stdout);
    check(WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGABRT), "the corrupt heap kills the process (SEGV or ABRT)");
    check(out.find("thread 'vibe-usrp'") != std::string::npos, "the thread is named vibe-usrp");
    check(out.find("end of stack scan") != std::string::npos, "the stack scan finished on the corrupt heap");
    check(count(out, "  exe+0x") >= 1, "and found at least one return address into the executable");
    check(out.find("end of stack scan") < out.find("backtrace ("), "before backtrace() was asked anything");

    std::puts("-- a fault at the top of a stack with an unreadable page above it");
    out = runChild(3, st);
    std::fputs(out.c_str(), stdout);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "the process still dies of SIGSEGV");
    check(out.find("thread 'vibe-small'") != std::string::npos, "the thread is named");
    check(out.find("the readable stack ends here") != std::string::npos || out.find("the mapped stack ends here") != std::string::npos,
          "the scan stopped at the unreadable page");
    check(out.find("end of report") != std::string::npos, "and the report still finished");
    std::printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
#else
int main() { std::puts("SKIP (Linux only)"); return 0; }
#endif
