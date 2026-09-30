// VibeServer — the Raspberry Pi's FIRMWARE clock and throttle flags, read through the VideoCore
// mailbox (/dev/vcio), the same property interface `vcgencmd` uses.
//
// ★★★ WHY THIS EXISTS — MEASURED ON THE PI 500 AT FULL LOAD, 2026-09-29/30:
//     `vcgencmd measure_clock arm` flipped 2400 ↔ 1000 MHz in step with `vcgencmd get_throttled`
//     0x50005 ↔ 0x50000 (under-voltage, EXT5V 4.72–4.78 V), while EVERY core's scaling_cur_freq
//     said 2400. sysfs reports the clock the KERNEL ASKED FOR; the firmware then delivers less
//     when the supply sags, and tells nobody but the mailbox. So the throttle snail and the admin
//     CPU CLOCK tile, both reading sysfs, showed 2400 and never fired through a night of real
//     throttling. The earlier conclusion (2026-09-25, "the bits are NOT evidence of a cap — every
//     core sat at its full 2400 MHz") was drawn from the very number that was lying.
//
// ★★ NO ROOT, NO SHELL. Stuart: "we will avoid root". The ioctl is issued directly — a `vcgencmd`
//    fork once a second per radio process would cost more than everything else the sampler does.
// ★★★ TWO DOORS, AND THE ONE WE CAN OPEN IS THE SECOND. The first B6 build assumed /dev/vcio was
//     root:video 0660. MEASURED on the Pi 500 (kernel 6.18, 2026-09-30): /dev/vcio is root:root
//     0600; Pi OS's udev rule gives `video` only /dev/vcio_gencmd (and _crypto), and the kernel lets
//     that node carry ONE tag — GET_GENCMD_RESULT, the text interface `vcgencmd` itself uses. The
//     raw property tags there fail EPERM. So: the property tags on /dev/vcio first (root, or a box
//     whose udev opens it up), then the SAME questions `vcgencmd` asks, as text, on
//     /dev/vcio_gencmd — "measure_clock arm", "get_throttled", "get_config arm_freq". The package
//     adds the service user to `video` (debian/postinst) and the units allow both devices.
// ★★ SILENT FALLBACK. Not a Pi, not in `video` yet (the group takes effect on the next service
//    start), a container, Android, a Mac: read() returns ok=false and every caller carries on with
//    what it did before. A missing firmware reading is not a fault worth a log line per second.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <mutex>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace vibevcio {

/** Firmware property tags (raspberrypi/firmware wiki, "Mailbox property interface"). */
enum : uint32_t {
    TAG_GET_CLOCK_RATE          = 0x00030002,   // the rate the firmware was ASKED for
    TAG_GET_MAX_CLOCK_RATE      = 0x00030004,
    TAG_GET_THROTTLED           = 0x00030046,
    TAG_GET_CLOCK_RATE_MEASURED = 0x00030047,   // what `vcgencmd measure_clock arm` reports
    TAG_GET_GENCMD_RESULT       = 0x00030080,   // a `vcgencmd` command as text — /dev/vcio_gencmd
    CLOCK_ARM                   = 3,
};

/** get_throttled's bits. ★ Only the "NOW" half (0-3) is used for any verdict — the sticky half
 *  (16-19) says "at some point since boot/since last cleared", and the kernel's own rpi_volt
 *  driver clears the sticky under-voltage bit every poll, so it is not ours to reason about. */
enum : uint32_t {
    THR_UNDERVOLT_NOW = 1u << 0,
    THR_ARM_CAPPED    = 1u << 1,
    THR_THROTTLED     = 1u << 2,
    THR_SOFT_TEMP     = 1u << 3,
};

struct Reading {
    bool ok = false;                 ///< the mailbox answered at all
    long long armHz = -1;            ///< MEASURED ARM clock (falls back to the set rate)
    long long armSetHz = -1;         ///< the rate the kernel asked the firmware for
    long long armMaxHz = -1;         ///< the firmware's maximum ARM clock
    long long throttled = -1;        ///< get_throttled bits, -1 = not readable
    bool measured = false;           ///< armHz came from GET_CLOCK_RATE_MEASURED
    bool underVoltNow() const { return throttled >= 0 && (throttled & THR_UNDERVOLT_NOW); }
    bool softTempNow()  const { return throttled >= 0 && (throttled & THR_SOFT_TEMP); }
    bool cappedNow()    const { return throttled >= 0 && (throttled & (THR_ARM_CAPPED | THR_THROTTLED)); }
};

