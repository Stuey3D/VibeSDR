// test-avi-service.cpp — V12 aviation service (vibe_avi_service.h): the aircraft table merges VDL2 (ICAO address),
// ACARS (registration) and HFDL (ICAO / flight) into one row per aircraft; message bodies only with the owner's opt-in;
// the thread drops the OLDEST block when it falls behind and counts it. docs/v12/AVIATION.md §Service.
//  `test-avi-service --cu8 <vdl2.u8> <rate> [hwCentreHz]` runs a real VDL2 capture through the service and prints the table
//  (the centre: 136.850 MHz unless given — the mode's band sits at 136.8625).
// ★★ V12 2026-11-24 — and the mode around it, on the radios Stuart asked about:
//    3. the Airspy HF+ (912 / 768 kS/s, the bands' own centres, every channel): ACARS and VDL2 decode, and the burst modes'
//       gain rule steps NOTHING on it (vibe_mode_gain.h) — the report names its AGC / attenuator, never a dB figure;
//    4. ACARS + VDL2 TOGETHER on one wide capture (6.144 and 8 MS/s, 134.25 MHz): both decoders on their own threads, one
//       aircraft heard on both merged into one row; and who is OFFERED it — never an RTL, never the HF+, an RSP at 2 MS/s
//       only with the owner's 8 MS/s switch (and then the rate it goes to), never with the rate pinned or the guard set.
#include "vibe_avi_service.h"
#include "vibe_avi_mode.h"
#include "vibe_avi_tx.h"
#include "vibe_mode_gain.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)


// ── synthetic captures through the SERVICE, fed in real time (as the DSP thread would) ─────────────────────────────────
struct Burst { bool vdl2; double hz; std::string reg; uint32_t icao; double at; };
static std::vector<vibeavi::cf32> makeCapture(double fs, double centre, double durS, const std::vector<Burst>& bursts, uint32_t seed) {
    const double sigma = 0.01;
    std::vector<vibeavi::cf32> cap(size_t(fs * durS));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(sigma));
    for (auto& v : cap) v = vibeavi::cf32(g(rng), g(rng));
    int k = 0;
    for (const Burst& b : bursts) {
        char txt[64]; std::snprintf(txt, sizeof txt, "M%02dAEZ%04d/TEST %d", k % 100, 1000 + k, k);
        if (b.vdl2) {
            const double nPow = 2 * sigma * sigma * vibevdl2::kSymRate / fs, A = std::sqrt(nPow * std::pow(10.0, 25.0 / 10));
            auto f = vibeavitx::avlcAcars(b.icao, 0x10949Au, b.reg, "H1", char('0' + k % 10), txt);
            vibeavitx::vdl2Burst(cap, fs, b.hz - centre, A, size_t(fs * b.at), vibeavitx::vdl2Symbols({ f }), 0.4 * k);
        } else {
            const double N0 = 2 * sigma * sigma / fs, A = std::sqrt(std::pow(10.0, 56.0 / 10) * N0);
            auto blk = vibeavitx::makeBlock('2', b.reg, 0x15, "H1", char('0' + k % 10), txt);
            vibeavitx::acarsBurst(cap, fs, b.hz - centre, A, size_t(fs * b.at), vibeavitx::airBits(blk));
        }
        ++k;
    }
    return cap;
}
struct Run { uint64_t msgs = 0, dropped = 0; size_t rows = 0, lanes = 0; std::string json; };
static Run runService(const char* kind, double fs, double centre, const std::vector<double>& chans, const std::vector<vibeavi::cf32>& cap) {
    vibeavi::Tracker tr(false);
    vibeavi::Service svc(tr);
    svc.start(kind, centre, fs, chans);
    Run r; r.lanes = svc.lanes();
    const size_t blk = size_t(fs / 50);   // 20 ms blocks, as the DSP hands them on
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o < cap.size(); o += blk) {
        svc.feed(cap.data() + o, std::min(blk, cap.size() - o));
        const double ahead = double(o + blk) / fs - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));   // the channelisers' and workers' tails
    svc.stop();
    r.msgs = svc.messages(); r.dropped = svc.dropped(); r.rows = tr.size(); r.json = tr.json(1e18, 1e30);
    std::printf("   %-10s at %.3f MS/s: %llu messages, %zu aircraft, %llu blocks dropped, %zu decoder thread(s)\n", kind, fs / 1e6,
                (unsigned long long)r.msgs, r.rows, (unsigned long long)r.dropped, r.lanes);
    return r;
}

