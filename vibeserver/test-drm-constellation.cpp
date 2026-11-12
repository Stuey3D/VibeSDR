// test-drm-constellation.cpp — the DRM constellation (docs/v12/DRM.md "The constellation"), V12 2026-11-12:
//
//   1. NOTHING WHILE NOBODY LOOKS: a receiver with wantConstellation off copies no cells — every channel empty, the
//      sequence never moves — through a whole locked run.
//   2. BOUNDED: with it on, every snapshot holds at most the budget (256 MSC, 64 SDC, 65 FAC points), whatever the mode
//      (A, B, D) and QAM; it moves once per super frame; it empties again when switched off.
//   3. IT CLUSTERS: the test transmitter (drm_tx.h) through AWGN — every point sits on the ideal QAM grid of its channel
//      within the noise's reach, the MSC fills all 16 (or 64) clusters, the FAC and a 4-QAM SDC all four, and the clouds
//      tighten as the SNR rises. At 6 dB they do not (the picture must show a bad signal as bad).
//   4. THE WIRE (vibe_drm_report.h drmCellsJson): {"type":"drm_iq"} with the scale and each channel's QAM, every value an
//      integer in ±127, a channel not decoded absent (not an empty array), well-formed JSON; and its size is what the
//      budget says (≲ 3 KB per 1.2 s super frame).
//   5. CPU: the receiver with and without the constellation, this machine.
#include "decoders/drm/drm_receiver.h"
#include "decoders/drm/drm_tx.h"
#include "vibe_drm_report.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace vibedrm;

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++fails; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Grid { int cluster; float dist; };
/** The nearest ideal point of a `qam` constellation (unit average power) to (x, y), in the snapshot's own units. */
static Grid nearest(int qam, float x, float y) {
    const int levels = qam == 64 ? 3 : qam == 16 ? 2 : 1;
    const int m = (1 << levels) - 1;
    const float a = qamScale(levels) * Constellation::kScale;
    auto ax = [&](float v, int& r) { r = int(std::lround((float(m) - v / a) / 2.0f)); r = std::min(std::max(r, 0), m); return (float(m) - 2.0f * float(r)) * a; };
    int ri, rq;
    const float ix = ax(x, ri), iy = ax(y, rq);
    return { ri * (m + 1) + rq, std::hypot(x - ix, y - iy) };
}

struct ChanStat { size_t n = 0; std::set<int> clusters; double rmsDist = 0; };
static ChanStat judge(const std::vector<int8_t>& v, int qam) {
    ChanStat s; double sum = 0;
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        const Grid g = nearest(qam, float(v[i]), float(v[i + 1]));
        s.clusters.insert(g.cluster); sum += double(g.dist) * g.dist; ++s.n;
    }
    s.rmsDist = s.n ? std::sqrt(sum / double(s.n)) : 0;
    return s;
}

struct Snap { Constellation c; bool locked = false; int superFrames = 0; uint64_t seqMoves = 0; size_t maxMsc = 0, maxSdc = 0, maxFac = 0; double dsp = 0; };

/** The test transmitter through AWGN (or `paths`); the receiver's constellation switched on from super frame `onAt`
 *  (−1 = never) and off again from `offAt` (−1 = never). */
static Snap run(const TxConfig& c, double snr, int superFrames, int onAt, int offAt = -1, uint32_t seed = 7) {
    Transmitter tx(c);
    int kmin, kmax; carrierRange(c.mode, c.so, kmin, kmax);
    Channel ch(std::vector<PathSpec>{ {0, 1, 0, 0} }, snr, 23.0, 2.0, (kmax - kmin + 1) * double(kFs) / modeInfo(c.mode).tu, seed);
    Receiver rx;
    Snap r; uint64_t lastSeq = 0;
    for (int sf = 0; sf < superFrames; ++sf) {
        rx.wantConstellation = onAt >= 0 && sf >= onAt && (offAt < 0 || sf < offAt);
        const auto y = ch.run(tx.superFrame());
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(y.data(), int(y.size()), 24000);
        r.dsp += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const Constellation& k = rx.constellation();
        if (k.seq != lastSeq) { ++r.seqMoves; lastSeq = k.seq; }
        r.maxMsc = std::max(r.maxMsc, k.msc.size() / 2); r.maxSdc = std::max(r.maxSdc, k.sdc.size() / 2); r.maxFac = std::max(r.maxFac, k.fac.size() / 2);
    }
    r.c = rx.constellation(); r.locked = rx.status().locked; r.superFrames = int(rx.status().superFrames);
    return r;
}

static void nothingUnlessAsked() {
    std::printf("── 1. nothing while nobody looks\n");
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.sdcLevels = 1;
    const Snap s = run(c, 25, 14, -1);
    std::printf("   locked %d, %d super frames: msc %zu sdc %zu fac %zu points, seq moved %llu times\n", int(s.locked), s.superFrames,
                s.maxMsc, s.maxSdc, s.maxFac, (unsigned long long)s.seqMoves);
    CHECK(s.locked && s.superFrames >= 8, "the run must lock (locked %d, %d super frames)", int(s.locked), s.superFrames);
    CHECK(s.maxMsc == 0 && s.maxSdc == 0 && s.maxFac == 0 && s.seqMoves == 0, "cells were copied with the constellation off");
}

