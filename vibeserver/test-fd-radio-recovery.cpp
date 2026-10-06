// test-fd-radio-recovery.cpp — the Android USB re-plug recovery for the HackRF and the Airspy R2/Mini,
// and the rules every fd radio shares (vibe_usb_recovery.h), against STUB libhackrf and libairspy.
//
// ★★ WHY (2026-10-05). The HF+ learned four things the night Nick's Pixel 6 lost its radio — open on our
//    own dup of the descriptor, release a dead handle on a deadline, adopt a fresh fd in a safe ORDER and
//    put every setting back, never release a radio that cannot be reopened — and the HackRF and R2/Mini
//    had the same gap. Nobody here owns a HackRF and the fault needs a night to happen, so the order is
//    read from a stub library's call log, a "hang" is a condition variable the test controls, and a dead
//    descriptor is a file truncated to nothing (usbfs answers a read of the device descriptor; a gone
//    device answers short). Sibling of test-airspyhf-restart.cpp.
//
//   c++ -std=c++17 -DVIBE_HAS_HACKRF -DVIBE_HACKRF_HAS_FD -DVIBE_HAVE_AIRSPY -I android/app/src/main/cpp \
//       -I android/app/src/main/cpp/libairspy vibeserver/test-fd-radio-recovery.cpp \
//       android/app/src/main/cpp/hackrf_source.cpp android/app/src/main/cpp/airspy_source.cpp \
//       -o /tmp/t -lpthread && /tmp/t
#include "hackrf_source.h"
#include "airspy_source.h"
#include "vibe_usb_recovery.h"
#include <libhackrf/hackrf.h>
#include "airspy.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

// ── The call log both stubs write ───────────────────────────────────────────────────────────────
static std::mutex g_mx;
static std::vector<std::string> g_log;
static void note(const std::string& s) { std::lock_guard<std::mutex> lk(g_mx); g_log.push_back(s); }
static std::vector<std::string> takeLog() { std::lock_guard<std::mutex> lk(g_mx); auto v = g_log; g_log.clear(); return v; }
static int indexOf(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (int)i;
    return -1;
}
static bool fdOpen(int fd) { return fd >= 0 && ::fcntl(fd, F_GETFD) != -1; }

// A "hang" the test releases by hand.
static std::atomic<bool> g_hang{false};
static std::mutex g_hangMx;
static std::condition_variable g_hangCv;
static bool g_release = false;
static void maybeHang() {
    if (!g_hang.load()) return;
    std::unique_lock<std::mutex> lk(g_hangMx);
    g_hangCv.wait(lk, [] { return g_release; });
}
static void releaseHang() {
    { std::lock_guard<std::mutex> lk(g_hangMx); g_release = true; }
    g_hangCv.notify_all();
    g_hang.store(false);
}

// A file standing in for a usbfs descriptor: 18+ bytes = "attached"; truncated = "gone".
static std::string tmpPath(const char* tag) { return std::string("/tmp/vibe-fdtest-") + tag + "-" + std::to_string(::getpid()); }
static int makeDeviceFd(const char* tag) {
    const std::string p = tmpPath(tag);
    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    const char d[32] = "device-descriptor-0123456789abc";
    if (fd >= 0) (void)!::write(fd, d, sizeof d);
    ::unlink(p.c_str());
    return fd;
}

