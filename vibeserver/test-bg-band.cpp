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
#include "decoders/cw_skimmer.h"
#include "decoders/rtty_skimmer.h"
#include "vibe_station_store.h"
#include "vibe_spot_report.h"
#include "benchsynth/psk_synth.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}

#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <map>
#include <cstring>
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

// ── ★★ V12 beta91: CW and RTTY inside a band slice (docs/v12/BAND-SKIM-CW-RTTY.md) ────────────────────────────────────
/** Keyed CW (ITU-R M.1677 timing, 5 ms raised-cosine edges) at `offHz` from the capture's centre, `text` repeated from
 *  `startSec` to the end. Amplitude from `snrDb` in 2500 Hz over noise of `sigma` per rail. */
static void addCw(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::string& text, double wpm, double snrDb, double sigma) {
    const double unit = 1.2 / wpm;
    std::vector<std::pair<bool, double>> el;   // on/off, seconds
    for (char ch : text) {
        if (ch == ' ') { el.push_back({ false, 4 * unit }); continue; }   // + the 3 after the letter = 7
        const std::string code = cw::morseCode(std::string(1, ch));
        for (size_t i = 0; i < code.size(); ++i) { el.push_back({ true, (code[i] == '-' ? 3 : 1) * unit }); el.push_back({ false, unit }); }
        el.push_back({ false, 2 * unit });
    }
    const double amp = std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / fs * 2500.0);
    const double edge = 0.005;
    size_t i = (size_t)(startSec * fs);
    double ph = 0;
    while (i < x.size()) {
        for (auto& e : el) {
            const size_t n = (size_t)(e.second * fs);
            for (size_t k = 0; k < n && i < x.size(); ++k, ++i) {
                ph += 2 * M_PI * offHz / fs; if (ph > M_PI) ph -= 2 * M_PI;
                if (!e.first) continue;
                const double t = k / fs, tt = (n - k) / fs;
                const double g = t < edge ? 0.5 - 0.5 * std::cos(M_PI * t / edge) : tt < edge ? 0.5 - 0.5 * std::cos(M_PI * tt / edge) : 1.0;
                x[i] += cf32((float)(amp * g * std::cos(ph)), (float)(amp * g * std::sin(ph)));
            }
            if (i >= x.size()) break;
        }
    }
}
/** RTTY (ITA2, start + 5 data + 1.5 stop, mark above space — normal) centred `offHz`, `text` repeated from `startSec`. */
static void addRtty(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::string& text, double baud, double shift,
                    double snrDb, double sigma) {
    std::vector<uint8_t> h;   // half bits
    auto frame = [&](int code) { h.push_back(0); h.push_back(0); for (int b = 0; b < 5; ++b) { const uint8_t v = (code >> b) & 1; h.push_back(v); h.push_back(v); } for (int k = 0; k < 3; ++k) h.push_back(1); };
    for (int k = 0; k < 20; ++k) h.push_back(1);
    bool figs = false; frame(31);
    for (char c : text) { bool f = figs; const int code = rttysk::ita2Code(c, f); if (code < 0) continue; if (f != figs) { frame(f ? 27 : 31); figs = f; } frame(code); }
    const double amp = std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / fs * 2500.0);
    const double hb = 1.0 / (2 * baud);
    double ph = 0, t = 0;
    for (size_t i = (size_t)(startSec * fs); i < x.size(); ++i) {
        int k = (int)(t / hb);
        if (k >= (int)h.size()) { t -= h.size() * hb; k = 0; }
        const double f = offHz + (h[(size_t)k] ? shift / 2 : -shift / 2);
        ph += 2 * M_PI * f / fs; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        t += 1.0 / fs;
    }
}

/** BPSK (G3PLX's PSK31 / PSK63: Varicode, a zero a cosine-shaped reversal) at `offHz`, `text` once from `startSec`. */
static void addPsk(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::string& text, double baud, double snrDb, double sigma) {
    const auto env = psksynth::envelope(psksynth::bits(text), baud, fs);
    double ms = 0; for (float v : env) ms += v * v; ms /= std::max<size_t>(1, env.size());
    const double amp = std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / fs * 2500.0 / ms);
    double ph = 0;
    const size_t s0 = (size_t)(startSec * fs);
    for (size_t i = 0; i < env.size() && s0 + i < x.size(); ++i) {
        ph += 2 * M_PI * offHz / fs; if (ph > M_PI) ph -= 2 * M_PI;
        x[s0 + i] += cf32((float)(amp * env[i] * std::cos(ph)), (float)(amp * env[i] * std::sin(ph)));
    }
}

