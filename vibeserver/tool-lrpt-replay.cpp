// tool-lrpt-replay.cpp — replay a developer LRPT capture (VIBE_LRPT_IQ_DUMP, vibe_lrpt_run.h Run::Dump) through the
// server's own LRPT run, with the Doppler the satellite engine fed it, second by second.
//
// ★★ A DEVELOPER TOOL (Stuart 2026-11-21: the capture is for debugging on his Pi 500, never a user feature). It exists
//    because a live Meteor-M2-3 pass ~4–5 dB over the floor stayed "listening" while test-lrpt decodes synthetic
//    QPSK at Es/N0 4 dB: the live path must be replayed to find the difference.
//
//   tool-lrpt-replay <capture.cf32> [capture.txt]      replay; prints the decoder's status once a second
//   tool-lrpt-replay --selftest <dir>                   synthesise a pass (Es/N0 4 dB, Doppler ramp), capture it through
//                                                       Run::Dump exactly as the server would, replay it, check frames
//
// Build: c++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/tool-lrpt-replay.cpp -o tool-lrpt-replay
#include "vibe_lrpt_run.h"
#include "vibe_lrpt_tx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vibelrpt;

namespace {

struct DopLog { std::map<long, double> at; double rate = 0; };

DopLog readLog(const std::string& path) {
    DopLog d;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return d;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') { const char* r = std::strstr(line, "rate "); if (r) d.rate = std::atof(r + 5); continue; }
        double t = 0, dop = 0, rate = 0;
        if (std::sscanf(line, "%lf %lf %lf", &t, &dop, &rate) >= 2) { d.at[long(t)] = dop; if (rate > 0) d.rate = rate; }
    }
    std::fclose(f);
    return d;
}

/** Replay options (developer knobs: what the server would have done with a different attach or no engine). */
struct ReplayOpts {
    double symbolRate = 0;      ///< 0 = AUTO over 72k / 80k (what the clients send); 72000 / 80000 = pinned
    bool noDoppler = false;     ///< ignore the logged feed-forward (the receiver acquires and tracks alone)
    double from = 0, to = 1e9;  ///< seconds of the capture to feed
    std::string png;            ///< write the pass's composite (or first channel) here
    bool cadu = false;          ///< print every CADU: time, ASM score, ambiguity, Viterbi metric, the four RS words
};
ReplayOpts gOpt;

