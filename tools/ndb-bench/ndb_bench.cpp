// ndb_bench — the NDB skimmer's measuring bench (scripts/test-ndb.sh builds it SIMD and scalar). Silent: nothing is played.
//
//   ndb_bench one <snrDb> [ident] [tone] [wpm]   one beacon, 3 minutes: what was read, when it was confirmed
//   ndb_bench accuracy                           confirmation rate and time vs SNR (several draws each)
//   ndb_bench multi                              forty beacons in 192 kHz (0.5 kHz grid, shared frequencies, both tones, A1A)
//   ndb_bench noise <min>                        pure noise: carriers and idents (must be none)
//   ndb_bench bias                               the floor's calibration on pure noise
//   ndb_bench cpu [s]                            CPU per input rate
//   ndb_bench wav <iq.wav> <centreHz> [loKHz hiKHz]   a real recording (stereo int16 I/Q)
#include "decoders/ndb_skimmer.h"
#include "ndb_synth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vibe::ndb;
using namespace ndbsynth;

static double threadCpu() {
    timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

struct Result {
    std::map<int, std::string> ident;     // id → confirmed ident
    std::map<int, double> identAt;        // id → seconds into the run
    std::map<int, double> hz;
    double cpu = 0;
};
static Result run(Skimmer& sk, const std::vector<cf>& iq, double rate, bool verbose = false) {
    Result r;
    sk.setStartUtcMs(0);
    sk.onIdent = [&](const BeaconView& v) {
        if (!r.ident.count(v.id)) r.identAt[v.id] = sk.nowMs() / 1000.0;
        r.ident[v.id] = v.ident; r.hz[v.id] = v.offsetHz;
        if (verbose) std::printf("  %6.1f s  IDENT %-5s %+9.1f Hz  tone %4.0f  %4.1f WPM  %+5.1f dB  (%d of %d)\n", sk.nowMs() / 1000.0,
                                 v.ident.c_str(), v.offsetHz, v.toneHz, v.wpm, v.snrDb, v.votes, v.ballots);
    };
    if (verbose) sk.onCopy = [&](const Copy& c) { std::printf("  %6.1f s  copy  %+9.1f Hz  %s\n", c.utcMs / 1000.0, c.offsetHz, c.text.c_str()); };
    const int blk = 4096;
    const double c0 = threadCpu();
    for (size_t i = 0; i < iq.size(); i += blk) sk.feedIq(iq.data() + i, (int)std::min<size_t>(blk, iq.size() - i));
    r.cpu = threadCpu() - c0;
    (void)rate;
    return r;
}

static int one(double snr, const std::string& ident, double tone, double wpm) {
    const double rate = 48000;
    Rng rng(std::getenv("NDB_SEED") ? (uint64_t)std::atoll(std::getenv("NDB_SEED")) : 3);
    Beacon b; b.ident = ident; b.offsetHz = 3000.3; b.snrDb = snr; b.toneHz = tone; b.wpm = wpm;
    if (std::getenv("NDB_PERIOD")) b.periodSec = std::atof(std::getenv("NDB_PERIOD"));
    if (std::getenv("NDB_OFF")) b.offsetHz = std::atof(std::getenv("NDB_OFF"));
    std::vector<cf> iq; synth({ b }, rate, std::getenv("NDB_SECS") ? std::atof(std::getenv("NDB_SECS")) : 180, rng, iq);
    Skimmer::Config c; c.rate = rate;
    Skimmer sk(c);
    std::printf("FFT %d, bin %.2f Hz, hop %.2f ms, searching %+.0f..%+.0f Hz, %s\n", sk.fftSize(), sk.binHzActual(), sk.hopSec() * 1e3,
                sk.searchedLoHz(), sk.searchedHiHz(), Skimmer::simdName());
    run(sk, iq, rate, true);
    for (auto& v : sk.beacons()) {
        std::printf("  beacon %d %+9.1f Hz  %+5.1f dB  tone %4.0f (%+.1f dB)  %4.1f WPM  ident '%s' (%d/%d)  reading '%s'  A1A %d  copy: %s\n",
                    v.id, v.offsetHz, v.snrDb, v.toneHz, v.toneSnrDb, v.wpm, v.ident.c_str(), v.votes, v.ballots, v.reading.c_str(),
                    v.keyedCarrier, v.copy.c_str());
    }
    return 0;
}

/** One beacon per draw; returns (confirmed right, confirmed wrong, mean seconds to confirm). */
static void trial(double snr, double tone, double wpm, int draws, double secs, int& right, int& wrong, double& tmean) {
    right = wrong = 0; tmean = 0;
    const char* idents[] = { "WOD", "BHX", "EME", "KS", "LBA", "OX", "BRI", "NGY" };
    for (int d = 0; d < draws; ++d) {
        Rng rng(100 + d * 7 + (uint64_t)(snr * 10 + 500));
        Beacon b; b.ident = idents[d % 8]; b.offsetHz = -2000 + 137.0 * d; b.snrDb = snr; b.toneHz = tone; b.wpm = wpm;
        b.phaseSec = 0.5 + 0.9 * d; b.periodSec = 8 + (d % 3) * 2;
        std::vector<cf> iq; synth({ b }, 48000, secs, rng, iq);
        Skimmer::Config c; c.rate = 48000; c.coherent = !std::getenv("NDB_INCOH");
        Skimmer sk(c);
        Result r = run(sk, iq, 48000);
        bool ok = false, bad = false; double at = 0;
        for (auto& kv : r.ident) { if (kv.second == b.ident) { ok = true; at = r.identAt[kv.first]; } else { bad = true; if (std::getenv("NDB_SHOW")) std::printf("    [wrong: %s for %s at %+.0f dB]\n", kv.second.c_str(), b.ident.c_str(), snr); } }
        right += ok; wrong += bad; if (ok) tmean += at;
    }
    if (right) tmean /= right;
}

/** One of trial()'s draws, traced (the same seed, ident, offset and period). */
static int draw(double snr, double tone, int d, double secs) {
    const char* idents[] = { "WOD", "BHX", "EME", "KS", "LBA", "OX", "BRI", "NGY" };
    Rng rng(100 + d * 7 + (uint64_t)(snr * 10 + 500));
    Beacon b; b.ident = idents[d % 8]; b.offsetHz = -2000 + 137.0 * d; b.snrDb = snr; b.toneHz = tone; b.wpm = 7;
    b.phaseSec = 0.5 + 0.9 * d; b.periodSec = 8 + (d % 3) * 2;
    std::printf("draw %d: %s at %+.0f Hz, %+.0f dB, tone %.0f, period %.0f s\n", d, b.ident.c_str(), b.offsetHz, snr, tone, b.periodSec);
    std::vector<cf> iq; synth({ b }, 48000, secs, rng, iq);
    Skimmer::Config c; c.rate = 48000; Skimmer sk(c);
    run(sk, iq, 48000, true);
    for (auto& v : sk.beacons()) std::printf("  beacon %d %+9.1f Hz  %+5.1f dB  tone %4.0f  %4.1f WPM  ident '%s'  reading '%s'\n", v.id, v.offsetHz, v.snrDb, v.toneHz, v.wpm, v.ident.c_str(), v.reading.c_str());
    return 0;
}

static int accuracy() {
    std::printf("One beacon, 4 min, 8 draws per cell: confirmed right / wrong, mean time to confirm.  SNR = carrier / noise in 2.5 kHz\n");
    std::printf("  %-8s %-26s %-26s %-26s\n", "SNR", "1020 Hz 7 WPM", "400 Hz 7 WPM", "A1A 7 WPM");
    for (double snr : { 10.0, 0.0, -5.0, -10.0, -13.0, -16.0, -19.0 }) {
        std::printf("  %+5.0f dB ", snr);
        for (double tone : { 1020.0, 400.0, 0.0 }) {
            int r, w; double t; trial(snr, tone, 7, 8, 240, r, w, t);
            char b[64]; std::snprintf(b, sizeof b, "%d/8 ok, %d wrong, %5.1f s", r, w, t);
            std::printf(" %-26s", b);
        }
        std::printf("\n");
    }
    std::printf("Speed (1020 Hz, -5 dB):");
    for (double wpm : { 5.0, 7.0, 10.0, 13.0 }) { int r, w; double t; trial(-5, 1020, wpm, 8, 180, r, w, t); std::printf("  %2.0f WPM %d/8 (%d wrong, %.0f s)", wpm, r, w, t); }
    std::printf("\n");
    return 0;
}

static int multi() {
    const double rate = 192000, secs = 240;
    Rng rng(42);
    const char* ids[] = { "WOD", "EME", "EMW", "BHX", "BRI", "KS", "LBA", "OX", "NGY", "CAM", "SAY", "BIA", "EKN", "FNY", "GLW",
                          "HAV", "IOF", "KW", "LAY", "MCH", "NOT", "OY", "PY", "RCH", "SBY", "SM", "STM", "TD", "BV", "AP",
                          "AQ", "BZ", "CBN", "DND", "EDN", "FOS", "GY", "HEN", "IW", "NN" };
    std::vector<Beacon> bs;
    for (int i = 0; i < 40; ++i) {
        Beacon b; b.ident = ids[i];
        b.offsetHz = -90000 + 4500.0 * i + (i % 7 == 3 ? 0 : 0);
        if (i == 5) b.offsetHz = bs[4].offsetHz + 500;     // ★ neighbours 0.5 kHz apart
        if (i == 6) b.offsetHz = bs[4].offsetHz + 1000;
        if (i == 12) b.offsetHz = bs[11].offsetHz + 9;      // ★ two beacons sharing a frequency (9 Hz apart)
        b.snrDb = -12 + rng.uni() * 40;
        b.toneHz = i % 3 == 0 ? 400 + rng.uni() * 20 : 1010 + rng.uni() * 30;
        if (i % 13 == 7) b.toneHz = 0;                      // A1A
        b.wpm = 6 + rng.uni() * 4; b.periodSec = 7 + rng.uni() * 8; b.phaseSec = rng.uni() * 6; b.repeats = i % 5 == 0 ? 2 : 1;
        // ★ the group must fit its period with a pause (two idents at 6 WPM take ≈ 13 s)
        const double groupSec = b.repeats * 45 * 1.2 / b.wpm;
        if (b.periodSec < groupSec + 3) b.periodSec = groupSec + 3;
        bs.push_back(b);
    }
    if (std::getenv("NDB_SHOW")) for (auto& b : bs) std::printf("  sent %-4s %+9.1f Hz %+5.1f dB tone %4.0f\n", b.ident.c_str(), b.offsetHz, b.snrDb, b.toneHz);
    std::vector<cf> iq; synth(bs, rate, secs, rng, iq);
    Skimmer::Config c; c.rate = rate;
    Skimmer sk(c);
    Result r = run(sk, iq, rate);
    int right = 0, wrong = 0, missed = 0; double tsum = 0;
    std::set<int> used;
    for (auto& b : bs) {
        bool ok = false;
        for (auto& kv : r.ident) if (kv.second == b.ident && std::fabs(r.hz[kv.first] - b.offsetHz) < 60) { ok = true; tsum += r.identAt[kv.first]; used.insert(kv.first); }
        if (ok) ++right; else { ++missed; std::printf("  missed %-4s %+9.1f Hz %+5.1f dB tone %4.0f %4.1f WPM period %.1f s\n", b.ident.c_str(), b.offsetHz, b.snrDb, b.toneHz, b.wpm, b.periodSec); }
    }
    for (auto& kv : r.ident) if (!used.count(kv.first)) { ++wrong; std::printf("  WRONG ident %s at %+.1f Hz\n", kv.second.c_str(), r.hz[kv.first]); }
    std::printf("multi: 40 beacons (SNR -12..+28 dB), 4 min at 192 kHz: %d identified, %d missed, %d wrong; mean time to confirm %.0f s; carriers %d; CPU %.3f %% of one core\n",
                right, missed, wrong, right ? tsum / right : 0, sk.carriers(), 100.0 * r.cpu / secs);
    return wrong ? 1 : 0;
}

static int noise(double minutes) {
    const double rate = 48000;
    int idents = 0, carriers = 0; uint64_t words = 0;
    for (int m = 0; m < (int)minutes; ++m) {
        // ★ one-minute pieces so memory stays small; one engine across them all
        static Skimmer* sk = nullptr;
        if (!sk) { Skimmer::Config c; c.rate = rate; sk = new Skimmer(c); sk->onIdent = [&](const BeaconView& v) { ++idents; std::printf("  FALSE IDENT %s at %+.1f Hz\n", v.ident.c_str(), v.offsetHz); }; }
        Rng rng(9000 + m);
        std::vector<cf> iq; synth({}, rate, 60, rng, iq);
        for (size_t i = 0; i < iq.size(); i += 4096) sk->feedIq(iq.data() + i, (int)std::min<size_t>(4096, iq.size() - i));
        carriers = std::max(carriers, sk->carriers());
        words = sk->wordsRead();
    }
    std::printf("noise: %.0f min of pure noise at 48 kHz: %d idents, at most %d carriers at once, %llu words read\n", minutes, idents, carriers, (unsigned long long)words);
    return idents ? 1 : 0;
}

static int bias() {
    Rng rng(5);
    std::vector<cf> iq; synth({}, 48000, 30, rng, iq);
    Skimmer::Config c; c.rate = 48000; Skimmer sk(c);
    for (size_t i = 0; i < iq.size(); i += 4096) sk.feedIq(iq.data() + i, (int)std::min<size_t>(4096, iq.size() - i));
    std::printf("floor calibration on noise: mean(L/floor) = %.4f (%.2f dB) — want 1.0\n", sk.noiseCalibration(), 10 * std::log10(sk.noiseCalibration()));
    return 0;
}

static int cpu(double secs) {
    for (double rate : { 192000.0, 384000.0, 600000.0 }) {
        Rng rng(11);
        std::vector<Beacon> bs;
        for (int i = 0; i < (int)(rate / 6000); ++i) { Beacon b; b.ident = "AB"; b.offsetHz = -rate * 0.45 + 3000.0 * i * 2; b.snrDb = 10; bs.push_back(b); }
        std::vector<cf> iq; synth(bs, rate, secs, rng, iq);
        Skimmer::Config c; c.rate = rate; Skimmer sk(c);
        Result r = run(sk, iq, rate);
        std::printf("  %4.0f kHz IQ  FFT %6d (%.2f Hz)  beacons %3zu  carriers %3d  identified %3d  %6.3f %% of one core  (%.1f ns/sample)  %s\n",
                    rate / 1e3, sk.fftSize(), sk.binHzActual(), bs.size(), sk.carriers(), sk.identified(), 100.0 * r.cpu / secs,
                    1e9 * r.cpu / (rate * secs), Skimmer::simdName());
    }
    return 0;
}

static int wav(const char* path, double centreHz, double loK, double hiK) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 1; }
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12) return 1;
    int rate = 0, ch = 0, bits = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) return 1;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len); if (std::fread(b.data(), 1, len, f) != len) return 1;
            ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; bits = b[14] | b[15] << 8;
        } else if (!std::memcmp(id, "data", 4)) break;
        else std::fseek(f, len, SEEK_CUR);
    }
    if (ch != 2 || bits != 16) { std::fprintf(stderr, "need stereo int16 I/Q (got %d ch %d bit)\n", ch, bits); return 1; }
    Skimmer::Config cfg; cfg.rate = rate;
    if (hiK > loK) { cfg.loHz = loK * 1e3 - centreHz; cfg.hiHz = hiK * 1e3 - centreHz; }
    Skimmer sk(cfg);
    sk.setStartUtcMs(0);
    std::printf("%s: %d Hz IQ, FFT %d (%.2f Hz bins, hop %.2f ms), searching %.1f–%.1f kHz, %s kernels\n", path, rate, sk.fftSize(),
                sk.binHzActual(), sk.hopSec() * 1e3, (centreHz + sk.searchedLoHz()) / 1e3, (centreHz + sk.searchedHiHz()) / 1e3, Skimmer::simdName());
    const bool quiet = std::getenv("NDB_QUIET") != nullptr;
    sk.onIdent = [&](const BeaconView& v) {
        std::printf("  %6.1f s  IDENT %-5s %9.3f kHz  tone %4.0f Hz  %4.1f WPM  %+5.1f dB  (%d of %d)\n", sk.nowMs() / 1000.0, v.ident.c_str(),
                    (centreHz + v.offsetHz) / 1e3, v.toneHz, v.wpm, v.snrDb, v.votes, v.ballots);
    };
    if (!quiet) sk.onCopy = [&](const Copy& c) { std::printf("  %6.1f s  copy  %9.3f kHz  %s\n", c.utcMs / 1000.0, (centreHz + c.offsetHz) / 1e3, c.text.c_str()); };
    std::vector<int16_t> raw(2 * 8192); std::vector<cf> iq(8192);
    double cpu = 0; long total = 0;
    for (;;) {
        const size_t got = std::fread(raw.data(), 4, 8192, f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
        const double c0 = threadCpu();
        sk.feedIq(iq.data(), (int)got);
        cpu += threadCpu() - c0; total += (long)got;
    }
    std::fclose(f);
    const double secs = (double)total / rate;
    std::printf("\n%.0f s in %.2f s of CPU = %.3f %% of one core; %d carriers and %d identified at the end; %llu words read\n",
                secs, cpu, 100.0 * cpu / secs, sk.carriers(), sk.identified(), (unsigned long long)sk.wordsRead());
    std::printf("\nBeacons (strongest active first, then gone):\n");
    for (auto& v : sk.beacons()) {
        std::printf("  %9.3f kHz  %-5s %-3s (%2d/%2d) read '%-4s' tone %4.0f Hz (%+5.1f dB)  %4.1f WPM  %+5.1f dB  %s  %s\n", (centreHz + v.offsetHz) / 1e3,
                    v.ident.c_str(), v.keyedCarrier ? "A1A" : "", v.votes, v.ballots, v.reading.c_str(), v.toneHz, v.toneSnrDb, v.wpm, v.snrDb,
                    v.active ? "" : "(gone)", v.copy.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    const std::string m = argc > 1 ? argv[1] : "";
    if (m == "one" && argc > 2) return one(std::atof(argv[2]), argc > 3 ? argv[3] : "WOD", argc > 4 ? std::atof(argv[4]) : 1020, argc > 5 ? std::atof(argv[5]) : 7);
    if (m == "accuracy") return accuracy();
    if (m == "draw" && argc > 4) return draw(std::atof(argv[2]), std::atof(argv[3]), std::atoi(argv[4]), argc > 5 ? std::atof(argv[5]) : 240);
    if (m == "multi") return multi();
    if (m == "noise") return noise(argc > 2 ? std::atof(argv[2]) : 30);
    if (m == "bias") return bias();
    if (m == "cpu") return cpu(argc > 2 ? std::atof(argv[2]) : 30);
    if (m == "wav" && argc > 3) return wav(argv[2], std::atof(argv[3]), argc > 5 ? std::atof(argv[4]) : 0, argc > 5 ? std::atof(argv[5]) : 0);
    std::fprintf(stderr, "usage: ndb_bench one <snr> [ident] [tone] [wpm] | accuracy | multi | noise [min] | bias | cpu [s] | wav <iq.wav> <centreHz> [loKHz hiKHz]\n");
    return 2;
}
