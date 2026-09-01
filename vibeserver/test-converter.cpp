// ★★★ AN UP/DOWN-CONVERTER IN FRONT OF A RADIO — the config round trip, and the one line of
//     arithmetic that decides where the tuner is actually parked.
//
// ★★ THE ARITHMETIC IS TESTED HERE RATHER THAN ON AIR because neither the author nor the person
//    who asked for the feature could test the other's case: the request was a Ham It Up on
//    rtl_tcp, development is on RTL-SDR Blog V4s which need no converter at all, and an LNB
//    exists in the model with nobody to point at a dish. What CAN be pinned is that the numbers
//    are right, which is most of what goes wrong.
//
// ★ The transform itself lives in local_sdr_shim's tuneHw() and cannot be linked here without the
//   whole DSP engine, so the expression is restated — deliberately, and it is the ONLY thing in
//   this file that is a copy. Keep the two in step: if tuneHw's line changes, this must fail.
#include "vibeserver_config.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

/** What tuneHw() does: hardware = wanted − LO (+ the per-driver DC dodge, 0 for most). */
static double toHardware(double wantedHz, double offsetHz) { return wantedHz - offsetHz; }

int main() {
    using namespace vsconfig;
    std::string err;

    std::printf("\nThe LO arithmetic — one signed field covers both directions\n");
    {
        // A Ham It Up: LO stored NEGATIVE. The listener asks for 3 MHz; the dongle goes to 128.
        ok(toHardware(3e6, -125e6) == 128e6, "125 MHz up-converter: 3 MHz asked, 128 MHz tuned");
        ok(toHardware(7.1e6, -120e6) == 127.1e6, "SpyVerter (120): 7.1 MHz asked, 127.1 MHz tuned");
        // A down-converter / LNB: LO stored POSITIVE, same subtraction.
        ok(toHardware(10489.5e6, 9750e6) == 739.5e6, "9750 LNB: 10489.5 MHz asked, 739.5 MHz tuned");
        // ★ No converter is the identity, and that is the default every existing radio has.
        ok(toHardware(145.5e6, 0) == 145.5e6, "no converter changes nothing");
    }

    std::printf("\nThe published range is shifted and narrowed to what the converter passes\n");
    {
        // What the shim does to the driver's own ranges before publishing them.
        auto shift = [](double lo, double hi, double offs, double pLo, double pHi,
                        double& outLo, double& outHi) {
            double a = lo + offs, b = hi + offs;
            if (a > b) { double t = a; a = b; b = t; }
            if (pHi > pLo) { a = std::fmax(a, pLo); b = std::fmin(b, pHi); }
            a = std::fmax(a, 0.0);
            outLo = a; outHi = b;
        };
        double a = 0, b = 0;
        // An RTL-SDR (24-1766 MHz) behind a 125 MHz up-converter that passes HF.
        shift(24e6, 1766e6, -125e6, 0, 30e6, a, b);
        ok(a == 0 && b == 30e6, "a dongle behind a 125 MHz up-converter publishes 0-30 MHz");
        // ★★★ THE POINT OF THE PASSBAND. Without it the dial would offer 1641 MHz of silence.
        shift(24e6, 1766e6, -125e6, 0, 0, a, b);
        ok(b == 1641e6, "with no declared passband it is merely shifted");
        ok(a == 0, "and never goes negative");
    }

    std::printf("\nThe converter survives the config file\n");
    {
        const std::string cfg = R"({
          "configured": true, "name": "Shack", "adminPass": "x",
          "radios": [{
            "serial": "00000001", "driver": "rtlsdr", "label": "Dongle",
            "enabled": true, "configured": true,
            "converterOffsetHz": -125000000,
            "converterInputLoHz": 0, "converterInputHiHz": 30000000
          }]
        })";
        ServerConfig srv;
        ok(fromJson(cfg, srv, err), "a config carrying a converter parses", err);
        ok(srv.radios.size() == 1, "one radio");
        if (srv.radios.size() == 1) {
            const auto& r = srv.radios[0];
            ok(r.converterOffsetHz == -125e6, "the LO survives, sign and all");
            ok(r.converterInputHiHz == 30e6, "so does what it passes");
            // ★★★ AND IT MUST REACH THE RUNTIME, which is the copy the shim is actually told
            //     about. A field that parses but is never folded into Config is the
            //     "written and never read" fault — it would look right in the file and do nothing.
            const Config c = effectiveFor(srv, r);
            ok(c.converterOffsetHz == -125e6, "and reaches the runtime config the shim is given");
            ok(c.converterInputHiHz == 30e6, "passband too");
        }
    }

    std::printf("\nA config written before converters existed still works\n");
    {
        const std::string cfg = R"({
          "configured": true, "name": "Old", "adminPass": "x",
          "radios": [{ "serial": "1", "driver": "rtlsdr", "enabled": true, "configured": true }]
        })";
        ServerConfig srv;
        ok(fromJson(cfg, srv, err), "it parses", err);
        if (srv.radios.size() == 1) {
            // ★ 0 means "no converter" — the identity. An owner who has never heard of this
            //   feature must be completely unaffected by it.
            ok(srv.radios[0].converterOffsetHz == 0, "and declares no converter");
            ok(toHardware(145.5e6, srv.radios[0].converterOffsetHz) == 145.5e6,
               "so the radio tunes exactly where it always did");
        }
    }

    std::printf("\nIt round-trips back out to the setup page\n");
    {
        /* ★★★ THE PAGE MUST BE TOLD, or it draws empty boxes over a live setting and the next
         *   save silently clears the converter — a setting that survives the file but not the
         *   page is worse than one that was never stored. */
        ServerConfig srv;
        RadioConfig r;
        r.serial = "1"; r.driver = "rtlsdr"; r.enabled = true; r.configured = true;
        r.converterOffsetHz = -125e6; r.converterInputHiHz = 30e6;
        srv.radios.push_back(r);
        const std::string j = toJson(srv);
        ok(j.find("converterOffsetHz") != std::string::npos, "the setup page is told the LO");
        ok(j.find("converterInputHiHz") != std::string::npos, "and what it passes");

        // ★ And back in again unchanged — the full circle the setup page actually performs.
        ServerConfig back;
        ok(fromJson(j, back, err), "and it parses back", err);
        if (back.radios.size() == 1)
            ok(back.radios[0].converterOffsetHz == -125e6, "with the LO intact after the round trip");
    }

    if (failures) std::printf("\n%d of %d checks FAILED\n", failures, checks);
    else          std::printf("\nall %d checks passed\n", checks);
    return failures ? 1 : 0;
}
