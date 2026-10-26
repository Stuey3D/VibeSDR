// wspr_bench — VibeSDR's WSPR decoder (decoders/wspr_decoder.cpp) measured: on a recording, slot by slot, against a
// reference decoder's spots for the same receiver and slots; on synthetic slots across SNR; on pure noise.
//
//   wspr_bench wav FILE.wav START_UTC DIAL_HZ [--ref SPOTS.jsonl] [--passes N] [--cycles N] [--cands N]
//       START_UTC  UTC (unix seconds) of the file's first sample (tools/ubersdr-rec prints it as start_utc=)
//       --ref      the UberSDR's own WSPR spots (one JSON object per line, as /ws/dxcluster sends digital_spot data)
//   wspr_bench sweep [--from -26] [--to -32] [--step 0.5] [--trials 40] [--passes N] [--cycles N]
//       synthetic slots (tools/wspr-bench/wspr_synth.h): one signal, random-ish frequency, DT, drift; decode rate per SNR
//   wspr_bench noise [--slots 50]                  pure noise: every decode is a false one
//   wspr_bench crowd [--signals 25] [--slots 4]    a busy band: many signals, overlapping, −10 … −29 dB; pass 1 vs 2
//
//   Output lines: "D slot pass snr dt freqHz drift message", "S slot ms candidates decodes", "R ..." for the
//   reference comparison. Build: tools/wspr-bench/build.sh (-O2, like the server).
#include "decoders/wspr_decoder.h"
#include "wspr_synth.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vibe;

static bool readWav(const char* path, std::vector<float>& out, int& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::fclose(f); return false; }
    int ch = 0, bits = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) { std::fclose(f); return false; }
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len);
            if (std::fread(b.data(), 1, len, f) != len) { std::fclose(f); return false; }
            ch = b[2] | b[3] << 8; rate = (int)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24); bits = b[14] | b[15] << 8;
        } else if (!std::memcmp(id, "data", 4)) {
            if (bits != 16 || ch < 1) { std::fclose(f); return false; }
            std::vector<int16_t> s(len / 2);
            const size_t got = std::fread(s.data(), 2, s.size(), f);
            out.resize(got / (size_t)ch);
            for (size_t i = 0; i < out.size(); ++i) out[i] = s[i * (size_t)ch] / 32768.f;
            std::fclose(f);
            return true;
        } else std::fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
}

static const char* arg(int argc, char** argv, const char* name, const char* def) {
    for (int i = 0; i + 1 < argc; ++i) if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return def;
}

// a minimal reader for the fields we compare (the spots are flat JSON objects)
static std::string jstr(const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = l.find(k); if (p == std::string::npos) return "";
    p += k.size();
    if (l[p] == '"') { const size_t e = l.find('"', p + 1); return l.substr(p + 1, e - p - 1); }
    size_t e = p; while (e < l.size() && l[e] != ',' && l[e] != '}') ++e;
    return l.substr(p, e - p);
}
static double isoToUtc(const std::string& s) {   // 2026-10-26T07:30:00Z
    struct tm t = {};
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
    t.tm_year -= 1900; t.tm_mon -= 1;
    return (double)timegm(&t);
}

static WsprSlotOptions opts(int argc, char** argv) {
    WsprSlotOptions o;
    o.maxPasses = std::atoi(arg(argc, argv, "--passes", "3"));
    o.fanoCyclesPerBit = std::atol(arg(argc, argv, "--cycles", "5000"));
    o.maxCandidates = std::atoi(arg(argc, argv, "--cands", "60"));
    o.llrScale = (float)std::atof(arg(argc, argv, "--llr", "0.9"));
    o.fanoDelta = (float)std::atof(arg(argc, argv, "--delta", "2"));
    return o;
}

