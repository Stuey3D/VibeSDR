// tool-orbcomm-sky-compare.cpp — a developer's replay (not a test): the SAME cu8 recording through the full ORBCOMM
// decoder (vibe_orbcomm.h Receiver + vibe_orbcomm_id.h Ident, exactly as tool-orbcomm-file) AND the light sky-plot
// watcher (vibe_orbcomm_sky.h SkyWatch), and the two modes' sky dots compared, 5 s bucket by 5 s bucket:
//   · per bird: buckets with a dot in each mode, in both, light-only (the full decoder did not name it there) and
//     full-only (the light mode missed it);
//   · a light dot on a bird the full decoder heard in another bucket only, or never: a WRONG-BIRD candidate;
//   · the SNR difference (light − full, each bucket's max) — mean and spread;
//   · the az/el the dots carry (SGP4) against the satellite's OWN position report (ephemeris packets → az/el).
// ★ Stuart (11-25): the light mode is fine "as long as the plot is still accurate enough". This is the proof.
//
//   tool-orbcomm-sky-compare <file.cu8> --rate 2400000 --centre 137500000 --start 2026-11-24T16:53:19Z --tle f.tle
//                            [--lat 51.88 --lon -0.42] [--seconds N]
#include "vibe_orbcomm_id.h"
#include "vibe_orbcomm_sky.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace vibeorb;

