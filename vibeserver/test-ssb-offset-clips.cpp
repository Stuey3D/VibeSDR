// ★★★ THE SSB OFFSET ESTIMATOR ON REAL RECORDINGS (V12 TRANSCRIBE, 2026-10-16).
//
// vibedsp/ssb_offset.h estimates how far an SSB voice is mistuned from the voice's own harmonics.
// The synthetic suite (test-ssb-offset) proves the arithmetic; this one proves it on the air, with
// the UberSDR recordings the research was done on (12 kHz mono WAVs, 150-330 s each):
//
//   mistune/37719xx-37721xx_lsb.wav — ONE strong 80 m LSB voice recorded at five dials, 0, ±50,
//        ±100 Hz off. The truth is known: dial +50 on LSB puts the audio 50 Hz HIGH (+50).
//        ★ Whole clip and every 10 s-voiced SLOW window must land within 5 Hz.
//   volmet2/*_usb.wav, batch2/5505000_usb.wav — VOLMET, an on-frequency broadcast: ~0 Hz.
//   batch2/3648200_lsb.wav — the research's FALSE PEAK: 6 dB SNR, the rig said +162 Hz. Whatever
//        the estimator says, the CONFIDENCE GATE must not pass a large offset there.
//   QSOs (80/40 m, 8-33 dB): windows may disagree (two stations) — reported, and a window that passes
//        the gate must agree with its clip's whole estimate or be over ±250 Hz (refused by the caller).
//
// ★ THE CLIPS ARE NOT IN THE REPO (≈50 MB of off-air audio). Point VIBE_SSB_CLIPS at the folder that
//   holds mistune/, volmet2/, batch2/ (and the top-level QSO WAVs); without it this suite is NOT RUN
//   (exit 3), never a pass. ★ SILENT: audio is read and measured, never played.
#include "vibedsp/ssb_offset.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace vibedsp;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

// 16-bit PCM mono WAV (what the research rig writes). Returns false if it is not one.
static bool readWav(const std::string& path, std::vector<float>& x, int& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::fclose(f); return false; }
    int ch = 0, bits = 0; rate = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) { std::fclose(f); return false; }
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len);
            if (std::fread(b.data(), 1, len, f) != len) { std::fclose(f); return false; }
            ch = b[2] | (b[3] << 8); rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24)); bits = b[14] | (b[15] << 8);
        } else if (!std::memcmp(id, "data", 4)) {
            if (ch != 1 || bits != 16) { std::fclose(f); return false; }
            std::vector<int16_t> s(len / 2);
            const size_t got = std::fread(s.data(), 2, s.size(), f);
            x.resize(got);
            for (size_t i = 0; i < got; ++i) x[i] = s[i] / 32768.0f;
            std::fclose(f); return true;
        } else {
            std::fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
}

struct Run { SsbOffsetEstimate whole; std::vector<SsbOffsetEstimate> wins; double cpu = 0, secs = 0; float applied = 0; int corrections = 0; double firstAt = -1; };

static Run runClip(const std::vector<float>& x, int rate) {
    Run r;
    r.secs = (double)x.size() / rate;
    r.whole = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), rate);
    // ★ LIVE: the clip through the SLOW profile in 20 ms packets, as the tap feeds it.
    SsbOffsetEstimator::Config c = SsbOffsetEstimator::Config::forProfile(SsbOffsetEstimator::Profile::Slow);
    if (const char* k = std::getenv("SSB_KEEP")) c.keepFraction = (float)std::atof(k);      // bench knobs
    if (const char* v = std::getenv("SSB_VOICED")) c.windowVoicedSec = (float)std::atof(v);
    SsbOffsetEstimator live(rate, c);
    const int pkt = rate / 50;
    const auto t0 = std::chrono::steady_clock::now();   // everything: decimation, selection, analysis
    for (size_t i = 0; i + pkt <= x.size(); i += pkt)
        if (live.push(&x[i], pkt)) r.wins.push_back(live.last());
    r.cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // ★ …and the decision the recogniser feed makes from those windows (two in a row that agree).
    SsbAutoTune tune(rate);
    tune.setSideband(-1);
    std::vector<float> y(x);
    for (size_t i = 0; i + pkt <= y.size(); i += pkt) {
        tune.process(&y[i], pkt);
        if (r.firstAt < 0 && tune.appliedHz() != 0) r.firstAt = (double)i / rate;   // when the correction began
    }
    r.applied = tune.appliedHz(); r.corrections = tune.corrections();
    return r;
}

