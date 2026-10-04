// sdr_presence.h — IS A CONFIGURED RADIO UNPLUGGED, OR MERELY IN SOMEBODY ELSE'S HANDS?
//
// ★★★ STUART'S HARD RULE (2026-10-04): never tell an owner a radio is unplugged when it has been
//     LENT to another program (released for OpenWebRX, borrowed by a decoder) or is otherwise held
//     by someone else. A false "check the USB connection" sends the owner crawling behind the
//     machine for a cable that is fine, and "remove it" on that screen throws away every setting
//     of a radio that was only on loan.
//
// ★★ WHY THE DRIVER'S OWN LIST CANNOT ANSWER IT. detectRadios() asks each driver who is there, and
//    the drivers answer "who can I name", not "who is plugged in":
//      • SDRplay's API omits an RSP another process has selected — absent from GetDevices is
//        exactly what "lent to OpenWebRX" looks like.
//      • librtlsdr, libairspyhf, libairspy and libhackrf read the serial by OPENING the device;
//        when that open fails (held, permissions, a wedged chip) the serial is empty or the
//        device is skipped outright.
//    So "not in the driver's list" means "could not be NAMED", never "is not there".
//
// ★★★ THE BUS IS THE WITNESS. Counting devices by VID:PID on the raw USB descriptor walk needs no
//     open and no claim, so a held device is still counted. Then, per driver:
//        on the bus  −  named by the driver  −  held by our own running radios that the driver
//        cannot see
//     If that leaves a device unaccounted for, there IS a radio of this kind on the bus that
//     nobody could name — and it might be ours. That is UNCERTAIN, not absent. Only when every
//     device of the kind is accounted for, and none of them is ours, is the radio PROVABLY absent.
//  ★ The arithmetic is format-agnostic on purpose: the driver's serial spelling ("Airspy HF+
//    (DD52…)", HackRF's 8-character tail) never has to be matched against a USB string descriptor.
//
// ★ Header-only and free of any driver so test-config-radios can drive it without hardware.
#pragma once

#include "radios.h"

#include <cstdio>
#include <string>
#include <vector>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

