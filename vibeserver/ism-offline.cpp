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
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o ism-offline vibeserver/ism-offline.cpp -lpthread
#include "vibe_ism_service.h"
#include "vibe_ism_gallery.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vibeism;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <capture> <rate> <centre Hz> [u8|cf32] [--bursts]\n", argv[0]); return 2; }
    const double rate = std::atof(argv[2]), centre = std::atof(argv[3]);
    const bool f32 = argc > 4 && std::strcmp(argv[4], "cf32") == 0;
    bool listBursts = false, lora = true;
    std::string galDir;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bursts") == 0) listBursts = true;
        if (std::strcmp(argv[i], "--no-lora") == 0) lora = false;
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
    // ★ V12 wM-Bus: a capture inside 863-870 MHz is searched for meters as the server's 868 band is (vibe_wmbus.h)
    const bool meters = centre >= 863e6 && centre <= 870e6;
    if (meters) { fe.setChanRate(500e3); fe.setKeepBaseband(true); }
    wmbus::Stats wst;
    std::string lastMeterId; double lastMeterT = -1;
    Tracker tr;
    FobWatch fobs;   // ★ undecoded presses → "Suspected key fob", as the server does
    vibelora::Engine le;
    vibelora::MeshTable mesh;
    if (lora) le.startInline(bandNearest(centre)->id, centre, rate, INFINITY, rate < 1e6 ? 0.5 : 0.45);
    if (lora) { std::printf("LoRa: %zu plan channels in this capture (%zu detectors)\n", le.channelsRun(), le.detectors());
                for (auto& n : le.channelNames()) std::printf("  %s\n", n.c_str()); }
    double t = 0;
    uint64_t nb = 0, nd = 0, nl = 0;
    std::vector<uint8_t> u(1 << 20);
    std::vector<float> fb(1 << 20);
    std::vector<cf> x;
    const auto t0 = std::chrono::steady_clock::now();
    auto onBurst = [&](const Burst& b) {
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
    for (;;) {
        size_t n;
        if (f32) { n = std::fread(fb.data(), sizeof(float), fb.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf(fb[2 * i], fb[2 * i + 1]); }
        else { n = std::fread(u.data(), 1, u.size(), f) / 2; x.resize(n); for (size_t i = 0; i < n; ++i) x[i] = cf((u[2 * i] - 127.5f) / 128.0f, (u[2 * i + 1] - 127.5f) / 128.0f); }
        if (!n) break;
        fe.feed(x.data(), n, onBurst);
        if (lora) {
            le.process(x.data(), n);
            for (auto& h : le.take()) {
                ++nl;
                const auto& m = h.m;
                const std::string line = m.proto == "lorawan" ? vibelora::lorawan::line(m) : m.proto == "meshtastic" ? vibelora::meshtastic::line(m)
                                       : m.proto == "meshcore" ? vibelora::meshcore::line(m)
                                       : "LoRa · sync 0x" + vibelora::crypto::toHex(&m.sync, 1, true) + " · " + std::to_string(m.len) + " bytes";
                std::printf("%8.3f s  %9.4f MHz  SF%d %5.1f kHz CR4/%d  SNR %5.1f dB  %s\n", t + double(n) / rate, h.f.freqHz / 1e6, h.f.sf,
                            h.f.bw / 1e3, 4 + h.f.cr, h.f.snrDb, line.c_str());
                mesh.accept(m, t);
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
