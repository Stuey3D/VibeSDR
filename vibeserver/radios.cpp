#include "radios.h"
#include <rtl-sdr.h>
#include "airspyhf_source.h"
#include "sdrplay_source.h"
#include "hackrf_source.h"
#include <algorithm>

namespace vibe {

std::vector<DetectedRadio> detectRadios() {
    std::vector<DetectedRadio> out;

    const uint32_t nRtl = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < nRtl; i++) {
        DetectedRadio r;
        r.driver = "rtlsdr";
        char mfr[256] = {0}, prd[256] = {0}, ser[256] = {0};
        // ★ THE USB DESCRIPTOR, NOT librtlsdr's GUESS. rtlsdr_get_device_name() returns the TUNER
        //   chip's generic name — "Generic RTL2832U OEM" — which is identical across wildly
        //   different dongles and tells the owner nothing about which one they are looking at.
        if (rtlsdr_get_device_usb_strings(i, mfr, prd, ser) == 0 && prd[0]) {
            r.name = mfr[0] ? (std::string(mfr) + " " + prd) : std::string(prd);
            r.serial = ser;
        } else {
            const char* n = rtlsdr_get_device_name(i);
            r.name = n ? n : "RTL-SDR";
        }
        r.driverIndex = (int)i;
        out.push_back(r);
    }

    const int nRsp = SdrplaySource::deviceCount();
    for (int i = 0; i < nRsp; i++) {
        DetectedRadio r;
        r.driver = "sdrplay";
        r.name   = SdrplaySource::deviceName(i);
        // ★ The API's name already ends with the serial ("SDRplay RSP1B 240513CA60"), and there is
        //   no USB serial to fall back on, so take it from there rather than inventing a lookup.
        const size_t sp = r.name.find_last_of(' ');
        if (sp != std::string::npos) r.serial = r.name.substr(sp + 1);
        r.driverIndex = i;
        out.push_back(r);
    }

    const int nAhf = AirspyHfSource::deviceCount();
    for (int i = 0; i < nAhf; i++) {
        DetectedRadio r;
        r.driver = "airspyhf";
        r.name   = AirspyHfSource::deviceName(i);
        // Named like "Airspy HF+ (DD52B980BE4946DA)".
        const size_t open = r.name.find('('), close = r.name.find(')');
        if (open != std::string::npos && close != std::string::npos && close > open + 1)
            r.serial = r.name.substr(open + 1, close - open - 1);
        r.driverIndex = i;
        out.push_back(r);
    }

    /* ★★ HackRF LAST, AND THAT ORDER IS DELIBERATE. detectRadios() decides the order radios
     *    appear in, and an EXPERIMENTAL driver nobody here can test should not push a working
     *    radio down the list on a machine that has both. It is also the only one that can be
     *    absent from the build entirely — deviceCount() returns 0 in that case, so this loop
     *    simply does not run and nothing else has to know. */
    const int nHrf = HackRfSource::deviceCount();
    for (int i = 0; i < nHrf; i++) {
        DetectedRadio r;
        r.driver = "hackrf";
        r.name   = HackRfSource::deviceName(i);
        // Named like "HackRF One (a1b2c3d4)" — the tail of the serial, see deviceName().
        const size_t open = r.name.find('('), close = r.name.find(')');
        if (open != std::string::npos && close != std::string::npos && close > open + 1)
            r.serial = r.name.substr(open + 1, close - open - 1);
        r.driverIndex = i;
        out.push_back(r);
    }

    for (size_t i = 0; i < out.size(); i++) out[i].index = (int)i;
    return out;
}

bool serialsCollide(const std::vector<DetectedRadio>& radios) {
    std::vector<std::string> seen;
    for (const auto& r : radios) {
        // ★ An EMPTY serial is not a collision on its own — it is simply no identity, which the
        //   caller handles differently (fall back to the port) than two radios claiming to be the
        //   same one.
        if (r.serial.empty()) continue;
        if (std::find(seen.begin(), seen.end(), r.serial) != seen.end()) return true;
        seen.push_back(r.serial);
    }
    return false;
}

}  // namespace vibe
