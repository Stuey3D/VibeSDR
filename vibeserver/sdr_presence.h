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

#include <string>
#include <vector>

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

}  // namespace vibe
