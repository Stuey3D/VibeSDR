// test-drm-signal.cpp — the DRM Signal pane's numbers (docs/v12/DRM.md "The Signal pane"), V12 2026-10-26:
//
//   1. DELAY SPREAD on synthetic channels with known paths — one path, two paths 0.5–4.5 ms apart, at several SNRs, in
//      modes A, B and D (each within its own guard interval): the 95 %-energy delay window, less the receiver's own
//      resolution, must read the paths' separation.
//   2. DOPPLER SPREAD on synthetic channels with known Gaussian Doppler (annex B's definition, Dsp = 2σ): two Rayleigh
//      paths at 0.1–2 Hz, at several SNRs, and a static channel (must read ~0).
//   3. THE LOCK GROUP: time-to-lock of each light in order, no re-acquisition on a clean signal; then an 8 s break in the
//      signal (longer than the 16 bad FACs a lock survives) must count ONE re-acquisition, say which test lost the lock ("fac"), and lock again with fresh times.
//   3b. HOLES: a block missing from the IQ (a listener's queue dropped it) loses the lock un-bridged and keeps it when
//      DrmListener bridges it with zeros (the flaky test-server-drm, 2026-10-26).
//   4. THE REPORT (vibe_drm_report.h): the new fields are present when measured and ABSENT when not, every field an
//      older client reads is unchanged, strings off the air are escaped, and the JSON is well formed.
//   5. Real recordings (VIBE_TEST_AUDIO=<folder holding drm/>): RRI 9570 kHz — the numbers it reads (printed; checked
//      only for being plausible: inside the guard interval, under 3 Hz). NOT RUN when the folder is absent.
//   6. CPU: the receiver with and without the Signal pane's statistics (VIBE_DRM_NOSTATS), this machine.
#include "decoders/drm/drm_receiver.h"
#include "decoders/drm/drm_tx.h"
#include "vibe_drm_report.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace vibedrm;

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++fails; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Run { RxStatus st; double secs = 0, dsp = 0; std::vector<float> delays, dopplers; };

/** The test transmitter through `paths`, sampling the two statistics once per super frame after `settleSf`. */
static Run run(const TxConfig& c, const std::vector<PathSpec>& paths, double snr, int superFrames, int settleSf, uint32_t seed = 11) {
    Transmitter tx(c);
    int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
    Channel ch(paths, snr, 37.0, 3.0, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, seed);
    Receiver rx;
    Run r;
    for (int sf = 0; sf < superFrames; ++sf) {
        const auto y = ch.run(tx.superFrame());
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(y.data(), int(y.size()), 24000);
        r.dsp += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.secs += double(y.size()) / kFs;
        if (sf >= settleSf) { r.delays.push_back(rx.status().delayMs); r.dopplers.push_back(rx.status().dopplerHz); }
    }
    r.st = rx.status();
    return r;
}
static double mean(const std::vector<float>& v) { double s = 0; int n = 0; for (float x : v) if (x >= 0) { s += x; ++n; } return n ? s / n : -1; }

static void delaySpread() {
    std::printf("── 1. delay spread (95 %% window less the receiver's own; two paths, second 2 dB down)\n");
    struct Case { Mode m; double d; };
    const Case cases[] = { {Mode::B, 0}, {Mode::B, 0.5}, {Mode::B, 1.0}, {Mode::B, 2.0}, {Mode::B, 3.0}, {Mode::B, 4.0},
                           {Mode::A, 0}, {Mode::A, 1.0}, {Mode::A, 2.0}, {Mode::D, 0}, {Mode::D, 2.0}, {Mode::D, 4.5} };
    // ★ The profile repeats every Tu/x samples (mode D: Tu = 9.33 ms), so a spread over HALF that (4.67 ms in D, 3.67 in C)
    //   reads the other way round: 5 ms apart in mode D is measured as 4.33 — the same channel, as far as pilots can tell.
    const double snrs[] = { 12, 20, 30 };
    for (const Case& k : cases) {
        std::printf("   mode %c, %.1f ms:", modeLetter(k.m), k.d);
        for (double snr : snrs) {
            TxConfig c; c.mode = k.m; c.mscLevels = 2; c.protB = 1; c.sdcLevels = k.m == Mode::D ? 2 : 1;
            std::vector<PathSpec> p = { {0, 1, 0, 0} };
            if (k.d > 0) p.push_back({k.d, 0.8, 0, 0});
            const Run r = run(c, p, snr, 25, 10);
            const double est = mean(r.delays);
            std::printf("  %2.0f dB → %.2f ms", snr, est);
            // ★ tolerance: a quarter of a millisecond (6 samples) or 12 % — the resolution of a 10 kHz channel is ~0.1 ms
            CHECK(r.st.locked && est >= 0 && std::fabs(est - k.d) <= std::max(0.25, 0.12 * k.d),
                  "mode %c, paths %.1f ms apart at %.0f dB: read %.2f ms (locked %d)", modeLetter(k.m), k.d, snr, est, int(r.st.locked));
        }
        std::printf("\n");
    }
    // annex B's own fading channels: the delay is the paths', whatever the fading does
    for (int chn : { 4, 5 }) {
        TxConfig c; c.mode = chn == 5 ? Mode::D : Mode::B; c.mscLevels = 2; c.protB = 0; c.sdcLevels = c.mode == Mode::D ? 2 : 1;
        const Run r = run(c, channelProfile(chn), 25, 30, 10);
        const double want = chn == 4 ? 2.0 : 4.0, est = mean(r.delays);
        std::printf("   annex B channel %d (mode %c, %.0f ms, fading): %.2f ms\n", chn, modeLetter(c.mode), want, est);
        CHECK(est >= 0 && std::fabs(est - want) <= 0.15 * want + 0.25, "annex B channel %d: read %.2f ms for %.1f", chn, est, want);
    }
}

