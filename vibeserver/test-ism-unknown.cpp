// test-ism-unknown.cpp — beta94 (vibe_ism_unknown.h): ISM's UNIDENTIFIED bursts described, steady carriers told from
// bursts, the descriptions kept bounded and carrying no samples, and the gain chip's figures in the report.
//   1. Synthetic bursts with a KNOWN shape through the real front end (detector → channel → slicer) at 2.4 MS/s: OOK PWM,
//      OOK PPM, OOK Manchester and FSK NRZ — the description says the modulation, the coding, the rate (±5 %), the length
//      (±3 ms), the pulse count and the frequency (±10 kHz).
//   2. A steady carrier (drifting 2 kHz) for 1.8 s → "carrier", never OOK/FSK; a long FSK DATA burst is not a carrier.
//   3. The descriptions: at most N kept, oldest gone first, and NO SAMPLES — no `iq`/`kb` (the owner's IQ clips were removed
//      before beta94 shipped, Stuart 2026-11-25: no keyless-entry capture).
//   4. The service on the 433 band (lossless, no threads racing): unidentified bursts described in the report (shape only),
//      the carrier listed under `steady` and NOT counted as unidentified, the chip's peak/clipPct.
// ★ Silent: no audio anywhere.
//   --write-clip <out.u8> <radio centre Hz> <rate>: a few seconds of rtl_tcp u8 IQ for scripts/test-server-ism-unknown.mjs
//   (an OOK NRZ burst at 433.80, an FSK burst at 434.05, a steady carrier at 433.65 MHz — nothing any decoder names).
#include "vibe_ism_service.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace vibeism;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)
static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Tx {
    Runs r;
    void add(double us) { const int d = int(std::lround(us)); if (!d) return; if (!r.empty() && ((r.back() > 0) == (d > 0))) r.back() += d; else r.push_back(d); }
    void mark(double us) { add(us); }
    void space(double us) { add(-us); }
    double len() const { double s = 0; for (int d : r) s += std::abs(d); return s; }
};
struct Sig { Runs runs; bool fsk = false; double offHz = 0, amp = 0.3, devHz = 30e3, startS = 0.05, driftHzPerS = 0; };
static std::vector<cf> synth(const std::vector<Sig>& sigs, double rate, double lenS, double noise, uint32_t seed) {
    std::vector<cf> x(size_t(rate * lenS));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, float(noise));
    for (auto& v : x) v = cf(g(rng), g(rng));
    for (const Sig& s : sigs) {
        double t = s.startS, ph = 0;
        for (int d : s.runs) {
            const size_t a = size_t(t * rate);
            t += std::abs(d) * 1e-6;
            const size_t e = std::min(x.size(), size_t(t * rate));
            for (size_t i = a; i < e; ++i) {
                const double f = (s.fsk ? s.offHz + (d > 0 ? s.devHz : -s.devHz) : s.offHz) + s.driftHzPerS * (double(i) / rate - s.startS);
                ph += 2 * M_PI * f / rate;
                if (s.fsk || d > 0) x[i] += cf(float(s.amp * std::cos(ph)), float(s.amp * std::sin(ph)));
            }
        }
    }
    return x;
}
static std::vector<Burst> front(const std::vector<cf>& x, double rate) {
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    std::vector<Burst> out;
    auto on = [&](const Burst& b) { out.push_back(b); };
    for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
    fe.flush(on);
    return out;
}
static std::mt19937 bitsRng(1234);
static std::vector<int> randBits(int n) { std::vector<int> b(static_cast<size_t>(n)); for (auto& x : b) x = int(bitsRng() & 1); return b; }
// ── the shapes ───────────────────────────────────────────────────────────────────────────────────────────────────────
static Runs pwm(int n, double shortUs, double longUs) {   // a 1 = long mark, short space; the period constant
    Tx tx; for (int b : randBits(n)) { tx.mark(b ? longUs : shortUs); tx.space(b ? shortUs : longUs); } return tx.r; }
static Runs ppm(int n, double markUs, double s0, double s1) {
    Tx tx; for (int b : randBits(n)) { tx.mark(markUs); tx.space(b ? s1 : s0); } tx.mark(markUs); return tx.r; }
static Runs manchester(int n, double halfUs) {
    Tx tx; tx.mark(halfUs); tx.space(halfUs);   // starts with a mark
    for (int b : randBits(n)) { if (b) { tx.mark(halfUs); tx.space(halfUs); } else { tx.space(halfUs); tx.mark(halfUs); } } return tx.r; }
