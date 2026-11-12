// ism_replay.cpp — the ISM decoder (vibe_ism*.h) on RECORDED captures: each file through the real front end
// (detector → channel → slicer) and Service::decodeBurst (every family, the band rule), one JSON line per reading.
// Scored against the recordings' own expected output by scripts/ism-corpus.mjs (the rtl_433_tests corpus — used here as
// test INPUT only, never shipped or committed; our decoders are our own code).
//
//   ism_replay [--dc HZ] [--lead S] FILE.cu8 …   8-bit unsigned I/Q; the rate and centre from the name: …_433.92M_250k.cu8
//                                          (or …_2500k.cu8, or a `samplerate` file beside it; else 250 k at 433.92;
//                                           .cs8 = 8-bit signed, .cs16 = 16-bit signed little-endian I/Q, by extension)
//
// ★ --dc: the detector's DC guard. Live, the service tunes OFF the band so a sensor never sits on the dongle's DC spike
//   and guards ±15 kHz; these recordings were made tuned ON the sensor (it sits within a few kHz of DC), so the replay
//   guards only ±1 kHz by default — the score is the decoders', not an artefact of how the corpus was recorded.
// ★ --lead S (off by default, so the corpus score stays comparable; 2026-11-11): live, the detector has watched the band's noise for minutes before a sensor
//   sends; these recordings are cut by a trigger, so most start with the sensor already on the air — and the detector's
//   first frames SEED its noise floor (vibe_ism.h), so a burst present from the first sample was learnt as "floor" and
//   never detected (the XC-0324 folder: 0 bursts found in 18 of 18 recordings). The replay therefore starts with this
//   much of the recording's own quietest 10 ms (its real noise), as the live band would have. With --lead 0.3 the whole
//   corpus read +37 matches, but bursts elsewhere moved (a stretch with a steady spur, repeated, is held as a hog) — so
//   it is a diagnostic for decoders whose recordings start mid-burst, not the score.
#include "vibe_ism_service.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

using namespace vibeism;

static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    return o + "\"";
}
static void num(std::string& o, const char* k, double v) {
    if (std::isnan(v)) return;
    char b[64]; std::snprintf(b, sizeof b, ",\"%s\":%.3f", k, v); o += b;
}

