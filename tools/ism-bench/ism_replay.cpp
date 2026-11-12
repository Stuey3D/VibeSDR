// ism_replay.cpp — the ISM decoder (vibe_ism*.h) on RECORDED captures: each file through the real front end
// (detector → channel → slicer) and Service::decodeBurst (every family, the band rule), one JSON line per reading.
// Scored against the recordings' own expected output by scripts/ism-corpus.mjs (the rtl_433_tests corpus — used here as
// test INPUT only, never shipped or committed; our decoders are our own code).
//
//   ism_replay [--dc HZ] FILE.cu8 …        8-bit unsigned I/Q; the rate and centre from the name: …_433.92M_250k.cu8
//
// ★ --dc: the detector's DC guard. Live, the service tunes OFF the band so a sensor never sits on the dongle's DC spike
//   and guards ±15 kHz; these recordings were made tuned ON the sensor (it sits within a few kHz of DC), so the replay
//   guards only ±1 kHz by default — the score is the decoders', not an artefact of how the corpus was recorded.
#include "vibe_ism_service.h"
#include <cmath>
#include <cstdio>
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
    double dc = 1000;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dc" && i + 1 < argc) dc = std::atof(argv[++i]);
        else files.push_back(a);
    }
    const std::regex nameRx(R"(_([0-9.]+)M_([0-9]+)k)");
    for (const auto& path : files) {
        double rate = 250e3, centre = 433.92e6;
        std::smatch m;
        if (std::regex_search(path, m, nameRx)) { centre = std::stod(m[1]) * 1e6; rate = std::stod(m[2]) * 1e3; }
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::printf("{\"file\":%s,\"error\":\"open\"}\n", jstr(path).c_str()); continue; }
        std::vector<uint8_t> raw;
        { uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + n); }
        std::fclose(f);
        std::vector<cf> x(raw.size() / 2);
        for (size_t i = 0; i < x.size(); ++i) x[i] = cf((raw[2 * i] - 127.5f) / 128.f, (raw[2 * i + 1] - 127.5f) / 128.f);
        // ★ the rtl_433 recordings are short; pad with a quarter second of silence so the last burst closes
        x.resize(x.size() + (size_t)(rate * 0.25), cf(0, 0));
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
                num(o, "t", b.tSec);
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
