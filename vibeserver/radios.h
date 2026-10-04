// radios.h — what is plugged into this machine, asked once and answered the same way everywhere.
//
// ★★★ ONE LIST, ONE ORDER. The setup screen, `--radio N`, the config file and the supervisor must
//     all mean the same radio by "the second one". They did not: `--device N` reached only the
//     dongle path, discovery was a preference chain (Airspy → SDRplay → RTL), and the TUI asked
//     `lsusb`. Three answers to one question, and with three radios plugged in the one you got was
//     a lottery — it moved the demo Pi off its RSP1B the moment a second radio appeared.
//
// ★★ IDENTITY COMES FROM THE DRIVER, PER DRIVER. Measured on the Pi with all three attached: RTL
//    dongles and the Airspy HF+ carry USB serials, but an SDRplay RSP presents NO USB serial at
//    all — sysfs shows an empty string — and is identified by a serial its own API hands out. Any
//    code that reads identity off the USB bus finds nothing for the RSP and silently falls back to
//    an index, which is how settings end up on the wrong radio.
#pragma once
#include <string>
#include <vector>

namespace vibe {

struct DetectedRadio {
    std::string driver;    // "rtlsdr" | "sdrplay" | "airspyhf" | "hackrf" (experimental) | "airspy" (R2/Mini)
    std::string name;      // human-readable, as the driver describes it
    std::string serial;    // as the DRIVER reports it; may be empty, and may not be unique
    int         index = 0; // position in this flat list — what `--radio N` takes
    /** ★★★ POSITION WITHIN ITS OWN DRIVER, carried from the SAME enumeration as `index`.
     *  Resolving a serial to a flat index and then re-deriving the driver from freshly counted
     *  totals is racy by construction: the counts move. Measured 2026-08-08 starting three radios
     *  at once — the first process claimed the RSP, SDRplay then reported one fewer device, and
     *  both other radios were routed into the Airspy branch and failed with "no Airspy HF+ at that
     *  index". One enumeration, one answer. */
    int         driverIndex = 0;
};

/** Everything attached, dongles first, then SDRplay RSPs, then Airspy HF+, then HackRF, then
 *  Airspy R2 / Mini (each new driver is appended, so no existing flat index moves).
 *  ★ The order is the contract: it is what `--radio` indexes and what the setup screen numbers. */
std::vector<DetectedRadio> detectRadios();

/** ★ True when two or more radios report the SAME serial — which RTL dongles do out of the box
 *  (stock ones are all "00000001"). Settings cannot be pinned to a serial that is not unique, so
 *  the caller must either fall back to the physical port or offer to rename one. */
bool serialsCollide(const std::vector<DetectedRadio>& radios);

/** ★★★ HOW MANY OF EACH KIND ARE ON THE USB BUS, BY VID:PID — WITHOUT OPENING ANY OF THEM.
 *  The witness sdr_presence.h needs: a device another program holds cannot be NAMED by its driver
 *  (that needs an open) but it is still COUNTED here, because reading a device descriptor needs no
 *  open and no claim. `ok` false = the walk itself failed, which proves nothing either way.
 *  ★ RTL uses librtlsdr's own count, which walks its VID:PID table the same way — so "an RTL" here
 *    means exactly what it means to the driver. The others: SDRplay 1df7:any, Airspy HF+ 03eb:800c,
 *    HackRF 1d50:6089/604b/cc15, Airspy R2/Mini 1d50:60a1 (the vendored libraries' own constants). */
struct UsbBusCounts {
    bool ok = false;
    int rtlsdr = 0, sdrplay = 0, airspyhf = 0, hackrf = 0, airspy = 0;
    /** -1 for an unknown driver or a failed walk — "no witness". */
    int forDriver(const std::string& driver) const {
        if (!ok) return -1;
        if (driver == "rtlsdr")   return rtlsdr;
        if (driver == "sdrplay")  return sdrplay;
        if (driver == "airspyhf") return airspyhf;
        if (driver == "hackrf")   return hackrf;
        if (driver == "airspy")   return airspy;
        return -1;
    }
    /** ★★★ HOW MANY OF EACH KIND SOME PROCESS HAS CLAIMED — Linux only, read from sysfs: an
     *  interface claimed through usbfs (libusb, which is what every SDR program uses) has its
     *  `driver` link pointing at "usbfs". Read-only, no open, nothing sent to the device.
     *  ★ RTL is not counted here (librtlsdr's VID:PID table is long and private to it); an RTL's
     *    busy is known exactly instead, from rtlsdr_open's own LIBUSB_ERROR_BUSY. -1 = unknown
     *    (macOS, no sysfs, or an RTL). */
    bool claimedOk = false;
    int claimedSdrplay = 0, claimedAirspyhf = 0, claimedHackrf = 0, claimedAirspy = 0;
    int claimedForDriver(const std::string& driver) const {
        if (!claimedOk) return -1;
        if (driver == "sdrplay")  return claimedSdrplay;
        if (driver == "airspyhf") return claimedAirspyhf;
        if (driver == "hackrf")   return claimedHackrf;
        if (driver == "airspy")   return claimedAirspy;
        return -1;
    }
};
UsbBusCounts usbBusCounts();

}  // namespace vibe