static void bounded() {
    std::printf("── 2. bounded, once per super frame, off again when nobody looks\n");
    struct Case { Mode m; int msc, sdc; };
    const Case cases[] = { {Mode::A, 3, 2}, {Mode::B, 2, 1}, {Mode::B, 3, 2}, {Mode::D, 2, 2} };
    for (const Case& k : cases) {
        TxConfig c; c.mode = k.m; c.mscLevels = k.msc; c.sdcLevels = k.sdc;
        const Snap s = run(c, 28, 16, 0);
        std::printf("   mode %c MSC %d-QAM SDC %d-QAM: max msc %zu sdc %zu fac %zu, %llu snapshots in %d super frames\n", modeLetter(k.m),
                    1 << (2 * k.msc), 1 << (2 * k.sdc), s.maxMsc, s.maxSdc, s.maxFac, (unsigned long long)s.seqMoves, s.superFrames);
        CHECK(s.locked, "mode %c did not lock", modeLetter(k.m));
        CHECK(s.maxMsc <= size_t(Constellation::kMsc) && s.maxSdc <= size_t(Constellation::kSdc) && s.maxFac <= size_t(Constellation::kFac),
              "mode %c: over budget (msc %zu sdc %zu fac %zu)", modeLetter(k.m), s.maxMsc, s.maxSdc, s.maxFac);
        CHECK(s.c.msc.size() == size_t(Constellation::kMsc) * 2 && s.c.sdc.size() == size_t(Constellation::kSdc) * 2
              && s.c.fac.size() == size_t(Constellation::kFac) * 2, "mode %c: a full snapshot should fill each budget", modeLetter(k.m));
        CHECK(s.c.mscQam == (1 << (2 * k.msc)) && s.c.sdcQam == (1 << (2 * k.sdc)), "mode %c: QAM %d/%d", modeLetter(k.m), s.c.mscQam, s.c.sdcQam);
        CHECK(s.seqMoves >= uint64_t(s.superFrames) - 1 && s.seqMoves <= uint64_t(s.superFrames),
              "mode %c: %llu snapshots for %d super frames — one each", modeLetter(k.m), (unsigned long long)s.seqMoves, s.superFrames);
    }
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.sdcLevels = 1;
    const Snap s = run(c, 28, 16, 4, 10);
    std::printf("   on at 4, off at 10: final msc %zu sdc %zu fac %zu points\n", s.c.msc.size() / 2, s.c.sdc.size() / 2, s.c.fac.size() / 2);
    CHECK(s.c.msc.empty() && s.c.sdc.empty() && s.c.fac.empty(), "switched off, the snapshot must empty");
    CHECK(s.seqMoves >= 4, "it was on for six super frames (%llu snapshots)", (unsigned long long)s.seqMoves);
}

static void clusters() {
    std::printf("── 3. it clusters on the QAM grid (rms distance to the nearest ideal point; unit power = %.0f)\n", Constellation::kScale);
    struct Case { int msc; double snr; };
    const Case cases[] = { {2, 30}, {2, 20}, {3, 30}, {3, 22} };
    double last16 = 1e9, last64 = 1e9;
    for (const Case& k : cases) {
        TxConfig c; c.mode = Mode::B; c.mscLevels = k.msc; c.sdcLevels = 1;
        const Snap s = run(c, k.snr, 14, 0);
        const int q = 1 << (2 * k.msc);
        const ChanStat m = judge(s.c.msc, q), sd = judge(s.c.sdc, 4), fa = judge(s.c.fac, 4);
        // the grid's half-spacing in snapshot units — a point further than this from its ideal is in the wrong cluster
        const float half = qamScale(k.msc == 3 ? 3 : 2) * Constellation::kScale;
        std::printf("   %2d-QAM at %2.0f dB: MSC %zu pts in %zu/%d clusters rms %.1f (half-spacing %.1f) · SDC %zu/4 rms %.1f · FAC %zu/4 rms %.1f\n",
                    q, k.snr, m.n, m.clusters.size(), q, m.rmsDist, half, sd.clusters.size(), sd.rmsDist, fa.clusters.size(), fa.rmsDist);
        CHECK(s.locked && m.n == size_t(Constellation::kMsc), "%d-QAM at %.0f dB: locked %d, %zu MSC points", q, k.snr, int(s.locked), m.n);
        CHECK(int(m.clusters.size()) == q, "%d-QAM at %.0f dB: the MSC filled %zu of %d clusters", q, k.snr, m.clusters.size(), q);
        CHECK(sd.clusters.size() == 4 && fa.clusters.size() == 4, "the 4-QAM SDC and FAC fill all four clusters (%zu, %zu)", sd.clusters.size(), fa.clusters.size());
        // ★ tight: the rms error well inside the half-spacing (64-QAM at 22 dB reads ~0.46 of it)
        CHECK(m.rmsDist < 0.6 * half, "%d-QAM at %.0f dB: MSC rms %.1f is not tight against half-spacing %.1f", q, k.snr, m.rmsDist, half);
        CHECK(fa.rmsDist < 0.45 * qamScale(1) * Constellation::kScale, "FAC rms %.1f", fa.rmsDist);
        // and tighter with more SNR (cases are ordered high → low within each QAM)
        double& last = q == 16 ? last16 : last64;
        if (last < 1e9) CHECK(m.rmsDist > last, "%d-QAM: a lower SNR must spread the cloud (%.1f then %.1f)", q, last, m.rmsDist);
        last = m.rmsDist;
    }
    // ★ a bad signal looks bad: 16-QAM at 6 dB — the MSC cloud must be wide, never the tidy grid
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.sdcLevels = 1;
    const Snap s = run(c, 6, 14, 0);
    const ChanStat f = judge(s.c.fac, 4);
    std::printf("   FAC at 6 dB: %zu pts rms %.1f (locked %d)\n", f.n, f.rmsDist, int(s.locked));
    CHECK(f.n == 0 || f.rmsDist > 0.25 * qamScale(1) * Constellation::kScale, "at 6 dB the FAC cloud must be visibly wide (rms %.1f)", f.rmsDist);
}