/** Feed a capture through a Run; returns good frames. Prints the status line once a second when `verbose`. */
unsigned long long replay(const std::string& cf32, const std::string& txt, bool verbose) {
    DopLog dl = readLog(txt);
    if (!(dl.rate > 0)) {   // the rate is in the file name too: lrpt-<unix>-<rate>.cf32
        const size_t dash = cf32.rfind('-'), dot = cf32.rfind('.');
        if (dash != std::string::npos && dot > dash) dl.rate = std::atof(cf32.substr(dash + 1, dot - dash - 1).c_str());
    }
    if (!(dl.rate > 0)) { std::fprintf(stderr, "no sample rate (no .txt and none in the name)\n"); return 0; }
    FILE* f = std::fopen(cf32.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", cf32.c_str()); return 0; }
    double tNow = 0;
    Run::Opts o;
    o.rateAuto = !(gOpt.symbolRate > 0);
    if (gOpt.symbolRate > 0) { o.symbolRate = gOpt.symbolRate >= 76000 ? 80000 : 72000; o.interleaved = o.symbolRate == 80000; }
    if (gOpt.noDoppler) o.dopplerHz = [] { return 0.0; }; else
    o.dopplerHz = [&] { auto it = dl.at.upper_bound(long(tNow)); if (it == dl.at.begin()) return dl.at.empty() ? 0.0 : dl.at.begin()->second; --it; return it->second; };
    std::string last;
    const Receiver* rxNow = nullptr;
    o.json = [&](const std::string& j) {
        last = j;
        if (verbose) std::printf("%7.1f s  esn0 %+5.1f dB  %s\n", tNow, rxNow ? 10 * std::log10(std::max(1e-3, rxNow->esn0())) : -99.0, j.c_str());
    };
    unsigned long long frames = 0;
    o.log = [&](const std::string& l) { if (verbose) std::printf("%7.1f s  %s\n", tNow, l.c_str()); };
    o.pictures = [&](const std::vector<Picture>& ps, bool partial) {
        std::printf("%7.1f s  pictures%s:", tNow, partial ? " (partial)" : "");
        for (const auto& p : ps) std::printf(" [%s %dx%d]", p.name.c_str(), p.w, p.h);
        std::printf("\n");
        if (!gOpt.png.empty() && !ps.empty()) {
            static int k = 0;
            const std::string path = gOpt.png + "-" + std::to_string(k++) + ".png";
            if (FILE* pf = std::fopen(path.c_str(), "wb")) { std::fwrite(ps.front().png.data(), 1, ps.front().png.size(), pf); std::fclose(pf); std::printf("          wrote %s\n", path.c_str()); }
        }
    };
    if (gOpt.cadu) o.made = [&](Receiver& rx) {
        rx.setCaduDiag([&](int sc, int x, int vit, const int* pw) {
            std::printf("%8.2f cadu asm %2d x %d vit %4d rs %d %d %d %d\n", tNow, sc, x, vit, pw[0], pw[1], pw[2], pw[3]);
        });
    };
    {
        Run run(o);
        const int blk = int(dl.rate / 20);
        std::vector<std::complex<float>> buf((size_t)blk);
        size_t got;
        if (gOpt.from > 0) { fseeko(f, off_t(std::llround(gOpt.from * dl.rate)) * off_t(sizeof(std::complex<float>)), SEEK_SET); tNow = gOpt.from; }
        while (tNow < gOpt.to && (got = std::fread(buf.data(), sizeof(std::complex<float>), buf.size(), f)) > 0) {
            run.feedIq(buf.data(), int(got), dl.rate, 0);
            rxNow = run.receiver();
            tNow += got / dl.rate;
        }
        if (const Receiver* rx = run.receiver()) frames = rx->stats().rsOk;
        run.finish(false);
    }
    std::fclose(f);
    std::printf("replayed %.1f s at %.0f S/s: %llu good frames\n", tNow, dl.rate, frames);
    return frames;
}

int selftest(const std::string& dir) {
    // A synthetic pass with a Doppler ramp, Es/N0 4 dB, captured through Run::Dump (VIBE_LRPT_IQ_DUMP) as the server would
    tx::TxConfig t; t.sampleRate = 187500; t.esn0Db = 4; t.carrierHz = -1800; t.dopplerRateHzS = 40;
    const auto iq = tx::modulate(t, tx::codeBits(t, tx::buildVcdus(t, 6)));
    setenv("VIBE_LRPT_IQ_DUMP", dir.c_str(), 1);
    unsigned long long liveFrames = 0;
    {
        Run::Opts o; o.rateAuto = true; double tNow = 0;
        o.dopplerHz = [&] { return 0.0; };   // the satellite engine's feed-forward (none here: the receiver tracks the ramp)
        Run run(o);
        const size_t blk = size_t(t.sampleRate / 20);
        for (size_t at = 0; at < iq.size() / 2; at += blk) {
            const size_t n = std::min(blk, iq.size() / 2 - at);
            run.feedIq(reinterpret_cast<const std::complex<float>*>(&iq[at * 2]), int(n), t.sampleRate, 0);
            tNow += n / t.sampleRate;
        }
        if (const Receiver* rx = run.receiver()) liveFrames = rx->stats().rsOk;
    }
    unsetenv("VIBE_LRPT_IQ_DUMP");
    // find the capture just written
    std::string cf32;
    if (FILE* p = popen(("ls -t " + dir + "/lrpt-*.cf32 2>/dev/null | head -1").c_str(), "r")) {
        char b[1024]; if (std::fgets(b, sizeof b, p)) { cf32 = b; while (!cf32.empty() && (cf32.back() == '\n' || cf32.back() == '\r')) cf32.pop_back(); }
        pclose(p);
    }
    if (cf32.empty()) { std::printf("FAIL no capture written in %s\n", dir.c_str()); return 1; }
    const std::string txt = cf32.substr(0, cf32.size() - 5) + ".txt";
    const unsigned long long rep = replay(cf32, txt, false);
    const bool ok = liveFrames > 0 && rep == liveFrames;
    std::printf("%s live %llu frames, replay %llu frames — the capture reproduces the run\n", ok ? "ok  " : "FAIL", liveFrames, rep);
    std::remove(cf32.c_str()); std::remove(txt.c_str());
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--selftest") == 0) return selftest(argv[2]);
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") gOpt.symbolRate = std::atof(val().c_str());
        else if (a == "--no-doppler") gOpt.noDoppler = true;
        else if (a == "--from") gOpt.from = std::atof(val().c_str());
        else if (a == "--to") gOpt.to = std::atof(val().c_str());
        else if (a == "--png") gOpt.png = val();
        else if (a == "--cadu") gOpt.cadu = true;
        else pos.push_back(a);
    }
    if (pos.empty()) { std::fprintf(stderr, "usage: %s <capture.cf32> [capture.txt] [--rate 72000|80000] [--no-doppler] [--from s] [--to s] [--png prefix] | --selftest <dir>\n", argv[0]); return 2; }
    const std::string cf32 = pos[0];
    const std::string txt = pos.size() >= 2 ? pos[1] : cf32.substr(0, cf32.size() - 5) + ".txt";
    return replay(cf32, txt, true) > 0 ? 0 : 1;
}
