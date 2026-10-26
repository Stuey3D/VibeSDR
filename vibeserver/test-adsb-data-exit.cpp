// test-adsb-data-exit.cpp — the aircraft-data updater's thread must survive the PROCESS ENDING under it.
//
// ★★★ STUART'S PI 2, 2026-10-26 23:53:13 UTC: a setup-page save ("Configuration saved — restarting to apply it") ended
//     the front door with `return 0` from main, and during exit() the detached `vibe-adsbdata` thread took SIGSEGV
//     (fault-addr 0x1b0c, pc in a shared library). exit() runs the static destructors — Updater::get()'s, Db::get()'s,
//     Db::images()' — while that thread is still waiting on the Updater's condition variable or working inside
//     runOnce(): it then locks a destroyed mutex, reads a freed std::string, calls a destroyed std::function. The
//     singletons are now never destroyed (vibe_adsb_data_update.h, vibe_adsb_data.h), so whatever the thread is doing
//     at exit, the memory it touches is still there.
// ★ Each case is a forked child that starts the updater, lets the thread reach a given point, and calls exit(0) — the
//   same exit a settings save takes. The parent requires a clean status 0. Run it under -fsanitize=address too
//   (the report says how): ASan names the use-after-free that a plain build only sometimes turns into a crash.
#include "vibe_adsb_data_update.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { if (c) passes++; else { fails++; std::printf("FAIL %s\n", what); } }

/** ★ THE WINDOW. exit() is not instant on a real server: every other static (the shim, the maps, the logs) is torn
 *  down too, and on the Pi 2 that took long enough for the thread to run into the Updater's corpse. This object is
 *  constructed BEFORE the singletons, so exit() destroys it AFTER them — and its destructor holds exit() open for
 *  a third of a second with the singletons already gone, as the rest of a real server's teardown does. */
struct SlowTeardown { ~SlowTeardown() { std::this_thread::sleep_for(std::chrono::milliseconds(350)); } };

/** In a child: start the updater on a fresh dir with a fetcher that takes `fetchMs`, kick it, wait `exitAfterMs`, exit. */
static int child(const std::string& dir, int fetchMs, int exitAfterMs, bool kick) {
    static SlowTeardown slow;   // ★ first, so it is destroyed last
    vibeadsbdata::Updater& u = vibeadsbdata::Updater::get();
    u.setDir(dir);
    u.setFetcher([fetchMs](const std::string&, const std::string& dest, const std::string&, const std::string&) {
        // ★ busy inside the job when exit() arrives — then it returns into runOnce, which touches State, the lock
        //   file, the Updater's members and Db::get().refresh()
        std::this_thread::sleep_for(std::chrono::milliseconds(fetchMs));
        vibeadsbdata::FetchResult r;
        r.status = 0; r.error = "test: no network";
        ::unlink(dest.c_str());
        return r;
    });
    u.start();
    if (kick) u.checkNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(exitAfterMs));
    std::exit(0);   // ★ the static destructors run here, exactly as `return 0` from main does on a settings save
}

static bool runCase(const char* what, int fetchMs, int exitAfterMs, bool kick) {
    char tmpl[] = "/tmp/vs-adsbexit-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) { ok(false, "mkdtemp"); return false; }
    const std::string dir = std::string(d) + "/adsb";
    std::fflush(stdout);
    const pid_t p = fork();
    if (p == 0) child(dir, fetchMs, exitAfterMs, kick);
    int st = 0;
    // ★ a deadline: a child hung in a destroyed condition variable must fail the test, not hang the suite
    bool done = false;
    for (int i = 0; i < 100 && !done; ++i) {
        if (waitpid(p, &st, WNOHANG) == p) done = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!done) { kill(p, SIGKILL); waitpid(p, &st, 0); }
    const bool clean = done && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!clean) std::printf("   %s: %s %d\n", what, !done ? "HUNG (killed after 5 s)" : WIFSIGNALED(st) ? "signal" : "exit",
                            !done ? 0 : WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    ok(clean, what);
    std::string rm = "rm -rf '" + std::string(d) + "'";
    if (std::system(rm.c_str()) != 0) {}
    return clean;
}

int main() {
    for (int rep = 0; rep < 5; ++rep) {
        runCase("exit while the thread sleeps before its first check (the usual case: within 4 min of a start)", 0, 20 + rep * 7, false);
        runCase("exit while a download is under way (the fetcher returns into runOnce after exit began)", 150, 40 + rep * 5, true);
        runCase("exit just as the download returns (runOnce writing state.txt and refreshing Db)", 30, 30 + rep * 3, true);
        runCase("exit after a check, the thread back waiting on its condition variable", 5, 120 + rep * 10, true);
    }
    std::printf("test-adsb-data-exit: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
