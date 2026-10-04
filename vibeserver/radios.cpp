#include "radios.h"
#include <rtl-sdr.h>
#include "airspyhf_source.h"
#include "sdrplay_source.h"
#include "hackrf_source.h"
#include "airspy_source.h"
#include <algorithm>
#if __has_include(<libusb.h>)
#  include <libusb.h>
#  define VIBE_HAVE_LIBUSB_H 1
#endif

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

    /* ★★★ AIRSPY R2 / MINI — AFTER THE HACKRF, AND THAT ORDER IS THE CONTRACT TOO. Every
     *     VibeServer build must drive every radio we support (Stuart, 2026-09-28); the R2/Mini
     *     worked on Android from 2026-09-22 and the desktop never enumerated it at all, so a Pi
     *     with one attached reported "no radio". Appending it LAST keeps every flat index that
     *     existed before it exactly where it was — a config that says `--radio 2` still means
     *     the same hardware on a machine that gains an R2.
     *  ★ A different driver from the HF+ ("airspyhf"): different library, different gain model.
     *    Absent from the build = deviceCount() 0, so this loop does not run. */
    const int nAsp = AirspySource::deviceCount();
    for (int i = 0; i < nAsp; i++) {
        DetectedRadio r;
        r.driver = "airspy";
        r.name   = AirspySource::deviceName(i);
        // Named like "Airspy (0123456789ABCDEF)" — libairspy's 64-bit serial, see deviceName().
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

UsbBusCounts usbBusCounts() {
    UsbBusCounts c;
#if VIBE_HAVE_LIBUSB_H
    /* ★★ A PRIVATE CONTEXT, and nothing but descriptors. libusb_get_device_descriptor() reads the
     *    copy the OS cached at enumeration (Linux: sysfs/usbfs, macOS: IOKit) — no open, no control
     *    transfer, no claim — so this cannot disturb a radio somebody is streaming. That matters:
     *    repeated USB probing has knocked running radios over before, and this runs only on demand. */
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return c;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n >= 0) {
        c.ok = true;
        for (ssize_t i = 0; i < n; i++) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
            if (d.idVendor == 0x1df7) c.sdrplay++;
            else if (d.idVendor == 0x03eb && d.idProduct == 0x800c) c.airspyhf++;
            else if (d.idVendor == 0x1d50 && (d.idProduct == 0x6089 || d.idProduct == 0x604b
                                              || d.idProduct == 0xcc15)) c.hackrf++;
            else if (d.idVendor == 0x1d50 && d.idProduct == 0x60a1) c.airspy++;
        }
        libusb_free_device_list(list, 1);
    }
    libusb_exit(ctx);
    // ★ librtlsdr's own table (dozens of VID:PIDs) — counted by the same descriptor walk inside it.
    if (c.ok) c.rtlsdr = (int)rtlsdr_get_device_count();
#endif
    return c;
}

}  // namespace vibe
