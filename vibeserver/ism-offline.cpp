// ism-offline.cpp — run the V12 ISM decoder (vibe_ism*.h) over a recorded capture and print what it heard.
//   ism-offline <capture> <rate Hz> <centre Hz> [u8|cf32] [--bursts] [--gallery <dir>]
// u8 = rtl_tcp bytes (what VibeServer's raw IQ out sends), cf32 = interleaved float32. Prints one readable line per
// decode (the same text the clients get), then a summary. --bursts also lists every burst with its slicing, for
// working on an unknown device — ★ never with its bits: a key fob's code is not printed here either.
// --gallery <dir> (V12 2026-10-25): also keep what was heard as the GALLERY does (vibe_ism_gallery.h) — the day's log and
// summary card for the nearest band, written into <dir> on the capture's own clock (it starts "now") — so a real
// recording proves the readable lines and the privacy rule in the files themselves.
// ★ V12 LoRa (2026-10-27, vibe_lora*.h): the band's LoRa plan runs over the same capture — every plan channel the
// recording holds (a narrow recording below 1 MS/s made for ONE channel may use its whole width) — and each frame is
// printed as the clients get it (LoRaWAN payloads by length only; mesh text from the published channels only), then
// the detectors' counts (preambles, SFDs, bad headers, bad CRCs). --no-lora skips it.
// ★ THE SERVER'S THREADS (2026-10-31, the Pi 2 thread split): --threads runs the capture through vibeism::Service itself —
// the detector, mixer and slicer each on their own thread (vibe-ism, vibe-ism2, vibe-ism3), LoRa on vibe-lora — fed in the
// radio's own 32 ms blocks. Without --realtime the queues WAIT rather than drop (Service::setLossless) and every burst is
// printed as the inline run prints it, so the two can be diffed; detection is then limited to the band's own span as the
// server limits it, so diff against an inline run with --band-only. --realtime feeds at the capture's own pace, drops as
// the server would, and prints each thread's CPU from /proc (Linux) — the Pi 2 figures in DECODER-SIMD-AUDIT.md.
// --lora-default: the LoRa channels the server would choose on this machine with no benchmark yet (the default set only
// on 32-bit ARM), instead of every plan channel.
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o ism-offline vibeserver/ism-offline.cpp -lpthread
#include "vibe_ism_service.h"
#include "vibe_ism_gallery.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#if defined(__linux__)
#include <dirent.h>
#endif
#include <cstring>
#include <string>
#include <vector>

using namespace vibeism;

/** Every thread of this process with its CPU seconds so far (Linux: /proc/self/task; elsewhere nothing). */
static std::vector<std::pair<std::string, double>> threadCpu() {
    std::vector<std::pair<std::string, double>> v;
#if defined(__linux__)
    const double hz = double(sysconf(_SC_CLK_TCK));
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string base = std::string("/proc/self/task/") + e->d_name;
            char comm[64] = {0}, st[1024] = {0};
            if (FILE* f = std::fopen((base + "/comm").c_str(), "r")) { if (!std::fgets(comm, sizeof comm, f)) comm[0] = 0; std::fclose(f); }
            if (FILE* f = std::fopen((base + "/stat").c_str(), "r")) { if (!std::fgets(st, sizeof st, f)) st[0] = 0; std::fclose(f); }
            std::string c(comm); while (!c.empty() && (c.back() == '\n' || c.back() == ' ')) c.pop_back();
            const char* r = std::strrchr(st, ')');
            if (!r) continue;
            // after ") ": state(3) ppid … utime is field 14, stime 15 — the 12th and 13th after the state
            unsigned long long ut = 0, stt = 0; int k = 0; const char* q = r + 2;
            while (*q && k < 11) { if (*q == ' ') ++k; ++q; }
            if (std::sscanf(q, "%llu %llu", &ut, &stt) == 2) v.push_back({c + "/" + e->d_name, double(ut + stt) / hz});
        }
        closedir(d);
    }
