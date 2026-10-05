// test-airspyhf-restart.cpp — AirspyHfSource's recovery ordering, against a STUB libairspyhf.
//
// ★★ WHY (2026-10-05). Nick's Pixel 6 HF+ crashed overnight and then "couldn't be found" until it was
//    re-plugged. The fixes are all about ORDER and DEADLINES — a hung stop must not hold the mutex for
//    ever, a dead handle is closed before a fresh fd is opened, our dup of the fd is closed only AFTER
//    the handle that used it, an unchanged rate never reaches the library — and none of that is
//    visible on real hardware until the night it goes wrong. A stub library records every call, so
//    the order is a fact the test can read, and a "hang" is a condition variable it controls.
//
//   c++ -std=c++17 -DVIBE_HAVE_AIRSPYHF -DVIBE_AIRSPYHF_HAS_FD -I android/app/src/main/cpp \
//       -I android/app/src/main/cpp/libairspyhf vibeserver/test-airspyhf-restart.cpp \
//       android/app/src/main/cpp/airspyhf_source.cpp -o /tmp/t -lpthread && /tmp/t
#include "airspyhf_source.h"
#include "airspyhf.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

// ── The stub library ─────────────────────────────────────────────────────────
struct airspyhf_device { int id; int fd; bool streaming; };
static std::mutex g_mx;
static std::vector<std::string> g_log;          // every call, in order
static int g_nextId = 1;
static std::atomic<bool> g_hangStop{false};     // airspyhf_stop blocks until released
static std::mutex g_hangMx;
static std::condition_variable g_hangCv;
static bool g_release = false;

static void note(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_log.push_back(s); }
static std::vector<std::string> takeLog() { std::lock_guard<std::mutex> lk(g_mx); auto v = g_log; g_log.clear(); return v; }
static int indexOf(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (int)i;
    return -1;
}
static int countOf(const std::vector<std::string>& v, const std::string& prefix) {
    int n = 0; for (auto& s : v) if (s.rfind(prefix, 0) == 0) ++n; return n;
}

extern "C" {
void airspyhf_set_thread_hook(void (*)(int)) {}
int airspyhf_list_devices(uint64_t*, int) { return 0; }
int airspyhf_open_sn(airspyhf_device_t**, uint64_t) { return AIRSPYHF_ERROR; }
int airspyhf_open_fd(airspyhf_device_t** d, int fd) {
    *d = new airspyhf_device{ g_nextId++, fd, false };
    note("open_fd " + std::to_string((*d)->id));
    return AIRSPYHF_SUCCESS;
}
int airspyhf_close(airspyhf_device_t* d) {
    note("close " + std::to_string(d->id));
    delete d;
    return AIRSPYHF_SUCCESS;
}
int airspyhf_get_samplerates(airspyhf_device_t*, uint32_t* buf, const uint32_t len) {
    if (len == 0) { *buf = 1; return AIRSPYHF_SUCCESS; }
    buf[0] = 912000; return AIRSPYHF_SUCCESS;
}
int airspyhf_set_lib_dsp(airspyhf_device_t*, const uint8_t) { return AIRSPYHF_SUCCESS; }
int airspyhf_set_samplerate(airspyhf_device_t* d, uint32_t r) {
    note("set_samplerate " + std::to_string(d->id) + " " + std::to_string(r) + (d->streaming ? " LIVE" : ""));
    return AIRSPYHF_SUCCESS;
}
int airspyhf_is_low_if(airspyhf_device_t*) { return 0; }
int airspyhf_start(airspyhf_device_t* d, airspyhf_sample_block_cb_fn, void*) {
    d->streaming = true; note("start " + std::to_string(d->id)); return AIRSPYHF_SUCCESS;
}
int airspyhf_stop(airspyhf_device_t* d) {
    note("stop " + std::to_string(d->id));
    if (g_hangStop.load()) {
        std::unique_lock<std::mutex> lk(g_hangMx);
        g_hangCv.wait(lk, [] { return g_release; });
    }
    d->streaming = false;
    return AIRSPYHF_SUCCESS;
}
int airspyhf_set_freq(airspyhf_device_t* d, const uint32_t hz) { note("set_freq " + std::to_string(d->id) + " " + std::to_string(hz)); return AIRSPYHF_SUCCESS; }
int airspyhf_set_hf_agc(airspyhf_device_t* d, uint8_t f) { note("agc " + std::to_string(d->id) + " " + std::to_string(f)); return AIRSPYHF_SUCCESS; }
int airspyhf_set_hf_agc_threshold(airspyhf_device_t* d, uint8_t f) { note("thr " + std::to_string(d->id) + " " + std::to_string(f)); return AIRSPYHF_SUCCESS; }
int airspyhf_set_hf_att(airspyhf_device_t* d, uint8_t v) { note("att " + std::to_string(d->id) + " " + std::to_string(v)); return AIRSPYHF_SUCCESS; }
int airspyhf_set_hf_lna(airspyhf_device_t* d, uint8_t f) { note("lna " + std::to_string(d->id) + " " + std::to_string(f)); return AIRSPYHF_SUCCESS; }
int airspyhf_set_calibration(airspyhf_device_t*, int32_t) { return AIRSPYHF_SUCCESS; }
}

