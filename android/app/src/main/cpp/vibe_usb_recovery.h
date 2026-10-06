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

/** ★★★ HOW LONG A RUNNING SERVER WAITS FOR A RADIO THAT HAS GONE — five minutes, Stuart's figure
 *  (2026-09-29: "if unplugged for a decent amount of time dont auto resume the server as there is a good
 *  job the owner may have forgotten they were serving"). VibeServerRestore.RADIO_BLIP_WINDOW_MS is this. */
constexpr long long kRadioBlipWindowMs = 5LL * 60 * 1000;

/** ★★★ STOP WAITING FOR A GONE RADIO? (2026-10-06, the Sony in standby.)
 *  `stamped`  — a departure is on record (the detach broadcast, or the engine finding its handle dead).
 *  `goneMs`   — how long since the radio was last SEEN: since it left, or since it last re-appeared without
 *               being adopted (an attach restarts the clock — see VibeServerRestore.noteRadioSeen); -1 when
 *               the stamp was written in an earlier boot.
 *  `keepAlive`— the owner's "Keep radio alive" switch.
 *  ★★★ KEEP RADIO ALIVE NEVER GIVES UP. The Sony lost its dongle at 21:12:14 in standby, the five minutes
 *      ran out at 21:17:15 and the server stopped AND DISARMED — and the dongle was back six seconds later,
 *      then every twelve seconds after that for the rest of the night, to a server that would never take it
 *      again. On a box whose USB ports come and go by themselves, "gone five minutes" says nothing about the
 *      owner having forgotten: the owner has said so, by turning this on.
 *  ★★ A REBOOT IS NEVER A BLIP, with or without it — an earlier boot's stamp (-1) always gives up. Coming
 *     back after a power cut is the "start when power returns" switch's decision, not this one.
 *  ★ No stamp, nothing to give up on. */
inline bool giveUpOnGoneRadio(bool stamped, long long goneMs, bool keepAlive) {
    if (!stamped) return false;
    if (goneMs < 0) return true;
    if (keepAlive) return false;
    return goneMs > kRadioBlipWindowMs;
}

/** ★★★ HOLD THE CPU AWAKE ONLY WHILE THERE IS A RADIO TO SERVE (2026-10-06).
 *  The Sony re-powers a USB port only when Android tries to SUSPEND, and Android never tries while an app
 *  holds a wake lock. From boot to 21:17:15 the server held one and there was not a single suspend attempt
 *  in the log; the dongle that dropped at 21:12:14 stayed off the bus for the whole five minutes. The server
 *  stopped, the lock went, the first suspend attempt came at 21:17:20 — "libsuspend: error writing to
 *  /sys/power/wakeup_count" — and the dongle re-attached at 21:17:21. Every later attach and detach in that
 *  log sits on the same line as one of those attempts.
 *  ★ So a wake lock held over a radio that has GONE is what keeps it gone. Once it has been away for
 *    kLetSleepAfterGoneMs the lock is let go; the attach that brings the radio back takes it again at once
 *    (RtlTcpServerService's attach watch). A quick re-enumeration (a nudge, ~2 s) never gets that far.
 *  ★ Nothing is lost by letting go: with no radio there is nothing to stream, and the Wi-Fi lock stays. */
constexpr long long kLetSleepAfterGoneMs = 20LL * 1000;
inline bool holdCpuAwake(bool stamped, long long goneMs) {
    if (!stamped || goneMs < 0) return true;
    return goneMs < kLetSleepAfterGoneMs;
}

/** ★ The sentence, per radio — one place, so the log, the admin page, the app and every listener's
 *  banner say the same thing. Plain words: what is wrong, and the one thing that fixes it. */
inline const char* replugAdvice() {
    return " has stopped answering and needs unplugging and plugging back in — this server cannot "
           "reset it from here";
}

}  // namespace usbrecovery
}  // namespace vibe
