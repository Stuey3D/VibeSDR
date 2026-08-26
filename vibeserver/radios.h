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
    std::string driver;    // "rtlsdr" | "sdrplay" | "airspyhf" | "hackrf" (experimental)
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

/** Everything attached, dongles first, then SDRplay RSPs, then Airspy HF+.
 *  ★ The order is the contract: it is what `--radio` indexes and what the setup screen numbers. */
std::vector<DetectedRadio> detectRadios();

/** ★ True when two or more radios report the SAME serial — which RTL dongles do out of the box
 *  (stock ones are all "00000001"). Settings cannot be pinned to a serial that is not unique, so
 *  the caller must either fall back to the physical port or offer to rename one. */
bool serialsCollide(const std::vector<DetectedRadio>& radios);

}  // namespace vibe