int main(int argc, char** argv) {
    double dc = 1000, lead = 0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dc" && i + 1 < argc) dc = std::atof(argv[++i]);
        else if (a == "--lead" && i + 1 < argc) lead = std::atof(argv[++i]);
        else files.push_back(a);
    }
    /* ★ the rate and the centre are read SEPARATELY (2026-11-11): 143 corpus recordings carry only a rate
     *  ("g003_1000k.cu8", "…_2000k.cu8", "….250k.cu8") or a fractional one ("_2359.3k"), and were replayed at 250 k —
     *  every emonTx, IDM and SCM+ recording was sliced at a tenth of its real speed. With no rate in the name, a
     *  `samplerate` file beside it; a name that says "_0M_" (m-bus/02) takes the folder README's `-f`. */
    const std::regex centreRx(R"(_([0-9.]+)M[_.])"), rateRx(R"([_.]([0-9.]+)k[_.])");
    for (const auto& path : files) {
        double rate = 250e3, centre = 433.92e6;
        bool haveRate = false;
        std::smatch m;
        const std::string base = path.substr(path.find_last_of('/') + 1), dir = path.substr(0, path.find_last_of('/') + 1);
        if (std::regex_search(base, m, centreRx)) centre = std::stod(m[1]) * 1e6;
        if (std::regex_search(base, m, rateRx)) { rate = std::stod(m[1]) * 1e3; haveRate = true; }
        if (centre < 1e6) {
            centre = 433.92e6;
            if (FILE* rd = std::fopen((dir + "README.md").c_str(), "r")) {
                std::string t; char buf[4096]; size_t k; while ((k = std::fread(buf, 1, sizeof buf, rd)) > 0) t.append(buf, k);
                std::fclose(rd);
                std::smatch fm;
                if (std::regex_search(t, fm, std::regex(R"(-f\s+([0-9]{8,10})\b)"))) centre = std::stod(fm[1]);
            }
        }
        if (!haveRate) if (FILE* sr = std::fopen((dir + "samplerate").c_str(), "r")) {
            double v = 0; if (std::fscanf(sr, "%lf", &v) == 1 && v > 1e4) rate = v;
            std::fclose(sr);
        }
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::printf("{\"file\":%s,\"error\":\"open\"}\n", jstr(path).c_str()); continue; }
        std::vector<uint8_t> raw;
        { uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + n); }
        std::fclose(f);
        std::vector<cf> x;
        const bool cs16 = path.size() > 5 && path.compare(path.size() - 5, 5, ".cs16") == 0;
        const bool cs8 = !cs16 && path.size() > 4 && path.compare(path.size() - 4, 4, ".cs8") == 0;
        if (cs16) {
            x.resize(raw.size() / 4);
            for (size_t i = 0; i < x.size(); ++i) {
                const int16_t re = int16_t(raw[4 * i] | raw[4 * i + 1] << 8), im = int16_t(raw[4 * i + 2] | raw[4 * i + 3] << 8);
                x[i] = cf(re / 32768.f, im / 32768.f);
            }
        } else {
            x.resize(raw.size() / 2);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = cs8 ? cf(int8_t(raw[2 * i]) / 128.f, int8_t(raw[2 * i + 1]) / 128.f)
                           : cf((raw[2 * i] - 127.5f) / 128.f, (raw[2 * i + 1] - 127.5f) / 128.f);
        }
        /* ★ the rtl_433 recordings are short; a quarter second more so the last burst closes. ★★ 2026-11-12: the
         *  recording's own quietest 10 ms (played forwards and backwards in turn, as --lead does), NOT digital silence:
         *  a burst that ran to the end of a recording took exact zeros into its channel, its 10th-percentile "noise"
         *  read −180 dB and the OOK slice was one long mark (ELV WS 2000, Fine Offset WH2…). A live capture is never
         *  silent; the replay must not be either. */
        {
            const size_t n0 = x.size(), w = size_t(rate * 0.01), padN = size_t(rate * 0.25);
            size_t best = 0; double bestP = 1e300;
            if (n0 >= 2 * w) for (size_t s0 = 0; s0 + w <= n0; s0 += w / 2) {
                double p = 0; for (size_t i = s0; i < s0 + w; ++i) p += std::norm(x[i]);
                if (p < bestP) { bestP = p; best = s0; }
            }
            if (n0 >= 2 * w) {
                x.reserve(n0 + padN + w);
                for (int k = 0; x.size() < n0 + padN; ++k)
                    for (size_t i = 0; i < w && x.size() < n0 + padN; ++i) x.push_back(x[best + ((k & 1) ? w - 1 - i : i)]);
            } else x.resize(n0 + padN, cf(0, 0));
        }
        // ★ --lead: the recording's own quietest stretch first (see the header) — its real noise, spectrum and all,
        //   played forwards and backwards in turn so the joins are continuous
        if (lead > 0 && raw.size() / 2 > size_t(rate * 0.02)) {
            const size_t n0 = raw.size() / 2, w = size_t(rate * 0.01);
            size_t best = 0; double bestP = 1e300;
            for (size_t s0 = 0; s0 + w <= n0; s0 += w / 2) {
                double p = 0; for (size_t i = s0; i < s0 + w; ++i) p += std::norm(x[i]);
                if (p < bestP) { bestP = p; best = s0; }
            }
            std::vector<cf> pre; pre.reserve(size_t(rate * lead) + w);
            for (int k = 0; pre.size() < size_t(rate * lead); ++k)
                for (size_t i = 0; i < w; ++i) pre.push_back(x[best + ((k & 1) ? w - 1 - i : i)]);
            x.insert(x.begin(), pre.begin(), pre.end());
        }
        FrontEnd fe(rate);
        fe.setDcGuardHz(dc);
        const bool m868 = centre >= 863e6 && centre <= 870e6;
        if (m868 && rate >= 1e6) { fe.setChanRate(500e3); fe.setKeepBaseband(true); }
        int bursts = 0, n = 0;
        auto on = [&](const Burst& b) {
            ++bursts;
            std::vector<Reading> got;
            Service::decodeBurst(b, centre, got);
            for (auto& r : got) {
                ++n;
                std::string o = "{\"file\":" + jstr(path) + ",\"kind\":" + jstr(r.kind) + ",\"make\":" + jstr(r.make) +
                                ",\"model\":" + jstr(r.model) + ",\"id\":" + jstr(r.pubId);
                if (r.channel >= 0) o += ",\"channel\":" + std::to_string(r.channel);
                if (r.secret) {
                    o += ",\"secret\":true";
                    /* ★ BENCH ONLY: the secret ID's low 40 bits, so the scorer can tell "the same sensor in a second
                     *  format" from a misread (ism-corpus.mjs, sameSensor). Public test recordings only — the service
                     *  never puts a secret ID in any output (vibe_ism_service.h, Tracker::accept). */
                    char sid[24]; std::snprintf(sid, sizeof sid, "%llx", (unsigned long long)(r.secretId & 0xFFFFFFFFFFull));
                    o += ",\"sid\":\"" + std::string(sid) + "\"";
                }
                num(o, "temperature_C", r.tempC); num(o, "humidity", r.hum); num(o, "pressure_kPa", r.kpa);
                num(o, "wind_avg_km_h", r.windKmh); num(o, "wind_max_km_h", r.gustKmh); num(o, "wind_dir_deg", r.windDir);
                num(o, "rain_mm", r.rainMm); num(o, "uv", r.uv); num(o, "power_W", r.watts); num(o, "pressure_hPa", r.baroHpa);
                if (r.battOk >= 0) o += ",\"battery_ok\":" + std::to_string(r.battOk);
                // ★ the frame's own values for the scorer (Reading::facts) — never an identity of a secret kind
                if (!r.facts.empty()) {
                    std::string f;
                    for (const auto& kv : r.facts) {
                        if (r.secret && kv.first == "id") continue;
                        f += (f.empty() ? "" : ",") + jstr(kv.first) + ":" + kv.second;
                    }
                    if (!f.empty()) o += ",\"facts\":{" + f + "}";
                }
                num(o, "t", b.tSec - lead);
                o += "}";
                std::puts(o.c_str());
            }
        };
        for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
        fe.flush(on);
        std::printf("{\"file\":%s,\"done\":true,\"bursts\":%d,\"readings\":%d,\"rate\":%.0f,\"centre\":%.0f}\n",
                    jstr(path).c_str(), bursts, n, rate, centre);
    }
    return 0;
}
