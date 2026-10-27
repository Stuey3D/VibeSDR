// test-bg-band.cpp — BACKGROUND DECODING: A BAND DIGITAL-SPOTS VFO (V12, 2026-10-27).
//
// Stuart (2026-10-27): "I'd like to add NDB to the background decoding … Also full band digital modes too." A profile's
// VFO may now run "Digital spots: Band (summary)" — the listener's own Band mode (every digital mode on the band, behind
// its CPU governor) on the band the VFO's frequency is in, kept as ONE summary card for the band, as FT8's is.
//
//   · the config: the decoder, its demodulator, the save's refusals in words (a frequency on no band; the band's modes
//     outside the window), and the span it is cut on;
//   · end to end without a server: synthetic 40 m IQ (two FT8 stations on 7.074) at 250 kS/s centred 7.000 MHz, through
//     BgRunner's channeliser and the Band slice → DecoderHost (Band) → the summary card ("Digital spots summary · Band
//     40 m"), stamped by the capture's own clock; the admin line; the process CPU per second of capture;
//   · the same band with nothing on it (the activity gate leaves every window undecoded) and its CPU;
//   · switched off on this receiver (the owner's Band switch / the benchmark's default): shown paused with the reason,
//     takes no slot, and a NAVTEX VFO beside it still runs.
// ★ Silent: nothing is played.
#include "vibe_bgdecode.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }
static double cpuSecs() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/bgband-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::vector<uint8_t> ft8Tones(const char* msg) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) return {};
    std::vector<uint8_t> t(FT8_NN); ft8_encode(m.payload, t.data());
    return t;
}
/** One FT8 transmission into the capture, `offHz` from its centre, starting `startSec` in (continuous-phase FSK). */
static void addFt8(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::vector<uint8_t>& tones, double amp) {
    const double sym = FT8_SYMBOL_PERIOD, spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * fs), ns = (size_t)std::llround(sym * fs);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / fs;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}

static void config() {
    std::printf("── the config: \"Digital spots: Band (summary)\" ──\n");
    bg::installBandSpan();
    const bg::DecoderDef* d = bg::decoderDef("spots-band");
    ok(d && std::string(d->label) == "Digital spots: Band (summary)", "the decoder is listed");
    ok(bg::decoderFitsDemod("spots-band", "usb") && !bg::decoderFitsDemod("spots-band", "lsb") && !bg::decoderFitsDemod("spots-band", "nfm"),
       "offered on a USB row only (a VFO row has a demodulator; none runs — it is fed IQ)");
    bg::BandSpan sp;
    ok(bg::bandSpan(7074000, sp) && sp.name == "40 m" && sp.loHz < 7040000 && sp.hiHz > 7080000 && sp.hiHz - sp.loHz <= 48000.0 + 1,
       "7074 names 40 m: its span " + std::to_string((long long)sp.loHz) + "–" + std::to_string((long long)sp.hiHz) + " Hz (WSPR's window to JS8's, ≤ 48 kHz)");
    ok(bg::bandSpan(7000500, sp) && sp.name == "40 m", "any frequency inside the band names it (7000.5 → 40 m)");
    bg::Config c; std::string err;
    ok(bg::parse(R"({"on":true,"profiles":[{"centre":7000000,"vfos":[{"hz":7074000,"mode":"usb","dec":"spots-band"},{"hz":7038600,"mode":"usb","dec":"ft8"}]}]})", c, err)
       && c.profiles[0].vfos[0].dec == "spots-band" && bg::toJson(c).find("\"dec\":\"spots-band\"") != std::string::npos, "parses and writes back");
    ok(bg::validate(c, 250000, err), "40 m Band at centre 7.000 MHz, 250 kS/s: valid (" + err + ")");
    ok(bg::validate(c, 2400000, err), "…and at 2.4 MS/s (" + err + ")");
    bg::Config far = c; far.profiles[0].centreHz = 6950000;
    ok(!bg::validate(far, 250000, err) && err.find("40 m band's digital modes") != std::string::npos && err.find("Move the RF centre") != std::string::npos,
       "the band's modes outside the window: refused, in words — " + err);
    bg::Config off = c; off.profiles[0].vfos[0].hz = 6500000; off.profiles[0].centreHz = 6500000;
    off.profiles[0].vfos.resize(1);
    ok(!bg::validate(off, 250000, err) && err.find("not in a band the digital modes use") != std::string::npos && err.find("7074") != std::string::npos,
       "a frequency on no band: refused, with an example — " + err);
    bg::Config lk = c; lk.profiles[0].vfos.resize(1);
    ok(!bg::validate(lk, 250000, err, 0, 7200000) && err.find("locked") != std::string::npos, "a locked radio whose window misses the band: refused — " + err);
    err.clear();
    ok(bg::validate(lk, 2400000, err, 0, 7200000), "…and allowed where the locked window holds it (" + err + ")");
}