/** A tiny JSON shape check: braces/brackets balanced, every array element an integer. */
static bool intArraysOk(const std::string& j, int lo, int hi) {
    int depth = 0; bool inArr = false; std::string tok;
    for (size_t i = 0; i < j.size(); ++i) {
        const char ch = j[i];
        if (ch == '{' || ch == '[') { ++depth; if (ch == '[') { inArr = true; tok.clear(); } }
        else if (ch == '}' || ch == ']') {
            if (ch == ']' && inArr) { if (tok.empty()) return false; const int v = std::atoi(tok.c_str()); if (v < lo || v > hi) return false; inArr = false; }
            if (--depth < 0) return false;
        } else if (inArr) {
            if (ch == ',') { if (tok.empty()) return false; const int v = std::atoi(tok.c_str()); if (v < lo || v > hi) return false; tok.clear(); }
            else if ((ch >= '0' && ch <= '9') || ch == '-') tok += ch; else return false;
        }
    }
    return depth == 0;
}

static void wire() {
    std::printf("── 4. the wire: {\"type\":\"drm_iq\"}\n");
    TxConfig c; c.mode = Mode::B; c.mscLevels = 2; c.sdcLevels = 1;
    const Snap s = run(c, 25, 12, 0);
    const std::string j = vibe::drmCellsJson(s.c);
    std::printf("   %zu bytes: %.90s…\n", j.size(), j.c_str());
    CHECK(j.rfind("{\"type\":\"drm_iq\",\"scale\":60", 0) == 0, "head: %.40s", j.c_str());
    CHECK(j.find("\"mscQam\":16") != std::string::npos && j.find("\"sdcQam\":4") != std::string::npos, "the QAM sizes");
    CHECK(j.find("\"msc\":[") != std::string::npos && j.find("\"sdc\":[") != std::string::npos && j.find("\"fac\":[") != std::string::npos, "three channels");
    CHECK(intArraysOk(j, -127, 127), "every value an integer in ±127, the JSON balanced");
    CHECK(j.size() < 3200, "budget: %zu bytes per super frame", j.size());
    // a channel not decoded is absent, never an empty array
    Constellation k; k.fac = s.c.fac; k.seq = 1;
    const std::string jf = vibe::drmCellsJson(k);
    CHECK(jf.find("\"msc\"") == std::string::npos && jf.find("\"sdc\"") == std::string::npos && jf.find("mscQam") == std::string::npos
          && jf.find("\"fac\":[") != std::string::npos, "FAC only: %s", jf.substr(0, 80).c_str());
    CHECK(intArraysOk(jf, -127, 127), "FAC only: well formed");
    // ★ the scale clips: a wild point saturates at ±127, never wraps
    Constellation w; w.fac = { 127, -127 }; w.seq = 1;
    CHECK(vibe::drmCellsJson(w).find("[127,-127]") != std::string::npos, "the clip at ±127");
}

static void cpu() {
    std::printf("── 5. CPU with and without the constellation (mode B 64-QAM, 30 super frames = 36 s)\n");
    TxConfig c; c.mode = Mode::B; c.mscLevels = 3; c.sdcLevels = 2;
    double off = 1e9, on = 1e9;
    for (int i = 0; i < 3; ++i) { off = std::min(off, run(c, 25, 30, -1).dsp); on = std::min(on, run(c, 25, 30, 0).dsp); }
    std::printf("   off %.3f s, on %.3f s of DSP for 36 s of signal (%.3f %% vs %.3f %% of one core; +%.1f %%)\n", off, on,
                off / 36 * 100, on / 36 * 100, (on / off - 1) * 100);
    CHECK(on < off * 1.10 + 0.01, "the constellation must cost next to nothing (off %.3f s, on %.3f s)", off, on);
}

int main() {
    nothingUnlessAsked();
    bounded();
    clusters();
    wire();
    cpu();
    std::printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