/** ★ One property call, in place: `buf` is a complete mailbox message (size, code, tags…, end).
 *  Returns 0 on success. THE TEST HOOK: a fake file cannot answer an ioctl, so a test swaps the
 *  whole transport for a function that plays the firmware (see test-health-snail.cpp). */
using MboxFn = int (*)(uint32_t* buf);
inline MboxFn& mboxOverride() { static MboxFn f = nullptr; return f; }
/** "/dev/vcio" in life. Settable so a test can point the REAL transport at a path that fails. */
inline std::string& devPath() { static std::string p = "/dev/vcio"; return p; }
/** The node `video` may open — it carries TAG_GET_GENCMD_RESULT only. Settable for the same reason. */
inline std::string& gencmdPath() { static std::string p = "/dev/vcio_gencmd"; return p; }

namespace detail {
#if defined(__linux__)
/** ★ The fd is opened once and kept: open() per sample would be most of the cost. A failed open is
 *  retried at most once a minute — the group is granted by the package and takes effect when the
 *  service restarts, so a process that could not open it will not suddenly be able to; the retry
 *  is only for a path a test swapped, or a device that appeared late at boot. */
/** One device node: the fd, kept open, and when opening it last failed. */
struct Dev {
    std::mutex m;
    int fd = -1;
    std::string openedPath;
    std::chrono::steady_clock::time_point lastTry{};
};
inline int mboxOn(Dev& d, const std::string& path, uint32_t* buf) {
    std::lock_guard<std::mutex> lk(d.m);
    int& fd = d.fd;
    if (fd >= 0 && d.openedPath != path) { ::close(fd); fd = -1; }
    if (fd < 0) {
        const auto now = std::chrono::steady_clock::now();
        if (d.lastTry.time_since_epoch().count() != 0 && d.openedPath == path
            && now - d.lastTry < std::chrono::seconds(60)) return -1;
        d.lastTry = now; d.openedPath = path;
        // ★ O_RDONLY, as the firmware's own mbox_open() does: the property ioctl needs no write
        //   access, so DeviceAllow= can stay read-only.
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
    }
    // IOCTL_MBOX_PROPERTY = _IOWR(100, 0, char*) — from the firmware's userland mailbox.h.
    if (::ioctl(fd, _IOWR(100, 0, char*), buf) < 0) return -1;
    return 0;
}
inline int realMbox(uint32_t* buf)       { static Dev d; return mboxOn(d, devPath(), buf); }
inline int realGencmdMbox(uint32_t* buf) { static Dev d; return mboxOn(d, gencmdPath(), buf); }
#else
inline int realMbox(uint32_t*) { return -1; }
inline int realGencmdMbox(uint32_t*) { return -1; }
#endif

/** Ask for ONE tag carrying `nIn` request words and room for `nOut` answer words. Returns the
 *  number of answer words the firmware filled (0 = no answer). */
inline int property(uint32_t tag, const uint32_t* in, int nIn, uint32_t* out, int nOut) {
    const int vals = nIn > nOut ? nIn : nOut;
    uint32_t buf[32];
    if (vals > 16) return 0;
    std::memset(buf, 0, sizeof buf);
    int i = 0;
    buf[i++] = 0;                          // total size, filled below
    buf[i++] = 0;                          // request code
    buf[i++] = tag;
    buf[i++] = (uint32_t)(vals * 4);       // value buffer size
    buf[i++] = (uint32_t)(nIn * 4);        // request length
    const int valAt = i;
    for (int k = 0; k < vals; k++) buf[i++] = k < nIn ? in[k] : 0;
    buf[i++] = 0;                          // end tag
    buf[0] = (uint32_t)(i * 4);
    MboxFn f = mboxOverride();
    const int rc = f ? f(buf) : realMbox(buf);
    if (rc != 0) return 0;
    // ★ Both answers must say "handled": the message (0x80000000) and the tag (bit 31 of its
    //   length word). An unknown tag — older firmware without MEASURED — leaves bit 31 clear.
    if (buf[1] != 0x80000000u) return 0;
    if (!(buf[4] & 0x80000000u)) return 0;
    const int got = (int)((buf[4] & 0x7fffffffu) / 4);
    const int n = got < nOut ? got : nOut;
    for (int k = 0; k < n; k++) out[k] = buf[valAt + k];
    return n;
}
/** ★ One `vcgencmd` command, the way vcgencmd sends it (raspberrypi/utils vcgencmd.c): a 1024-byte
 *  value buffer, word 0 of it the firmware's error code, the text from word 1. Returns false on any
 *  refusal. The test fake answers this tag through the same override as the property tags. */
inline bool gencmd(const char* cmd, std::string& out) {
    constexpr int kMax = 1024;
    uint32_t buf[(kMax >> 2) + 7];
    std::memset(buf, 0, sizeof buf);
    const size_t len = std::strlen(cmd);
    if (len + 1 >= (size_t)kMax) return false;
    int i = 0;
    buf[i++] = 0;                          // total size, filled below
    buf[i++] = 0;                          // request code
    buf[i++] = TAG_GET_GENCMD_RESULT;
    buf[i++] = kMax;                       // value buffer size
    buf[i++] = 0;                          // request length
    buf[i++] = 0;                          // the firmware's error code comes back here
    std::memcpy(buf + i, cmd, len + 1);
    i += kMax >> 2;
    buf[i++] = 0;                          // end tag
    buf[0] = (uint32_t)(i * 4);
    MboxFn f = mboxOverride();
    const int rc = f ? f(buf) : realGencmdMbox(buf);
    if (rc != 0 || buf[1] != 0x80000000u || buf[5] != 0) return false;
    const char* txt = reinterpret_cast<const char*>(buf + 6);
    out.assign(txt, ::strnlen(txt, kMax - 8));
    return !out.empty();
}
/** The number after the '=' of "frequency(0)=1000015168" / "throttled=0x50005" / "arm_freq=2400". */
inline bool afterEquals(const std::string& s, long long& v, int base) {
    const size_t eq = s.find('=');
    if (eq == std::string::npos) return false;
    const char* p = s.c_str() + eq + 1;
    char* end = nullptr;
    const long long x = std::strtoll(p, &end, base);
    if (end == p) return false;
    v = x;
    return true;
}

inline long long clockTag(uint32_t tag) {
    const uint32_t in[2] = { CLOCK_ARM, 0 };
    uint32_t out[2] = { 0, 0 };
    if (property(tag, in, 2, out, 2) < 2 || out[0] != CLOCK_ARM || out[1] == 0) return -1;
    return (long long)out[1];
}
}  // namespace detail