/** BgRunner with one Band VFO on 40 m, fed `secs` of capture stamped from `utc0`; returns the process CPU it took. */
struct Run { double cpu = 0, capSec = 0; std::string status, list, body; uint64_t dropped = 0; int running = 0; };
static Run runBand(bool stations, const char* tag) {
    Run out;
    gallery::Store s; s.setDir(tmpDir(tag));
    FileSlots slots("", [] { return 4; });
    const double fs = 250000, centre = 7000000;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &s;
    p.lat = 52.29; p.lon = -0.85;   // ★ the furthest station is measured from here
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{7074000, "usb", "spots-band"} };
    p.log = [](const std::string&) {};
    r.start(p);
    out.running = r.decodersRunning();
    // ★ 32 s of 40 m: two FT8 stations in the first slot (G4IRX at +1200 Hz, JA1ABC at +1800 Hz above 7.074), noise
    const double utc0 = std::floor(1793000000.0 / 15.0) * 15.0;   // a slot boundary
    const double secs = 32.0;
    std::vector<cf32> cap((size_t)(secs * fs));
    std::mt19937 g(11); const float sigma = 0.01f; std::normal_distribution<float> n(0.0f, sigma);
    for (auto& v : cap) v = cf32(n(g), n(g));
    if (stations) {
        // SNR −8 dB in 2500 Hz (WSJT-X's convention): amp from the noise density
        const double amp = std::sqrt(std::pow(10.0, -8 / 10.0) * 2 * sigma * sigma / fs * 2500.0);
        addFt8(cap, fs, 74000 + 1200, 0.5, ft8Tones("CQ G4IRX IO91"), amp);
        addFt8(cap, fs, 74000 + 1800, 0.5, ft8Tones("CQ JA1ABC PM95"), amp);
        addFt8(cap, fs, 74000 + 1200, 15.5, ft8Tones("CQ G4IRX IO91"), amp);
    }
    const double c0 = cpuSecs();
    const size_t chunk = 16384;
    for (size_t o = 0; o < cap.size(); o += chunk) {
        const int k = (int)std::min(chunk, cap.size() - o);
        r.feedIq(&cap[o], k, utc0 + (double)(o + (size_t)k) / fs);   // ★ the capture's own clock
        std::this_thread::sleep_for(std::chrono::milliseconds(4));    // ★ paced: the runner drops past 0.5 s
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));      // ★ the last slot's decode
    out.status = r.statusJson();
    out.dropped = r.droppedSamples();
    r.stop();
    out.cpu = cpuSecs() - c0; out.capSec = secs;
    ok(slots.inUse() == 0, std::string(tag) + ": stopped — the slot is back");
    out.list = s.listJson("ft8", 20);
    const std::string first = gallery::lineStr(out.list, "f");
    std::string path, ct;
    if (!first.empty() && s.resolve(first, path, ct)) {
        FILE* f = std::fopen(path.c_str(), "r"); char b[512]; size_t k;
        while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) out.body.append(b, k);
        if (f) std::fclose(f);
    }
    return out;
}

static void endToEnd() {
    std::printf("\n── end to end: a 40 m Band VFO on synthetic IQ (two FT8 stations), 250 kS/s centred 7.000 MHz ──\n");
    const Run a = runBand(true, "busy");
    ok(a.running == 1, "the Band VFO took its decoder slot and runs");
    ok(a.status.find("\"band\":\"40 m\"") != std::string::npos && a.status.find("FT8") != std::string::npos,
       "the admin line names the band and what the governor runs: " + a.status.substr(0, 400));
    ok(a.status.find("\"spots\":") != std::string::npos, "…with the run's live spot count");
    ok(a.list.find("\"d\":\"ft8band\"") != std::string::npos || a.list.find("ft8band") != std::string::npos, "the gallery holds a Band summary (decoder ft8band)");
    ok(a.body.find("Digital spots summary \xc2\xb7 Band 40 m \xc2\xb7 background decoding (Profile 1)") != std::string::npos,
       "the card's first line: \"Digital spots summary · Band 40 m · background decoding (Profile 1)\"");
    ok(a.body.find("Japan") != std::string::npos && a.body.find("JA1ABC") != std::string::npos, "JA1ABC heard — the furthest, Japan");
    ok(a.body.find("England") != std::string::npos || a.body.find("United Kingdom") != std::string::npos, "G4IRX heard too");
    ok(a.dropped == 0, "no IQ dropped at this pace");
    std::printf("%s\n", a.body.c_str());
    std::printf("  CPU (busy 40 m, two stations): %.2f s of CPU for %.0f s of capture = %.1f %% of one core (this machine, the whole process)\n",
                a.cpu, a.capSec, 100.0 * a.cpu / a.capSec);

    std::printf("\n── the same band, nothing on it (the activity gate) ──\n");
    const Run q = runBand(false, "quiet");
    ok(q.list.find("\"f\":") == std::string::npos, "nothing heard: no card at all");
    std::printf("  CPU (quiet 40 m): %.2f s for %.0f s = %.1f %% of one core\n", q.cpu, q.capSec, 100.0 * q.cpu / q.capSec);
    // ★ CPU on a box this fast is noise either way; what the gate does is SAID in the admin line: nothing decoded
    ok(q.status.find(" active") == std::string::npos && q.status.find("FT8 7.0740 idle") != std::string::npos,
       "every mode idle on an empty band — the gate decodes nothing there");
}

