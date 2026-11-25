// ★★★ beta94 THE TRANSPONDER SIGNAL FINDER (vibe_xpdr.h, vibe_xpdr_run.h, vibe_sat_engine.h linkXpdr / decoderOf "xpdr" /
// the follow) on SYNTHETIC IQ — docs/v12/TRANSPONDER-FINDER.md. Stuart 11-24 on RS-44: "as soon as a carrier is detected it
// gives you a button to listen to it" · "vfo a tracks the signal".
//   1. the catalogue: RS-44's transponder and beacon are one span; AO-7's Mode B beacon joins Mode B, not Mode A; decoderOf
//   2. VoiceFinder: two SSB voices found with their suppressed carriers (±150 Hz); a steady carrier and keyed CW are NOT voices
//   3. ToneTracker: an FLL starting 40 Hz off a keyed tone lands within 5 Hz
//   4. uplinkDriftHz: the sign and the ratio
//   5. the Run: CW rows (decoded text, the beacon marked) + voice rows in xpdr_status; LISTEN → the follow (waits for the
//      tune, then guess → voice); a manual tune (seq) ends it
//   6. the engine's follow: setFollow slews the shift, nudge ends it, a single channel refuses it
//   7. ★★★ beta94 TRANSPONDER + TELEMETRY (Stuart 11-25, JO-97 / AO-73: "handled as one" · "the audio from the telemetry
//      … listenable too"): FUNcube BPSK 700 Hz off its listing beside an SSB voice — frames decode from the finder's slice,
//      the BPSK is ONE TELEMETRY row (no CW / VOICE row on it), the decoder's own trim follows it, the voice row stays;
//      LISTEN on the telemetry puts VFO A 1.5 kHz below its carrier and follows the decoder's measurement
//   8. the engine: a transponder lock's nominal may sit on the telemetry side of its span (LISTEN lands there)
#include "vibe_xpdr_run.h"
#include "vibe_fctlm_tx.h"
#include "vibe_sat_engine.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) {
    if (c) ++passes; else ++fails;
    std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what.c_str());
}
using cf32 = std::complex<float>;
static const double kRate = 96000.0;

/** A keyed CW signal at `hz`: `text` at `wpm`, repeated, amplitude `a`. */
struct CwSig {
    double hz, a; std::vector<uint8_t> keys; double unit; double ph = 0;
    CwSig(double hz_, double a_, const std::string& text, double wpm) : hz(hz_), a(a_) {
        unit = 1.2 / wpm;
        for (char ch : text) {
            if (ch == ' ') { for (int i = 0; i < 4; ++i) keys.push_back(0); continue; }
            const std::string code = vibe::cw::morseCode(std::string(1, ch));
            for (char e : code) { int n = e == '.' ? 1 : 3; for (int i = 0; i < n; ++i) keys.push_back(1); keys.push_back(0); }
            keys.push_back(0); keys.push_back(0);
        }
        for (int i = 0; i < 7; ++i) keys.push_back(0);
    }
    cf32 at(double t) {
        const size_t k = (size_t)(t / unit) % keys.size();
        ph += 2 * M_PI * hz / kRate;
        return keys[k] ? cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph))) : cf32(0, 0);
    }
};
/** An SSB voice: 40 tones 300–2700 Hz above `carrier` (USB), in syllables (on 70 % of the time, 0.2–0.5 s each). */
struct Voice {
    double carrier, a; std::vector<double> ph, f; std::mt19937 rng; double nextAt = 0; bool on = true; double on1 = 1;
    Voice(double c, double a_, unsigned seed) : carrier(c), a(a_), rng(seed) {
        for (int i = 0; i < 40; ++i) { f.push_back(c + 300 + i * 60.0 + (rng() % 20)); ph.push_back((rng() % 1000) / 159.0); }
    }
    cf32 at(double t) {
        if (t >= nextAt) { on = (rng() % 10) < 7; nextAt = t + 0.2 + (rng() % 300) / 1000.0; on1 = 0.6 + (rng() % 40) / 100.0; }
        if (!on) return cf32(0, 0);
        double re = 0, im = 0;
        for (size_t i = 0; i < f.size(); ++i) { ph[i] += 2 * M_PI * f[i] / kRate; re += std::cos(ph[i]); im += std::sin(ph[i]); }
        const double g = a * on1 / std::sqrt((double)f.size());
        return cf32((float)(g * re), (float)(g * im));
    }
};