#endif
    return v;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <capture> <rate> <centre Hz> [u8|cf32] [--bursts]\n", argv[0]); return 2; }
    const double rate = std::atof(argv[2]), centre = std::atof(argv[3]);
    const bool f32 = argc > 4 && std::strcmp(argv[4], "cf32") == 0;
    bool listBursts = false, lora = true, threads = false, realtime = false, bandOnly = false, loraDefault = false;
    std::string galDir;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bursts") == 0) listBursts = true;
        if (std::strcmp(argv[i], "--no-lora") == 0) lora = false;
        if (std::strcmp(argv[i], "--threads") == 0) threads = true;
        if (std::strcmp(argv[i], "--realtime") == 0) realtime = threads = true;
        if (std::strcmp(argv[i], "--band-only") == 0) bandOnly = true;
        if (std::strcmp(argv[i], "--lora-default") == 0) loraDefault = true;
        if (std::strcmp(argv[i], "--gallery") == 0 && i + 1 < argc) galDir = argv[++i];
    }
    vibe::gallery::Store store;
    std::shared_ptr<vibe::ismgal::DayLog> galLog;
    const int64_t wall0 = vibe::gallery::nowUtc();
    double capT = 0;   // ★ the capture's own clock, for the gallery's lines
    if (!galDir.empty()) {
        store.setDir(galDir);
        store.setLimitBytes(100ull << 20);
        const Band* b = bandNearest(centre);
        vibe::ismgal::DayLog::Options o;
        o.bandId = b->id; o.bandName = b->name; o.bandHz = b->centreHz; o.origin = "live";
        o.clock = [&] { return wall0 + int64_t(capT); };
        galLog = std::make_shared<vibe::ismgal::DayLog>(store, o);
        if (store.dir().empty()) { std::fprintf(stderr, "cannot write the gallery into %s\n", galDir.c_str()); return 1; }
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    const Band* band = bandNearest(centre);
    if (bandOnly) {   // ★ as the service limits it (Service::start): the band's own span and a margin, around this centre
        const double mid = band->centreHz - centre, half = band->needHz + 40e3;
        fe.setUsableHz(mid - half, mid + half);
    }
    const double loraBudget = loraDefault ? vibelora::budgetFromBench(0) : INFINITY;
    // ★ V12 wM-Bus: a capture inside 863-870 MHz is searched for meters as the server's 868 band is (vibe_wmbus.h)
    const bool meters = centre >= 863e6 && centre <= 870e6;
    if (meters) { fe.setChanRate(500e3); fe.setKeepBaseband(true); }
    wmbus::Stats wst;
    std::string lastMeterId; double lastMeterT = -1;
    Tracker tr;
    FobWatch fobs;   // ★ undecoded presses → "Suspected key fob", as the server does
    vibelora::Engine le;
    vibelora::MeshTable mesh;
    if (lora && !threads) le.startInline(bandNearest(centre)->id, centre, rate, loraBudget, rate < 1e6 ? 0.5 : 0.45);
    if (lora) { std::printf("LoRa: %zu plan channels in this capture (%zu detectors)\n", le.channelsRun(), le.detectors());
                for (auto& n : le.channelNames()) std::printf("  %s\n", n.c_str()); }
    double t = 0;
    uint64_t nb = 0, nd = 0, nl = 0;
    std::vector<uint8_t> u(1 << 20);
    std::vector<float> fb(1 << 20);
    std::vector<cf> x;
    const auto t0 = std::chrono::steady_clock::now();
    auto onBurstBody = [&](const Burst& b) {
        ++nb;
        std::vector<Reading> got;
        Service::decodeBurst(b, centre, got, &wst);
        // ★ the same frame out of a second channel on its skirt (the service drops it across a block — dedupeMeters)
        if (!got.empty() && got[0].kind == "meter") {
            const auto& m = *got[0].meter;
            if (lastMeterId == m.id && b.tSec - lastMeterT < 0.2) got.clear();
            else { lastMeterId = m.id; lastMeterT = b.tSec; }
        }
        if (listBursts) {
            // ★ The burst's shape for working on an unknown device (2026-10-25, Stuart's car key): its length, how much
            //   of it the carrier was up (OOK mark share), and the FSK deviation when the discriminator ran.
            double on = 0, all = 0;
            for (int d : b.ook) { all += std::abs(d); if (d > 0) on += d; }
            std::printf("  burst %8.3f s  %9.4f MHz  level %5.1f dBFS  snr %4.1f dB  %s%zu runs (shortest %.0f µs)  "
                        "len %6.1f ms  mark %3.0f %%  dev %4.1f kHz%s\n", b.tSec,
                        (centre + b.offHz + b.carrierHz) / 1e6, b.levelDb, b.levelDb - b.noiseDb,
                        b.fsk.empty() ? "OOK " : "FSK ", b.fsk.empty() ? b.ook.size() : b.fsk.size(),
                        FrontEnd::shortestRun(b.fsk.empty() ? b.ook : b.fsk), all / 1000.0, all > 0 ? 100.0 * on / all : 0.0,
                        b.fskDevHz / 1000.0, got.empty() ? "" : "  → decoded");
        }
        if (got.empty()) { Reading sf; if (fobs.offer(b, centre, sf)) got.push_back(sf); }
        for (double hz : fobs.takePeriodic()) { tr.forget("suspectfob", hz, 50e3); std::printf("  (%.4f MHz is on a timer — a sensor, not a fob)\n", hz / 1e6); }
        for (auto& r : got) {
            ++nd;
            Heard h;
            capT = b.tSec;
            if (tr.accept(r, b.tSec, galLog ? &h : nullptr) && galLog) galLog->heard(h);
            std::printf("%8.3f s  %9.4f MHz  %s\n", b.tSec, r.freqHz / 1e6, readable(r, 0).c_str());
        }
    };
#if defined(VIBE_ISM_PROF)
    // ★ VIBE_ISM_PROF: the decoders' share, taken out of the front end's detect figure (vibe_ism.h IsmProf)
    double tLora = 0;
    auto onBurst = [&](const Burst& b) { const double tp0 = ismProfNow(); onBurstBody(b); fe.prof.decode += ismProfNow() - tp0; };
#else
    auto& onBurst = onBurstBody;
#endif
    if (threads) {
        // ★ The server's own pipeline: Service, its threads and queues, fed in the radio's 32 ms blocks.
        Service svc;
        std::mutex printMtx;
#if defined(VIBE_ISM_OLD_SERVICE)
        // ★ The bench's "before": this file built against the headers from before the thread split (one worker), real time
        //   only — the old service had none of the test hooks.
        if (!realtime) { std::fprintf(stderr, "VIBE_ISM_OLD_SERVICE: --realtime only\n"); return 2; }
#else
        svc.setLossless(!realtime);
        if (!realtime) svc.setTap([&](const Burst& b, const std::vector<Reading>&) { std::lock_guard<std::mutex> g(printMtx); onBurstBody(b); });
#endif
        svc.setLora(lora, loraBudget);
        svc.start(*band, centre, rate, 0, false, galLog);
        const size_t blk = size_t(rate * 0.032);
        std::vector<uint8_t> ub(2 * blk);
        std::vector<float> fbb(2 * blk);
        std::vector<cf> xb;
        const auto w0 = std::chrono::steady_clock::now();
        double fed = 0;
        for (;;) {
            size_t n;
            if (f32) { n = std::fread(fbb.data(), sizeof(float), fbb.size(), f) / 2; xb.resize(n); for (size_t i = 0; i < n; ++i) xb[i] = cf(fbb[2 * i], fbb[2 * i + 1]); }
            else { n = std::fread(ub.data(), 1, ub.size(), f) / 2; xb.resize(n); for (size_t i = 0; i < n; ++i) xb[i] = cf((ub[2 * i] - 127.5f) / 128.0f, (ub[2 * i + 1] - 127.5f) / 128.0f); }
            if (!n) break;
            svc.feed(xb.data(), n);
            fed += double(n) / rate;
            if (realtime) std::this_thread::sleep_until(w0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(fed)));
        }
        std::fclose(f);
#if !defined(VIBE_ISM_OLD_SERVICE)
        if (!realtime) svc.endOfStream();   // ★ close what is open, as the inline run's flush() does
        // drain: lossless waits until every stage is idle; real time gives the stages a last second, as a live server would
        if (!realtime) { while (!svc.idle()) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        else
#endif
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        const auto cpu = threadCpu();
        std::string rep = svc.report();
        svc.stop();
        t = fed;
        std::printf("\n%.1f s of capture through the service's threads (%s) in %.1f s\n", t, realtime ? "real time, drops allowed" : "lossless", wall);
        if (realtime && !cpu.empty()) {
            double tot = 0;
            for (const auto& c : cpu) tot += c.second;
            std::printf("THREADS  (CPU over %.1f s of capture; %% of one core in real time)\n", t);
            for (const auto& c : cpu) if (c.second > 0.005) std::printf("THREAD  %-22s %7.2f s  %6.1f %%\n", c.first.c_str(), c.second, 100.0 * c.second / t);
            std::printf("THREAD  %-22s %7.2f s  %6.1f %%\n", "TOTAL", tot, 100.0 * tot / t);
        }
        // the service's own summary: its counts and drops by stage (report(), the JSON the clients get)
        auto field = [&](const char* k) { const size_t p = rep.find(std::string("\"") + k + "\":"); return p == std::string::npos ? std::string("?") : rep.substr(p + std::strlen(k) + 3, rep.find_first_of(",}", p + std::strlen(k) + 3) - (p + std::strlen(k) + 3)); };
        const size_t dp = rep.find("\"drops\":[");
        std::printf("service: bursts %s, decoded %s, unknown %s, dropped %s %s, capture %s %%\n", field("bursts").c_str(), field("decoded").c_str(),
                    field("unknown").c_str(), field("dropped").c_str(), dp == std::string::npos ? "" : rep.substr(dp, rep.find(']', dp) - dp + 1).c_str(),
                    field("capture").c_str());
        // ★ LoRa's own counts (its "lora" object: frames, and blocks dropped before either of its threads)
        const size_t lp = rep.find("\"lora\":{");
        if (lp != std::string::npos) {
            const std::string lo = rep.substr(lp, rep.find('}', lp) - lp);
            auto lf = [&](const char* k) { const size_t p = lo.find(std::string("\"") + k + "\":"); return p == std::string::npos ? std::string("?") : lo.substr(p + std::strlen(k) + 3, lo.find_first_of(",}", p + std::strlen(k) + 3) - (p + std::strlen(k) + 3)); };
            std::printf("lora: %s channels, %s detectors, %s frames, dropped %s\n", lf("chans").c_str(), lf("dets").c_str(), lf("frames").c_str(), lf("dropped").c_str());
        }
        if (!realtime) {
            std::printf("\n%.1f s of capture: %llu bursts, %llu decodes, %zu devices\n", t, (unsigned long long)nb, (unsigned long long)nd, tr.size());
            std::printf("\nthe device list (as the clients get it):\n%s\n", tr.json(t, nullptr).c_str());
        }
        if (galLog) galLog->finish();
        return 0;
    }
    for (;;) {
        size_t n;
        if (f32) { n = std::fread(fb.data(), sizeof(float), fb.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf(fb[2 * i], fb[2 * i + 1]); }
        else { n = std::fread(u.data(), 1, u.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf((u[2 * i] - 127.5f) / 128.0f, (u[2 * i + 1] - 127.5f) / 128.0f); }
        if (!n) break;
        fe.feed(x.data(), n, onBurst);
        if (lora) {
#if defined(VIBE_ISM_PROF)
            const double tl0 = ismProfNow();
            le.process(x.data(), n);
            tLora += ismProfNow() - tl0;
#else
            le.process(x.data(), n);
#endif
            for (auto& h : le.take()) {
                ++nl;
                const auto& m = h.m;
                const std::string line = m.proto == "lorawan" ? vibelora::lorawan::line(m) : m.proto == "meshtastic" ? vibelora::meshtastic::line(m)
                                       : m.proto == "meshcore" ? vibelora::meshcore::line(m)
                                       : "LoRa · sync 0x" + vibelora::crypto::toHex(&m.sync, 1, true) + " · " + std::to_string(m.len) + " bytes";
                std::printf("%8.3f s  %9.4f MHz  SF%d %5.1f kHz CR4/%d  SNR %5.1f dB  %s\n", t + double(n) / rate, h.f.freqHz / 1e6, h.f.sf,
                            h.f.bw / 1e3, 4 + h.f.cr, h.f.snrDb, line.c_str());
                std::vector<vibelora::Event> ev;
                mesh.accept(m, t, galLog ? &ev : nullptr);
                capT = t;
                for (const auto& e : ev) { Heard hd; hd.kind = e.kind; hd.name = e.name; hd.line = e.line; hd.freqHz = e.freqHz; galLog->heard(hd); }
            }
        }
        t += n / rate;
        capT = t;
        if (galLog) galLog->tick();
    }
    fe.flush(onBurst);
    std::fclose(f);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%.1f s of capture: %llu bursts, %llu decodes, %zu devices; noise floor %.1f dBFS/bin; %.2f s CPU (%.1f %% of real time)\n",
                t, (unsigned long long)nb, (unsigned long long)nd, tr.size(), fe.noiseDb(), sec, 100.0 * sec / std::max(t, 1e-9));
    if (meters)
        std::printf("meters: %llu bursts at 868.30 / 868.95 tried, %llu frames (%llu damaged), %llu from collectors; "
                    "★ encrypted payloads are never printed — only their length\n", (unsigned long long)wst.tried,
                    (unsigned long long)wst.frames, (unsigned long long)wst.damaged, (unsigned long long)wst.collector);
#if defined(VIBE_ISM_PROF)
    {
        const IsmProf& p = fe.prof;
        const double tot = p.copy + p.detect + p.mix + p.slice + p.decode + tLora;
        std::printf("PROF  (s of CPU over %.1f s of capture; %% of one core in real time)\n", t);
        auto row = [&](const char* k, double v) { std::printf("PROF  %-8s %8.3f s  %6.2f %%\n", k, v, 100.0 * v / t); };
        row("copy", p.copy); row("detect", p.detect); row("mix", p.mix); row("slice", p.slice); row("decode", p.decode); row("lora", tLora); row(" chan", le.profChan);
        if (const auto* chz = le.channeliser()) { row("  fft", chz->profFft); row("  ifft", chz->profIfft); row("  rot", chz->profRot); row("  resamp", chz->profRes); }
        row(" dets", le.profDemod); row("sum", tot);
        std::printf("PROF  detects %llu  opens %llu  cancels %llu  promotes %llu  closeQuiet %llu  closeLong %llu  fewHits %llu  sliceNo %llu\n",
                    (unsigned long long)p.detects, (unsigned long long)p.opens, (unsigned long long)p.cancels, (unsigned long long)p.promotes,
                    (unsigned long long)p.closeQuiet, (unsigned long long)p.closeLong, (unsigned long long)p.fewHits, (unsigned long long)p.sliceNo);
        std::printf("PROF  holds %llu  detections held %llu\n", (unsigned long long)p.holds, (unsigned long long)p.held);
        std::printf("PROF  mixed %.2f Msamp (%.2f channels on average)  sliced %.2f Msamp  in ran-too-long channels %.2f Msamp\n",
                    p.mixSamp / 1e6, double(p.mixSamp) / std::max(1.0, t * fe.workRate()), p.sliceSamp / 1e6, p.longSamp / 1e6);
    }
#endif
    if (lora) std::printf("LoRa: %llu frames; per detector that heard anything:\n%s", (unsigned long long)nl, le.stats().c_str());
    const auto loraRows = mesh.rowsJson(t);
    std::printf("\nthe device list (as the clients get it):\n%s\n", tr.json(t, &loraRows).c_str());
    if (lora && mesh.texts()) std::printf("\npublic messages:\n%s\n", mesh.msgsJson(t).c_str());
    if (galLog) {
        galLog->finish();
        std::printf("\nthe gallery (%s): log %s, card %s\n", galDir.c_str(), galLog->logName().c_str(), galLog->cardName().c_str());
    }
    return 0;
}