static int cmdWav(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "wspr_bench wav FILE START_UTC DIAL_HZ [--ref spots.jsonl]\n"); return 2; }
    std::vector<float> a; int rate = 0;
    if (!readWav(argv[2], a, rate)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    const double t0 = std::atof(argv[3]), dial = std::atof(argv[4]);
    const WsprSlotOptions o = opts(argc, argv);
    const char* ref = arg(argc, argv, "--ref", nullptr);
    struct RefSpot { std::string msg, call; int snr; double freq, dt; bool seen = false; };
    std::map<long, std::vector<RefSpot>> refs;
    if (ref) {
        std::ifstream f(ref); std::string l;
        while (std::getline(f, l)) {
            if (std::fabs(std::atof(jstr(l, "frequency").c_str()) - dial) > 1) continue;
            RefSpot r;
            r.msg = jstr(l, "message"); r.call = jstr(l, "callsign");
            r.snr = std::atoi(jstr(l, "snr").c_str()); r.freq = std::atof(jstr(l, "tx_frequency").c_str()); r.dt = std::atof(jstr(l, "dt").c_str());
            // collapse the reference's double spaces ("G0EIY/C  23")
            std::string m; for (char c : r.msg) if (!(c == ' ' && !m.empty() && m.back() == ' ')) m += c;
            r.msg = m;
            refs[(long)isoToUtc(jstr(l, "timestamp"))].push_back(r);
        }
    }
    const double first = std::ceil((t0 + wspr::kLeadSec) / wspr::kSlotSec) * wspr::kSlotSec;
    int both = 0, oursOnly = 0, refOnly = 0, slots = 0;
    double snrDiff = 0, snrAbs = 0, fDiff = 0, totalMs = 0;
    std::vector<double> snrDiffs;
    WsprSlotDecoder dec;
    for (double slot = first;; slot += wspr::kSlotSec) {
        const long i0 = std::lround((slot - wspr::kLeadSec - t0) * rate);
        const long n = (long)(wspr::kSlotSec * rate);
        if (i0 < 0) continue;
        if (i0 + (long)((wspr::kLeadSec + 1 + wspr::kTxSec + 0.5) * rate) > (long)a.size()) break;   // the transmissions must be inside
        const long nn = std::min<long>(n, (long)a.size() - i0);
        std::vector<WsprSpot> out; WsprSlotStats st;
        const auto c0 = std::chrono::steady_clock::now();
        dec.decodeAudio(a.data() + i0, (int)nn, rate, dial, slot, o, out, st);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        totalMs += ms; ++slots;
        time_t ts = (time_t)slot; char hm[16]; std::strftime(hm, sizeof hm, "%H%M", gmtime(&ts));
        std::sort(out.begin(), out.end(), [](const WsprSpot& x, const WsprSpot& y) { return x.snr > y.snr; });
        auto& rl = refs[(long)slot];
        for (const WsprSpot& s : out) {
            const RefSpot* hit = nullptr;
            // the reference prints a type 3 as "CALL GRID6 dBm"; ours is "<CALL> GRID6 dBm"
            std::string m = s.message; m.erase(std::remove(m.begin(), m.end(), '<'), m.end()); m.erase(std::remove(m.begin(), m.end(), '>'), m.end());
            for (auto& r : rl) if (r.msg == m) { r.seen = true; hit = &r; }
            std::printf("D %s %d %4d %5.1f %11.1f %5.2f %-22s %s", hm, s.pass, s.snr, s.dt, s.freqHz, s.drift, s.message.c_str(),
                        ref ? (hit ? "both" : "OURS ONLY") : "");
            if (hit) { std::printf("  ref snr %d dt %.1f Δf %+.1f", hit->snr, hit->dt, s.freqHz - hit->freq); snrDiffs.push_back(s.snr - hit->snr);
                       snrDiff += s.snr - hit->snr; snrAbs += std::fabs(s.snr - hit->snr); fDiff += std::fabs(s.freqHz - hit->freq); ++both; }
            else if (ref) ++oursOnly;
            std::printf("\n");
        }
        if (ref) for (auto& r : rl) if (!r.seen) { std::printf("D %s ref  %4d %5.1f %11.1f       %-22s REF ONLY\n", hm, r.snr, r.dt, r.freq, r.msg.c_str()); ++refOnly; }
        std::printf("S %s %.0f ms  passes %d  cands %d/%d  decodes %d/%d  fano %.0f ms (%ld cycles, %d fails)  search %.1f refine %.1f sub %.1f dec %.1f\n",
                    hm, ms, st.passes, st.candidates[0], st.candidates[1], st.decodes[0], st.decodes[1], st.fanoMs, st.fanoCycles, st.fanoFails,
                    st.searchMs, st.refineMs, st.subtractMs, st.decimateMs);
    }
    if (ref) std::printf("R slots %d  both %d  ours-only %d  ref-only %d  (ours %d, ref %d)  snr ours−ref mean %+.1f |%.1f| dB  |Δf| %.2f Hz\n",
                         slots, both, oursOnly, refOnly, both + oursOnly, both + refOnly, both ? snrDiff / both : 0, both ? snrAbs / both : 0, both ? fDiff / both : 0);
    std::printf("CPU %.0f ms per slot (mean of %d)\n", slots ? totalMs / slots : 0, slots);
    return 0;
}