/** ★ `test-xpdr --write-clip FILE RATE CENTRE_HZ SECONDS`: unsigned 8-bit IQ for the fake rtl_tcp (--hf FILE --hf-centre
 *  CENTRE_HZ) — RS-44's beacon keyed at 435.605 MHz, a CQ at 435.650 MHz and an SSB voice (carrier 435.622 MHz), on a
 *  little noise: what scripts/test-server-xpdr.mjs and scripts/test-web-xpdr.mjs feed the real server. No Doppler in the
 *  clip: the finder turns the LOCK's Doppler out, so with a lock the rows sit that far from these figures, without one on them. */
static int writeClip(const char* path, double rate, double centre, double sec) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    struct Sig { double hz, a; std::vector<uint8_t> keys; double unit, ph; };
    auto cwOf = [&](double hz, double a, const std::string& text, double wpm) {
        Sig s{hz, a, {}, 1.2 / wpm, 0.0};
        for (char ch : text) {
            if (ch == ' ') { for (int i = 0; i < 4; ++i) s.keys.push_back(0); continue; }
            for (char e : vibe::cw::morseCode(std::string(1, ch))) { int n = e == '.' ? 1 : 3; for (int i = 0; i < n; ++i) s.keys.push_back(1); s.keys.push_back(0); }
            s.keys.push_back(0); s.keys.push_back(0);
        }
        for (int i = 0; i < 7; ++i) s.keys.push_back(0);
        return s;
    };
    std::vector<Sig> cws = { cwOf(435605000 - centre, 0.10, "RS44 RS44 BEACON ", 16), cwOf(435650000 - centre, 0.08, "CQ SAT DE G4ABC G4ABC K ", 22) };
    std::vector<double> vf, vp; std::mt19937 rng(5);
    for (int i = 0; i < 20; ++i) { vf.push_back(435622000 - centre + 300 + i * 120.0 + (rng() % 30)); vp.push_back((rng() % 1000) / 159.0); }
    std::normal_distribution<float> nd(0.0f, 0.012f);
    double nextAt = 0, g = 1; bool on = true;
    const long long N = (long long)(sec * rate);
    std::vector<uint8_t> out; out.reserve(1 << 20);
    for (long long n = 0; n < N; ++n) {
        const double t = n / rate;
        double re = nd(rng), im = nd(rng);
        for (auto& s : cws) {
            s.ph += 2 * M_PI * s.hz / rate;
            if (s.keys[(size_t)(t / s.unit) % s.keys.size()]) { re += s.a * std::cos(s.ph); im += s.a * std::sin(s.ph); }
        }
        if (t >= nextAt) { on = (rng() % 10) < 7; nextAt = t + 0.2 + (rng() % 300) / 1000.0; g = 0.6 + (rng() % 40) / 100.0; }
        for (size_t i = 0; i < vf.size(); ++i) {
            vp[i] += 2 * M_PI * vf[i] / rate;
            if (on) { re += 0.016 * g * std::cos(vp[i]); im += 0.016 * g * std::sin(vp[i]); }
        }
        out.push_back((uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + 127.0 * re))));
        out.push_back((uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + 127.0 * im))));
        if (out.size() >= (1u << 20)) { std::fwrite(out.data(), 1, out.size(), f); out.clear(); }
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 0;
}

/** ★★★ beta94 `test-xpdr --write-clip-tlm FILE RATE CENTRE_HZ TLM_HZ VOICE_CARRIER_HZ`: 10.0 s of u8 IQ — FUNcube BPSK 1200
 *  at TLM_HZ (two frames, WO1 + WO2, seq 777: exactly 12 000 symbols, so the fake rtl_tcp's loop is seamless) beside an
 *  SSB voice (USB, its suppressed carrier at VOICE_CARRIER_HZ), on a little noise. scripts/test-server-xpdr.mjs's AO-73
 *  half: the finder decodes the telemetry while the voice is a row. */