static void doppler() {
    std::printf("── 2. Doppler spread (Dsp = 2σ; two Rayleigh paths 0.5 ms apart)\n");
    const double dsps[] = { 0.1, 0.25, 0.5, 1.0, 2.0 };
    const double snrs[] = { 15, 25 };
    for (double dsp : dsps) {
        std::printf("   %.2f Hz:", dsp);
        for (double snr : snrs) {
            // ★ three seeds: a sum of 24 sinusoids is ONE draw of the Doppler spectrum, and its own spread varies
            double acc = 0; int n = 0; bool lockedAll = true;
            for (uint32_t seed : { 3u, 5u, 9u }) {
                TxConfig c; c.mode = dsp >= 1.0 ? Mode::D : Mode::B; c.mscLevels = 2; c.protB = 0; c.sdcLevels = c.mode == Mode::D ? 2 : 1;
                const Run r = run(c, { {0, 1, 0, dsp}, {0.5, 1, 0, dsp} }, snr, 40, 12, seed);
                const double e = mean(r.dopplers);
                if (e >= 0) { acc += e; ++n; }
                lockedAll = lockedAll && r.st.frameSync;
            }
            const double est = n ? acc / n : -1;
            std::printf("  %2.0f dB → %.2f Hz", snr, est);
            CHECK(lockedAll && est >= 0 && std::fabs(est - dsp) <= 0.3 * dsp + 0.1, "Doppler %.2f Hz at %.0f dB: read %.2f Hz", dsp, snr, est);
        }
        std::printf("\n");
    }
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.protB = 1;
    const Run r = run(c, { {0, 1, 0, 0}, {1.0, 0.7, 0, 0} }, 20, 25, 10);
    const double e = mean(r.dopplers);
    std::printf("   static two-path channel, 20 dB: %.3f Hz\n", e);
    CHECK(e >= 0 && e < 0.12, "a static channel reads %.3f Hz of Doppler", e);
}