static void config() {
    std::printf("── the config: \"Band skimmer (summary)\" ──\n");
    bg::installBandSpan();
    const bg::DecoderDef* d = bg::decoderDef("spots-band");
    ok(d && std::string(d->label) == "Band skimmer (summary)", "the decoder is listed");
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
    ok(!bg::validate(off, 250000, err) && err.find("not in a band the digital modes use") != std::string::npos && err.find("choose a band") != std::string::npos,
       "a frequency on no band: refused, with an example — " + err);
    // ★ 2026-10-28: the setup page PICKS the band — a row with none says to choose it, not "set a frequency"
    bg::Config none = off; none.profiles[0].vfos[0].hz = 0;
    ok(!bg::validate(none, 250000, err) && err.find("choose its band") != std::string::npos, "a Band row with no band: refused, in words — " + err);
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
    // ★ beta92: the last-10 card is the server's ("every radio on this server"); the hourly card names the background run
    ok(a.body.find("Digital spots summary \xc2\xb7 Band 40 m \xc2\xb7 background decoding (Profile 1)") != std::string::npos
       || a.body.find("Digital spots summary \xc2\xb7 Band 40 m \xc2\xb7 every radio on this server") != std::string::npos,
       "the card's first line: \"Digital spots summary · Band 40 m · background decoding (Profile 1)\" (or, the last-10 card, every radio)");
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
    // ★ V12 beta91: the CW / RTTY / PSKW chips are continuous and have no activity gate of their own (their skimmers are
    //   the detectors) — so "every AUDIO mode idle": drop their entries from the admin line before looking
    std::string audioOnly;
    {
        const size_t a = q.status.find("\"modes\":\""), e = a == std::string::npos ? a : q.status.find('"', a + 9);
        const std::string modes = a == std::string::npos ? std::string() : q.status.substr(a + 9, e - a - 9);
        size_t p = 0;
        while (p < modes.size()) {
            size_t c = modes.find(", ", p); if (c == std::string::npos) c = modes.size();
            const std::string one = modes.substr(p, c - p);
            if (one.rfind("CW ", 0) != 0 && one.rfind("RTTY ", 0) != 0 && one.rfind("PSKW ", 0) != 0) audioOnly += one + ", ";
            p = c + 2;
        }
    }
    ok(!audioOnly.empty() && audioOnly.find(" active") == std::string::npos && audioOnly.find("FT8 7.0740 idle") != std::string::npos,
       "every audio mode idle on an empty band — the gate decodes nothing there (" + audioOnly.substr(0, 160) + ")");
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
    p.bandWhyNot = [] { return std::string("the server owner has switched the band skimmer off on this receiver (Setup \xe2\x86\x92 Modes and decoders)"); };
    std::vector<std::string> logs; std::mutex lm;
    p.log = [&](const std::string& t) { std::lock_guard<std::mutex> lk(lm); logs.push_back(t); };
    r.start(p);
    ok(r.decodersRunning() == 1 && slots.inUse() == 1, "one decoder (NAVTEX), one slot");
    const std::string st = r.statusJson();
    ok(st.find("\"state\":\"paused\"") != std::string::npos && st.find("switched the band skimmer off") != std::string::npos, "the Band VFO: paused, with why");
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

/** ★★ V12 beta91 — the 40 m capture the CW / RTTY tests share: noise, plus (when `signals`) G4CWX calling on CW at
 *  7012.3 kHz, G4RTY calling on RTTY (45.45 Bd / 170 Hz) centred 7045.0 kHz, and FT8 G4IRX on 7.074 + 1200 Hz — or, with
 *  `crowd`, only things that must spot NOTHING on CW or RTTY: four FT8 stations inside the RTTY window, a steady carrier
 *  and randomly keyed CW (no callsign) in the CW window. */
static std::vector<cf32> capture40(double fs, double centre, double secs, bool signals, bool crowd, double utc0) {
    (void)utc0;
    std::vector<cf32> cap((size_t)(secs * fs));
    const float sigma = 0.01f;
    std::mt19937 g(17); std::normal_distribution<float> n(0.0f, sigma);
    for (auto& v : cap) v = cf32(n(g), n(g));
    if (signals) {
        addCw(cap, fs, 7012300 - centre, 0.7, "CQ CQ DE G4CWX G4CWX K   ", 26, 6, sigma);
        addRtty(cap, fs, 7045000 - centre, 0.4, "CQ CQ DE G4RTY G4RTY PSE K\n", 45.45, 170, 12, sigma);
        // ★ PSK63 at 7046.3 kHz — OUTSIDE the original PSK chip's window (7040.15–7043.65): only wide PSK can hear it
        addPsk(cap, fs, 7046300 - centre, 0.6, "CQ CQ DE G4PSK G4PSK K  CQ CQ DE G4PSK G4PSK K  CQ CQ DE G4PSK G4PSK K  ", 62.5, 5, sigma);
        const double amp = std::sqrt(std::pow(10.0, -8 / 10.0) * 2 * sigma * sigma / fs * 2500.0);
        addFt8(cap, fs, 74000 + 1200, 0.5, ft8Tones("CQ G4IRX IO91"), amp);
        addFt8(cap, fs, 74000 + 1200, 15.5, ft8Tones("CQ G4IRX IO91"), amp);
    }
    if (crowd) {
        const double amp = std::sqrt(std::pow(10.0, 0 / 10.0) * 2 * sigma * sigma / fs * 2500.0);
        const char* msgs[] = { "CQ G4AAA IO91", "CQ DL1BBB JO62", "CQ F5CCC JN18", "CQ EA4DDD IN80" };
        for (int k = 0; k < 4; ++k) for (double t0 : { 0.5, 15.5 }) addFt8(cap, fs, 41500 + 2000.0 * k - (centre - 7000000), t0, ft8Tones(msgs[k]), amp);
        // a steady carrier and randomly keyed CW (no callsign) in the CW window
        const double ca = std::sqrt(std::pow(10.0, 10 / 10.0) * 2 * sigma * sigma / fs * 2500.0);
        std::uniform_real_distribution<double> u(0, 1);
        double ph1 = 0, ph2 = 0, next = 0.06; bool on = true;
        for (size_t i = 0; i < cap.size(); ++i) {
            const double t = i / fs;
            if (t >= next) { on = !on; next = t + 0.05 * (on ? (u(g) < 0.5 ? 1 : 3) : (u(g) < 0.6 ? 1 : 3)); }
            ph1 += 2 * M_PI * (7030000 - centre) / fs; ph2 += 2 * M_PI * (7025000 - centre) / fs;
            cap[i] += cf32((float)(ca * std::cos(ph1)), (float)(ca * std::sin(ph1)));
            if (on) cap[i] += cf32((float)(ca * std::cos(ph2)), (float)(ca * std::sin(ph2)));
        }
    }
    return cap;
}

struct Heard { std::string mode, call; double rf; };
/** BgRunner, one 40 m Band VFO, the capture above; what the stations store heard, the reporter's datagrams, the status. */
struct SkimRun { std::vector<Heard> heard; std::string status, body, list; int running = 0, slotsDuring = 0; std::vector<std::string> logs; double cpu = 0; };
static SkimRun runSkim(bool signals, bool crowd, const char* tag, std::function<int(const std::string&)> ov = nullptr) {
    SkimRun out;
    gallery::Store gs; gs.setDir(tmpDir(tag));
    vibestations::Store st(nullptr, false);
    std::mutex hm;
    const int sub = st.subscribe([&](const vibestations::Reception& r) { std::lock_guard<std::mutex> lk(hm); out.heard.push_back({ r.mode, r.call, r.rfHz }); });
    FileSlots slots("", [] { return 4; });
    const double fs = 250000, centre = 7000000;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &gs; p.stations = &st;
    p.lat = 52.29; p.lon = -0.85;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{7074000, "usb", "spots-band"} };
    if (ov) p.bandOverride = ov;
    std::mutex lm;
    p.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); out.logs.push_back(l); };
    r.start(p);
    out.running = r.decodersRunning();
    const double utc0 = std::floor(1793000000.0 / 15.0) * 15.0, secs = 32.0;
    const auto cap = capture40(fs, centre, secs, signals, crowd, utc0);
    const double c0 = cpuSecs();
    const size_t chunk = 16384;
    const auto w0 = std::chrono::steady_clock::now();
    // ★ the 32 s are fed TWICE (64 s of capture, the stamps running on) so Band's minute line is written once
    size_t fed = 0;
    for (int pass = 0; pass < 2; ++pass)
    for (size_t o = 0; o < cap.size(); o += chunk) {
        const int k = (int)std::min(chunk, cap.size() - o);
        fed += (size_t)k;
        r.feedIq(&cap[o], k, utc0 + (double)fed / fs);
        // ★ paced at 4× real time: the CW / RTTY workers keep up (a live radio is 1×), the runner's queue never drops
        std::this_thread::sleep_until(w0 + std::chrono::microseconds((long long)((double)fed / fs / 4.0 * 1e6)));
        if (pass == 0 && !out.slotsDuring && o >= cap.size() / 2) out.slotsDuring = slots.inUse();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    out.status = r.statusJson();
    r.stop();
    out.cpu = cpuSecs() - c0;
    st.unsubscribe(sub);
    const std::string list = gs.listJson("ft8", 20), first = gallery::lineStr(list, "f");
    out.list = list;
    std::string path, ct;
    if (!first.empty() && gs.resolve(first, path, ct)) {
        FILE* f = std::fopen(path.c_str(), "r"); char b[512]; size_t k;
        while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) out.body.append(b, k);
        if (f) std::fclose(f);
    }
    return out;
}
static const Heard* find(const std::vector<Heard>& v, const char* mode, const char* call) {
    for (const auto& h : v) if (h.mode == mode && h.call == call) return &h;
    return nullptr;
}

