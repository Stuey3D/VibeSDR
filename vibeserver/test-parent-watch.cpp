// ★★★ DOES A SUPERVISED RADIO ACTUALLY DIE WITH ITS FRONT DOOR?
//
// A radio process outliving its supervisor keeps the SDR claimed, so nothing can restart it and
// the hardware looks broken. On Linux PR_SET_PDEATHSIG guarantees it and survives execve. macOS
// has no equivalent, so the child arms a kqueue watch itself — and an unverified guarantee is not
// a guarantee.
//
// ★★ IT KILLS THE PARENT WITH SIGKILL, deliberately. A parent that exits cleanly could be covered
//    by any amount of tidy-up code; the case that matters is the one where no cleanup can run —
//    crash, kill -9, force quit. That is the failure this mechanism exists for.
//
// ★ On Linux both watch functions return false (nothing to arm) and the test says so and passes,
//   rather than asserting behaviour the platform gets from the kernel instead.

#include "parent_watch.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

int main() {
    std::printf("parent watch — a radio must not outlive its front door\n");

#if !defined(__APPLE__)
    ok(!vibe::dieWithParent(),
       "Linux: nothing to arm — PR_SET_PDEATHSIG is set before exec and survives it");
    std::printf("\n\033[32mpassed\033[0m (platform needs no watch)\n");
    return 0;
#else
    // A pipe is how the child tells us it noticed. It must be created BEFORE the fork so both
    // ends exist in both processes.
    int pfd[2];
    if (::pipe(pfd) != 0) { std::printf("  pipe failed\n"); return 1; }

    // ── The "front door": a process we can kill uncleanly ────────────────────
    const pid_t doorPid = ::fork();
    if (doorPid == 0) {
        // ★ The door does nothing but stay alive. Its only job is to be killed.
        ::close(pfd[0]); ::close(pfd[1]);
        for (;;) ::pause();
        ::_exit(0);
    }

    // ── The "radio": watches the door, and reports rather than exiting, so the
    //    test can observe it. Same function the real child calls.
    const pid_t radioPid = ::fork();
    if (radioPid == 0) {
        ::close(pfd[0]);
        static int wfd; wfd = pfd[1];
        const bool armed = vibe::watchPidForExit((int)doorPid, [] {
            const char c = 'X';
            ssize_t n = ::write(wfd, &c, 1);   // the door has gone — say so
            (void)n;
            ::_exit(0);
        });
        if (!armed) { const char c = '!'; ssize_t n = ::write(pfd[1], &c, 1); (void)n; ::_exit(2); }
        ::sleep(10);                            // ample; the watch should fire long before
        ::_exit(3);                             // reached only if the watch never fired
    }
    ::close(pfd[1]);

    ::usleep(300 * 1000);                       // let the radio arm its watch
    ok(::kill(doorPid, 0) == 0, "the front door is running");

    // ★★★ SIGKILL — no cleanup can possibly run in the door.
    ::kill(doorPid, SIGKILL);
    int st = 0; ::waitpid(doorPid, &st, 0);

    // Did the radio notice? Read with a timeout by checking the child's exit instead of blocking
    // forever — a hung read would report as a test that never finishes rather than one that fails.
    char c = 0;
    const ssize_t n = ::read(pfd[0], &c, 1);    // the write end is closed if the child died
    ok(n == 1 && c == 'X', "the radio noticed the front door die and released the radio");

    int rst = 0; ::waitpid(radioPid, &rst, 0);
    ok(WIFEXITED(rst) && WEXITSTATUS(rst) == 0,
       "the radio process exited, rather than running on as an orphan");

    std::printf(failures ? "\n\033[31m%d failed\033[0m\n" : "\n\033[32mpassed\033[0m\n", failures);
    return failures ? 1 : 0;
#endif
}
