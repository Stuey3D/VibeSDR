#include "parent_watch.h"

#include <cstdio>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/types.h>
#endif

namespace vibe {

bool watchPidForExit(int pid, void (*onExit)()) {
#if defined(__APPLE__)
    if (pid <= 1 || !onExit) return false;
    const int kq = ::kqueue();
    if (kq < 0) return false;
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
    // ★★ REGISTRATION IS ALSO THE LIVENESS CHECK. If the process is already gone, kevent() fails
    //    with ESRCH — so "the parent died between fork and here", the race the Linux side has to
    //    close explicitly, is closed here by construction. Report it as a fire, not a failure:
    //    the caller's contract is "tell me when the parent is not there", and it is not there.
    if (::kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) { ::close(kq); onExit(); return true; }
    std::thread([kq, onExit] {
        struct kevent out;
        // ★ Blocking wait, no timeout: this thread exists only to sleep until the parent exits.
        if (::kevent(kq, nullptr, 0, &out, 1, nullptr) > 0) onExit();
        ::close(kq);
    }).detach();
    return true;
#else
    (void)pid; (void)onExit;
    return false;      // Linux: PR_SET_PDEATHSIG already covers it, set before exec
#endif
}

bool dieWithParent() {
#if defined(__APPLE__)
    return watchPidForExit((int)::getppid(), [] {
        std::fprintf(stderr, "VibeServer: the front door exited — releasing the radio\n");
        // ★ _exit, not exit: no atexit handlers, no half-finished teardown. The point is to stop
        //   holding the SDR immediately, and anything clever here is a way to fail to.
        ::_exit(0);
    });
#else
    return false;
#endif
}

}  // namespace vibe