static void lockGroup() {
    std::printf("── 3. the lock group: time to each light, re-acquisitions\n");
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.protB = 1;
    Transmitter tx(c);
    int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
    Channel ch(1, 22, 120, 5, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, 4);
    Receiver rx;
    for (int sf = 0; sf < 12; ++sf) { const auto y = ch.run(tx.superFrame()); rx.feed(y.data(), int(y.size()), 24000); }
    const RxStatus& a = rx.status();
    std::printf("   clean: time %.1f · freq %.1f · frame %.1f · FAC %.1f · SDC %.1f · MSC %.1f s, re-acquisitions %lld\n",
                a.lockS[0], a.lockS[1], a.lockS[2], a.lockS[3], a.lockS[4], a.lockS[5], (long long)a.reacquisitions);
    bool ordered = true; for (int i = 0; i < 6; ++i) { ordered = ordered && a.lockS[i] >= 0; if (i) ordered = ordered && a.lockS[i] >= a.lockS[i - 1]; }
    CHECK(ordered, "the six times are all reached, in the order a receiver gains them");
    CHECK(a.lockS[5] > 0 && a.lockS[5] < 7.0, "MSC within 7 s of signal (%.1f s: 0.8 s search + frames + the 2 s long interleaver)", a.lockS[5]);
    CHECK(a.reacquisitions == 0 && a.lastLoss.empty(), "no re-acquisition on a clean signal");
    // an 8 s break: noise only — longer than the 16 bad FACs (6.4 s) a lock rides out
    std::vector<cf> noise(size_t(8 * kFs));
    std::mt19937 g(1); std::normal_distribution<float> n(0, 0.05f);
    for (cf& v : noise) v = cf(n(g), n(g));
    rx.feed(noise.data(), int(noise.size()), 24000);
    CHECK(rx.status().reacquisitions == 1 && rx.status().lastLoss == "fac", "an 8 s break: one re-acquisition, lost on the FAC (%lld, \"%s\")",
          (long long)rx.status().reacquisitions, rx.status().lastLoss.c_str());
    CHECK(!rx.status().locked && rx.status().lockS[3] < 0, "…the lights it had are out");
    for (int sf = 0; sf < 12; ++sf) { const auto y = ch.run(tx.superFrame()); rx.feed(y.data(), int(y.size()), 24000); }
    const RxStatus& b = rx.status();
    std::printf("   after the break: FAC %.1f · MSC %.1f s after the loss, lost at %.1f s\n", b.lockS[3], b.lockS[5], b.lastLossAtS);
    CHECK(b.locked && b.mscOk && b.lockS[5] > 0, "locked again, the MSC decoding, with a fresh time (%.1f s)", b.lockS[5]);
    CHECK(b.reacquisitions == 1, "still one re-acquisition");
}

static void report() {
    std::printf("── 4. the report's JSON\n");
    using vibe::drmReportJson; using vibe::DrmReportExtras;
    RxStatus s;
    // nothing yet: the old minimum, plus the counters
    std::string j = drmReportJson(s, DrmReportExtras());
    CHECK(j.rfind("{\"type\":\"drm\",\"sync\":false", 0) == 0, "starts as before: %s", j.c_str());
    CHECK(j.find("lockS") == std::string::npos && j.find("delayMs") == std::string::npos && j.find("dopplerHz") == std::string::npos,
          "nothing measured → no lock times, delay or Doppler: %s", j.c_str());
    CHECK(j.find("\"reacq\":0") != std::string::npos && j.find("\"dropped\":0}") != std::string::npos, "counters present");
    // locked RRI-like
    s.timeSync = s.freqSync = s.frameSync = s.facOk = s.sdcOk = s.mscOk = s.locked = true;
    s.mode = Mode::B; s.so = 3; s.mscLevels = 2; s.sdcLevels = 2; s.codeRateB = 0.62; s.facMerDb = 17.4f; s.mscMerDb = 16.1f;
    s.sdcMerDb = 15.2f; s.sdcRate = 0.5; s.freqOffsetHz = -3.25; s.sroPpm = 1.5; s.delayMs = 1.234f; s.dopplerHz = 0.31f;
    for (int i = 0; i < 6; ++i) s.lockS[i] = 0.8 + i * 0.6;
    s.reacquisitions = 2; s.lastLoss = "fac"; s.lastLossAtS = 42.0; s.facGood = 40; s.facBad = 2; s.sdcGood = 13; s.sdcBad = 1; s.superFrames = 14;
    s.fac.serviceId = 0x3E1C2; s.fac.shortId = 0; s.fac.language = 5; s.fac.descriptor = 1;
    s.sdc.svc[0].label = "SNR \"Tiganesti\" E1"; s.sdc.svc[0].country2 = "ro";
    s.sdc.svc[0].audio.present = true; s.sdc.svc[0].audio.coding = 0; s.sdc.svc[0].audio.sbr = 1; s.sdc.svc[0].audio.audioMode = 1; s.sdc.svc[0].audio.srCode = 3;
    s.bitrateBps = 16300;
    DrmReportExtras x; x.canDecode = true; x.decoderOpen = true; x.decoderTried = true; x.backend = "FAAD2";
    x.audioUnits = 500; x.audioBad = 7; x.concealed = 3; x.underruns = 1; x.dropped = 4; x.gaps = 3; x.blanked = 0.0123;
    j = drmReportJson(s, x);
    std::printf("   %s\n", j.c_str());
    for (const char* k : { "\"locked\":true", "\"robustness\":\"B\"", "\"bwKhz\":10", "\"mscQam\":16", "\"sdcQam\":16", "\"interleave\":\"long\"",
                           "\"codeRate\":0.62", "\"snr\":17.4", "\"mscMer\":16.1", "\"offsetHz\":-3.2", "\"clockPpm\":1.5",
                           "\"serviceId\":\"03E1C2\"", "\"codec\":\"HE-AAC v2\"", "\"audioMode\":\"parametric stereo\"", "\"audioRate\":24000",
                           "\"bitrate\":16300", "\"playable\":true", "\"decoder\":\"FAAD2\"", "\"audioUnits\":500", "\"audioBad\":7",
                           "\"concealed\":3", "\"facOk\":40", "\"facBad\":2", "\"dropped\":4}" })
        CHECK(j.find(k) != std::string::npos, "an older client's field unchanged: %s", k);
    for (const char* k : { "\"so\":3", "\"sdcRate\":0.50", "\"sdcMer\":15.2", "\"delayMs\":1.23", "\"dopplerHz\":0.31", "\"sbr\":true", "\"ps\":true",
                           "\"underruns\":1", "\"sdcOk\":13", "\"sdcBad\":1", "\"superFrames\":14", "\"lockS\":[0.8,1.4,2.0,2.6,3.2,3.8]",
                           "\"reacq\":2", "\"iqGaps\":3", "\"lostWhy\":\"fac\"", "\"lostAtS\":42.0", "\"blankedPct\":1.23" })
        CHECK(j.find(k) != std::string::npos, "the Signal pane's field: %s", k);
    CHECK(j.find("\"label\":\"SNR \\\"Tiganesti\\\" E1\"") != std::string::npos, "a label off the air is escaped");
    CHECK(j.find("codeRateA") == std::string::npos, "no part A → no part-A rate");
    // well formed: braces/brackets balance outside strings, no trailing comma
    int depth = 0; bool str = false, esc = false, bad = false;
    for (size_t i = 0; i < j.size(); ++i) {
        const char ch = j[i];
        if (str) { if (esc) esc = false; else if (ch == '\\') esc = true; else if (ch == '"') str = false; continue; }
        if (ch == '"') str = true; else if (ch == '{' || ch == '[') ++depth; else if (ch == '}' || ch == ']') { --depth; if (i && j[i - 1] == ',') bad = true; }
        if (depth < 0) bad = true;
    }
    CHECK(!bad && depth == 0 && !str, "well-formed JSON");
    // a searching receiver after a loss: the lock times say how far it has got, the loss is remembered
    RxStatus q; q.timeSync = true; q.mode = Mode::B; q.lockS[0] = 0.9; q.reacquisitions = 1; q.lastLoss = "fac"; q.lastLossAtS = 10;
    j = drmReportJson(q, DrmReportExtras());
    CHECK(j.find("\"lockS\":[0.9,-1.0,-1.0,-1.0,-1.0,-1.0]") != std::string::npos && j.find("\"lostWhy\":\"fac\"") != std::string::npos
          && j.find("delayMs") == std::string::npos, "searching after a loss: %s", j.c_str());
}