static bool fdOpen(int fd) { return fd >= 0 && ::fcntl(fd, F_GETFD) != -1; }

static std::string err;

int main() {
    std::printf("AirspyHfSource — restart deadlines, fd ownership, rate guard, fresh-fd reopen\n");

    // A real descriptor stands in for Android's: /dev/null answers fcntl, which is all the dup test needs.
    const int kotlinFd = ::open("/dev/null", O_RDONLY);
    {
        vibe::AirspyHfSource s;
        CHECK(s.openFd(kotlinFd, 0, 7100000, 300, err), "openFd failed: %s", err.c_str());
        CHECK(s.fdOpened(), "fdOpened() false after openFd");
        auto log = takeLog();
        CHECK(countOf(log, "set_samplerate 1 912000") == 1, "open did not program the rate exactly once");
        // ── #5: we work on our OWN dup, and Kotlin's fd is untouched by a close ──
        CHECK(s.start(err), "start: %s", err.c_str());
        takeLog();

        // ── #4: an unchanged rate never reaches the library, live or not ──
        CHECK(s.setSampleRate(912000), "setSampleRate(912000) returned false");
        CHECK(s.setSampleRate(768000), "setSampleRate(768000) returned false");   // resolves to 912000
        log = takeLog();
        CHECK(countOf(log, "set_samplerate") == 0, "an unchanged rate reached the library (%d calls)", countOf(log, "set_samplerate"));

        // ── #1: a shallow restart whose stop HANGS returns on the deadline and frees the mutex ──
        g_hangStop.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = s.restartStream(false, err);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!ok, "a restart whose stop hung reported success");
        CHECK(took < 7.0, "the hung restart took %.1f s (deadline is 5 s)", took);
        CHECK(!s.isOpen(), "the abandoned handle is still reported open");
        // The mutex is free: a control call returns at once instead of queueing behind the hang.
        const auto t1 = std::chrono::steady_clock::now();
        s.setFrequency(7200000);
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 0.5,
              "a control call queued behind the hung restart");
        // Release the stuck worker; it must not touch anything we then do (it holds handle 1 only).
        { std::lock_guard<std::mutex> lk(g_hangMx); g_release = true; }
        g_hangCv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_hangStop.store(false);
        log = takeLog();
        CHECK(indexOf(log, "close 1") < 0, "the abandoned handle was CLOSED (it must be left to its worker)");

        // ── #2: a fresh fd after a re-plug — reopen and replay everything ──
        s.setLna(true);
        s.setAgcThreshold(true);
        takeLog();
        const int freshFd = ::open("/dev/null", O_RDONLY);
        CHECK(s.reopenOnFd(freshFd, err), "reopenOnFd failed: %s", err.c_str());
        log = takeLog();
        CHECK(indexOf(log, "open_fd 2") >= 0, "the fresh fd was not opened");
        CHECK(indexOf(log, "set_samplerate 2 912000") >= 0, "the rate was not re-programmed on the fresh handle");
        CHECK(indexOf(log, "set_freq 2 7200000") >= 0, "the tuning was not replayed (want 7200000)");
        CHECK(indexOf(log, "lna 2 1") >= 0, "the preamp was not replayed");
        CHECK(indexOf(log, "thr 2 1") >= 0, "the AGC threshold was not replayed");
        CHECK(indexOf(log, "att 2 3") >= 0, "the gain (30 dB -> 3 att steps) was not replayed");
        CHECK(indexOf(log, "start 2") >= 0, "a stream that was wanted was not restarted");
        CHECK(s.isOpen(), "not open after reopenOnFd");
        CHECK(fdOpen(freshFd), "reopenOnFd closed the CALLER's fd (it must take its own dup)");
        ::close(freshFd);

        // ── #2: releasing a dead handle closes the handle BEFORE our fd, and a reopen afterwards works ──
        s.releaseDeadHandle();
        log = takeLog();
        CHECK(indexOf(log, "stop 2") >= 0 && indexOf(log, "close 2") > indexOf(log, "stop 2"),
              "the dead handle was not stopped then closed");
        const int fresh2 = ::open("/dev/null", O_RDONLY);
        CHECK(s.reopenOnFd(fresh2, err), "second reopenOnFd failed: %s", err.c_str());
        log = takeLog();
        CHECK(indexOf(log, "start 3") >= 0, "the stream was not restarted after a release + reopen");
        ::close(fresh2);

        // ── a reopen while a handle is still held closes the old one FIRST ──
        const int fresh3 = ::open("/dev/null", O_RDONLY);
        CHECK(s.reopenOnFd(fresh3, err), "third reopenOnFd failed: %s", err.c_str());
        log = takeLog();
        CHECK(indexOf(log, "close 3") >= 0 && indexOf(log, "close 3") < indexOf(log, "open_fd 4"),
              "the old handle was not closed before the fresh fd was opened");
        ::close(fresh3);
        // close() (destructor) follows; Kotlin's fd must survive it.
    }
    CHECK(fdOpen(kotlinFd), "the source closed KOTLIN's fd — it must only ever close its own dup");
    ::close(kotlinFd);

    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("all passed\n");
    return 0;
}