namespace vibe {

enum class Presence {
    Attached,   ///< the driver names it right now
    Absent,     ///< PROVABLY not on the bus — the only state that may say "check the USB connection"
    Busy,       ///< known to be held by someone else (named on the bus, but it could not be opened)
    Uncertain,  ///< cannot tell unplugged from in use — a soft notice, never the alarm
};

inline const char* presenceName(Presence p) {
    switch (p) {
        case Presence::Attached:  return "attached";
        case Presence::Absent:    return "absent";
        case Presence::Busy:      return "busy";
        case Presence::Uncertain: return "uncertain";
    }
    return "uncertain";
}

/**
 * @param driver            the radio's driver ("rtlsdr", "sdrplay", "airspyhf", "hackrf", "airspy")
 * @param serial            the radio's configured serial
 * @param detected          detectRadios(), taken just now
 * @param onBus             devices of this driver on the raw USB walk; NEGATIVE = the walk failed
 * @param heldUnseen        our OWN running radios of this driver that `detected` does not name
 *                          (an RSP a sibling process is streaming is invisible to the API)
 */
inline Presence decidePresence(const std::string& driver, const std::string& serial,
                               const std::vector<DetectedRadio>& detected,
                               int onBus, int heldUnseen) {
    int named = 0;
    for (const auto& d : detected) {
        if (d.driver != driver) continue;
        if (!serial.empty() && d.serial == serial) return Presence::Attached;
        if (!d.serial.empty()) named++;
    }
    // ★ No witness, no verdict. A walk that failed proves nothing about the bus.
    if (onBus < 0 || serial.empty()) return Presence::Uncertain;
    const int unaccounted = onBus - named - (heldUnseen > 0 ? heldUnseen : 0);
    return unaccounted > 0 ? Presence::Uncertain : Presence::Absent;
}

/** ★★★ WHO HAS CLAIMED WHICH RADIO, FROM sysfs (Linux) — never by asking the device.
 *  `<root>/<dev>` carries idVendor/idProduct; each interface `<dev>:<cfg>.<if>` has a `driver` link,
 *  and an interface a program claimed through libusb (every SDR program) points at "usbfs".
 *  Readable by any user, the sandboxed service included: it is sysfs, not /proc, so
 *  ProtectProc=invisible does not hide it. It says THAT something holds the radio, never WHO.
 *  ★ RTL is not counted (librtlsdr's VID:PID table is its own); an RTL's busy comes exactly from
 *    rtlsdr_open's LIBUSB_ERROR_BUSY instead.
 *  ★ Header-only and root-parameterised so the test can point it at a fake tree.
 *  Returns false when the directory cannot be read (= unknown). */
inline bool sysfsClaimedCounts(const std::string& root, int& sdrplay, int& airspyhf, int& hackrf, int& airspy) {
    sdrplay = airspyhf = hackrf = airspy = 0;
    DIR* d = ::opendir(root.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') names.push_back(e->d_name);
    ::closedir(d);
    auto readHex = [](const std::string& path) -> int {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return -1;
        unsigned v = 0; const int n = std::fscanf(f, "%x", &v); std::fclose(f);
        return n == 1 ? (int)v : -1;
    };
    const std::string base = root + "/";
    for (const auto& dev : names) {
        if (dev.find(':') != std::string::npos) continue;           // an interface, not a device
        const int vid = readHex(base + dev + "/idVendor"), pid = readHex(base + dev + "/idProduct");
        if (vid < 0 || pid < 0) continue;
        int* slot = nullptr;
        if (vid == 0x1df7) slot = &sdrplay;
        else if (vid == 0x03eb && pid == 0x800c) slot = &airspyhf;
        else if (vid == 0x1d50 && (pid == 0x6089 || pid == 0x604b || pid == 0xcc15)) slot = &hackrf;
        else if (vid == 0x1d50 && pid == 0x60a1) slot = &airspy;
        if (!slot) continue;
        for (const auto& itf : names) {
            if (itf.rfind(dev + ":", 0) != 0) continue;
            char link[PATH_MAX] = {0};
            const ssize_t n = ::readlink((base + itf + "/driver").c_str(), link, sizeof link - 1);
            if (n <= 0) continue;
            const std::string drv(link, (size_t)n);
            const size_t sl = drv.find_last_of('/');
            if ((sl == std::string::npos ? drv : drv.substr(sl + 1)) == "usbfs") { (*slot)++; break; }
        }
    }
    return true;
}

/** ★★★ DO WE *KNOW* ANOTHER PROGRAM HAS THIS RADIO? Stuart's rule for saying "in use by another app"
 *  anywhere a listener can see it: only when we KNOW, never as a guess — unknown says nothing.
 *  Known when:
 *    • an RTL's own open said LIBUSB_ERROR_BUSY (`rtlBusy`) — the kernel's word, about this device; or
 *    • (Linux) a device of this kind is claimed by a program that is not one of OUR running radios,
 *      AND it is the only device of this kind on the bus that our running radios do not account
 *      for — so it is this radio's, held by someone else.
 *  @param onBus       devices of this driver on the bus (UsbBusCounts::forDriver), <0 unknown
 *  @param claimed     devices of this driver claimed through usbfs (claimedForDriver), <0 unknown
 *  @param runningOurs OUR other radios of this driver that are running (each holds one device)
 *  ★ The one case it cannot see through: ours unplugged AND a different radio of the same kind,
 *    not in our config, plugged in AND held by another program. Rare enough to accept; the
 *    alternative is never saying it at all on an RSP, the radio people most often lend out. */
inline bool knownInUseElsewhere(bool rtlBusy, int onBus, int claimed, int runningOurs) {
    if (rtlBusy) return true;
    if (onBus < 0 || claimed < 0) return false;
    const int ours = runningOurs > 0 ? runningOurs : 0;
    return claimed - ours >= 1 && onBus == ours + 1;
}

}  // namespace vibe
