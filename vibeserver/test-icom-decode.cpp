// test-icom-decode.cpp — vibe_icom_decode.h: what an Icom is set to for each decoder (V12, receive only).
//
// ★ The rules under test (Stuart, 2026-11-04): the listener tunes the signal and presses the decoder, exactly as on an
//   SDR; the server changes only the radio's MODE and FILTER so the audio holds that signal — the dial only from CW /
//   RTTY (where the dial is the signal, not a carrier), and then by the decoder's pitch with the readout unchanged. What
//   one audio channel cannot do (the skimmers, paging, DRM) is not offered.
#include "vibe_icom_decode.h"
#include <cstdio>
#include <string>

using namespace vibeicomdec;
static int checks = 0, failures = 0;
static void ok(bool c, const std::string& what) {
    checks++;
    std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str());
    if (!c) failures++;
}
static std::string show(const Plan& p) {
    if (!p.ok) return "not offered: " + p.why;
    return "mode=" + (p.mode.empty() ? std::string("(keep)") : p.mode) + " fil=" + std::to_string(p.fil)
         + " offset=" + std::to_string(p.offsetHz);
}

int main() {
    std::printf("── what an Icom is set to for a decoder ──\n");
    Plan p;
    // ── SSB-tone decoders from the listener's own sideband: the filter only, never the dial, never DATA ──
    p = plan("spots", "usb", 2, audioCentreFor("spots", 0));
    ok(p.ok && p.mode.empty() && p.fil == 1 && p.offsetHz == 0, "FT8 spots from USB FIL2 → USB kept, FIL1, dial untouched (" + show(p) + ")");
    p = plan("spots", "usbd", 1, 1500);
    ok(p.ok && !p.changes(), "FT8 spots from USB-D FIL1 (an owner's FT8 setup) → nothing changed (" + show(p) + ")");
    p = plan("fsk", "lsb", 3, audioCentreFor("fsk", 1000));
    ok(p.mode.empty() && p.fil == 1 && p.offsetHz == 0, "RTTY from LSB FIL3 → LSB kept (the listener's side), FIL1 (" + show(p) + ")");
    p = plan("wefax", "am", 1, audioCentreFor("wefax", 0));
    ok(p.mode == "usb" && p.fil == 1 && p.offsetHz == 0, "WEFAX from AM → USB FIL1, no move (the AM dial is the carrier too) (" + show(p) + ")");
    p = plan("sstv", "fm", 2, audioCentreFor("sstv", 0));
    ok(p.mode == "usb" && p.fil == 1 && p.offsetHz == 0, "SSTV from FM → USB FIL1 (" + show(p) + ")");
    // ── from CW / RTTY the dial is the SIGNAL: the radio moves by the pitch, the readout does not ──
    p = plan("fsk", "cw", 2, audioCentreFor("fsk", 1000));
    ok(p.mode == "usb" && p.fil == 1 && p.offsetHz == 1000, "RTTY from CW → USB, radio 1000 Hz below what is shown (" + show(p) + ")");
    p = plan("navtex", "rtty", 1, audioCentreFor("navtex", 0));
    ok(p.mode == "lsb" && p.offsetHz == -500, "NAVTEX from the radio's RTTY (LSB-sided) → LSB, radio 500 Hz above (" + show(p) + ")");
    p = plan("psk", "cwr", 1, audioCentreFor("psk", 1200));
    ok(p.mode == "lsb" && p.offsetHz == -1200, "PSK from CW-R at the client's 1200 Hz passband centre → LSB, +1200 Hz (" + show(p) + ")");
    p = plan("spots", "rttyr", 1, audioCentreFor("spots", 0));
    ok(p.mode == "usb" && p.offsetHz == 1500, "FT8 from RTTY-R → USB, the slot's middle at 1500 Hz (" + show(p) + ")");
    // ── CW and the time signals: heard the way the listener tuned them ──
    ok(!plan("cw", "cw", 2, 600).changes(), "CW in CW: nothing changed (the listener's filter is theirs)");
    ok(!plan("cw", "usb", 1, 600).changes(), "CW in USB: nothing changed (a wide USB lists every CW signal in it)");
    p = plan("cw", "am", 1, 600);
    ok(p.mode == "cw" && p.fil == 1 && p.offsetHz == 0, "CW from AM → the radio's CW, FIL1 (" + show(p) + ")");
    ok(!plan("time", "am", 1, 0).changes() && !plan("msf", "cw", 3, 0).changes(), "time signals: whatever they are tuned in");
    // ── FM signals stay FM ──
    ok(!plan("aprs", "fm", 1, 0).changes(), "VHF APRS in FM: kept");
    ok(!plan("dsc", "fm", 1, 0).changes(), "DSC channel 70 in FM: kept");
    p = plan("dsc", "usb", 2, audioCentreFor("dsc", 0));
    ok(p.mode.empty() && p.fil == 1, "HF DSC in USB: FIL1 (" + show(p) + ")");
    // ── what one audio channel cannot do: not offered ──
    for (const char* e : { "cwskim", "pskskim", "paging", "drm" }) {
        p = plan(e, "usb", 1, 1000);
        ok(!p.ok && !p.why.empty(), std::string(e) + " is not offered on an Icom (" + p.why + ")");
    }
    ok(plan("drm", "am", 1, 0).why.find("3.6 kHz") != std::string::npos, "DRM's refusal names the 3.6 kHz SSB filter limit");
    // ── not read yet: nothing judged, nothing changed ──
    ok(plan("fsk", "", 0, 1000).ok && !plan("fsk", "", 0, 1000).changes(), "the radio's mode not read yet: nothing changed");
    // ── the centres ──
    ok(audioCentreFor("navtex", 0) == 500 && audioCentreFor("fsk", 0) == 1000 && audioCentreFor("fsk", 1712) == 1712
       && audioCentreFor("wefax", 0) == 1900, "audio centres: NAVTEX 500, RTTY 1000 (or the message's own), WEFAX 1900");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