static Runs nrz(int n, double bitUs, bool startHigh = true) {
    Tx tx; if (startHigh) tx.mark(bitUs); for (int b : randBits(n)) { if (b) tx.mark(bitUs); else tx.space(bitUs); } tx.mark(bitUs); return tx.r; }
static Runs steady(double us) { Tx tx; tx.mark(us); return tx.r; }

static const BurstDesc* only(std::vector<BurstDesc>& v, double hz, double tol = 15e3) {
    const BurstDesc* got = nullptr;
    for (const auto& d : v) if (std::fabs(d.freqHz - hz) <= tol && (!got || d.durMs > got->durMs)) got = &d;
    return got;
}
static std::vector<BurstDesc> describeAll(const std::vector<Sig>& s, double lenS, double centre = 433.92e6, uint32_t seed = 7) {
    const double rate = 2.4e6;
    std::vector<BurstDesc> v;
    for (const Burst& b : front(synth(s, rate, lenS, 0.02, seed), rate)) v.push_back(describeBurst(b, centre));
    return v;
}

static int writeClip(const char* out, double centre, double rate) {
    std::vector<Sig> s;
    Sig a{nrz(120, 300)}; a.offHz = 433.80e6 - centre; a.startS = 0.30; s.push_back(a);           // OOK NRZ 3.33 kBd
    Sig b{nrz(160, 1e6 / 4800)}; b.fsk = true; b.offHz = 434.05e6 - centre; b.startS = 0.90; s.push_back(b);   // FSK 4.8 kBd
    Sig a2 = a; a2.startS = 1.70; s.push_back(a2);
    Sig c{steady(1.0e6)}; c.offHz = 433.65e6 - centre; c.startS = 2.2; c.amp = 0.08; s.push_back(c);  // a carrier, 1 s of each loop
    const auto x = synth(s, rate, 3.4, 0.02, 99);
    FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i]     = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 127.5f + 127.5f))));
        u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 127.5f + 127.5f))));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    printf("wrote %zu samples (%.1f s) centred on %.3f MHz\n", x.size(), x.size() / rate, centre / 1e6);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    printf("test-ism-unknown — unidentified ISM bursts described (shape only), steady carriers\n");
    const double C = 433.92e6;

    // 1. Known shapes → the right description
    {
        printf(" descriptions\n");
        Sig s{pwm(64, 250, 750)}; s.offHz = 120e3;
        auto v = describeAll({s}, 0.30);
        const BurstDesc* d = only(v, C + 120e3);
        CHECK(d, "PWM: no burst described");
        if (d) {
            CHECK(d->mod == "OOK" && d->rs.coding == "PWM", "PWM: %s %s", d->mod.c_str(), d->rs.coding.c_str());
            CHECK(approx(d->rs.rate, 1000, 50), "PWM rate %.0f (want 1000 bit/s)", d->rs.rate);
            CHECK(approx(d->durMs, 64.0, 3.0), "PWM length %.1f ms (want ~64)", d->durMs);
            CHECK(d->rs.pulses == 64, "PWM pulses %d", d->rs.pulses);
            CHECK(approx(d->freqHz, C + 120e3, 10e3), "PWM freq %.0f", d->freqHz);
            CHECK(d->snrDb > 15 && d->levelDb > -20, "PWM level %.1f snr %.1f", d->levelDb, d->snrDb);
            printf("   PWM : %s %s %.0f bit/s, %.1f ms, %d pulses, %.4f MHz, %.1f dBFS\n", d->mod.c_str(), d->rs.coding.c_str(), d->rs.rate, d->durMs, d->rs.pulses, d->freqHz / 1e6, d->levelDb);
        }
        Sig p{ppm(48, 300, 600, 1800)}; p.offHz = -150e3;
        v = describeAll({p}, 0.30);
        d = only(v, C - 150e3);
        CHECK(d && d->mod == "OOK" && d->rs.coding == "PPM", "PPM: %s", d ? (d->mod + " " + d->rs.coding).c_str() : "none");
        if (d) {
            CHECK(d->rs.rate >= 1e6 / 2100 * 0.95 && d->rs.rate <= 1e6 / 900 * 1.05, "PPM rate %.0f", d->rs.rate);
            CHECK(d->rs.pulses == 49, "PPM pulses %d", d->rs.pulses);
            printf("   PPM : %s %s %.0f bit/s, %.1f ms, %d pulses\n", d->mod.c_str(), d->rs.coding.c_str(), d->rs.rate, d->durMs, d->rs.pulses);
        }
        Sig m{manchester(64, 500)}; m.offHz = 200e3;
        v = describeAll({m}, 0.30);
        d = only(v, C + 200e3);
        CHECK(d && d->mod == "OOK" && d->rs.coding == "NRZ/Manchester", "Manchester: %s", d ? (d->mod + " " + d->rs.coding).c_str() : "none");
        if (d) {
            CHECK(approx(d->rs.rate, 2000, 100) && approx(d->rs.unitUs, 500, 25), "Manchester symbol rate %.0f unit %.0f", d->rs.rate, d->rs.unitUs);
            CHECK(approx(d->durMs, 65.0, 3.0), "Manchester length %.1f", d->durMs);
            printf("   MAN : %s %s %.0f Bd (unit %.0f µs), %.1f ms\n", d->mod.c_str(), d->rs.coding.c_str(), d->rs.rate, d->rs.unitUs, d->durMs);
        }
        Sig f{nrz(160, 1e6 / 4800)}; f.fsk = true; f.offHz = -80e3; f.devHz = 30e3;
        v = describeAll({f}, 0.30);
        d = only(v, C - 80e3);
        CHECK(d && d->mod == "FSK", "FSK: %s", d ? d->mod.c_str() : "none");
        if (d) {
            CHECK(approx(d->rs.rate, 4800, 240), "FSK rate %.0f (want 4800)", d->rs.rate);
            CHECK(approx(d->devHz, 30e3, 7.5e3), "FSK deviation %.0f (want 30000)", d->devHz);
            CHECK(approx(d->durMs, 162 / 4.8, 3.0), "FSK length %.1f (want %.1f)", d->durMs, 162 / 4.8);
            CHECK(approx(d->freqHz, C - 80e3, 10e3), "FSK freq %.0f", d->freqHz);
            printf("   FSK : %s %s %.0f Bd, ±%.1f kHz, %.1f ms, %.4f MHz\n", d->mod.c_str(), d->rs.coding.c_str(), d->rs.rate, d->devHz / 1e3, d->durMs, d->freqHz / 1e6);
        }
    }

    // 2. A steady carrier is a carrier; a long data burst is not
    {
        printf(" carriers\n");
        Sig c{steady(1.8e6)}; c.offHz = 180e3; c.driftHzPerS = 1100; c.startS = 0.1;
        std::vector<Burst> bs = front(synth({c}, 2.4e6, 2.2, 0.02, 3), 2.4e6);
        int carriers = 0, other = 0;
        for (const Burst& b : bs) { const BurstDesc d = describeBurst(b, C); (d.mod == "carrier" ? carriers : other)++;
            if (std::getenv("UNK_DIAG")) { printf("   diag: dur %.3f mod %s ookAll %zu fskAll %zu dev %.0f\n", b.durS, d.mod.c_str(), b.ookAll.size(), b.fskAll.size(), b.fskDevHz);
                for (const auto& r : b.ookAll) { printf("    ook:"); for (size_t i = 0; i < std::min<size_t>(r.size(), 12); ++i) printf(" %d", r[i]); printf(" (%zu)\n", r.size()); }
                for (const auto& r : b.fskAll) { printf("    fsk:"); for (size_t i = 0; i < std::min<size_t>(r.size(), 12); ++i) printf(" %d", r[i]); printf(" (%zu)\n", r.size()); } } }
        CHECK(carriers >= 1 && other == 0, "steady carrier: %d carrier, %d other (of %zu)", carriers, other, bs.size());
        Sig l{nrz(1200, 1e6 / 2400)}; l.fsk = true; l.offHz = -60e3;   // 0.5 s of FSK data
        bs = front(synth({l}, 2.4e6, 0.8, 0.02, 4), 2.4e6);
        int lc = 0; for (const Burst& b : bs) lc += isCarrier(b);
        CHECK(!bs.empty() && lc == 0, "a 0.5 s FSK data burst read as a carrier (%d of %zu)", lc, bs.size());
        Sig k{nrz(1500, 300)}; k.offHz = 60e3;   // 0.45 s of OOK data
        bs = front(synth({k}, 2.4e6, 0.8, 0.02, 5), 2.4e6);
        lc = 0; for (const Burst& b : bs) lc += isCarrier(b);
        CHECK(!bs.empty() && lc == 0, "a 0.45 s OOK data burst read as a carrier");
        CarrierList cl;
        cl.offer(868.100e6, -30, 0); cl.offer(868.104e6, -31, 1); cl.offer(868.270e6, -40, 1); cl.offer(868.112e6, -29, 2);
        CHECK(cl.size() == 2, "carrier list merges a drifting carrier: %zu", cl.size());
        CHECK(cl.list()[0].lo == 868.100e6 && cl.list()[0].hi == 868.112e6 && cl.list()[0].n == 3, "drift range kept");
        for (int i = 0; i < 20; ++i) cl.offer(860e6 + i * 1e6, -50, 3 + i);
        CHECK(cl.size() == size_t(CarrierList::kMax), "carrier list bounded: %zu", cl.size());
        cl.prune(3 + 19 + CarrierList::kForgetS + 1);
        CHECK(cl.size() == 0, "carriers forgotten after %.0f s", CarrierList::kForgetS);
    }

    // 3. The descriptions: bounded, shape only
    {
        printf(" descriptions\n");
        UnknownBook::Limits lim; lim.descs = 4;
        UnknownBook bk(lim);
        for (int i = 0; i < 10; ++i) { BurstDesc d; d.freqHz = 433.9e6; d.mod = "OOK"; bk.add(d); }
        CHECK(bk.descCount() == 4, "descriptions kept: %zu (want 4)", bk.descCount());
        const std::string js = bk.json(0);
        CHECK(std::count(js.begin(), js.end(), '{') == 4, "json lists the 4 kept descriptions");
        CHECK(js.find("\"id\":10") != std::string::npos && js.find("\"id\":6,") == std::string::npos, "the newest kept, the oldest gone");
        CHECK(js.find("\"iq\"") == std::string::npos && js.find("\"kb\"") == std::string::npos, "no samples: %s", js.substr(0, 160).c_str());
        UnknownBook big;
        for (int i = 0; i < 40; ++i) { BurstDesc d; d.freqHz = 868.3e6; big.add(d); }
        CHECK(big.descCount() == 16, "default descriptions: %zu", big.descCount());
    }

    // 4. The service end to end on the 433 band
    {
        printf(" service\n");
        const double rate = 2.4e6, hw = 434.13e6;
        std::vector<Sig> s;
        Sig a{nrz(120, 300)}; a.offHz = 433.80e6 - hw; a.startS = 0.30; s.push_back(a);
        Sig b{nrz(160, 1e6 / 4800)}; b.fsk = true; b.offHz = 434.05e6 - hw; b.startS = 0.90; s.push_back(b);
        Sig c{steady(1.0e6)}; c.offHz = 433.65e6 - hw; c.startS = 2.0; c.amp = 0.08; s.push_back(c);
        const auto x = synth(s, rate, 3.2, 0.02, 11);
        Service svc;
        svc.setLossless(true);
        svc.setLora(false, 0);
        svc.start(*bandById("433"), hw, rate);
        for (size_t i = 0; i < x.size(); i += 65536) svc.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        svc.endOfStream();
        for (int i = 0; i < 300 && !svc.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));   // the housekeeping second (health, held carriers)
        const std::string r = svc.report();
        svc.stop();
        auto field = [&](const char* k) { const size_t p = r.find(std::string("\"") + k + "\":"); return p == std::string::npos ? -1e9 : std::atof(r.c_str() + p + std::strlen(k) + 3); };
        const double unknown = field("unknown"), bursts = field("bursts");
        printf("   %.0f bursts, %.0f unidentified, steadyN %.0f\n", bursts, unknown, field("steadyN"));
        if (std::getenv("UNK_DIAG")) printf("%s\n", r.c_str());
        const size_t pu = r.find("\"unk\":["), ps = r.find("\"steady\":[");
        CHECK(pu != std::string::npos && ps != std::string::npos, "report has unk and steady");
        const std::string unk = pu == std::string::npos ? "" : r.substr(pu, r.find(']', pu) - pu);
        const std::string st = ps == std::string::npos ? "" : r.substr(ps, r.find(']', ps) - ps);
        CHECK(unk.find("\"mod\":\"OOK\"") != std::string::npos && unk.find("\"mod\":\"FSK\"") != std::string::npos, "both bursts described: %s", unk.substr(0, 300).c_str());
        CHECK(unk.find("\"mod\":\"carrier\"") == std::string::npos, "no carrier among the unidentified");
        CHECK(unk.find("\"iq\"") == std::string::npos && unk.find("\"kb\"") == std::string::npos, "no samples kept or offered");
        CHECK(st.find("\"hz\":43365") != std::string::npos || st.find("\"hz\":43364") != std::string::npos, "the carrier at 433.65 listed as steady: %s", st.c_str());
        CHECK(unknown >= 2 && unknown <= bursts - field("steadyN"), "unidentified %.0f excludes carriers (bursts %.0f)", unknown, bursts);
        CHECK(field("peak") > -30 && field("peak") < 0.1 && field("clipPct") >= 0, "the chip's peak %.1f dBFS, clip %.1f %%", field("peak"), field("clipPct"));
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