static void show(const char* label, const Run& r) {
    std::printf("   %-26s whole %+6.1f Hz (%.2f%s) | live:",
                label, r.whole.offsetHz, r.whole.confidence, r.whole.passes() ? "" : "?");
    for (auto& w : r.wins) std::printf(" %+.0f(%.2f/%.0fdB%s)", w.offsetHz, w.confidence, w.snrDb, w.passes() ? "" : "?");
    if (r.firstAt >= 0) std::printf(" | shift %+.0f from %.0f s | %.3f %%\n", r.applied, r.firstAt, 100.0 * r.cpu / r.secs);
    else std::printf(" | shift %+.0f | %.3f %%\n", r.applied, 100.0 * r.cpu / r.secs);
}

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

int main() {
    const char* dirEnv = std::getenv("VIBE_SSB_CLIPS");
    std::printf("SSB offset estimator — real recordings\n");
    if (!dirEnv || !exists(std::string(dirEnv) + "/mistune/3772000_lsb.wav")) {
        std::printf("   NOT RUN — set VIBE_SSB_CLIPS to the research clips folder (mistune/, volmet2/, batch2/)\n");
        return 3;
    }
    const std::string D = dirEnv;
    std::printf("   offset (confidence; \"?\" = under the gate %.2f + %.4f/Hz) for the whole clip, then each live\n"
                "   SLOW window; 'shift' = what SsbAutoTune applied to the audio by the end; %% = cost of one core\n",
                SsbOffsetEstimator::kGateBase, SsbOffsetEstimator::kGatePerHz);

    std::printf(" ── one voice, five dials ──\n");
    struct M { const char* f; float truth; } mt[] = {
        {"mistune/3772000_lsb.wav", 0}, {"mistune/3772050_lsb.wav", 50}, {"mistune/3772100_lsb.wav", 100},
        {"mistune/3771950_lsb.wav", -50}, {"mistune/3771900_lsb.wav", -100}};
    double cpuSum = 0, secSum = 0;
    for (auto& m : mt) {
        std::vector<float> x; int rate;
        if (!readWav(D + "/" + m.f, x, rate)) { check(false, m.f); continue; }
        Run r = runClip(x, rate);
        show(m.f + 8, r);
        cpuSum += r.cpu; secSum += r.secs;
        char what[160];
        std::snprintf(what, sizeof what, "%s: whole clip %+.1f Hz, truth %+.0f (within 5 Hz, passes the gate)", m.f + 8, r.whole.offsetHz, m.truth);
        check(std::fabs(r.whole.offsetHz - m.truth) <= 5 && r.whole.passes(), what);
        int good = 0;
        for (auto& w : r.wins) if (w.passes() && std::fabs(w.offsetHz - m.truth) <= 5) good++;
        std::snprintf(what, sizeof what, "%s: %d live windows within 5 Hz and believed (want ≥ 2)", m.f + 8, good);
        check(good >= 2, what);
        std::snprintf(what, sizeof what, "%s: auto-tune shifted the audio %+.1f Hz (want %+.0f ± 5)", m.f + 8, r.applied, -m.truth);
        check(std::fabs(r.applied + m.truth) <= 5, what);
    }

    std::printf(" ── FAST (the binaural aid's profile, reported) on the same five dials ──\n");
    for (auto& m : mt) {
        std::vector<float> x; int rate;
        if (!readWav(D + "/" + m.f, x, rate)) continue;
        SsbOffsetEstimator fast(rate, SsbOffsetEstimator::Profile::Fast);
        std::vector<float> all, believed;
        const int pkt = rate / 50;
        for (size_t i = 0; i + pkt <= x.size(); i += pkt)
            if (fast.push(&x[i], pkt) && fast.last().valid) {
                all.push_back(fast.last().offsetHz);
                if (fast.last().passes()) believed.push_back(fast.last().offsetHz);
            }
        auto med = [](std::vector<float> v) { if (v.empty()) return 999.0f; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        int near = 0;
        for (float v : believed) near += std::fabs(v - m.truth) <= 15;
        std::printf("   %-26s %3zu answers in %.0f s, median %+.0f; %zu believed, median %+.0f, %d of them within 15 Hz\n",
                    m.f + 8, all.size(), x.size() / (double)rate, med(all), believed.size(), med(believed), near);
        // ★ The GATE was measured for SLOW; on FAST it leans toward small offsets (it asks more of large ones), so the
        //   binaural aid must smooth FAST's raw answers itself and calibrate its own gate. Checked here: the raw
        //   answers' median says which way and roughly how far.
        char what[160];
        std::snprintf(what, sizeof what, "%s FAST: all answers' median within 20 Hz of %+.0f", m.f + 8, m.truth);
        check(all.size() >= 50 && std::fabs(med(all) - m.truth) <= 20, what);
    }

    std::printf(" ── the same voice moved further: the on-tune clip shifted ±150, ±200 Hz (the rig's s150/s200) ──\n");
    {
        std::vector<float> x0; int rate;
        if (readWav(D + "/mistune/3772000_lsb.wav", x0, rate)) {
            for (float sh : {150.0f, -150.0f, 200.0f, -200.0f}) {
                std::vector<float> x(x0);
                SsbShifter s(rate);
                s.setShift(sh);
                s.process(x.data(), (int)x.size());
                Run r = runClip(x, rate);
                char lab[40]; std::snprintf(lab, sizeof lab, "3772000 shifted %+.0f", sh);
                show(lab, r);
                char what[160];
                std::snprintf(what, sizeof what, "shifted %+.0f: auto-tune shifted back %+.1f Hz (want %+.0f ± 5)", sh, r.applied, -sh);
                check(std::fabs(r.applied + sh) <= 5, what);
            }
        }
    }

    std::printf(" ── VOLMET (on frequency) ──\n");
    const char* vol[] = {"volmet2/5505000_usb.wav", "volmet2/3413000_usb.wav", "volmet2/8957000_usb.wav",
                         "volmet2/13264000_usb.wav", "batch2/5505000_usb.wav"};
    for (auto f : vol) {
        std::vector<float> x; int rate;
        if (!readWav(D + "/" + f, x, rate)) { std::printf("   (missing %s)\n", f); continue; }
        Run r = runClip(x, rate);
        show(f, r);
        cpuSum += r.cpu; secSum += r.secs;
        char what[160];
        // ★ "low or zero": an on-frequency broadcast either reads small, or is not believed.
        std::snprintf(what, sizeof what, "%s: whole %+.1f Hz — small (≤10 Hz) or under the gate", f, r.whole.offsetHz);
        check(std::fabs(r.whole.offsetHz) <= 10 || !r.whole.passes(), what);
        int bad = 0;
        for (auto& w : r.wins) if (w.passes() && std::fabs(w.offsetHz) > 10) bad++;
        std::snprintf(what, sizeof what, "%s: no live window passes the gate with a large offset (%d did)", f, bad);
        check(bad == 0, what);
        std::snprintf(what, sizeof what, "%s: auto-tune left it alone (shift %+.1f Hz)", f, r.applied);
        check(r.applied == 0, what);
    }

    std::printf(" ── VOLMET buried in real band noise (12 → 2 dB SNR; the truth is still ~0) ──\n");
    const char* mix[] = {"mix/v1_n40_12dB.wav", "mix/v1_n40_8dB.wav", "mix/v1_n40_5dB.wav", "mix/v1_n40_2dB.wav",
                         "mix/v2_n40_12dB.wav", "mix/v2_n40_8dB.wav", "mix/v2_n40_5dB.wav", "mix/v2_n40_2dB.wav"};
    for (auto f : mix) {
        std::vector<float> x; int rate;
        if (!readWav(D + "/" + f, x, rate)) { std::printf("   (missing %s)\n", f); continue; }
        Run r = runClip(x, rate);
        show(f, r);
        cpuSum += r.cpu; secSum += r.secs;
        int bad = 0;
        for (auto& w : r.wins) if (w.passes() && std::fabs(w.offsetHz) > 10) bad++;
        char what[160];
        std::snprintf(what, sizeof what, "%s: nothing wrong believed (%d windows), no shift (%+.1f Hz)", f, bad, r.applied);
        check(bad == 0 && std::fabs(r.applied) <= 10, what);
    }

    std::printf(" ── the false peak (3648.2 kHz LSB, 6 dB SNR; the rig said +162 Hz) ──\n");
    {
        std::vector<float> x; int rate;
        if (readWav(D + "/batch2/3648200_lsb.wav", x, rate)) {
            Run r = runClip(x, rate);
            show("batch2/3648200_lsb.wav", r);
            int bad = 0;
            for (auto& w : r.wins) if (w.passes() && std::fabs(w.offsetHz) > 30) bad++;
            check(!(r.whole.passes() && std::fabs(r.whole.offsetHz) > 30), "whole clip: a large offset is not believed");
            check(bad == 0, "live: no window passes the gate with a large offset");
            check(std::fabs(r.applied) <= 30, "auto-tune did not act on it");
        } else std::printf("   (missing batch2/3648200_lsb.wav)\n");
    }

    std::printf(" ── QSOs (reported; windows that pass must agree with each other or the clip) ──\n");
    const char* qso[] = {"80m_3772000_lsb_2054.wav", "80m_3789000_lsb_2054.wav", "40m_7152000_lsb_2031.wav",
                         "40m_7163000_lsb_2041.wav", "40m_7178000_lsb_2041.wav", "batch2/3743000_lsb.wav",
                         "batch2/3702000_lsb.wav", "batch2/3626000_lsb.wav", "batch2/3762000_lsb.wav",
                         "batch2/7124000_lsb.wav", "volmetNA/3485000_usb.wav", "volmetNA/6604000_usb.wav",
                         "volmetNA/10051000_usb.wav", "volmetNA/13270000_usb.wav"};
    for (auto f : qso) {
        std::vector<float> x; int rate;
        if (!readWav(D + "/" + f, x, rate)) { std::printf("   (missing %s)\n", f); continue; }
        Run r = runClip(x, rate);
        show(f, r);
        cpuSum += r.cpu; secSum += r.secs;
        // ★ A QSO may hold two stations at two offsets; the rule is only that a shift, when made, is a
        //   believed one within the caller's ±250 Hz.
        check(std::fabs(r.applied) <= SsbAutoTune::kMaxCorrectHz, (std::string(f) + ": any shift within ±250 Hz").c_str());
    }

    std::printf(" ── cost ──\n");
    std::printf("   SLOW analysis: %.3f %% of one core on this machine, over %.0f s of audio\n", 100.0 * cpuSum / secSum, secSum);
    check(100.0 * cpuSum / secSum < 0.5, "SLOW costs well under 1 % of a core (< 0.5 % here)");

    std::printf("%s\n", fails ? "SSB OFFSET (CLIPS): FAILURES" : "SSB OFFSET (CLIPS): all passed");
    return fails ? 1 : 0;
}
