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
#include "vibe_xpdr_run.h"
#include "vibe_sat_engine.h"

#include <cstdio>
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

int main() {
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
    std::printf("%s  %d passed, %d failed\n", fails ? "FAIL" : "ok ", passes, fails);
    return fails ? 1 : 0;
}
