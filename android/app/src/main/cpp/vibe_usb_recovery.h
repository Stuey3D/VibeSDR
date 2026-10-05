// vibe_usb_recovery.h — the Android USB re-plug recovery's RULES, as pure functions (2026-10-05).
//
// ★★ WHY A HEADER OF ITS OWN. The recovery lives in the shim's watchdog, which cannot be run in a
//    test without a radio, a phone and a night of waiting for the fault. The decisions it makes —
//    may this radio be released, how long until the next fresh fd is asked for, when does the owner
//    have to be told to re-plug it — are plain arithmetic on a few facts, so they are written here
//    once and the watchdog, releaseRadio() and vibeserver/test-fd-radio-recovery.cpp all read the
//    SAME rule. A rule written twice is a rule that will disagree with itself.
#pragma once
#include <algorithm>

namespace vibe {
namespace usbrecovery {

/** ★★★ NEVER RELEASE A RADIO THAT CANNOT BE REOPENED FROM HERE — park it instead.
 *  A radio that came in by an Android descriptor has no index (Android forbids enumeration, so the
 *  shim records -1) and nothing below the Java side can get a new descriptor. Releasing it — "release
 *  when idle", the battery floor — closed it for good: Nick's HF+, and the HackRF and R2/Mini fd paths
 *  the same day. Parking keeps the handle and drops the samples at the source.
 *  ★ Either fact is enough: an fd-opened source, OR no index to reopen by. */
inline bool parkInsteadOfRelease(bool fdOpened, int reopenIndex) {
    return fdOpened || reopenIndex < 0;
}

/** ★★★ HOW LONG TO WAIT BEFORE ASKING ANDROID FOR ANOTHER FRESH fd, after `failures` that did not open.
 *  2 s, then doubling, capped at 60 s: 2, 4, 8, 16, 32, 60, 60…
 *  ★★ WHY IT BACKS OFF AT ALL. A WEDGED-BUT-PRESENT HF+ (still plugged in, its abandoned handle's fd
 *     still claiming the interface) fails every fresh fd, and with no back-off Kotlin offered one every
 *     2 s tick for ever: a usbfs open + claim every two seconds against a radio that cannot answer, and
 *     a log that buried the one line that mattered. 0 failures = no wait. */
inline double freshFdRetryDelaySecs(int failures) {
    if (failures <= 0) return 0.0;
    const int shift = std::min(failures - 1, 5);          // 2 << 5 = 64 -> capped below
    return std::min(60.0, 2.0 * (double)(1 << shift));
}

/** ★★ AFTER THIS MANY FAILED ADOPTIONS, SAY PLAINLY THAT THE RADIO NEEDS UNPLUGGING AND PLUGGING BACK
 *  IN. Four is ~30 s of trying (2 + 4 + 8 + 16): long enough that a radio still re-enumerating has had
 *  its chance, short enough that the owner reads it while they still remember touching the cable. */
constexpr int kReplugAfterFailures = 4;
inline bool needsReplug(int failures) { return failures >= kReplugAfterFailures; }

/** ★ The sentence, per radio — one place, so the log, the admin page, the app and every listener's
 *  banner say the same thing. Plain words: what is wrong, and the one thing that fixes it. */
inline const char* replugAdvice() {
    return " has stopped answering and needs unplugging and plugging back in — this server cannot "
           "reset it from here";
}

}  // namespace usbrecovery
}  // namespace vibe