static std::vector<float> slotBuf() { return std::vector<float>((size_t)(12000 * wspr::kSlotSec), 0.f); }

static int cmdSweep(int argc, char** argv) {
    const double from = std::atof(arg(argc, argv, "--from", "-26")), to = std::atof(arg(argc, argv, "--to", "-32"));
    const double step = std::atof(arg(argc, argv, "--step", "0.5"));
    const int trials = std::atoi(arg(argc, argv, "--trials", "40"));
    const WsprSlotOptions o = opts(argc, argv);
    WsprSlotDecoder dec;
    for (double snr = from; snr >= to - 1e-9; snr -= step) {
        int ok = 0, falses = 0; double ms = 0, snrErr = 0; float minSync = 1;
        for (int t = 0; t < trials; ++t) {
            std::vector<float> a = slotBuf();
            wsprsynth::addNoise(a, 1000 + (uint64_t)t);
            wsprsynth::Sig s; s.msg = wsprsynth::msg1("G4ABC", "IO92", 37); s.snr = snr;
            s.centreHz = 1410 + std::fmod(7.3 * t, 180.0); s.dt = -1.0 + std::fmod(0.37 * t, 4.0); s.drift = (t % 9) * 0.5 - 2.0;
            wsprsynth::addSignal(a, 12000, s, 1.0, 500 + (uint64_t)t);
            wspr::callHashes().clear();
            std::vector<WsprSpot> out; WsprSlotStats st;
            const auto c0 = std::chrono::steady_clock::now();
            dec.decodeAudio(a.data(), (int)a.size(), 12000, 14095600, 0, o, out, st);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            for (auto& x : out) { if (x.message == "G4ABC IO92 37") { ++ok; snrErr += x.snr - snr; minSync = std::min(minSync, x.sync); } else ++falses; }
        }
        std::printf("SNR %6.1f dB: %3d/%d decoded (%3.0f %%), false %d, reported SNR error %+.1f dB, %.0f ms/slot, weakest sync %.3f\n",
                    snr, ok, trials, 100.0 * ok / trials, falses, ok ? snrErr / ok : 0, ms / trials, minSync);
    }
    return 0;
}

static int cmdNoise(int argc, char** argv) {
    const int slots = std::atoi(arg(argc, argv, "--slots", "50"));
    const WsprSlotOptions o = opts(argc, argv);
    WsprSlotDecoder dec;
    int falses = 0; double ms = 0; long cands = 0;
    for (int t = 0; t < slots; ++t) {
        std::vector<float> a = slotBuf();
        wsprsynth::addNoise(a, 90000 + (uint64_t)t);
        std::vector<WsprSpot> out; WsprSlotStats st;
        const auto c0 = std::chrono::steady_clock::now();
        dec.decodeAudio(a.data(), (int)a.size(), 12000, 14095600, 0, o, out, st);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        cands += st.candidates[0];
        for (auto& x : out) { ++falses; std::printf("FALSE slot %d: %s snr %d\n", t, x.message.c_str(), x.snr); }
    }
    std::printf("noise: %d slots, %d false decodes, %.1f candidates/slot, %.0f ms/slot\n", slots, falses, (double)cands / slots, ms / slots);
    return 0;
}