int main(int argc, char** argv) {
    std::string file, tle, start;
    double rate = 2.4e6, centre = 137.5e6, lat = 51.88, lon = -0.42, maxSec = 1e9;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") rate = std::atof(nx().c_str());
        else if (a == "--centre") centre = std::atof(nx().c_str());
        else if (a == "--start") start = nx();
        else if (a == "--tle") tle = nx();
        else if (a == "--lat") lat = std::atof(nx().c_str());
        else if (a == "--lon") lon = std::atof(nx().c_str());
        else if (a == "--seconds") maxSec = std::atof(nx().c_str());
        else file = a;
    }
    if (file.empty() || tle.empty()) { std::fprintf(stderr, "usage: see the header\n"); return 2; }
    double t0 = 0;
    { double jd; if (!start.empty() && vibe::sat::parseIsoUtc(start, jd)) t0 = vibe::sat::unixFromJd(jd); }
    std::vector<Bird> birds; std::vector<SkyBird> sky;
    {
        std::ifstream in(tle); std::string n, l1, l2;
        while (std::getline(in, n) && std::getline(in, l1) && std::getline(in, l2)) {
            while (!n.empty() && (n.back() == ' ' || n.back() == '\r')) n.pop_back();
            vibe::sat::Elements e; std::string err;
            if (!vibe::sat::parseTle(l1, l2, e, err, n)) continue;
            Bird b; b.norad = e.norad; b.name = n; b.ok = b.sgp.init(e); birds.push_back(b);
            SkyBird s; s.norad = e.norad; s.name = n; s.ok = s.sgp.init(e); s.epochUnix = e.epochUnix(); sky.push_back(s);
        }
    }
    const vibe::sat::Observer obs(lat, lon, 100);
    Ident id; id.setBirds(birds); id.setObserver(obs);
    std::map<long long, std::string> names; for (auto& b : birds) names[b.norad] = b.name;

    // the full decoder's dots: (norad, bucket) → max SNR
    std::map<std::pair<long long, long>, int> fullDots, lightDots;
    std::map<std::pair<long long, long>, std::pair<float, float>> lightPos;
    double clockOff = 0;
    double sumDAz = 0, sumDEl = 0, maxDAz = 0, maxDEl = 0; int nEph = 0;
    Receiver::Cfg cfg; cfg.maxChans = 4;
    Receiver rx(cfg);
    rx.onPacket = [&](const Rx& r) {
        if (!r.p.ok) return;
        if (r.p.type() == kEphemeris) {
            Ephemeris e; decodeEphemeris(r.p, e, r.t + clockOff);
            if (e.unix > 0 && std::fabs(e.unix - (r.t + clockOff)) < 1800) clockOff = e.unix - r.t;
        }
        Rx rc = r; rc.t = r.t + clockOff;
        const Ident::Answer a = id.onPacket(rc, rc.t);
        if (!a.norad || !a.sky) return;
        const long bk = long(std::floor(rc.t / vibe::skyplot::kBucketSec));
        const int snr = int(std::lround(r.snrDb));
        auto it = fullDots.find({ a.norad, bk });
        if (it == fullDots.end()) fullDots[{ a.norad, bk }] = snr; else it->second = std::max(it->second, snr);
        // ★ the dot's place (SGP4) vs the satellite's own position report
        if (r.p.type() == kEphemeris && !r.p.repaired) {
            Ephemeris e; decodeEphemeris(r.p, e, rc.t);
            const double sx = e.x / 1000, sy = e.y / 1000, sz = e.z / 1000;
            const double rx_ = sx - obs.ecef[0], ry = sy - obs.ecef[1], rz = sz - obs.ecef[2];
            const double south = obs.sinLat * obs.cosLon * rx_ + obs.sinLat * obs.sinLon * ry - obs.cosLat * rz;
            const double east = -obs.sinLon * rx_ + obs.cosLon * ry;
            const double up = obs.cosLat * obs.cosLon * rx_ + obs.cosLat * obs.sinLon * ry + obs.sinLat * rz;
            const double rng = std::sqrt(rx_ * rx_ + ry * ry + rz * rz);
            const double el = std::asin(up / rng) * 180 / M_PI;
            double az = std::atan2(east, -south) * 180 / M_PI; if (az < 0) az += 360;
            double dAz = std::fabs(az - a.azDeg); if (dAz > 180) dAz = 360 - dAz;
            dAz *= std::cos(el * M_PI / 180);   // ★ on the sky, not in azimuth degrees (near the zenith those shrink)
            const double dEl = std::fabs(el - a.elDeg);
            sumDAz += dAz; sumDEl += dEl; maxDAz = std::max(maxDAz, dAz); maxDEl = std::max(maxDEl, dEl); nEph++;
        }
    };
    SkyWatch::Opts so;
    so.birds = [&] { return sky; };
    so.observer = [&](vibe::sat::Observer& o) { o = obs; return true; };
    so.dots = [&](const std::vector<vibe::skyplot::Dot>& d) {
        for (auto& x : d) { const long bk = long(std::floor(double(x.t) / vibe::skyplot::kBucketSec)); lightDots[{ x.norad, bk }] = x.snr; lightPos[{ x.norad, bk }] = { x.azDeg, x.elDeg }; }
    };
    so.log = [](const std::string& l) { std::printf("  %s\n", l.c_str()); };
    SkyWatch sw(so);
    const int N = SkyWatch::fftFor(rate);
    const uint64_t every = uint64_t(rate / SkyWatch::kFramesPerSec);
    std::vector<std::complex<float>> frame; frame.reserve(size_t(N));

    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) { std::perror(file.c_str()); return 1; }
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RTL0", 4) != 0) std::fseek(f, 0, SEEK_SET);
    const int B = 16384;
    std::vector<uint8_t> buf(size_t(B) * 2);
    std::vector<std::complex<float>> x(B);
    uint64_t n = 0, nextFrame = 0;
    double fullCpu = 0, lightCpu = 0;
    using clk = std::chrono::steady_clock;
    while (true) {
        const size_t got = std::fread(buf.data(), 1, buf.size(), f) / 2;
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) x[i] = { (float(buf[2 * i]) - 127.5f) / 128.f, (float(buf[2 * i + 1]) - 127.5f) / 128.f };
        auto a0 = clk::now();
        rx.feed(x.data(), int(got), rate, centre, t0 + double(n) / rate);
        auto a1 = clk::now();
        for (size_t i = 0; i < got; i++) {
            const uint64_t s = n + i;
            if (s < nextFrame) continue;
            frame.push_back(x[i]);
            if (int(frame.size()) == N) {
                sw.frame(frame.data(), N, rate, centre, t0 + double(s) / rate + clockOff);
                frame.clear(); nextFrame = s + std::max<uint64_t>(1, every - uint64_t(N));
            }
        }
        auto a2 = clk::now();
        fullCpu += std::chrono::duration<double>(a1 - a0).count();
        lightCpu += std::chrono::duration<double>(a2 - a1).count();
        n += got;
        if (double(n) / rate > maxSec) break;
        if (getenv("CANDS") && n % (uint64_t(rate) * 20) < uint64_t(B)) for (auto& c : sw.candidates()) std::printf("  [%.0f] %s\n", double(n) / rate, c.c_str());
    }
    sw.flush();
    for (auto& c : sw.candidates()) std::printf("  candidate %s\n", c.c_str());
    std::fclose(f);
    const double secs = double(n) / rate;
    std::printf("\n%.1f s of IQ at %.0f kS/s; clock offset from ephemeris %.1f s\n", secs, rate / 1000, clockOff);
    std::printf("CPU: full decoder %.2f s (%.1f %% of real time) · light sky plot %.3f s (%.2f %% of real time) — %.0f× less\n",
                fullCpu, 100 * fullCpu / secs, lightCpu, 100 * lightCpu / secs, lightCpu > 0 ? fullCpu / lightCpu : 0);
    std::set<long long> all;
    for (auto& kv : fullDots) all.insert(kv.first.first);
    for (auto& kv : lightDots) all.insert(kv.first.first);
    double sumD = 0, sumD2 = 0; int nD = 0, wrong = 0;
    for (long long b : all) {
        int nf = 0, nl = 0, both = 0, lonly = 0, fonly = 0;
        for (auto& kv : fullDots) if (kv.first.first == b) { nf++; if (lightDots.count(kv.first)) both++; else fonly++; }
        for (auto& kv : lightDots) if (kv.first.first == b) {
            nl++;
            if (!fullDots.count(kv.first)) {
                lonly++;
                // a light dot where the full decoder heard ANOTHER bird in this bucket and never this one nearby = wrong bird
                bool fullHeardThisBird = false;
                for (long d = -6; d <= 6; d++) if (fullDots.count({ b, kv.first.second + d })) fullHeardThisBird = true;
                if (!fullHeardThisBird) wrong++;
            }
        }
        for (auto& kv : fullDots) if (kv.first.first == b && lightDots.count(kv.first)) { const double d = lightDots[kv.first] - kv.second; sumD += d; sumD2 += d * d; nD++; }
        std::printf("%-22s full %4d dots · light %4d · both %4d · light only %3d · full only %3d\n", names[b].c_str(), nf, nl, both, lonly, fonly);
        std::string lo, fo;
        for (auto& kv : lightDots) if (kv.first.first == b && !fullDots.count(kv.first)) { char q[64]; std::snprintf(q, sizeof q, " %ld(%ddB,el%.0f)", long(kv.first.second * vibe::skyplot::kBucketSec - t0), kv.second, lightPos[kv.first].second); lo += q; }
        for (auto& kv : fullDots) if (kv.first.first == b && !lightDots.count(kv.first)) fo += " " + std::to_string(long(kv.first.second * vibe::skyplot::kBucketSec - t0));
        std::printf("   light only at s:%s\n   full only at s:%s\n", lo.c_str(), fo.c_str());
    }
    const double m = nD ? sumD / nD : 0, sd = nD ? std::sqrt(std::max(0.0, sumD2 / nD - m * m)) : 0;
    { int mn = 999, mx = -999; for (auto& kv : fullDots) { mn = std::min(mn, kv.second); mx = std::max(mx, kv.second); }
      int ln = 999, lx = -999; for (auto& kv : lightDots) { ln = std::min(ln, kv.second); lx = std::max(lx, kv.second); }
      std::printf("dot SNR range: full %d .. %d dB · light %d .. %d dB\n", mn, mx, ln, lx); }
    std::printf("SNR light − full over %d shared buckets: mean %+.1f dB, sd %.1f dB\n", nD, m, sd);
    std::printf("light dots with no full-decoder dot for that bird within ±30 s (wrong-bird candidates): %d\n", wrong);
    if (nEph) std::printf("dot place (SGP4) vs the bird's own position report, %d reports: on the sky mean %.2f° (max %.2f°) across, elevation mean %.2f° (max %.2f°)\n",
                          nEph, sumDAz / nEph, maxDAz, sumDEl / nEph, maxDEl);
    return 0;
}