static int writeClipTlm(const char* path, double rate, double centre, double tlmHz, double voiceHz) {
    std::vector<std::vector<uint8_t>> fr = { vibefc::tx::sampleFrame(0, 777), vibefc::tx::sampleFrame(1, 777) };
    std::vector<std::complex<float>> z;
    vibefc::tx::Cfg c; c.rate = rate; c.carrierHz = tlmHz - centre; c.amp = 0.05; c.phase = 0; c.iqOut = &z;
    (void)vibefc::tx::modulate(fr, c, 0.0, 0.0);
    const size_t n = (size_t)std::llround(10.0 * rate);
    if (z.size() < n) return 3;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::vector<double> vf, vp; std::mt19937 rng(5);
    for (int i = 0; i < 20; ++i) { vf.push_back(voiceHz - centre + 300 + i * 120.0 + (rng() % 30)); vp.push_back((rng() % 1000) / 159.0); }
    std::normal_distribution<float> nd(0.0f, 0.012f);
    double nextAt = 0, g = 1; bool on = true;
    std::vector<uint8_t> out; out.reserve(1 << 20);
    for (size_t k = 0; k < n; ++k) {
        const double t = k / rate;
        double re = z[k].real() + nd(rng), im = z[k].imag() + nd(rng);
        if (t >= nextAt) { on = (rng() % 10) < 7; nextAt = t + 0.2 + (rng() % 300) / 1000.0; g = 0.6 + (rng() % 40) / 100.0; }
        for (size_t i = 0; i < vf.size(); ++i) {
            vp[i] += 2 * M_PI * vf[i] / rate;
            if (on) { re += 0.016 * g * std::cos(vp[i]); im += 0.016 * g * std::sin(vp[i]); }
        }
        out.push_back((uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + 127.0 * re))));
        out.push_back((uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + 127.0 * im))));
        if (out.size() >= (1u << 20)) { std::fwrite(out.data(), 1, out.size(), f); out.clear(); }
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 0;
}