static int cmdCrowd(int argc, char** argv) {
    const int nsig = std::atoi(arg(argc, argv, "--signals", "25")), slots = std::atoi(arg(argc, argv, "--slots", "4"));
    WsprSlotOptions o = opts(argc, argv);
    WsprSlotDecoder dec;
    static const char* calls[] = {"G4ABC","DL1XYZ","F5AAA","EA3BBB","OH2CC","SM5DDD","K1ABC","W9EEE","VK2FF","JA1GGG","PA3HHH","ON4III",
        "OK1JJJ","SP9KKK","HA5LLL","YO3MMM","LZ2NNN","9A1OO","S51PP","OE3QQ","HB9RRR","LA9SS","OZ1TTT","EI2UU","GM4VVV","GW3WW","2E0XYZ","M0ZZZ","VE3AB","ZL1CD"};
    static const char* grids[] = {"IO92","JO62","JN18","JN11","KP20","JO89","FN42","EN52","QF56","PM95","JO22","JO10","JO70","KN09","JN97",
        "KN34","KN22","JN75","JN76","JN78","JN47","JP99","JO55","IO63","IO85","IO81","IO91","IO93","FN03","RF73"};
    int tot1 = 0, tot2 = 0, totSent = 0, falses = 0;
    for (int t = 0; t < slots; ++t) {
        std::vector<float> a = slotBuf();
        wsprsynth::addNoise(a, 7000 + (uint64_t)t);
        std::set<std::string> sent;
        wsprsynth::Rng r(300 + (uint64_t)t);
        for (int i = 0; i < nsig && i < 30; ++i) {
            wsprsynth::Sig s; s.msg = wsprsynth::msg1(calls[i], grids[i], 23 + (i % 3) * 7 - (i % 3 == 2 ? 1 : 0));
            if (s.msg.dbm == 36) s.msg.dbm = 37;
            s.snr = -10 - r.uni() * 19; s.centreHz = 1405 + r.uni() * 190; s.dt = r.uni() * 2 - 0.5; s.drift = r.uni() * 2 - 1;
            wsprsynth::addSignal(a, 12000, s, 1.0, 900 + (uint64_t)(t * 100 + i));
            sent.insert(s.msg.text());
        }
        totSent += (int)sent.size();
        std::vector<WsprSpot> out; WsprSlotStats st;
        std::vector<float> b = a;
        dec.decodeAudio(b.data(), (int)b.size(), 12000, 14095600, 0, o, out, st);
        int p1 = 0, p2 = 0;
        for (auto& x : out) { if (!sent.count(x.message)) { ++falses; std::printf("FALSE %s\n", x.message.c_str()); } else if (x.pass == 1) ++p1; else ++p2; }
        tot1 += p1; tot2 += p2;
        std::printf("crowd slot %d: sent %zu, pass 1 %d, pass 2 +%d, %.0f ms\n", t, sent.size(), p1, p2, st.passMs[0] + st.passMs[1] + st.decimateMs);
    }
    std::printf("crowd: sent %d, decoded %d (pass 2 added %d), false %d\n", totSent, tot1 + tot2, tot2, falses);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "wspr_bench wav|sweep|noise|crowd ...\n"); return 2; }
    if (!std::strcmp(argv[1], "wav")) return cmdWav(argc, argv);
    if (!std::strcmp(argv[1], "sweep")) return cmdSweep(argc, argv);
    if (!std::strcmp(argv[1], "noise")) return cmdNoise(argc, argv);
    if (!std::strcmp(argv[1], "crowd")) return cmdCrowd(argc, argv);
    std::fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