static void hfPlusAndCombined() {
    using namespace vibeavi;
    const Band* acars = bandById("acars"); const Band* vdl2 = bandById("vdl2"); const Band* both = bandById("acars+vdl2");
    std::printf("3. the Airspy HF+: ACARS and VDL2 at its own rates, nothing stepped, the report true\n");
    CHECK(acars && vdl2 && both, "the three bands");
    if (!acars || !vdl2 || !both) return;
    // ★ the HF+ crops 72 kHz of filter skirt each side at 912 kS/s (768 kHz shown), 60 kHz at 768 (648 shown):
    //   every channel of either band must sit inside the receivable part, with the channeliser's 10 % edge
    for (double fs : { 912000.0, 768000.0 }) {
        const double usable = fs - 2.0 * (fs > 768001.0 ? (fs - 768000.0) / 2.0 : 60000.0);
        CHECK(acars->minRateHz() <= usable && vdl2->minRateHz() <= usable,
              "%.0f kS/s: ACARS needs %.0f, VDL2 %.0f kS/s of the %.0f receivable", fs / 1e3, acars->minRateHz() / 1e3, vdl2->minRateHz() / 1e3, usable / 1e3);
        CHECK(!offered(*both, usable, wideBoostRate(true, false, false, { fs })), "%.0f kS/s: ACARS + VDL2 never offered (no 8 MS/s, switch or not)", fs / 1e3);
    }
    for (double fs : { 912000.0, 768000.0 }) {
        std::vector<Burst> bv;
        double t = 0.05;
        for (size_t c = 0; c < vdl2->chans.size(); ++c, t += 0.06) bv.push_back({ true, vdl2->chans[c], ".G-HFP" + std::to_string(c % 10), 0x400200u + uint32_t(c), t });
        const Run rv = runService("vdl2", fs, vdl2->centreHz, vdl2->chans, makeCapture(fs, vdl2->centreHz, t + 0.15, bv, 7));
        CHECK(rv.msgs == bv.size() && rv.dropped == 0, "%.0f kS/s VDL2 at 136.8625: %llu of %zu channels decoded (every one, 136.700 to 136.975), %llu dropped",
              fs / 1e3, (unsigned long long)rv.msgs, bv.size(), (unsigned long long)rv.dropped);
        std::vector<Burst> ba;
        t = 0.05;
        for (size_t c = 0; c < acars->chans.size(); ++c, t += 0.3) ba.push_back({ false, acars->chans[c], ".G-HFA" + std::to_string(c), 0, t });
        const Run ra = runService("acars", fs, acars->centreHz, acars->chans, makeCapture(fs, acars->centreHz, t + 0.15, ba, 8));
        CHECK(ra.msgs == ba.size() && ra.dropped == 0, "%.0f kS/s ACARS at 131.675: %llu of %zu channels decoded, %llu dropped",
              fs / 1e3, (unsigned long long)ra.msgs, ba.size(), (unsigned long long)ra.dropped);
    }
    {
        // ★ the gain: an R820T's list steps (85 % up: 43.4 dB); the HF+ steps nothing, whatever list it is handed
        const std::vector<int> r820 = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };
        CHECK(vibemodegain::steps("rtl", r820) && r820[size_t(vibemodegain::startIdx("rtl", r820))] == 434, "an RTL: stepped, from 43.4 dB");
        CHECK(!vibemodegain::steps("airspyhf", r820) && vibemodegain::startIdx("airspyhf", r820) == -1, "the HF+: no gain step, even handed a list");
        CHECK(!vibemodegain::steps("airspyhf", {}) && !vibemodegain::steps("sdrplay", {}), "no list, no steps");
        std::string hf = "{\"type\":\"avdata\",\"msgs\":3}";
        vibemodegain::appendGain(hf, true, 434, true, vibemodegain::hfNote(true, false, 2, true));
        CHECK(hf.find("\"gain\":") == std::string::npos && hf.find("\"gainAuto\"") == std::string::npos
              && hf.find("\"gainNote\":\"HF+ AGC \xc2\xb7 attenuator 12 dB \xc2\xb7 preamp\"}") != std::string::npos, "HF+ report: %s", hf.c_str());
        std::string hf2 = "{\"type\":\"ais\"}";
        vibemodegain::appendGain(hf2, true, -1, false, vibemodegain::hfNote(false, false, 0, false));
        CHECK(hf2 == "{\"type\":\"ais\",\"gainNote\":\"HF+ AGC off\"}", "HF+ AGC off: %s", hf2.c_str());
        std::string rtl = "{\"type\":\"avdata\"}";
        vibemodegain::appendGain(rtl, false, 434, true, "");
        CHECK(rtl == "{\"type\":\"avdata\",\"gain\":43.4,\"gainAuto\":1}", "RTL report unchanged: %s", rtl.c_str());
    }

    std::printf("4. ACARS + VDL2 together on one wide capture\n");
    CHECK(std::fabs(both->centreHz - 134.25e6) < 1 && both->chans.size() == acars->chans.size() + vdl2->chans.size() && both->wide,
          "134.25 MHz, all %zu channels, wide", both->chans.size());
    {
        double nearest = 1e18;
        for (double h : both->chans) nearest = std::min(nearest, std::fabs(h - both->centreHz));
        CHECK(nearest > 2.0e6, "the DC is %.3f MHz from the nearest channel", nearest / 1e6);
        CHECK(both->minRateHz() > 6.0e6 && both->minRateHz() < 6.144e6, "needs %.3f MS/s: 6.144 holds it, 6.0 does not", both->minRateHz() / 1e6);
    }
    // ★ who is offered it
    const std::vector<double> rtlRates = { 250000, 1024000, 1536000, 1792000, 1920000, 2048000, 2160000, 2400000 };
    const std::vector<double> rspRates = { 8000000, 6000000, 5000000, 4000000, 3000000, 2048000, 2000000 };
    const std::vector<double> r2Rates = { 10000000, 2500000 };
    CHECK(!offered(*both, 2.4e6, wideBoostRate(true, false, false, rtlRates)), "an RTL at 2.4 MS/s: never (even with the switch)");
    CHECK(boostRateFrom(rtlRates) == 0, "an RTL lists no rate to raise to");
    CHECK(offered(*acars, 2.4e6) && offered(*vdl2, 2.4e6), "…but ACARS and VDL2 are");
    CHECK(offered(*both, 6.144e6) && offered(*both, 8e6) && !needsBoost(*both, 8e6, 8e6), "a running 6.144 / 8 MS/s capture: offered, no rate change");
    CHECK(!offered(*both, 2.0e6, wideBoostRate(false, false, false, rspRates)), "an RSP at 2 MS/s, switch off: not offered");
    CHECK(!offered(*both, 6.0e6, wideBoostRate(false, false, false, rspRates)), "an RSP at 6 MS/s, switch off: not offered (6.083 needed)");
    CHECK(wideBoostRate(true, false, false, rspRates) == 8e6 && offered(*both, 2.0e6, 8e6) && needsBoost(*both, 2.0e6, 8e6),
          "an RSP at 2 MS/s, switch on: offered, raised to 8 MS/s");
    CHECK(!needsBoost(*vdl2, 2.0e6, 8e6) && !needsBoost(*acars, 2.0e6, 8e6), "a single band never raises the rate");
    CHECK(wideBoostRate(true, true, false, rspRates) == 0 && wideBoostRate(true, false, true, rspRates) == 0,
          "never with the owner's rate pinned, never after the guard recorded a fall-over");
    CHECK(wideBoostRate(true, false, false, r2Rates) == 10e6, "an Airspy R2: its 10 MS/s (it has no 8)");
    // ★ both decoders, their own threads, one aircraft heard on both = one row
    for (double fs : { 6.144e6, 8.0e6 }) {
        const std::vector<Burst> bb = { { false, 131.725e6, ".G-JZHU", 0, 0.05 }, { true, 136.975e6, ".G-JZHU", 0x4070E8u, 0.32 },
                                        { false, 131.525e6, ".EI-EST", 0, 0.40 }, { true, 136.700e6, ".D-AIJH", 0x3C6548u, 0.66 } };
        const Run rb = runService("acars+vdl2", fs, both->centreHz, both->chans, makeCapture(fs, both->centreHz, 0.85, bb, 9));
        CHECK(rb.lanes == 2, "%.3f MS/s: two decoder threads (%zu)", fs / 1e6, rb.lanes);
        CHECK(rb.msgs == bb.size() && rb.dropped == 0, "%.3f MS/s: %llu of %zu bursts decoded (ACARS at both ends of its set, VDL2 at both ends of its), %llu dropped",
              fs / 1e6, (unsigned long long)rb.msgs, bb.size(), (unsigned long long)rb.dropped);
        CHECK(rb.rows == 3 && rb.json.find("\"icao\":\"4070E8\"") != std::string::npos && rb.json.find("G-JZHU") != std::string::npos,
              "%.3f MS/s: G-JZHU heard on ACARS and VDL2 is ONE row (%zu rows)", fs / 1e6, rb.rows);
    }
}