static void cwRttyChips() {
    std::printf("\n── ★ V12 beta91: CW and RTTY chips inside the 40 m Band VFO (one slot), 250 kS/s centred 7.000 MHz ──\n");
    vibe::skimbands::bandRegion().store(1);
    // ★ PSK Reporter on (a test sink — nothing leaves the machine): what the band's CW and RTTY spots become there
    auto& rp = vibe::spotreport::Reporter::shared();
    std::mutex um; std::string dgrams;
    rp.setUdp([&](const std::string&, int, const std::vector<uint8_t>& b, std::string&) { std::lock_guard<std::mutex> lk(um); dgrams.append((const char*)b.data(), b.size()); return true; });
    vibe::spotreport::Config rc; rc.on = true; rc.call = "G0TST"; rc.grid = "IO92"; rc.pskHost = "127.0.0.1"; rp.configure(rc);

    const SkimRun a = runSkim(true, false, "skim");
    ok(a.running == 1 && a.slotsDuring == 1, "ONE decoder slot for the whole band — CW and RTTY ride the Band VFO's (" + std::to_string(a.slotsDuring) + " in use mid-run)");
    ok(a.status.find("CW 7.0200") != std::string::npos && a.status.find("RTTY 7.0450") != std::string::npos,
       "the admin line lists the CW window (7.000–7.040, centre 7.0200) and the RTTY window (7.040–7.050, centre 7.0450)");
    const Heard* c = find(a.heard, "CW", "G4CWX");
    const Heard* t = find(a.heard, "RTTY", "G4RTY");
    const Heard* f8 = find(a.heard, "FT8", "G4IRX");
    char b[200];
    std::snprintf(b, sizeof b, "CW: G4CWX spotted at %.1f Hz (sent at 7012300) — mode CW, into the stations store", c ? c->rf : 0.0);
    ok(c && std::fabs(c->rf - 7012300) < 60, b);
    std::snprintf(b, sizeof b, "RTTY: G4RTY spotted at %.1f Hz (centred 7045000) — mode RTTY", t ? t->rf : 0.0);
    ok(t && std::fabs(t->rf - 7045000) < 60, b);
    ok(f8 != nullptr, "FT8 G4IRX on 7.074 still decoded beside them");
    const Heard* p63 = find(a.heard, "PSK63", "G4PSK");
    std::snprintf(b, sizeof b, "wide PSK: G4PSK on PSK63 at %.1f Hz (sent at 7046300, outside the old 3.5 kHz PSK window) — mode PSK63", p63 ? p63->rf : 0.0);
    ok(p63 && std::fabs(p63->rf - 7046300) < 30, b);
    ok(a.status.find("PSKW 7.0450") != std::string::npos, "the admin line lists wide PSK on the RTTY segment's window");
    int others = 0; std::string what;
    for (const auto& h : a.heard) if (h.call != "G4CWX" && h.call != "G4RTY" && h.call != "G4IRX" && h.call != "G4PSK") { ++others; what += " " + h.mode + ":" + h.call; }
    ok(others == 0, "nothing else spotted (no false CW / RTTY calls)" + what);
    {
        const size_t l = a.body.find("Spots heard:");
        const std::string line = l == std::string::npos ? std::string() : a.body.substr(l, a.body.find('\n', l) - l);
        ok(line.find("CW ") != std::string::npos && line.find("RTTY ") != std::string::npos && line.find("PSK63 ") != std::string::npos,
           "the band's ONE summary card counts them by mode: " + line);
    }
    rp.flushNow();
    std::string d; { std::lock_guard<std::mutex> lk(um); d = dgrams; }
    ok(d.find("G4CWX") != std::string::npos && d.find(std::string("\x02" "CW", 3)) != std::string::npos,
       "PSK Reporter: G4CWX sent with ADIF mode \"CW\"");
    ok(d.find("G4RTY") != std::string::npos && d.find(std::string("\x04" "RTTY", 5)) != std::string::npos,
       "PSK Reporter: G4RTY sent with ADIF mode \"RTTY\"");
    ok(d.find("G4PSK") != std::string::npos && d.find(std::string("\x05" "PSK63", 6)) != std::string::npos,
       "PSK Reporter: G4PSK sent with mode \"PSK63\"");
    bool chipsLine = false;
    for (const auto& l : a.logs) if (l.find("chips:") != std::string::npos && l.find(" CW:") != std::string::npos && l.find(" RTTY:") != std::string::npos) chipsLine = true;
    ok(chipsLine, "the minute line's \"chips:\" names CW and RTTY");
    std::printf("  CPU (40 m, CW + RTTY + FT8 heard, 4x real time): %.2f s for 64 s = %.1f %% of one core, whole process\n", a.cpu, 100.0 * a.cpu / 64.0);

    const SkimRun q = runSkim(false, false, "skimq");
    int qn = 0; for (const auto& h : q.heard) if (h.mode == "CW" || h.mode == "RTTY" || h.mode.rfind("PSK", 0) == 0) ++qn;
    ok(qn == 0, "noise only: no CW, RTTY or PSK spot (" + std::to_string(qn) + ")");
    const SkimRun k = runSkim(false, true, "skimc");
    int kn = 0; std::string kw; for (const auto& h : k.heard) if (h.mode == "CW" || h.mode == "RTTY" || h.mode.rfind("PSK", 0) == 0) { ++kn; kw += " " + h.mode + ":" + h.call; }
    ok(kn == 0, "an FT8 crowd inside the RTTY / wide-PSK window, a steady carrier and call-less keyed CW in the CW window: no CW, RTTY or PSK spot" + kw);

    // ★ the owner's toggles (Band, mode by mode: "spotsband-cw" / "spotsband-rtty")
    const SkimRun o = runSkim(true, false, "skimoff", [](const std::string& m) { return (m == "CW" || m == "RTTY" || m == "PSKW") ? -1 : 0; });
    int on = 0; for (const auto& h : o.heard) if (h.mode == "CW" || h.mode == "RTTY" || h.call == "G4PSK") ++on;
    // ★ CW's own slice is not even cut when its chip is off (it has no run at all); RTTY and PSKW ride the main slice: "off"
    ok(on == 0 && o.status.find("CW 7.0200") == std::string::npos && o.status.find("RTTY 7.0450 off") != std::string::npos
       && o.status.find("PSKW 7.0450 off") != std::string::npos,
       "switched off by the owner: CW not run at all (its slice not cut), RTTY and wide PSK \"off\", nothing spotted");
    rc.on = false; rp.configure(rc); rp.setUdp(nullptr);
}