// ── Stub libhackrf ─────────────────────────────────────────────────────────────────────────────
struct hackrf_device { int id; int fd; bool closedFdOpenAtClose; };
static int g_hrfNext = 1;
static int g_hrfLastFd = -1;
static bool g_hrfFdOpenAtClose = false;
extern "C" {
int hackrf_init() { return HACKRF_SUCCESS; }
hackrf_device_list_t* hackrf_device_list() {
    auto* l = new hackrf_device_list_t{};
    static char sn[] = "0000000000000000a06063c8234e925f";
    static char* sns[] = { sn };
    l->serial_numbers = sns; l->devicecount = 1;
    return l;
}
void hackrf_device_list_free(hackrf_device_list_t* l) { delete l; }
int hackrf_device_list_open(hackrf_device_list_t*, int, hackrf_device** d) {
    *d = new hackrf_device{ g_hrfNext++, -1, false }; note("hrf open_idx " + std::to_string((*d)->id)); return HACKRF_SUCCESS;
}
int hackrf_open_fd(hackrf_device** d, int fd) {
    *d = new hackrf_device{ g_hrfNext++, fd, false }; g_hrfLastFd = fd;
    note("hrf open_fd " + std::to_string((*d)->id)); return HACKRF_SUCCESS;
}
int hackrf_close(hackrf_device* d) {
    g_hrfFdOpenAtClose = d->fd < 0 || fdOpen(d->fd);   // ★ our dup must outlive the handle
    note("hrf close " + std::to_string(d->id)); delete d; return HACKRF_SUCCESS;
}
int hackrf_start_rx(hackrf_device* d, hackrf_sample_block_cb_fn, void*) { note("hrf start " + std::to_string(d->id)); return HACKRF_SUCCESS; }
int hackrf_stop_rx(hackrf_device* d) { note("hrf stop " + std::to_string(d->id)); maybeHang(); return HACKRF_SUCCESS; }
int hackrf_set_freq(hackrf_device* d, const uint64_t hz) { note("hrf freq " + std::to_string(d->id) + " " + std::to_string(hz)); return HACKRF_SUCCESS; }
int hackrf_set_sample_rate(hackrf_device* d, const double r) { note("hrf rate " + std::to_string(d->id) + " " + std::to_string((long long)r)); return HACKRF_SUCCESS; }
uint32_t hackrf_compute_baseband_filter_bw_round_down_lt(const uint32_t bw) { return bw; }
int hackrf_set_baseband_filter_bandwidth(hackrf_device*, const uint32_t) { return HACKRF_SUCCESS; }
int hackrf_set_amp_enable(hackrf_device* d, const uint8_t v) { note("hrf amp " + std::to_string(d->id) + " " + std::to_string(v)); return HACKRF_SUCCESS; }
int hackrf_set_lna_gain(hackrf_device* d, uint32_t v) { note("hrf lna " + std::to_string(d->id) + " " + std::to_string(v)); return HACKRF_SUCCESS; }
int hackrf_set_vga_gain(hackrf_device* d, uint32_t v) { note("hrf vga " + std::to_string(d->id) + " " + std::to_string(v)); return HACKRF_SUCCESS; }
int hackrf_set_antenna_enable(hackrf_device* d, const uint8_t v) { note("hrf bias " + std::to_string(d->id) + " " + std::to_string(v)); return HACKRF_SUCCESS; }
}