/** ★ A listener's queue dropping blocks (DrmListener::noteGap): 10.7 ms holes — one 384-sample block at 36 kHz, 256
 *  samples here at 24 kHz — every 4 s. Un-bridged, each hole costs the frame alignment; bridged with zeros, the lock holds
 *  and only the frames around the holes can suffer. */
static void holes() {
    std::printf("── 3b. holes in the IQ (a listener's dropped blocks)\n");
    for (int bridged = 0; bridged < 2; ++bridged) {
        TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.protB = 1;
        Transmitter tx(c);
        int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
        Channel ch(1, 20, 4, 0, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, 6);
        Receiver rx; int64_t good = 0, at = 0, nextHole = 10 * kFs;
        rx.onLogicalFrame = [&](const LogicalFrame& lf) {
            if (lf.bytes.size() < 8) return;
            const int64_t n = (int64_t(lf.bytes[0]) << 24) | (lf.bytes[1] << 16) | (lf.bytes[2] << 8) | lf.bytes[3];
            const auto ref = testPayload(n, lf.bytes.size() - 4);
            if (std::equal(ref.begin(), ref.end(), lf.bytes.begin())) ++good;
        };
        int nHoles = 0;
        for (int sf = 0; sf < 40; ++sf) {
            auto y = ch.run(tx.superFrame());
            for (size_t i = 0; i < y.size(); i += 384) {
                const int n = int(std::min<size_t>(384, y.size() - i));
                if (at >= nextHole) {
                    nextHole += 4 * kFs; ++nHoles;
                    if (bridged) { std::vector<cf> z(size_t(n), cf(0, 0)); rx.feed(z.data(), n, 24000); }
                } else rx.feed(y.data() + i, n, 24000);
                at += n;
            }
        }
        const RxStatus& st = rx.status();
        std::printf("   %s: %d holes, re-acquisitions %lld, frames %lld of 120\n", bridged ? "bridged with zeros" : "not bridged", nHoles,
                    (long long)st.reacquisitions, (long long)good);
        if (bridged) CHECK(st.reacquisitions == 0 && good >= 100, "bridged holes keep the lock (%lld re-acquisitions, %lld frames)", (long long)st.reacquisitions, (long long)good);
        else CHECK(st.reacquisitions >= 1, "an un-bridged hole costs the lock — the failure the bridge exists for (%lld)", (long long)st.reacquisitions);
    }
}