/** One reading. Cheap — three or four ioctls — and safe to call once a second from several threads. */
/** ★ The second door — see the note at the top. The same three figures, as `vcgencmd` text. No "asked
 *  for" figure exists here; callers already take that from sysfs. */
inline Reading readGencmd() {
    Reading r;
    std::string t;
    long long v = 0;
    if (!detail::gencmd("measure_clock arm", t) || !detail::afterEquals(t, v, 10) || v <= 0) return r;
    r.armHz = v; r.measured = true;
    // get_config reports MHz; a firmware that will not say leaves the maximum unknown, and a
    // reading without a maximum is no reading (nobody can say it is BELOW anything).
    if (detail::gencmd("get_config arm_freq", t) && detail::afterEquals(t, v, 10) && v > 0)
        r.armMaxHz = v * 1000000LL;
    if (detail::gencmd("get_throttled", t) && detail::afterEquals(t, v, 16) && v >= 0)
        r.throttled = v;
    r.ok = r.armHz > 0 && r.armMaxHz > 0;
    return r;
}

inline Reading read() {
    Reading r;
    r.armMaxHz = detail::clockTag(TAG_GET_MAX_CLOCK_RATE);
    if (r.armMaxHz <= 0) return readGencmd();          // not root: the node `video` may open
    r.armSetHz = detail::clockTag(TAG_GET_CLOCK_RATE);
    const long long meas = detail::clockTag(TAG_GET_CLOCK_RATE_MEASURED);
    if (meas > 0) { r.armHz = meas; r.measured = true; }
    else          r.armHz = r.armSetHz;
    {
        // ★ Request value 0: clear NO sticky bits. The kernel's rpi_volt driver passes 0xffff and
        //   owns the sticky under-voltage latch; clearing it from here would steal its evidence.
        const uint32_t in[1] = { 0 };
        uint32_t out[1] = { 0 };
        if (detail::property(TAG_GET_THROTTLED, in, 1, out, 1) >= 1) r.throttled = (long long)out[0];
    }
    r.ok = r.armHz > 0;
    return r;
}

}  // namespace vibevcio
