// tool-orbcomm-file.cpp — a developer's replay (not a test, not a feature): an rtl_tcp cu8 recording (12-byte "RTL0"
// header, then unsigned 8-bit I/Q) through the ORBCOMM receiver exactly as the server feeds it, with CelesTrak elements
// to name the birds and the station's location for the sky plot's az/el. Prints a summary (and every non-fill packet with
// -v), and writes the decodes as CSV (t,chan,rfHz,snr,type,ok,norad,az,el,hex) for the sky plot's render.
//
//   tool-orbcomm-file <file.raw> --rate 250000 --centre 137300000 --start 2026-11-24T08:31:30Z --tle orbcomm.tle
//                     --lat 51.88 --lon -0.42 [--csv out.csv] [-v] [--chans 4]
#include "vibe_orbcomm_id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace vibeorb;

int main(int argc, char** argv) {
    std::string file, tle, csv, start;
    double rate = 250000, centre = 137.3e6, lat = 51.88, lon = -0.42;
    bool verbose = false; int chans = 4; double maxSec = 1e9;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") rate = std::atof(nx().c_str());
        else if (a == "--centre") centre = std::atof(nx().c_str());
        else if (a == "--start") start = nx();
        else if (a == "--tle") tle = nx();
        else if (a == "--lat") lat = std::atof(nx().c_str());
        else if (a == "--lon") lon = std::atof(nx().c_str());
        else if (a == "--csv") csv = nx();
        else if (a == "--chans") chans = std::atoi(nx().c_str());
        else if (a == "--seconds") maxSec = std::atof(nx().c_str());
        else if (a == "-v") verbose = true;
        else file = a;
    }
    if (file.empty()) { std::fprintf(stderr, "usage: see the header\n"); return 2; }
    double t0 = 0;
    { double jd; if (!start.empty() && vibe::sat::parseIsoUtc(start, jd)) t0 = vibe::sat::unixFromJd(jd); }
    std::vector<Bird> birds;
    if (!tle.empty()) {
        std::ifstream in(tle); std::string n, l1, l2;
        while (std::getline(in, n) && std::getline(in, l1) && std::getline(in, l2)) {
            while (!n.empty() && (n.back() == ' ' || n.back() == '\r')) n.pop_back();
            vibe::sat::Elements e; std::string err;
            if (!vibe::sat::parseTle(l1, l2, e, err, n)) continue;
            Bird b; b.norad = e.norad; b.name = n; b.ok = b.sgp.init(e);
            birds.push_back(std::move(b));
        }
    }
    Ident id; id.setBirds(birds); id.setObserver(vibe::sat::Observer(lat, lon, 100));
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) { std::perror(file.c_str()); return 1; }
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RTL0", 4) != 0) std::fseek(f, 0, SEEK_SET);
    std::FILE* out = csv.empty() ? nullptr : std::fopen(csv.c_str(), "w");
    if (out) std::fprintf(out, "t,chan,rfHz,snr,type,ok,repaired,norad,how,az,el,doppler,predicted,hex\n");

    Receiver::Cfg cfg; cfg.maxChans = chans;
    Receiver rx(cfg);
    std::map<int, std::map<std::string, int>> perType;          // chan → type → good count
    std::map<int, int> bad, good, repaired;
    std::map<int, std::set<std::string>> birdOf;
    std::map<int, double> firstT, lastT, rfFirst, rfLast, snrMax;
    std::map<std::string, int> messages;
    int ephShown = 0;
    double clockOff = 0; int anchors = 0;
    rx.onPacket = [&](const Rx& r) {
        if (!r.p.ok) { bad[r.chan]++; return; }
        good[r.chan]++; if (r.p.repaired) repaired[r.chan]++;
        perType[r.chan][typeName(r.p.type())]++;
        if (!firstT.count(r.chan)) { firstT[r.chan] = r.t; rfFirst[r.chan] = r.rfHz; }
        lastT[r.chan] = r.t; rfLast[r.chan] = r.rfHz; snrMax[r.chan] = std::max(snrMax[r.chan], r.snrDb);
        /* ★ A recording with DROPPED samples (the 2026-11-24 capture arrived at ~70 % of its rate) runs slow on its own sample
         *  clock. The satellites' ephemeris carries GPS time: each one re-anchors the file's clock (sample time + offset). */
        if (r.p.type() == kEphemeris) {
            Ephemeris e; decodeEphemeris(r.p, e, r.t + clockOff);
            if (e.unix > 0 && std::fabs(e.unix - (r.t + clockOff)) < 1800) { clockOff = e.unix - r.t; anchors++; }
        }
        Rx rc = r; rc.t = r.t + clockOff;
        const Ident::Answer a = id.onPacket(rc, rc.t);
        if (a.norad) birdOf[r.chan].insert(a.name + " (" + a.how + ")");
        if (out) std::fprintf(out, "%.3f,%d,%.1f,%.1f,%s,%d,%d,%lld,%s,%.2f,%.2f,%.1f,%.1f,%s\n", rc.t, r.chan, r.rfHz, r.snrDb, typeName(r.p.type()), 1,
                              r.p.repaired, a.norad, a.how.c_str(), a.azDeg, a.elDeg, a.dopplerHz, a.predictedHz, r.p.hex().c_str());
        if (r.p.type() == kEphemeris && ephShown < 6) {
            Ephemeris e; decodeEphemeris(r.p, e, r.t);
            std::printf("  EPH ch%d id 0x%02X week %d tow %d → sat clock %.1f (file %.1f, Δ %.2f s) lat %.2f lon %.2f alt %.1f km v %.3f km/s · match %.1f km\n",
                        r.chan, e.id, e.week, e.tow, e.unix, r.t, e.unix - r.t, e.latDeg, e.lonDeg, e.altKm, e.speedKmS, id.lastEphemerisMatchKm());
            ephShown++;
        }
        if (r.p.type() == kMessage) messages[r.p.hex()]++;
        if (verbose && r.p.type() != kFill) std::printf("%9.3f ch%d %.1f Hz snr %.1f %-9s %s %s\n", r.t, r.chan, r.rfHz, r.snrDb, typeName(r.p.type()), r.p.hex().c_str(), a.name.c_str());
    };
    const int B = 16384;
    std::vector<uint8_t> buf(size_t(B) * 2);
    std::vector<std::complex<float>> x(B);
    uint64_t n = 0;
    while (true) {
        const size_t got = std::fread(buf.data(), 1, buf.size(), f) / 2;
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) x[i] = { (float(buf[2 * i]) - 127.5f) / 128.f, (float(buf[2 * i + 1]) - 127.5f) / 128.f };
        rx.feed(x.data(), int(got), rate, centre, t0 + double(n) / rate);
        n += got;
        if (double(n) / rate > maxSec) break;
        if (n % (uint64_t(rate) * 60) < uint64_t(B)) {
            std::printf("[%.0f s] carriers:", double(n) / rate);
            for (auto& c : rx.channels()) std::printf(" #%d %.3f MHz snr %.1f %s ok %llu bad %llu;", c.id, c.rfHz / 1e6, c.snrDb, c.locked ? "LOCK" : "hunt", (unsigned long long)c.ok, (unsigned long long)c.bad);
            std::printf("\n"); std::fflush(stdout);
        }
    }
    std::fclose(f); if (out) std::fclose(out);
    std::printf("\n%.1f s of IQ\n", double(n) / rate);
    for (auto& g : good) {
        const int c = g.first;
        std::printf("carrier #%d: %.4f → %.4f MHz, %.0f s → %.0f s, good %d (repaired %d), bad %d, best SNR %.1f dB, sc ID 0x%02X, DCN %d (%.4f MHz)\n",
                    c, rfFirst[c] / 1e6, rfLast[c] / 1e6, firstT[c] - t0, lastT[c] - t0, g.second, repaired[c], bad[c], snrMax[c], id.scIdOf(c) & 0xFF,
                    id.channelOf(c), downlinkHz(id.channelOf(c)) / 1e6);
        std::printf("   types:"); for (auto& t : perType[c]) std::printf(" %s %d", t.first.c_str(), t.second); std::printf("\n");
        std::printf("   bird:"); for (auto& b : birdOf[c]) std::printf(" %s;", b.c_str()); std::printf("\n");
    }
    std::printf("learnt IDs:"); for (auto& m : id.learntIds()) std::printf(" 0x%02X→%lld", m.first, m.second); std::printf("\n");
    std::printf("distinct subscriber message packets: %zu\nclock anchors (ephemeris): %d, final offset %.1f s (the satellite clock minus the recording clock)\n", messages.size(), anchors, clockOff);
    return 0;
}
