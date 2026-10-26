// test-drm-faad.cpp — DRM's AAC decoded by FAAD2's DRM build INSIDE the server (vibe_drm_audiodec.h, VIBE_HAVE_FAAD2), on
// the real recording: RRI 9570 kHz off Stuart's UberSDR (2026-10-26), mode B, AAC mono 12 kHz.
//   1. CLEAN: every frame the receiver delivers decodes; the 48 kHz stereo out is programme (envelope, band, level).
//   2. DAMAGED: one frame in seven corrupted on purpose. Concealment, not glitches — every concealed frame still yields
//      its 80 ms (no gap: the stream length matches the clean run), and the largest sample-to-sample step is no bigger
//      than the clean programme's own (no click).
//   3. CPU per DRM listener with audio (receiver + FAAD2 + upsampling), this machine.
// NOT RUN (exit 3) without FAAD2 (VIBE_HAVE_FAAD2) or without VIBE_TEST_AUDIO/drm/uber_rri_9570000_iq48.wav.
#include "decoders/drm/drm_audio.h"
#include "decoders/drm/drm_receiver.h"
#include "vibe_drm_audiodec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace vibedrm;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

#if !defined(VIBE_HAVE_FAAD2)
int main() { std::printf("   NOT RUN — this build has no FAAD2 (VIBE_HAVE_FAAD2)\n"); return 3; }
#else
struct Out { std::vector<float> pcm; int64_t good = 0, conc = 0; double dsp = 0, secs = 0; };

static bool run(const std::string& path, int corruptEvery, Out& o) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[44]; if (std::fread(h, 1, 44, f) != 44) { std::fclose(f); return false; }
    int rate; std::memcpy(&rate, h + 24, 4);
    Receiver rx; AudioFramer fr; vibe::DrmAudioDecoder dec; int64_t nUnit = 0;
    rx.onLogicalFrame = [&](const LogicalFrame& lf) {
        const ServiceSdc& sv = rx.status().sdc.svc[0];
        if (!sv.audio.present || sv.audio.streamId != lf.stream) return;
        if (fr.configure(sv.audio)) dec.open(fr);
        std::vector<AccessUnit> aus;
        fr.push(lf.bytes.data(), lf.bytes.size() - (sv.audio.text ? 4 : 0), aus);
        for (auto& au : aus) {
            if (corruptEvery && ++nUnit % corruptEvery == 0 && au.data.size() > 8)
                for (size_t i = 2; i < au.data.size(); i += 3) au.data[i] ^= 0x5A;   // ★ a burst through the whole frame
            dec.decode(au, o.pcm);
        }
    };
    std::vector<int16_t> b(9600); std::vector<cf> x(4800); size_t n;
    const auto t0 = std::chrono::steady_clock::now();
    while ((n = std::fread(b.data(), 4, 4800, f)) > 0) {
        for (size_t i = 0; i < n; ++i) x[i] = cf(b[2 * i], b[2 * i + 1]);
        rx.feed(x.data(), int(n), rate); o.secs += double(n) / rate;
    }
    o.dsp = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    o.good = dec.unitsGood(); o.conc = dec.unitsConcealed();
    std::fclose(f);
    return true;
}

static double maxStep(const std::vector<float>& p, double q) {
    std::vector<float> d;
    for (size_t i = 2; i < p.size(); i += 2) d.push_back(std::fabs(p[i] - p[i - 2]));
    if (d.empty()) return 0;
    std::sort(d.begin(), d.end());
    return d[size_t(q * double(d.size() - 1))];
}

int main() {
    const char* dir = getenv("VIBE_TEST_AUDIO");
    if (!dir) { std::printf("   NOT RUN — VIBE_TEST_AUDIO is not set\n"); return 3; }
    const std::string path = std::string(dir) + "/drm/uber_rri_9570000_iq48.wav";
    Out clean, dmg;
    if (!run(path, 0, clean)) { std::printf("   NOT RUN — %s missing\n", path.c_str()); return 3; }
    run(path, 7, dmg);
    std::printf("── 1. clean: %lld frames decoded, %lld concealed (the pre-lock frames), %.1f s of 48 kHz audio\n",
                (long long)clean.good, (long long)clean.conc, clean.pcm.size() / 2 / 48000.0);
    CHECK(clean.good > 1400, "only %lld frames decoded", (long long)clean.good);
    // programme: 50 ms envelope, band
    std::vector<double> env;
    for (size_t i = 0; i + 4800 <= clean.pcm.size(); i += 4800) { double e = 0; for (size_t k = 0; k < 4800; k += 2) e += clean.pcm[i + k] * clean.pcm[i + k]; env.push_back(10 * std::log10(e / 2400 + 1e-12)); }
    std::sort(env.begin(), env.end());
    const double p10 = env[env.size() / 10], p50 = env[env.size() / 2], p90 = env[env.size() * 9 / 10];
    std::printf("   envelope p10 %.1f / p50 %.1f / p90 %.1f dBFS\n", p10, p50, p90);
    CHECK(p50 > -45 && p50 < -3, "level %.1f dBFS is not a programme level", p50);
    CHECK(p90 - p10 > 10, "the envelope moves %.1f dB — not programme", p90 - p10);
    std::printf("── 2. damaged (1 frame in 7 corrupted): %lld decoded, %lld concealed, %.1f s of audio\n",
                (long long)dmg.good, (long long)dmg.conc, dmg.pcm.size() / 2 / 48000.0);
    CHECK(dmg.conc > clean.conc + 150, "the corrupted frames were not concealed (%lld)", (long long)dmg.conc);
    CHECK(std::fabs(double(dmg.pcm.size()) - double(clean.pcm.size())) < 48000.0 * 2 * 0.25,
          "a gap: %zu samples vs %zu clean", dmg.pcm.size(), clean.pcm.size());
    const double cMax = maxStep(clean.pcm, 0.99999), dMax = maxStep(dmg.pcm, 0.99999);
    std::printf("   largest sample-to-sample step (99.999th pct): clean %.4f, damaged %.4f\n", cMax, dMax);
    CHECK(dMax <= cMax * 1.25, "concealment clicks: %.4f vs %.4f", dMax, cMax);
    std::printf("── 3. CPU per DRM listener with audio: %.2f %% of one core (%.2f s for %.0f s)\n", 100 * clean.dsp / clean.secs, clean.dsp, clean.secs);
    std::printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
#endif