int main(int argc, char** argv) {
    if (argc > 3 && !std::strcmp(argv[1], "--cu8")) {
        vibeavi::Tracker tr(false);
        vibeavi::Service svc(tr);
        const double rate = std::atof(argv[3]);
        svc.start("vdl2", argc > 4 ? std::atof(argv[4]) : 136.850e6, rate, vibevdl2::defaultChannels());
        FILE* f = std::fopen(argv[2], "rb");
        std::vector<uint8_t> buf(1 << 17); std::vector<vibeavi::cf32> z(1 << 16);
        size_t r;
        while (f && (r = std::fread(buf.data(), 2, 1 << 16, f)) > 0) {
            vibeavi::u8ToCf(buf.data(), r, z.data());
            svc.feed(z.data(), r);
            std::this_thread::sleep_for(std::chrono::microseconds(2000));   // faster than real time, not unbounded
        }
        if (f) std::fclose(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        svc.stop();
        std::printf("messages %llu, dropped blocks %llu\n%s\n", (unsigned long long)svc.messages(), (unsigned long long)svc.dropped(), tr.json(1e18, 1e30).c_str());
        return 0;
    }
    std::printf("1. one row per aircraft, from three decoders\n");
    {
        vibeavi::Tracker tr(false);
        vibevdl2::Frame v; v.src.addr = 0x4070E8; v.src.type = 1; v.dst.addr = 0x109E6A; v.dst.type = 4; v.hz = 136.975e6; v.levelDb = -20;
        v.hasAcars = true; v.acarsCrcOk = true; v.acars.reg = "G-JZHU"; v.acars.flight = "LS08DE"; v.acars.label = "H1"; v.acars.text = "#DFB SECRET";
        tr.addVdl2(v, 100);
        vibeacars::Block b; b.reg = "G-JZHU"; b.flight = "LS08DE"; b.label = "16"; b.text = "POS N51"; b.hz = 131.725e6; b.levelDb = -30;
        tr.addAcars(b, 110);
        vibehfdl::Message h; h.downlink = true; h.icao = 0x4070E8; h.hz = 8977e3; h.dstId = 3; h.flight = "EXS8DE"; h.lat = 51.0; h.lon = -1.0;
        tr.addHfdl(h, 120);
        vibevdl2::Frame g; g.src.addr = 0x10949A; g.src.type = 4; g.dst.addr = 0; g.dst.type = 7;   // ground to all: no aircraft
        tr.addVdl2(g, 121);
        CHECK(tr.size() == 1, "rows %zu (VDL2 ICAO + ACARS registration + HFDL ICAO = one aircraft)", tr.size());
        const std::string j = tr.json(130);
        CHECK(j.find("\"icao\":\"4070E8\"") != std::string::npos && j.find("\"reg\":\"G-JZHU\"") != std::string::npos && j.find("\"lat\":51.0000") != std::string::npos,
              "merged row: %s", j.c_str());
        CHECK(j.find("SECRET") == std::string::npos && j.find("POS N51") == std::string::npos, "no message body without the opt-in: %s", j.c_str());
        CHECK(j.find("\"via\":\"HFDL 8977 Reykjavik\"") != std::string::npos, "via the last decoder: %s", j.c_str());
        tr.setBodies(true);
        tr.addAcars(b, 140);
        CHECK(tr.json(141).find("POS N51") != std::string::npos, "the body with the owner's opt-in");
        tr.setBodies(false);
        CHECK(tr.json(141).find("POS N51") == std::string::npos, "and gone again when it is switched off");
    }
    std::printf("2. the thread drops the oldest and counts it (a decoder that cannot keep up never stalls the radio)\n");
    {
        vibeavi::Tracker tr;
        vibeavi::Service svc(tr);
        svc.start("vdl2", 136.850e6, 2.4e6, vibevdl2::defaultChannels());
        std::vector<vibeavi::cf32> blk(1 << 16, vibeavi::cf32(0.01f, 0));
        for (int i = 0; i < 400; ++i) svc.feed(blk.data(), blk.size());   // 11 s of capture at once
        svc.stop();
        std::printf("   dropped %llu blocks of 400\n", (unsigned long long)svc.dropped());
        CHECK(svc.dropped() > 0, "drops counted");
    }
    hfPlusAndCombined();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    {   // ★★ beta96 MESSAGES PER MINUTE (Stuart 11-26: "like ais … check antenna performance quickly") — the last 60 s
        std::printf("── messages per minute ──\n");
        vibeavi::Tracker tr(false);
        auto rate = [&](double t) { const std::string j = tr.json(t); const size_t k = j.find("\"msgRate\":"); return k == std::string::npos ? -1.0 : std::atof(j.c_str() + k + 10); };
        vibeacars::Block b; b.reg = "G-EUPT"; b.flight = "BA123"; b.label = "H1"; b.hz = 131.725e6; b.levelDb = -30;
        for (int i = 0; i < 30; ++i) tr.addAcars(b, 1000 + i);                 // 30 in the first 30 s
        const double r1 = rate(1030);
        CHECK(r1 > 59 && r1 < 61, "30 messages in the first 30 s listened = 60 /min, not 30 (the span is the time listened): %.2f", r1);
        for (int i = 0; i < 30; ++i) tr.addAcars(b, 1030 + i);                 // another 30: 60 in the last minute
        const double r2 = rate(1060);
        CHECK(r2 > 59 && r2 < 61, "60 in the last 60 s = 60 /min: %.2f", r2);
        const double r3 = rate(1090);
        CHECK(r3 > 29 && r3 < 31, "half of them older than a minute → 30 /min: %.2f", r3);
        CHECK(rate(1200) == 0, "a quiet minute → 0 /min (the window empties; the total stays)");
        CHECK(tr.json(1200).find("\"msgs\":60") != std::string::npos, "the running total is unchanged");
        tr.clear();
        CHECK(rate(5000) == 0, "cleared: 0 /min");
    }
    return g_fail ? 1 : 0;
}