/** ★★ THE LIVE PATH (Stuart: "the CW and RTTY chips must work in a LISTENER's live full-band digital-spots decode too"):
 *  a listener's DecoderHost in Band, fed the way the server's per-listener extract feeds it — the main slice on the dial
 *  and every extra slice the host asks for (iqExtraSlices), each with its id and bins — and the decoder socket's frames. */
struct LivePeer : DecoderPeer {
    std::mutex m; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t*, size_t) override {}
    void text(const std::string& t) override { std::lock_guard<std::mutex> lk(m); txt.push_back(t); }
    int count(const std::string& a, const std::string& b) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) n += t.find(a) != std::string::npos && t.find(b) != std::string::npos; return n; }
    std::string last(const std::string& a) { std::lock_guard<std::mutex> lk(m); for (auto it = txt.rbegin(); it != txt.rend(); ++it) if (it->find(a) != std::string::npos) return *it; return ""; }
};
static void livePath() {
    std::printf("\n── ★ the LIVE path: a listener's Band (DecoderHost) on 40 m — CW, RTTY and wide PSK spots and chips on the decoder socket ──\n");
    vibe::skimbands::bandRegion().store(1);
    const double fs = 250000, hw = 7000000; const int fft = 8192;
    const double dial = digi::spanOf(*digi::bandById("40m"), digi::modes()).centreHz;
    DecoderHost::Env env;
    env.dialHz = [dial] { return dial; };
    std::vector<std::string> logs; std::mutex lm;
    env.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); };
    auto host = std::make_shared<DecoderHost>(env, "live-test");
    auto peer = std::make_shared<LivePeer>();
    host->addPeer(peer);
    ok(host->startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Ok, "a listener starts Band on 40 m (one spots slot)");
    ExtraSlice xs[kMaxExtraSlices];
    const int nx = host->iqExtraSlices(xs);
    bool cwX = false; for (int k = 0; k < nx; ++k) if (xs[k].centreHz - xs[k].widthHz / 2 < 7000500 && xs[k].centreHz + xs[k].widthHz / 2 > 7039500) cwX = true;
    ok(nx >= 2 && cwX, "the host asks for its extra slices — JTTY's and the CW window's (7000–7040) among " + std::to_string(nx));
    const double binHz = fs / fft;
    struct Sl { int bins, cb; double rate, centre; std::vector<cf32> buf; std::vector<float> pw; vibedsp::Channelizer::ExtractCtx ctx; };
    std::vector<Sl> sls;
    { Sl m; m.bins = bandSliceBins(fs, fft, std::min(host->iqWidthHz(), fs * 0.8)); m.cb = (int)std::lround((dial - hw) / binHz);
      m.rate = fs * m.bins / fft; m.centre = hw + m.cb * binHz; sls.push_back(std::move(m)); }
    for (int k = 0; k < nx; ++k) {
        Sl x; x.bins = bandSliceBins(fs, fft, xs[k].widthHz); x.cb = (int)std::lround((xs[k].centreHz - hw) / binHz);
        x.rate = fs * x.bins / fft; x.centre = hw + x.cb * binHz;
        sls.push_back(std::move(x));
    }
    const double utc0 = std::floor(1793000000.0 / 15.0) * 15.0, secs = 32.0;
    const auto cap = capture40(fs, hw, secs, true, false, utc0);
    vibedsp::Channelizer ch(fft);
    const int chunk = 16384;
    const auto w0 = std::chrono::steady_clock::now();
    // ★ the 32 s fed TWICE (64 s of capture): the continuous modes start kIqStartSec (20 s) into a Band run
    for (int pass = 0; pass < 2; ++pass)
    for (size_t o = 0; o < cap.size(); o += (size_t)chunk) {
        const int n = (int)std::min<size_t>((size_t)chunk, cap.size() - o);
        const size_t base = (size_t)pass * cap.size();
        ch.feed(&cap[o], n, [&](const cf32* b, int) {
            const double utc = utc0 + (double)((ch.blockIndex() + 1) * (long long)ch.hop()) / fs;
            for (size_t i = 0; i < sls.size(); ++i) {
                Sl& sl = sls[i];
                if (!captureHolds(fs, hw, sl.centre, sl.rate)) continue;
                sl.buf.resize((size_t)sl.bins); sl.pw.resize((size_t)sl.bins);
                const int got = ch.extract(b, sl.cb, sl.bins, sl.buf.data(), sl.ctx, ch.blockIndex(), kBandTaperDiv);
                digi::slicePower(b, fft, sl.cb, sl.bins, sl.pw.data());
                if (got > 0) host->feedIq(sl.buf.data(), got, sl.rate, sl.centre, sl.pw.data(), sl.bins, binHz, utc, (int)i);
            }
        });
        std::this_thread::sleep_until(w0 + std::chrono::microseconds((long long)((double)(base + o + (size_t)n) / fs / 4.0 * 1e6)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    const std::string st = host->spotsStatusJson();
    ok(st.find("\"mode\":\"CW\"") != std::string::npos && st.find("\"mode\":\"RTTY\"") != std::string::npos && st.find("\"mode\":\"PSKW\"") != std::string::npos,
       "the chips (digital_spots_status) carry CW, RTTY and PSKW beside FT8 …: " + st.substr(0, 420));
    ok(peer->count("\"type\":\"digital_spots_status\"", "\"mode\":\"CW\"") > 0, "…and the decoder socket was sent them");
    ok(peer->count("\"mode\":\"CW\",\"callsign\":\"G4CWX\"", "digital_spot") > 0, "a digital_spot frame: CW G4CWX — " + peer->last("G4CWX").substr(0, 240));
    ok(peer->count("\"mode\":\"RTTY\",\"callsign\":\"G4RTY\"", "digital_spot") > 0, "a digital_spot frame: RTTY G4RTY — " + peer->last("G4RTY").substr(0, 240));
    ok(peer->count("\"mode\":\"PSK63\",\"callsign\":\"G4PSK\"", "digital_spot") > 0, "a digital_spot frame: PSK63 G4PSK (wide PSK) — " + peer->last("G4PSK").substr(0, 240));
    ok(peer->count("\"callsign\":\"G4IRX\"", "\"mode\":\"FT8\"") > 0, "…and FT8 G4IRX as ever");
    /* ★★ beta92 THE BAND SKIMMER'S FRAME (Stuart 2026-11-23): the CW and RTTY windows being skimmed (the VFO's width), and
     *  each signal followed with its frequency, call and readable text (EXPAND, the waterfall marks) — bounded. */
    {
        const std::string sk = host->spotsSkimJson();
        ok(sk.rfind("{\"type\":\"band_skim\",\"data\":{\"windows\":[", 0) == 0, "band_skim: the frame's shape — " + sk.substr(0, 160));
        ok(sk.find("{\"mode\":\"CW\",\"lo\":7000000,\"hi\":7040000") != std::string::npos, "band_skim: 40 m's CW window, 7000–7040 kHz (the VFO's width)");
        ok(sk.find("{\"mode\":\"RTTY\",\"lo\":") != std::string::npos, "band_skim: and its RTTY window");
        const size_t cw = sk.find("\"mode\":\"CW\",\"id\":");
        ok(cw != std::string::npos, "band_skim: a CW signal followed");
        const size_t g4 = sk.find("\"call\":\"G4CWX\"");
        ok(g4 != std::string::npos, "band_skim: …with its call, G4CWX");
        const size_t tx = g4 == std::string::npos ? std::string::npos : sk.find("\"text\":\"", g4);
        const std::string txt = tx == std::string::npos ? "" : sk.substr(tx + 8, sk.find('"', tx + 8) - tx - 8);
        ok(txt.find("G4CWX") != std::string::npos && txt.size() <= 120, "band_skim: …and its decoded text, readable, ≤ 120 characters: \"" + txt + "\"");
        ok(sk.find("\"mode\":\"RTTY\",\"id\":") != std::string::npos && sk.find("\"call\":\"G4RTY\"") != std::string::npos,
           "band_skim: an RTTY signal followed, with its call G4RTY");
        ok(sk.find('\n') == std::string::npos && sk.find('\r') == std::string::npos, "band_skim: no raw newline in the frame (RTTY text's lines are spaces)");
        ok(sk.size() < 80 * 420 + 2000, "band_skim: bounded (" + std::to_string(sk.size()) + " bytes)");
        ok(peer->count("\"type\":\"band_skim\"", "\"windows\"") > 0, "band_skim: …and the decoder socket was sent it");
        ok(peer->count("\"type\":\"band_skim\"", "G4CWX") > 0, "band_skim: …with the CW signal in it");
    }
    host->stopSpots();
    ok(host->spotsSkimJson().empty(), "band_skim: nothing once Band stops");
    host->shutdown();
    // ★ the owner switched the three chips off for Band: their slice is not cut at all (the DSP thread's extract and a
    //   one-pipeline radio's front-end decimation are given back) — only JTTY's extra slice is asked for
    DecoderHost::Env e2 = env;
    e2.bandOverride = [](const std::string& m) { return (m == "CW" || m == "RTTY" || m == "PSKW") ? -1 : 0; };
    auto h2 = std::make_shared<DecoderHost>(e2, "live-test-off");
    h2->startSpots(DecoderHost::SpotScope::Band);
    ExtraSlice x2[kMaxExtraSlices];
    const int n2 = h2->iqExtraSlices(x2);
    ok(n2 == nx - 1, "CW, RTTY and PSKW switched off for Band: their slice is not cut (" + std::to_string(nx) + " extra slices → " + std::to_string(n2) + ")");
    const std::string st2 = h2->spotsStatusJson();
    ok(st2.find("{\"mode\":\"CW\",\"state\":\"off\"") != std::string::npos, "…and its chip says \"off\" (the owner's), never \"out\"");
    h2->shutdown();
}

/** ★ Old configs load unchanged: a profile with a Band VFO and standalone CW / RTTY skimmer VFOs (beta90's shape). */
static void oldConfigs() {
    std::printf("\n── old configs load unchanged ──\n");
    bg::installBandSpan();
    const char* js = R"({"on":true,"profiles":[{"centre":7050000,"vfos":[{"hz":7074000,"mode":"usb","dec":"spots-band"},{"hz":7024000,"mode":"usb","dec":"cw-skim-48"},{"hz":7064000,"mode":"usb","dec":"rtty-skim-48"}]}]})";
    bg::Config c; std::string err;
    ok(bg::parse(js, c, err) && c.profiles.size() == 1 && c.profiles[0].vfos.size() == 3, "a beta90 profile (Band + CW skimmer + RTTY skimmer) parses (" + err + ")");
    bg::Config c2; std::string err2;
    ok(bg::parse(bg::toJson(c), c2, err2) && bg::toJson(c2) == bg::toJson(c), "…and writes back the same, nothing migrated");
    ok(bg::validate(c, 2400000, err), "…and is valid at 2.4 MS/s (" + err + ")");
}

/* ── ★★ b94 A CROWDED FT8 SLOT THROUGH THE BACKGROUND BAND PATH (the Pi 500's "almost no FT8 on a busy 40 m") ──────────
 *  40 FT8 stations in one slot on 7.074 (and again in the next), 80 Hz apart, from +25 dB down to −15 dB (2500 Hz), through
 *  BgRunner's channeliser → the Band slice → DecoderHost → the governor and gate → the FT8 engine → the stations store.
 *  A busy slot must give its decodes — above all the strong ones (a local at +17 dB is never missed). */
static void crowdedSlot() {
    std::printf("\n── b94: a crowded 40 m FT8 slot (40 stations, +25 … −15 dB) through the background Band path ──\n");
    gallery::Store gs; gs.setDir(tmpDir("crowd"));
    vibestations::Store st(nullptr, false);
    std::mutex hm; std::set<std::string> heard;
    const int sub = st.subscribe([&](const vibestations::Reception& r) { if (r.mode == "FT8") { std::lock_guard<std::mutex> lk(hm); heard.insert(r.call); } });
    FileSlots slots("", [] { return 4; });
    const double fs = 250000, centre = 7000000;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &gs; p.stations = &st;
    p.lat = 52.29; p.lon = -0.85;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{7074000, "usb", "spots-band"} };
    p.log = [](const std::string&) {};
    r.start(p);
    const double utc0 = std::floor(1793000000.0 / 15.0) * 15.0, secs = 32.0;
    std::vector<cf32> cap((size_t)(secs * fs));
    const float sigma = 0.01f;
    std::mt19937 g(23); std::normal_distribution<float> n(0.0f, sigma);
    for (auto& v : cap) v = cf32(n(g), n(g));
    const char* pfx[] = { "G4", "DL1", "F5", "EA4", "I2", "ON4", "PA3", "OK1", "SP9", "HA5", "OE3", "HB9", "LA9", "SM5", "OZ1", "OH2",
                          "EI4", "GM4", "GW4", "CT1", "9A3", "S51", "YO3", "LZ1", "SV1", "YU1", "UR5", "ES1", "YL2", "LY2", "M0", "2E0",
                          "DK2", "F4", "EA7", "IK4", "ON7", "PD0", "OK2", "SP3" };
    const char* grids[] = { "IO91", "JO62", "JN18", "IN80", "JN45", "JO20", "JO22", "JO70", "JO90", "JN97" };
    std::vector<std::string> strong, all;
    for (int k = 0; k < 40; ++k) {
        const std::string call = std::string(pfx[k]) + (char)('A' + k % 26) + (char)('B' + (k * 7) % 25) + (char)('C' + (k * 3) % 24);
        const std::string msg = "CQ " + call + " " + grids[k % 10];
        const double snr = 25.0 - k;                                  // ★ +25 … −14 dB
        const double amp = std::sqrt(std::pow(10.0, snr / 10.0) * 2 * sigma * sigma / fs * 2500.0);
        const double off = 74000 + 250 + 80.0 * ((k * 17) % 40);    // ★ strong and weak interleaved across the passband
        const auto tones = ft8Tones(msg.c_str());
        if (tones.empty()) continue;
        for (double t0 : { 0.5 + 0.02 * (k % 5), 15.5 + 0.02 * (k % 5) }) addFt8(cap, fs, off, t0, tones, amp);
        all.push_back(call);
        if (snr >= 0) strong.push_back(call);
    }
    const auto w0 = std::chrono::steady_clock::now();
    size_t fed = 0;
    for (size_t o = 0; o < cap.size(); o += 16384) {
        const int k = (int)std::min<size_t>(16384, cap.size() - o);
        fed += (size_t)k;
        r.feedIq(&cap[o], k, utc0 + (double)fed / fs);
        std::this_thread::sleep_until(w0 + std::chrono::microseconds((long long)((double)fed / fs / 4.0 * 1e6)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    r.stop();
    st.unsubscribe(sub);
    int s = 0, a = 0; std::string missed;
    { std::lock_guard<std::mutex> lk(hm);
      for (const auto& c : strong) { if (heard.count(c)) ++s; else missed += " " + c; }
      for (const auto& c : all) if (heard.count(c)) ++a; }
    char b[200];
    std::snprintf(b, sizeof b, "every station at 0 dB or stronger decoded (%d of %zu)%s", s, strong.size(), missed.empty() ? "" : " — missed:");
    ok(s == (int)strong.size(), b + missed);
    std::snprintf(b, sizeof b, "the slot gives most of its 40 stations (%d; at least 30)", a);
    ok(a >= 30, b);
}

/* ── `test-bg-band real <wav> <utc0> <recCentreHz> [L=40] [centre=6800000] [hw=15000] [vfos=7074000,...] [secs=]` ──
 *  A DEVELOPER'S REPLAY (b94, the Pi 500's "almost no FT8 on 40 m"): a stereo int16 I/Q recording (UberSDR iq192) interpolated
 *  ×L into a radio's capture centred `centre` + `hw` (the Pi 500's RSP1B: 6.800 MHz + 15 kHz, 8 MS/s ≈ L=40 → 7.68 MS/s),
 *  through the REAL BgRunner (its channeliser, the Band slices, DecoderHost, the governor) with the Pi's Band VFOs, played
 *  in REAL TIME on stamps moved on by whole two-minute steps (so the FT8 pass budget sees a live slot). Prints what the
 *  stations store heard per mode and band, and the runner's log. */
static bool loadIq16(const char* path, std::vector<cf32>& x, uint32_t& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    uint16_t chans = 0; size_t data = 0, dlen = 0; rate = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 2 || !rate || !data) return false;
    x.resize(dlen / 4);
    for (size_t i = 0; i < x.size(); ++i) {
        const int16_t a = (int16_t)(raw[data + 4 * i] | raw[data + 4 * i + 1] << 8), b = (int16_t)(raw[data + 4 * i + 2] | raw[data + 4 * i + 3] << 8);
        x[i] = cf32(a / 32768.0f, b / 32768.0f);
    }
    return true;
}
static int replayReal(int argc, char** argv) {
    if (argc < 5) { std::printf("usage: test-bg-band real <wav> <utc0> <recCentreHz> [L=40] [centre=6800000] [hw=15000] [vfos=a,b] [secs=]\n"); return 2; }
    const double utc0 = std::atof(argv[3]), recCentre = std::atof(argv[4]);
    double busyDrop = 0;
    int L = 40, D = 1; double centre = 6800000, hw = 15000, secs = 1e9;
    std::vector<double> vfos = { 5357000, 3573000, 7074000, 10136000 };
    for (int i = 5; i < argc; ++i) {
        if (!std::strncmp(argv[i], "L=", 2)) L = std::atoi(argv[i] + 2);
        if (!std::strncmp(argv[i], "D=", 2)) D = std::max(1, std::atoi(argv[i] + 2));   // ★ ×L/D: L=125 D=3 = exactly 8 MS/s from 192 kHz
        if (!std::strncmp(argv[i], "centre=", 7)) centre = std::atof(argv[i] + 7);
        if (!std::strncmp(argv[i], "hw=", 3)) hw = std::atof(argv[i] + 3);
        if (!std::strncmp(argv[i], "secs=", 5)) secs = std::atof(argv[i] + 5);
        if (!std::strncmp(argv[i], "busydrop=", 9)) busyDrop = std::atof(argv[i] + 9);   // ★ seconds a minute the DSP thread finds the queue busy
        if (!std::strncmp(argv[i], "vfos=", 5)) { vfos.clear(); for (const char* q = argv[i] + 5; *q;) { vfos.push_back(std::atof(q)); q = std::strchr(q, ','); if (!q) break; ++q; } }
    }
    std::vector<cf32> x; uint32_t rate = 0;
    if (!loadIq16(argv[2], x, rate)) { std::printf("cannot read %s\n", argv[2]); return 3; }
    const double fs = (double)rate * L / D, hwC = centre + hw;
    // ★ start at the point of the recording whose UTC, moved on by whole two-minute steps, is now
    double shift = std::floor((vibeUtcNow() - utc0) / 120.0) * 120.0;
    size_t from = (size_t)std::max(0.0, (vibeUtcNow() - utc0 - shift) * rate);
    if (from + (size_t)(30 * rate) > x.size()) { shift += 120.0; from = 0; }
    while (vibeUtcNow() < utc0 + shift + (double)from / rate) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    x.erase(x.begin(), x.begin() + (std::ptrdiff_t)from);
    if (x.size() > secs * rate) x.resize((size_t)(secs * rate));
    std::printf("replay: %.0f s from %.1f s into the recording, capture %.0f S/s centred %.3f MHz (+%.0f Hz), %zu Band VFOs\n",
                x.size() / (double)rate, from / (double)rate, fs, centre / 1e6, hw, vfos.size());
    gallery::Store gs; gs.setDir(tmpDir("real"));
    vibestations::Store st(nullptr, false);
    std::mutex hm; std::map<std::string, std::set<std::string>> calls; std::map<std::string, int> spots;
    const int sub = st.subscribe([&](const vibestations::Reception& r) { std::lock_guard<std::mutex> lk(hm); const std::string k = r.mode + " " + r.band; spots[k]++; calls[k].insert(r.call);
        if (std::getenv("VIBE_REPLAY_SHOW")) std::printf("  heard %s %s %s %.0f Hz %+d dB\n", r.mode.c_str(), r.band.c_str(), r.call.c_str(), r.rfHz, r.snr); });
    FileSlots slots("", [] { return 16; });
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.hwOffsetHz = hw;
    { int n = 4096; while (n < 65536 && n < fs / 40.0) n <<= 1; p.fftSize = n; }   // ★ the shim's bgParams
    p.slots = &slots; p.store = &gs; p.stations = &st; p.lat = 52.29; p.lon = -0.85;
    p.profile.centreHz = centre;
    for (double v : vfos) p.profile.vfos.push_back(bg::Vfo{v, "usb", "spots-band"});
    if (std::getenv("VIBE_REPLAY_FULL")) {   // ★ the Pi 500's whole profile 1 (11-25): + 3 DSC, RTTY weather, WEFAX stations
        p.profile.vfos.push_back(bg::Vfo{4205800, "usb", "dsc"}); p.profile.vfos.push_back(bg::Vfo{6310300, "usb", "dsc"});
        p.profile.vfos.push_back(bg::Vfo{8412800, "usb", "dsc"}); p.profile.vfos.push_back(bg::Vfo{4582000, "usb", "rtty-weather"});
        bg::Vfo fx{0, "usb", "wefax-stations"}; fx.fax = { 4610000, 8040000, 3855000, 7880000, 4482900, 8106900, 5336000, 6445500 }; p.profile.vfos.push_back(fx);
    }
    if (std::getenv("VIBE_REPLAY_LOADED")) p.loaded = [] { return true; };   // ★ the server says it is loaded (every engine light)
    if (const char* u = std::getenv("VIBE_REPLAY_UNITS")) { const double units = std::atof(u); p.bandBudgetUnits = [units] { return units; }; }
    std::mutex lm; std::vector<std::string> logs;
    p.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); };
    r.start(p);
    // ×L polyphase interpolation (Blackman-windowed sinc, 16 taps a phase) and the shift to where the capture holds it
    const int K = 16, NT = L * K;
    std::vector<float> h((size_t)NT);
    { const double fc = 0.47 / L; for (int k = 0; k < NT; ++k) { const double m = k - (NT - 1) / 2.0; const double sn = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        h[(size_t)k] = (float)(sn * (0.42 - 0.5 * std::cos(2 * M_PI * k / (NT - 1)) + 0.08 * std::cos(4 * M_PI * k / (NT - 1))) * L); } }
    std::vector<cf32> hist((size_t)K, cf32(0, 0)), up;
    double ph = 0; const double dph = 2 * M_PI * (recCentre - hwC) / fs;
    const size_t chunk = rate / 25;   // 40 ms of recording a chunk
    const auto w0 = std::chrono::steady_clock::now();
    uint64_t produced = 0, phase = 0;
    std::mt19937 dropRng(5);
    const double base = utc0 + shift + (double)from / rate;
    for (size_t o = 0; o < x.size(); o += chunk) {
        const size_t k = std::min(chunk, x.size() - o);
        up.clear(); up.reserve(k * (size_t)L / (size_t)D + 2);
        for (size_t i = 0; i < k; ++i) {
            std::memmove(hist.data() + 1, hist.data(), (size_t)(K - 1) * sizeof(cf32));
            hist[0] = x[o + i];
            for (int q = 0; q < L; ++q) {
                if (phase++ % D) continue;
                float re = 0, im = 0;
                for (int t = 0; t < K; ++t) { const float c = h[(size_t)(t * L + q)]; re += c * hist[(size_t)t].real(); im += c * hist[(size_t)t].imag(); }
                up.push_back(cf32(re, im) * cf32((float)std::cos(ph), (float)std::sin(ph)));
                ph += dph; if (ph > M_PI) ph -= 2 * M_PI;
            }
        }
        produced += up.size();
        const double utc = base + (double)produced / fs;
        std::this_thread::sleep_until(w0 + std::chrono::microseconds((long long)(1e6 * (double)produced / fs)));
        // ★ the Pi's "0.3–0.4 s of IQ dropped a minute": a chunk the DSP thread could not hand over (the worker held the queue)
        if (busyDrop > 0 && dropRng() < busyDrop / 60.0 * (double)std::numeric_limits<uint32_t>::max()) { auto lk = r.holdQueueForTest(); r.feedIq(up.data(), (int)up.size(), utc); continue; }
        r.feedIq(up.data(), (int)up.size(), utc);
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    const std::string status = r.statusJson();
    const uint64_t dropped = r.droppedSamples();
    r.stop();
    st.unsubscribe(sub);
    std::printf("IQ dropped by the runner: %.2f s\n", dropped / fs);
    for (auto& kv : spots) {
        std::printf("%-10s %4d spots, %3zu calls", kv.first.c_str(), kv.second, calls[kv.first].size());
        if (calls[kv.first].size() <= 12) for (const auto& c : calls[kv.first]) std::printf(" %s", c.c_str());
        std::printf("\n");
    }
    for (const auto& l : logs) if (std::getenv("VIBE_REPLAY_ALLLOG") || l.find("Band decode") != std::string::npos || l.find("paused") != std::string::npos) std::printf("log: %s\n", l.substr(0, 420).c_str());
    (void)status;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "real")) return replayReal(argc, argv);
    config();
    oldConfigs();
    endToEnd();
    cwRttyChips();
    livePath();
    switchedOff();
    crowdedSlot();
    if (!std::getenv("VIBE_BG_BAND_NO_COST")) cost();
    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