/** The rows of an xpdr_status (kind, dial, car) — enough for the checks. */
struct JRow { std::string id, kind; double dial = 0, car = 0, hz = 0; };
static std::vector<JRow> rowsOf(const std::string& j) {
    std::vector<JRow> v;
    size_t k = j.find("\"rows\":[");
    while (k != std::string::npos) {
        k = j.find("{\"id\":\"", k);
        if (k == std::string::npos) break;
        const size_t e = j.find('}', k);
        const std::string r = j.substr(k, e - k);
        JRow x;
        x.id = r.substr(7, r.find('"', 7) - 7);
        auto f = [&](const char* key) { const size_t a = r.find(key); return a == std::string::npos ? 0.0 : std::atof(r.c_str() + a + std::strlen(key)); };
        const size_t ka = r.find("\"kind\":\"");
        if (ka != std::string::npos) x.kind = r.substr(ka + 8, r.find('"', ka + 8) - ka - 8);
        x.dial = f("\"dial\":"); x.car = f("\"car\":"); x.hz = f("\"hz\":");
        v.push_back(x);
        k = e;
    }
    return v;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]));
    if (argc >= 7 && std::string(argv[1]) == "--write-clip-tlm") return writeClipTlm(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6]));
    using namespace vibe::sat;
    // ── 1. the catalogue ──
    std::printf("── 1. the catalogue: transponder + beacon = one span ──\n");
    {
        auto v = curatedCatalogue();
        auto find = [&](const char* id) -> SatDef* { for (auto& d : v) if (d.id == id) return &d; return nullptr; };
        SatDef* rs = find("RS-44");
        ok(rs && rs->tx[0].xpdr() && rs->tx[0].xpdrLo == 435605000 && rs->tx[0].xpdrHi == 435670000 && rs->tx[0].beaconHz == 435605000,
           "RS-44: the span runs from the beacon (435.605) to the transponder's top (435.670)");
        ok(rs && rs->tx[1].xpdr() && xpdrOwnerOf(*rs, 1) == 0, "RS-44's beacon belongs to the transponder (its pick opens the finder)");
        ok(rs && rs->tx[0].inverting, "RS-44 inverts (its note)");
        SatDef* ao7 = find("AO-7");
        ok(ao7 && ao7->tx[0].beaconHz == 145977500 && ao7->tx[1].beaconHz == 29502000, "AO-7: Mode B's beacon with Mode B, Mode A's with Mode A");
        SatDef* to = find("TO-108");
        ok(to && to->tx[0].xpdr() && to->tx[0].beaconHz == 0, "TO-108: no beacon listed — its passband alone");
        std::string p;
        ok(rs && decoderOf(rs->tx[0], p) == "xpdr" && p.find("\"beacon\":435605000") != std::string::npos && p.find("\"inv\":1") != std::string::npos,
           "decoderOf: RS-44's transponder → xpdr (" + p + ")");
        const double w = rs ? xpdrSliceHz(rs->tx[0]) : 0;
        ok(w > 90000 && w < 110000, "RS-44's slice ~99 kHz (" + std::to_string((int)w) + ")");
        SatDef* iss = find("ISS");
        ok(iss && !iss->tx[0].xpdr() && decoderOf(iss->tx[0], p) != "xpdr", "an FM repeater is not a transponder");
        // ★★★ beta94 the FUNcube telemetry rides in its transponder's span
        SatDef* jo = find("JO-97");
        ok(jo && jo->tx[0].tlmHz == 145840000 && jo->tx[0].xpdrLo == 145840000 && jo->tx[0].xpdrHi == 145875000,
           "JO-97: the telemetry (145.840) joins the transponder's span (145.840–145.875)");
        ok(jo && xpdrOwnerOf(*jo, 1) == 0, "JO-97's telemetry belongs to the transponder (its pick opens the finder)");
        ok(jo && decoderOf(jo->tx[0], p) == "xpdr" && p.find("\"tlm\":145840000") != std::string::npos, "decoderOf: JO-97's finder carries \"tlm\" (" + p + ")");
        ok(jo && decoderOf(jo->tx[1], p) == "fctlm" && !jo->tx[1].xpdr(), "the telemetry downlink keeps its own decoder (an unattended pass of it: fctlm)");
        SatDef* ao = find("AO-73");
        ok(ao && ao->tx[0].tlmHz == 145935000 && xpdrOwnerOf(*ao, 1) == 0, "AO-73: telemetry 145.935 with the transponder");
        const double wa = ao ? xpdrSliceHz(ao->tx[0]) : 0;
        ok(wa > 50000 && wa < 56000, "AO-73's slice covers the telemetry too: ~53 kHz (" + std::to_string((int)wa) + ")");
        ok(ao && ao->tx[0].nominalLo() <= 145935000 - 1500 - 3000, "AO-73's lock may put the dial 1.5 kHz below the telemetry (and more)");
        SatDef* mo = find("MO-122");
        ok(mo && mo->tx[0].tlmHz == 0 && xpdrOwnerOf(*mo, 1) < 0, "MO-122's Fox telemetry (heard, not decoded) is not linked");
        ok(rs && rs->tx[0].tlmHz == 0 && decoderOf(rs->tx[0], p) == "xpdr" && p.find("\"tlm\"") == std::string::npos, "RS-44: no telemetry, no \"tlm\"");
    }

    // ── 2. VoiceFinder ──
    std::printf("── 2. VoiceFinder: voices found, carriers and CW are not voices ──\n");
    {
        vibexpdr::VoiceFinder vf(kRate, -40000, 40000, true);
        Voice v1(-12000, 0.02, 1), v2(9400, 0.012, 2);
        CwSig cw(3000, 0.03, "CQ CQ DE G4ABC G4ABC K ", 20);
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.0f, 0.003f);
        double ph = 0;
        std::vector<cf32> buf(4800);
        for (int b = 0; b < 200; ++b) {   // 10 s
            for (int i = 0; i < 4800; ++i) {
                const double t = (b * 4800 + i) / kRate;
                ph += 2 * M_PI * -25000 / kRate;
                buf[i] = cf32(nd(rng), nd(rng)) + v1.at(t) + v2.at(t) + cw.at(t) + cf32((float)(0.05 * std::cos(ph)), (float)(0.05 * std::sin(ph)));
            }
            vf.feed(buf.data(), (int)buf.size());
        }
        auto vs = vf.voices();
        ok(vs.size() == 2, "two voices (" + std::to_string(vs.size()) + ")");
        bool c1 = false, c2 = false;
        for (auto& x : vs) {
            if (std::fabs(x.carrierHz - -12000) < 150) c1 = true;
            if (std::fabs(x.carrierHz - 9400) < 150) c2 = true;
            std::printf("     voice %d: carrier %+.0f Hz, centre %+.0f, %.1f dB, %d frames\n", x.id, x.carrierHz, x.centreHz, x.snrDb, x.frames);
        }
        ok(c1 && c2, "★ each voice's suppressed carrier within 150 Hz (−12000, +9400)");
        bool cwVoice = false, carVoice = false;
        for (auto& x : vs) { if (std::fabs(x.centreHz - 3000) < 1500) cwVoice = true; if (std::fabs(x.centreHz + 25000) < 1500) carVoice = true; }
        ok(!cwVoice && !carVoice, "the keyed CW and the steady carrier are not voices");
    }

    // ── 3. ToneTracker ──
    std::printf("── 3. ToneTracker: an FLL on a keyed tone ──\n");
    {
        CwSig cw(1237, 0.05, "TEST DE RS44 ", 18);
        vibexpdr::ToneTracker tt(kRate, 1197);
        std::mt19937 rng(3); std::normal_distribution<float> nd(0.0f, 0.004f);
        std::vector<cf32> buf(4800);
        for (int b = 0; b < 120; ++b) {
            for (int i = 0; i < 4800; ++i) buf[i] = cw.at((b * 4800 + i) / kRate) + cf32(nd(rng), nd(rng));
            tt.feed(buf.data(), (int)buf.size());
        }
        ok(tt.measured() && std::fabs(tt.hz() - 1237) < 5, "from 40 Hz off to " + std::to_string(tt.hz()) + " Hz (1237)");
    }

    // ── 4. the first guess ──
    std::printf("── 4. uplinkDriftHz ──\n");
    {
        const double d = vibexpdr::uplinkDriftHz(9000, 145960000, 435640000, true);
        ok(d < 0 && std::fabs(d + 9000 * 145960000.0 / 435640000.0) < 1, "inverting, 2 m up / 70 cm down: −⅓ of our +9 kHz (" + std::to_string((int)d) + " Hz)");
        ok(vibexpdr::uplinkDriftHz(9000, 145960000, 435640000, false) > 0, "non-inverting: the same way as ours");
        ok(vibexpdr::uplinkDriftHz(9000, 0, 435640000, true) == 0, "no uplink listed: no guess");
    }

    // ── 5. the Run ──
    std::printf("── 5. the Run: rows, LISTEN, the follow, a manual tune ──\n");
    {
        const double C = 435637500;   // RS-44's span centre (satellite frame)
        vibexpdr::Run::Opts o;
        o.widthHz = 99000; o.centreHz = C; o.spanLoHz = 435605000; o.spanHiHz = 435670000; o.beaconHz = 435605000; o.upHz = 145935000;
        double dop = 8000;   // the model's Doppler at the centre (constant here: the slice arrives already turned the same)
        o.dopplerHz = [&](double hz) { return dop * hz / C; };
        std::string lastJson; int statuses = 0;
        o.json = [&](const std::string& j) { lastJson = j; ++statuses; };
        double nominal = 0; bool following = false; unsigned seq = 1; double setTo = 0; int sets = 0;
        o.follow.state = [&](double& n, bool& f, unsigned& s) { n = nominal; f = following; s = seq; return true; };
        o.follow.set = [&](double hz) { setTo = hz; ++sets; following = true; };
        o.follow.clear = [&] { following = false; };
        vibexpdr::Run run(o);
        // the IQ as the host cuts it: centred on the span's satellite-frame centre (RF C) — so the run turns +8 kHz out of it;
        // build the signals at their RF (satellite frame + Doppler) relative to the slice's centre C
        CwSig beacon(435605000 + dop - C, 0.04, "RS44 BEACON ", 16);
        CwSig cq(435650000 + dop - C, 0.03, "CQ SAT DE G4ABC G4ABC K ", 22);
        Voice v(435622000 + dop - C, 0.025, 11);
        std::mt19937 rng(9); std::normal_distribution<float> nd(0.0f, 0.003f);
        std::vector<cf32> buf(4800);
        double t = 0;
        auto run_for = [&](double sec) {
            for (int b = 0; b < (int)(sec * kRate / 4800); ++b) {
                for (int i = 0; i < 4800; ++i) { const double tt = t + i / kRate; buf[i] = beacon.at(tt) + cq.at(tt) + v.at(tt) + cf32(nd(rng), nd(rng)); }
                t += 4800 / kRate;
                run.feedIq(buf.data(), (int)buf.size(), kRate, C);
            }
        };
        run_for(40);
        ok(statuses >= 35, "xpdr_status once a second (" + std::to_string(statuses) + ")");
        ok(lastJson.find("\"slice\":{\"lo\":") != std::string::npos && lastJson.find("\"beacon\":435605000") != std::string::npos, "the slice and the beacon in the status");
        ok(lastJson.find("\"kind\":\"voice\"") != std::string::npos, "★ a voice row");
        ok(lastJson.find("\"beacon\":true") != std::string::npos, "★ the beacon is a CW row, marked");
        ok(lastJson.find("G4ABC") != std::string::npos, "★ the CQ's callsign decoded on its CW row");
        std::printf("     %s\n", lastJson.substr(0, 600).c_str());
        // the voice row's id and dial
        std::string vid; double vdial = 0;
        {
            const size_t k = lastJson.find("\"kind\":\"voice\"");
            const size_t a = lastJson.rfind("\"id\":\"", k);
            vid = lastJson.substr(a + 6, lastJson.find('"', a + 6) - a - 6);
            const size_t d = lastJson.find("\"dial\":", k);
            vdial = std::atof(lastJson.c_str() + d + 7);
        }
        ok(std::fabs(vdial - 435622000) < 150, "the voice's dial = its carrier, satellite frame (" + std::to_string((long long)vdial) + ")");
        // LISTEN: the follow waits for the tune to land
        nominal = 435640000;   // somewhere else
        run.follow(vid);
        run_for(2);
        ok(sets == 0 && lastJson.find("\"src\":\"wait\"") != std::string::npos, "LISTEN asked: the follow waits until the dial is there");
        nominal = vdial; ++seq;   // the client's LISTEN tune lands (it is a tune: seq moves)
        run_for(3);
        ok(sets > 0 && std::fabs(setTo - 435622000) < 200, "★ the dial landed: VFO A follows the voice (" + std::to_string((long long)setTo) + ")");
        ok(lastJson.find("\"src\":\"voice\"") != std::string::npos || lastJson.find("\"src\":\"held\"") != std::string::npos, "measured from the voice (or held between overs)");
        // the station drifts (their uncorrected uplink): the voice moves +400 Hz; the follow goes with it
        v = Voice(435622400 + dop - C, 0.025, 11);
        run_for(8);
        ok(std::fabs(setTo - 435622400) < 200, "★ the voice drifted 400 Hz: VFO A went with it (" + std::to_string((long long)setTo) + ")");
        // a manual tune: seq moves → the follow ends
        ++seq; nominal = 435630000;
        const int before = sets;
        run_for(2);
        ok(lastJson.find("\"follow\"") == std::string::npos && sets == before, "★ a manual tune: the follow ends, nothing more is set");
    }

    // ── 6. the engine's follow ──
    std::printf("── 6. the engine: setFollow, nudge ──\n");
    {
        Engine e;
        std::string js;
        { FILE* f = std::fopen("vibeserver/testdata-sat/celestrak-amateur-2026-11-19.json", "rb");
          if (f) { char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n); std::fclose(f); } }
        ok(e.loadOmm(js) > 0, "CelesTrak's amateur sets (testdata-sat, 2026-11-19)");
        e.setObserver(52.31, -0.88);
        {
            Clock clk; clk.unix = 1795564800.0 + 0; clk.measured = true;   // 2026-11-25 00:00 UTC — six days from the sets (stale, still tracked)
            bool up = false;
            for (int k = 0; k < 2 * 86400 && !up; k += 30) {
                clk.unix = 1795564800.0 + k;
                Track t = e.start("s1", "o", 435640000, "usb", clk, 44909, 0);
                if (t.state == LockState::Track) up = true; else e.stop("s1");
            }
            ok(up, "RS-44 above the horizon at some time in the next two days");
            if (up) {
                auto f0 = e.followState("s1");
                ok(f0.lock && f0.passband && !f0.following, "a passband lock, not following");
                ok(e.setFollow("s1", f0.nominal + 1200), "setFollow +1200 Hz");
                Track t;
                for (int i = 0; i < 25 * 12; ++i) { clk.unix += 0.04; t = e.tick("s1", clk); }
                ok(std::fabs(t.followHz - 1200) < 1 && std::fabs(t.shiftHz - (t.modelHz + t.trimHz + 1200)) < 1, "★ slewed in: the follow is part of the shift (" + std::to_string(t.followHz) + ")");
                const unsigned s0 = e.followState("s1").seq;
                ok(e.nudge("s1", f0.nominal + 3000), "a manual tune inside the passband");
                auto f1 = e.followState("s1");
                t = e.tick("s1", clk);
                ok(!f1.following && f1.seq == s0 + 1 && t.followHz == 0, "★ the manual tune wins: no follow, seq moved");
                e.setFollow("s1", f1.nominal + 99999);
                for (int i = 0; i < 25 * 60; ++i) { clk.unix += 0.04; t = e.tick("s1", clk); }
                ok(std::fabs(t.followHz) <= Engine::kFollowClampHz + 0.5, "the follow is clamped to ±6 kHz");
                Track b = e.start("s2", "o", 435605000, "cw", clk, 44909, 1);
                ok(b.state == LockState::Track && !e.setFollow("s2", 435606000), "a single channel (the beacon) refuses a follow");
            }
        }
    }
    // ── 7. transponder + telemetry ──
    std::printf("── 7. transponder + telemetry: one slice, frames, the TELEMETRY row, its LISTEN ──\n");
    {
        const double C = 145952500, TLM = 145935000, ERR = 700;   // AO-73's span centre; the bird 700 Hz above its listing
        vibexpdr::Run::Opts o;
        o.widthHz = 53000; o.centreHz = C; o.spanLoHz = 145935000; o.spanHiHz = 145970000; o.upHz = 435130000; o.tlmHz = TLM;
        double dop = -2500;
        o.dopplerHz = [&](double hz) { return dop * hz / C; };
        std::string lastJson; int statuses = 0, tlmFrames = 0, fcStatus = 0, lines = 0;
        o.json = [&](const std::string& j) { lastJson = j; ++statuses; };
        o.tlmOut.json = [&](const std::string& j) {
            if (j.find("\"sat_tlm\"") != std::string::npos) ++tlmFrames;
            if (j.find("\"fctlm_status\"") != std::string::npos) ++fcStatus;
        };
        o.tlmOut.line = [&](const std::string&) { ++lines; };
        o.tlmOut.satName = [] { return std::string("AO-73"); };
        double nominal = 145960000; bool following = false; unsigned seq = 1; double setTo = 0; int sets = 0;
        o.follow.state = [&](double& n, bool& f, unsigned& s) { n = nominal; f = following; s = seq; return true; };
        o.follow.set = [&](double hz) { setTo = hz; ++sets; following = true; };
        o.follow.clear = [&] { following = false; };
        vibexpdr::Run run(o);
        ok(run.tlm() != nullptr, "the finder owns a FUNcube decoder (\"tlm\" in its span)");
        // the BPSK at its RF (satellite frame + Doppler) relative to the slice's centre, 12 frames (~60 s)
        std::vector<std::vector<uint8_t>> fr;
        for (int i = 0; i < 12; ++i) fr.push_back(vibefc::tx::sampleFrame(i % 3, 900 + (uint32_t)i));
        std::vector<cf32> bpsk;
        vibefc::tx::Cfg c; c.rate = kRate; c.carrierHz = TLM + ERR + dop - C; c.amp = 0.03; c.iqOut = &bpsk;
        (void)vibefc::tx::modulate(fr, c, 1.0, 1.0);
        Voice v(145958000 + dop - C, 0.025, 21);
        std::mt19937 rng(13); std::normal_distribution<float> nd(0.0f, 0.003f);
        std::vector<cf32> buf(4800);
        size_t k0 = 0; double t = 0;
        auto run_for = [&](double sec) {
            for (int b = 0; b < (int)(sec * kRate / 4800); ++b) {
                for (int i = 0; i < 4800; ++i) {
                    const double tt = t + i / kRate;
                    const cf32 s = k0 + i < bpsk.size() ? bpsk[k0 + i] : cf32(0, 0);
                    buf[i] = s + v.at(tt) + cf32(nd(rng), nd(rng));
                }
                k0 += 4800; t += 4800 / kRate;
                run.feedIq(buf.data(), (int)buf.size(), kRate, C);
            }
        };
        run_for(30);
        ok(tlmFrames >= 3, "★ FUNcube frames decode from the finder's slice (" + std::to_string(tlmFrames) + " sat_tlm, " + std::to_string(lines) + " gallery lines)");
        ok(fcStatus >= 25, "fctlm_status once a second, as a telemetry pick's (" + std::to_string(fcStatus) + ")");
        ok(lastJson.find("\"tlm\":145935000") != std::string::npos, "the listed telemetry in xpdr_status");
        auto rows = rowsOf(lastJson);
        const JRow* tr = nullptr; bool onTlm = false, voice = false;
        for (auto& r : rows) {
            if (r.kind == "tlm") tr = &r;
            else if (std::fabs(r.dial - (TLM + ERR)) <= 1500 || (r.kind == "voice" && std::fabs(r.dial - (TLM + ERR)) <= 3000)) onTlm = true;
            if (r.kind == "voice" && std::fabs(r.dial - 145958000) < 200) voice = true;
        }
        std::printf("     %s\n", lastJson.substr(0, 700).c_str());
        ok(tr != nullptr, "★ a TELEMETRY row");
        ok(!onTlm, "★ no CW or VOICE row on the BPSK");
        ok(voice, "★ the SSB voice in the transponder is still its own row");
        ok(tr && std::fabs(tr->car - (TLM + ERR)) < 60, "the telemetry's carrier measured, satellite frame (" + std::to_string(tr ? (long long)tr->car : 0LL) + ", the bird at 145935700)");
        ok(tr && std::fabs(tr->dial - (tr->car - 1500)) < 1, "its LISTEN dial is 1.5 kHz below the carrier (USB: the carrier heard at 1.5 kHz)");
        ok(tr && std::fabs(tr->hz - (tr->car + dop)) < 2, "its arrow at RF now (carrier + Doppler)");
        // LISTEN on the telemetry: waits for the tune, then VFO A follows the decoder's measurement
        run.follow("t1");
        run_for(1);
        ok(sets == 0 && lastJson.find("\"src\":\"wait\"") != std::string::npos, "LISTEN on the telemetry: the follow waits for the tune");
        nominal = std::round(tr ? tr->dial : TLM - 800); ++seq;
        run_for(3);
        ok(sets > 0 && std::fabs(setTo - (TLM + ERR - 1500)) < 60, "★ VFO A follows: 1.5 kHz below the telemetry's real carrier (" + std::to_string((long long)setTo) + ")");
        ok(lastJson.find("\"src\":\"tlm\"") != std::string::npos, "measured by the telemetry decoder (src tlm)");
        run_for(24);
        ok(std::fabs(run.tlmTrim() - ERR) < 120, "★ the decoder's own trim took it to the bird (" + std::to_string((int)run.tlmTrim()) + " Hz of 700) — the lock never asked");
        ok(std::fabs(setTo - (TLM + ERR - 1500)) < 60, "★ the trim moved the decoder's aim, not VFO A: the follow still on the carrier − 1500 (" + std::to_string((long long)setTo) + ")");
        ok(tlmFrames >= 8, "frames all the while (" + std::to_string(tlmFrames) + ")");
        ++seq; nominal = 145960000;
        const int before = sets;
        run_for(1);
        ok(lastJson.find("\"follow\"") == std::string::npos && sets == before, "★ a manual tune ends the telemetry's follow too");
    }

    // ── 8. the engine: the telemetry side of the span ──
    std::printf("── 8. the engine: a transponder lock's dial on the telemetry side ──\n");
    {
        Engine e;
        std::string js;
        { FILE* f = std::fopen("vibeserver/testdata-sat/celestrak-amateur-2026-11-19.json", "rb");
          if (f) { char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n); std::fclose(f); } }
        ok(e.loadOmm(js) > 0, "CelesTrak's amateur sets");
        e.setObserver(52.31, -0.88);
        Clock clk; clk.measured = true;
        bool up = false;
        for (int k = 0; k < 2 * 86400 && !up; k += 30) {
            clk.unix = 1795564800.0 + k;
            Track t = e.start("a", "o", 145960000, "usb", clk, 39444, 0);
            if (t.state == LockState::Track) up = true; else e.stop("a");
        }
        ok(up, "AO-73 above the horizon at some time in the next two days");
        if (up) {
            ok(e.nudge("a", 145933500) && std::fabs(e.followState("a").nominal - 145933500) < 1, "★ a tune 1.5 kHz below the telemetry keeps the lock, the nominal THERE (not clamped to 145.950)");
            ok(e.setFollow("a", 145933700), "the follow from there");
            ok(!e.nudge("a", 145900000), "a tune far below the span still ends the lock");
        }
    }
    std::printf("%s  %d passed, %d failed\n", fails ? "FAIL" : "ok ", passes, fails);
    return fails ? 1 : 0;
}