static int realRecording() {
    std::printf("── 5. real recording\n");
    const char* dir = getenv("VIBE_TEST_AUDIO");
    if (!dir) { std::printf("   NOT RUN — VIBE_TEST_AUDIO is not set\n"); return 3; }
    const std::string path = std::string(dir) + "/drm/uber_rri_9570000_iq48.wav";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("   NOT RUN — %s missing\n", path.c_str()); return 3; }
    uint8_t h[44]; if (std::fread(h, 1, 44, f) != 44) { std::fclose(f); return 3; }
    int rate; std::memcpy(&rate, h + 24, 4);
    Receiver rx;
    std::vector<int16_t> b(9600); std::vector<cf> x(4800); size_t n; double secs = 0;
    std::vector<float> dl, dp;
    double nextS = 20;
    while ((n = std::fread(b.data(), 4, 4800, f)) > 0) {
        for (size_t i = 0; i < n; ++i) x[i] = cf(b[2 * i], b[2 * i + 1]);
        rx.feed(x.data(), int(n), rate); secs += double(n) / rate;
        if (secs >= nextS) { nextS += 10; dl.push_back(rx.status().delayMs); dp.push_back(rx.status().dopplerHz);
            std::printf("   %3.0f s: delay %.2f ms, Doppler %.2f Hz, FAC MER %.1f, MSC MER %.1f, SDC MER %.1f dB\n", secs,
                        rx.status().delayMs, rx.status().dopplerHz, rx.status().facMerDb, rx.status().mscMerDb, rx.status().sdcMerDb); }
    }
    std::fclose(f);
    const RxStatus& s = rx.status();
    std::printf("   RRI 9570 kHz (%.0f s): lock times %.1f/%.1f/%.1f/%.1f/%.1f/%.1f s, re-acquisitions %lld\n", secs,
                s.lockS[0], s.lockS[1], s.lockS[2], s.lockS[3], s.lockS[4], s.lockS[5], (long long)s.reacquisitions);
    const double md = mean(dl), mp = mean(dp);
    CHECK(md >= 0 && md < 5.33, "RRI 9570: the delay spread is measured and inside mode B's guard (%.2f ms)", md);
    CHECK(mp >= 0 && mp < 3.0, "RRI 9570: the Doppler is measured and plausible for HF (%.2f Hz)", mp);
    CHECK(s.sdcMerDb > 5 && s.sdcRate > 0.4 && s.sdcRate < 0.6, "RRI 9570: SDC MER %.1f dB, rate %.2f", s.sdcMerDb, s.sdcRate);
    return 0;
}

static void cpu() {
    std::printf("── 6. CPU: the receiver with and without the Signal pane's statistics (this machine)\n");
    for (Mode m : { Mode::A, Mode::B, Mode::D }) {
        TxConfig c; c.mode = m; c.mscLevels = 3; c.protB = 1; c.sdcLevels = m == Mode::D ? 2 : 1;
        // ★ the knob is read once per process (a static), so the A/B is a second process — see main()
        const Run r = run(c, channelProfile(1), 25, 25, 25);
        const double with = 100 * r.dsp / r.secs;
        std::printf("   mode %c 64-QAM: %.2f %% of one core%s\n", modeLetter(m), with, getenv("VIBE_DRM_NOSTATS") ? " (statistics OFF)" : "");
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "cpu") == 0) { cpu(); return 0; }   // ★ VIBE_DRM_NOSTATS=1 ./test-drm-signal cpu — the A/B
    delaySpread();
    doppler();
    lockGroup();
    holes();
    report();
    const int real = realRecording();
    cpu();
    std::printf("\n%d checks, %d failed%s\n", checks, fails, real == 3 ? " (real recording NOT RUN)" : "");
    return fails ? 1 : 0;
}