static void switchedOff() {
    std::printf("\n── Band switched off on this receiver: shown paused with the reason, no slot, NAVTEX beside it runs ──\n");
    gallery::Store s; s.setDir(tmpDir("off"));
    FileSlots slots("", [] { return 4; });
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = 2400000; p.centreHz = 6500000; p.fftSize = 65536; p.slots = &slots; p.store = &s;
    p.profile.centreHz = 6500000;
    p.profile.vfos = { bg::Vfo{7074000, "usb", "spots-band"}, bg::Vfo{6000000, "usb", "navtex"} };
    p.bandWhyNot = [] { return std::string("the server owner has switched Band spots off on this receiver (Setup \xe2\x86\x92 Modes and decoders)"); };
    std::vector<std::string> logs; std::mutex lm;
    p.log = [&](const std::string& t) { std::lock_guard<std::mutex> lk(lm); logs.push_back(t); };
    r.start(p);
    ok(r.decodersRunning() == 1 && slots.inUse() == 1, "one decoder (NAVTEX), one slot");
    const std::string st = r.statusJson();
    ok(st.find("\"state\":\"paused\"") != std::string::npos && st.find("switched Band spots off") != std::string::npos, "the Band VFO: paused, with why");
    r.slotTickForTest();
    ok(r.decodersRunning() == 1, "…and the slot housekeeping never starts it behind the owner's back");
    bool said = false; { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) if (l.find("not started") != std::string::npos) said = true; }
    ok(said, "the log says so once");
    r.stop();
}

/** ★★ THE COST AT A REAL RADIO'S RATE (2.4 MS/s, the runner's own 65536-point FFT — an RSP or an RTL as the Lenovo
 *  runs): `secs` of noise through a runner with the VFOs given, fed a little faster than real time; process CPU per
 *  second of capture. One second of noise is made once and looped (its stamps advance), so making it costs nothing. */
static double costAt(const std::vector<bg::Vfo>& vfos, double secs) {
    gallery::Store s; s.setDir(tmpDir("cost"));
    FileSlots slots("", [] { return 4; });
    const double fs = 2400000, centre = 7000000;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 65536; p.slots = &slots; p.store = &s;
    p.profile.centreHz = centre; p.profile.vfos = vfos;
    p.log = [](const std::string&) {};
    std::vector<cf32> sec((size_t)fs);
    std::mt19937 g(5); std::normal_distribution<float> n(0.0f, 0.01f);
    for (auto& v : sec) v = cf32(n(g), n(g));
    r.start(p);
    const double utc0 = std::floor(1793000000.0 / 15.0) * 15.0;
    const double c0 = cpuSecs();
    const auto w0 = std::chrono::steady_clock::now();
    const size_t chunk = 65536; size_t fed = 0;
    while ((double)fed < secs * fs) {
        const size_t o = fed % sec.size();
        const int k = (int)std::min(chunk, sec.size() - o);
        r.feedIq(&sec[o], k, utc0 + (double)(fed + (size_t)k) / fs);
        fed += (size_t)k;
        // ★ paced to ~1.25× real time: the runner's queue holds half a second
        const auto due = w0 + std::chrono::microseconds((long long)((double)fed / fs / 1.25 * 1e6));
        std::this_thread::sleep_until(due);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t dropped = r.droppedSamples();
    r.stop();
    const double cpu = cpuSecs() - c0;
    if (dropped) std::printf("  (%.2f s of IQ dropped)\n", (double)dropped / fs);
    return 100.0 * cpu / secs;
}
static void cost() {
    std::printf("\n── the cost at 2.4 MS/s (65536-point FFT), 20 s of noise each, %% of one core for the whole process ──\n");
    const double ft8 = costAt({ bg::Vfo{7074000, "usb", "ft8"} }, 20);
    const double band = costAt({ bg::Vfo{7074000, "usb", "spots-band"} }, 20);
    const double both = costAt({ bg::Vfo{7074000, "usb", "spots-band"}, bg::Vfo{7038600, "usb", "navtex"} }, 20);
    std::printf("  one FT8/FT4 VFO (summary):        %.1f %%\n", ft8);
    std::printf("  one Band VFO (40 m, every mode):  %.1f %%\n", band);
    std::printf("  Band + a NAVTEX VFO beside it:    %.1f %%\n", both);
    ok(band < 60.0, "a quiet band's Band VFO stays well inside one core at 2.4 MS/s on this machine");
}

int main() {
    config();
    endToEnd();
    switchedOff();
    if (!std::getenv("VIBE_BG_BAND_NO_COST")) cost();
    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