// ── Stub libairspy ─────────────────────────────────────────────────────────────────────────────
struct airspy_device { int id; int fd; };
static int g_aspNext = 1;
static int g_aspLastFd = -1;
static bool g_aspFdOpenAtClose = false;
extern "C" {
void airspy_set_thread_hook(void (*)(int)) {}
int airspy_list_devices(uint64_t*, int) { return 0; }
int airspy_open_sn(airspy_device**, uint64_t) { return AIRSPY_ERROR_NOT_FOUND; }
int airspy_open_fd(airspy_device** d, int fd) {
    *d = new airspy_device{ g_aspNext++, fd }; g_aspLastFd = fd;
    note("asp open_fd " + std::to_string((*d)->id)); return AIRSPY_SUCCESS;
}
int airspy_close(airspy_device* d) {
    g_aspFdOpenAtClose = fdOpen(d->fd);
    note("asp close " + std::to_string(d->id)); delete d; return AIRSPY_SUCCESS;
}
int airspy_get_samplerates(airspy_device*, uint32_t* buf, const uint32_t len) {
    if (len == 0) { *buf = 2; return AIRSPY_SUCCESS; }
    buf[0] = 6000000; buf[1] = 3000000; return AIRSPY_SUCCESS;
}
int airspy_set_samplerate(airspy_device* d, uint32_t r) { note("asp rate " + std::to_string(d->id) + " " + std::to_string(r)); return AIRSPY_SUCCESS; }
int airspy_start_rx(airspy_device* d, airspy_sample_block_cb_fn, void*) { note("asp start " + std::to_string(d->id)); return AIRSPY_SUCCESS; }
int airspy_stop_rx(airspy_device* d) { note("asp stop " + std::to_string(d->id)); return AIRSPY_SUCCESS; }
int airspy_board_id_read(airspy_device*, uint8_t* v) { *v = 0; return AIRSPY_SUCCESS; }
int airspy_board_partid_serialno_read(airspy_device*, airspy_read_partid_serialno_t* p) { *p = {}; return AIRSPY_SUCCESS; }
int airspy_set_sample_type(airspy_device*, enum airspy_sample_type) { return AIRSPY_SUCCESS; }
int airspy_set_freq(airspy_device* d, const uint32_t hz) { note("asp freq " + std::to_string(d->id) + " " + std::to_string(hz)); return AIRSPY_SUCCESS; }
int airspy_set_lna_gain(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
int airspy_set_mixer_gain(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
int airspy_set_vga_gain(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
int airspy_set_lna_agc(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
int airspy_set_mixer_agc(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
int airspy_set_linearity_gain(airspy_device* d, uint8_t v) { note("asp linearity " + std::to_string(d->id) + " " + std::to_string(v)); return AIRSPY_SUCCESS; }
int airspy_set_sensitivity_gain(airspy_device* d, uint8_t v) { note("asp sensitivity " + std::to_string(d->id) + " " + std::to_string(v)); return AIRSPY_SUCCESS; }
int airspy_set_rf_bias(airspy_device* d, uint8_t v) { note("asp bias " + std::to_string(d->id) + " " + std::to_string(v)); return AIRSPY_SUCCESS; }
int airspy_set_packing(airspy_device*, uint8_t) { return AIRSPY_SUCCESS; }
const char* airspy_error_name(enum airspy_error) { return "stub error"; }
const char* airspy_board_id_name(enum airspy_board_id) { return "AIRSPY"; }
}

// ── The shared rules ───────────────────────────────────────────────────────────────────────────
static void testRules() {
    std::printf("vibe_usb_recovery.h rules\n");
    using namespace vibe::usbrecovery;
    // ★ Never release what cannot be reopened: fd-opened, OR no index — either is enough.
    CHECK(parkInsteadOfRelease(true, -1), "fd radio with no index must park");
    CHECK(parkInsteadOfRelease(true, 0),  "fd radio must park even with an index recorded");
    CHECK(parkInsteadOfRelease(false, -1), "no index to reopen by must park");
    CHECK(!parkInsteadOfRelease(false, 0), "an index-opened radio may be released");
    // ★ 2 s doubling, capped at 60 s; nothing owed before the first failure.
    const double want[] = { 0, 2, 4, 8, 16, 32, 60, 60, 60 };
    for (int n = 0; n < 9; ++n)
        CHECK(freshFdRetryDelaySecs(n) == want[n], "back-off after %d failures = %.0f, want %.0f",
              n, freshFdRetryDelaySecs(n), want[n]);
    CHECK(freshFdRetryDelaySecs(1000) == 60.0, "the cap holds for ever");
    // ★ Told to re-plug after the fourth failure (~30 s of trying), not before.
    CHECK(!needsReplug(kReplugAfterFailures - 1), "re-plug advice too early");
    CHECK(needsReplug(kReplugAfterFailures), "re-plug advice missing at %d failures", kReplugAfterFailures);
    CHECK(std::string(replugAdvice()).find("unplugging and plugging back in") != std::string::npos,
          "the advice must say what to do");
}

// ── A radio that has gone: give up, and hold the CPU? (2026-10-06, the Sony in standby) ─────────
/* ★ The departure stamp's three moves, exactly as VibeServerRestore makes them: noteRadioGone stamps ONCE
 *   (the first sign wins), noteRadioSeen restarts a stamp that exists (an attach that has not been adopted
 *   yet), noteRadioBack clears it. The VERDICTS are the header's; this only drives them through time. */
struct GoneStamp {
    bool stamped = false; long long since = 0;
    void left(long long t)  { if (!stamped) { stamped = true; since = t; } }
    void seen(long long t)  { if (stamped) since = t; }
    void back()             { stamped = false; }
    long long goneMs(long long t) const { return stamped ? t - since : -1; }
};

static void testGoneRadio() {
    std::printf("gone radio: give up / hold the CPU\n");
    using namespace vibe::usbrecovery;
    const long long S = 1000;
    // ★ No stamp: never give up, always hold.
    CHECK(!giveUpOnGoneRadio(false, -1, false), "nothing on record — nothing to give up on");
    CHECK(holdCpuAwake(false, -1), "no departure — the CPU is held");
    // ★ An earlier boot's stamp gives up, keep-alive or not: a reboot is never a blip.
    CHECK(giveUpOnGoneRadio(true, -1, false), "a reboot is never a blip");
    CHECK(giveUpOnGoneRadio(true, -1, true), "a reboot is never a blip, even with keep radio alive");
    // ★ Stuart's five minutes, to the millisecond either side.
    CHECK(!giveUpOnGoneRadio(true, kRadioBlipWindowMs, false), "exactly five minutes is still a blip");
    CHECK(giveUpOnGoneRadio(true, kRadioBlipWindowMs + 1, false), "past five minutes the server stops");
    CHECK(!giveUpOnGoneRadio(true, 24 * 3600 * S, true), "keep radio alive waits a day, and longer");

    // ★★★ THE SONY, 2026-10-06, replayed: the dongle left at 21:12:14 and was not back for 307 s.
    {
        GoneStamp g; const long long t0 = 0;
        g.left(t0);
        CHECK(holdCpuAwake(g.stamped, g.goneMs(t0 + 2 * S)), "a 2 s re-enumeration keeps the CPU held");
        CHECK(!holdCpuAwake(g.stamped, g.goneMs(t0 + kLetSleepAfterGoneMs)),
              "gone %lld s — the CPU must be let go so the TV can re-power the port", kLetSleepAfterGoneMs / S);
        CHECK(giveUpOnGoneRadio(g.stamped, g.goneMs(t0 + 301 * S), false),
              "keep radio alive OFF: stopped at five minutes, as at 21:17:15");
        CHECK(!giveUpOnGoneRadio(g.stamped, g.goneMs(t0 + 301 * S), true),
              "keep radio alive ON: still waiting at five minutes");
        // …and then back at 307 s, every 12 s, ~10 s at a time, for an hour. Each attach restarts the clock;
        // none is adopted (the worst case). Not ONE tick of that hour may give up, even with keep-alive OFF.
        bool gaveUp = false, heldAtAttach = true;
        for (long long t = t0 + 307 * S; t < t0 + 3600 * S; t += 2 * S) {
            if ((t - (t0 + 307 * S)) % (12 * S) == 0) { g.seen(t); heldAtAttach &= holdCpuAwake(g.stamped, g.goneMs(t)); }
            gaveUp |= giveUpOnGoneRadio(g.stamped, g.goneMs(t), false);
        }
        CHECK(!gaveUp, "a radio that keeps coming back must never be given up on");
        CHECK(heldAtAttach, "every attach must take the CPU back at once");
        g.back();
        CHECK(!giveUpOnGoneRadio(g.stamped, g.goneMs(t0 + 7200 * S), false), "adopted — the stamp is spent");
        CHECK(holdCpuAwake(g.stamped, g.goneMs(t0 + 7200 * S)), "adopted — the CPU is held again");
    }
    // ★ A departure seen twice (the broadcast, then the engine) does not restart the clock.
    {
        GoneStamp g; g.left(0); g.left(200 * S);
        CHECK(g.goneMs(301 * S) == 301 * S, "the first sign of a departure wins");
    }
}

// ── HackRF ─────────────────────────────────────────────────────────────────────────────────────
static void testHackRf() {
    std::printf("HackRfSource\n");
    // An index-opened radio is not an fd radio — release may close it.
    {
        vibe::HackRfSource h; std::string err;
        CHECK(h.open(0, 2e6, 100e6, -1, err), "open by index: %s", err.c_str());
        CHECK(!h.fdOpened(), "an index-opened HackRF must not read as fd-opened");
        CHECK(h.fdAlive(), "an index-opened HackRF has nothing to go dead");
        h.close(); takeLog();
    }
    const int caller = makeDeviceFd("hrf1");
    vibe::HackRfSource h; std::string err;
    CHECK(h.openFd(caller, 2e6, 145e6, -1, err), "openFd: %s", err.c_str());
    CHECK(h.fdOpened(), "openFd must mark the radio fd-opened (park, not release)");
    CHECK(g_hrfLastFd >= 0 && g_hrfLastFd != caller, "libhackrf must get OUR dup, not the caller's fd");
    const int ourDup = g_hrfLastFd;
    h.setAmpEnable(true); h.setLnaGainDb(16); h.setVgaGainDb(20); h.setBiasTee(true);
    CHECK(h.start(err), "start: %s", err.c_str());
    CHECK(h.fdAlive(), "a readable descriptor is alive");
    takeLog();

    // The radio goes: the descriptor reads short. fdAlive says so.
    CHECK(::ftruncate(ourDup, 0) == 0, "truncate");
    CHECK(!h.fdAlive(), "a descriptor that no longer reads the device descriptor is dead");

    // ★ A release that HANGS returns on its 3 s deadline; the handle is abandoned, never double-closed.
    g_release = false; g_hang.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    h.releaseDeadHandle();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took >= 2.5 && took < 5.0, "hung release must return on its deadline (took %.2f s)", took);
    CHECK(!h.isOpen(), "a released handle is not open");
    CHECK(fdOpen(caller), "the caller's fd is never ours to close");
    CHECK(fdOpen(ourDup), "an abandoned handle's fd is leaked, not closed under a stuck call");
    // A tune while dead is remembered for the reopen.
    h.setFrequency(146.5e6);
    releaseHang();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto log = takeLog();
    CHECK(indexOf(log, "hrf close 2") >= 0, "the worker closes the abandoned handle once it returns");
    CHECK(g_hrfFdOpenAtClose, "our dup must still be open when the library closes the handle");
    CHECK(!fdOpen(ourDup), "the worker closes our dup AFTER the handle");

    // ★ The fresh fd: opened on a NEW dup, every stage and the tune put back, the wanted stream restarted.
    const int fresh = makeDeviceFd("hrf2");
    CHECK(h.reopenOnFd(fresh, err), "reopenOnFd: %s", err.c_str());
    CHECK(g_hrfLastFd != fresh && g_hrfLastFd >= 0, "reopen must open on our own dup of the fresh fd");
    CHECK(fdOpen(fresh), "the fresh fd stays the caller's");
    log = takeLog();
    const int op = indexOf(log, "hrf open_fd 3");
    CHECK(op >= 0, "reopen must open the fresh fd");
    CHECK(indexOf(log, "hrf freq 3 146500000") > op, "a tune made while dead must be where it comes back");
    CHECK(indexOf(log, "hrf amp 3 1") > op && indexOf(log, "hrf lna 3 16") > op
          && indexOf(log, "hrf vga 3 20") > op && indexOf(log, "hrf bias 3 1") > op,
          "the four stages must be put back as the owner left them");
    CHECK(indexOf(log, "hrf start 3") > op, "a stream that was wanted must be restarted");
    h.close();
    CHECK(fdOpen(fresh), "close never closes the caller's fd");
    ::close(caller); ::close(fresh);
}

// ── Airspy R2 / Mini ────────────────────────────────────────────────────────────────────────────
static void testAirspy() {
    std::printf("AirspySource\n");
    const int caller = makeDeviceFd("asp1");
    vibe::AirspySource a; std::string err;
    CHECK(a.openFd(caller, 3e6, 100e6, 150, err), "openFd: %s", err.c_str());
    CHECK(a.fdOpened(), "openFd must mark the radio fd-opened (park, not release)");
    CHECK(g_aspLastFd >= 0 && g_aspLastFd != caller, "libairspy must get OUR dup, not the caller's fd");
    const int ourDup = g_aspLastFd;
    // The owner's choices: the SENSITIVITY curve at preset 12, bias-T on.
    a.setPreset(vibe::AirspySource::GainSensitive, 120);
    a.setBiasTee(true);
    CHECK(a.start(err), "start: %s", err.c_str());
    takeLog();

    CHECK(::ftruncate(ourDup, 0) == 0, "truncate");
    CHECK(!a.fdAlive(), "a descriptor that no longer reads the device descriptor is dead");
    a.releaseDeadHandle();
    auto log = takeLog();
    CHECK(indexOf(log, "asp close 1") >= 0, "the dead handle must be closed");
    CHECK(g_aspFdOpenAtClose, "our dup must still be open when the library closes the handle");
    CHECK(!fdOpen(ourDup), "our dup is closed after the handle");
    CHECK(fdOpen(caller), "the caller's fd is never ours to close");
    a.setFrequency(433.92e6);   // a tune while dead

    const int fresh = makeDeviceFd("asp2");
    CHECK(a.reopenOnFd(fresh, err), "reopenOnFd: %s", err.c_str());
    log = takeLog();
    const int op = indexOf(log, "asp open_fd 2");
    CHECK(op >= 0, "reopen must open the fresh fd");
    CHECK(indexOf(log, "asp start 2") > op, "a stream that was wanted must be restarted");
    // ★ finishOpen resets the gain mode from one number — the owner's curve must survive it.
    CHECK(indexOf(log, "asp sensitivity 2 12") > op, "the sensitivity preset must be put back, not linearity");
    CHECK(indexOf(log, "asp linearity 2 15") < 0, "finishOpen's default must not win over the owner's mode");
    CHECK(indexOf(log, "asp bias 2 1") > op, "bias-T must be put back");
    CHECK(indexOf(log, "asp freq 2 433920000") > op, "a tune made while dead must be where it comes back");
    CHECK(a.gainMode() == vibe::AirspySource::GainSensitive, "gain mode kept across the reopen");
    a.close();
    CHECK(fdOpen(fresh), "close never closes the caller's fd");
    ::close(caller); ::close(fresh);
}

int main() {
    testRules();
    testGoneRadio();
    testHackRf();
    testAirspy();
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("all passed\n");
    return 0;
}
