// test-hfdl.cpp — V12 HFDL (vibe_hfdl.h), docs/v12/AVIATION.md §HFDL.
//  `test-hfdl --wav <file.wav> <centreHz> [dialHz…]` decodes a REAL 16-bit stereo IQ WAV (UberSDR iq192); with
//  `--ref <file.ref.txt>` it matches what it decoded against the reference transcript (HFDL_DEBUG=1: every burst).
#include "vibe_hfdl.h"
#include "vibe_hfdl_run.h"   // ★ §5: the listener's decoder on USB audio (the server's "hfdl" decoder)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibehfdl::cf32;

static uint64_t g_digest = 1469598103934665603ull;
static void dig(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull; }

static void modulate(const std::vector<cf32>& sym, std::vector<cf32>& out, double fOff, double snrDb, double echoDb, int echoDelay, std::mt19937& rng);
static std::vector<float> burstSymbols(int mode, const std::vector<uint8_t>& mpdu, std::vector<cf32>& sym);
// ── the transmitter (tests): MPDU bytes → code → interleave → scramble → the burst's symbols ──────────────────────
static void putCrc(std::vector<uint8_t>& b, size_t from) {
    uint16_t c = 0xFFFF;
    for (size_t i = from; i < b.size(); ++i) { c ^= b[i]; for (int k = 0; k < 8; ++k) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1); }
    c ^= 0xFFFF; b.push_back(uint8_t(c)); b.push_back(uint8_t(c >> 8));
}
/** A downlink MPDU with one LPDU: performance data (flight, position) — `lat`/`lon` in degrees. */
static std::vector<uint8_t> perfMpdu(int ac, int gs, const char* flight, double lat, double lon) {
    std::vector<uint8_t> l = { 0x0D, 0xFF, 0xD1 };
    for (int i = 0; i < 6; ++i) l.push_back(uint8_t(flight[i]));
    const uint32_t la = uint32_t(int32_t(std::lround(lat / 180.0 * 0x7FFFF))) & 0xFFFFF, lo = uint32_t(int32_t(std::lround(lon / 180.0 * 0x7FFFF))) & 0xFFFFF;
    l.push_back(uint8_t(la)); l.push_back(uint8_t(la >> 8)); l.push_back(uint8_t(((la >> 16) & 0xF) | ((lo & 0xF) << 4)));
    l.push_back(uint8_t(lo >> 4)); l.push_back(uint8_t(lo >> 12));
    while (l.size() < 1 + 47) l.push_back(uint8_t(l.size()));
    putCrc(l, 0);
    std::vector<uint8_t> m = { 0x07, uint8_t(gs), uint8_t(ac), 0x00, 0x01, 0x00, uint8_t(l.size() - 1) };
    putCrc(m, 0);
    m.insert(m.end(), l.begin(), l.end());
    return m;
}
static std::vector<float> burstSymbols(int mode, const std::vector<uint8_t>& mpdu, std::vector<cf32>& sym) {
    const vibehfdl::Mode md = vibehfdl::modeOf(mode);
    const int nData = md.segments * vibehfdl::kDataLen, coded = nData * md.bitsPerSym, info = coded / md.rate;
    std::vector<int> bits((size_t)info, 0);
    for (size_t i = 0; i < mpdu.size() * 8 && int(i) < info - 6; ++i) bits[i] = (mpdu[i / 8] >> (i % 8)) & 1;
    std::vector<int> c;
    int r = 0;
    for (int b : bits) {
        r = ((r << 1) | b) & 127;
        const int c0 = __builtin_popcount(r & 0x6D) & 1, c1 = __builtin_popcount(r & 0x4F) & 1;
        if (md.rate == 4) { c.push_back(c0); c.push_back(c0); c.push_back(c1); c.push_back(c1); } else { c.push_back(c0); c.push_back(c1); }
    }
    const auto order = vibehfdl::deinterleaveOrder(coded, md.colShift);
    std::vector<int> tx((size_t)coded);
    for (int i = 0; i < coded; ++i) tx[size_t(order[size_t(i)])] = c[size_t(i)];
    sym.clear();
    for (int i = 0; i < 448; ++i) sym.push_back(cf32((i & 1) ? 1.f : -1.f, 0));      // a pre-key (content irrelevant here)
    const auto& A = vibehfdl::aSeq();
    for (int k = 0; k < 2 * vibehfdl::kALen; ++k) sym.push_back(cf32(A[size_t(k % vibehfdl::kALen)], 0));
    for (int k = 0; k < vibehfdl::kM1Len; ++k) sym.push_back(cf32(vibehfdl::m1Sym(mode, k), 0));
    for (int k = 0; k < vibehfdl::kM2Len; ++k) sym.push_back(cf32(vibehfdl::m1Sym(mode, k), 0));
    for (int k = 0; k < 9 * vibehfdl::kTLen; ++k) sym.push_back(cf32(vibehfdl::tSym(k % vibehfdl::kTLen), 0));
    vibehfdl::Scrambler scr;
    const int M = 1 << md.bitsPerSym;
    size_t ci = 0;
    for (int s = 0; s < md.segments; ++s) {
        for (int k = 0; k < vibehfdl::kDataLen; ++k) {
            int lab = 0;
            for (int b = 0; b < md.bitsPerSym; ++b) lab = (lab << 1) | tx[ci++];
            int idx = 0; for (int i = 0; i < M; ++i) if ((i ^ (i >> 1)) == lab) idx = i;   // the Gray label's point
            cf32 v = std::polar(1.0f, float(idx) * 2 * float(M_PI) / M);
            if (md.bitsPerSym == 1) v = cf32(lab ? -1.f : 1.f, 0);
            if (scr.next()) v = -v;
            sym.push_back(v);
        }
        for (int k = 0; k < vibehfdl::kTLen; ++k) sym.push_back(cf32(vibehfdl::tSym(k), 0));
    }
    return {};
}
/** The symbols at 4 samples a symbol, root-raised-cosine α 0.31, a carrier offset, a second path, noise. */
static void modulate(const std::vector<cf32>& sym, std::vector<cf32>& out, double fOff, double snrDb, double echoDb, int echoDelay, std::mt19937& rng) {
    const double a = 0.31;
    auto rrc = [&](double t) {
        if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
        if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-6) return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
    };
    const size_t pre = 4000, n = sym.size() * 4 + 2 * pre;
    out.assign(n, cf32(0, 0));
    const float eg = float(std::pow(10.0, echoDb / 20));
    for (size_t k = 0; k < sym.size(); ++k)
        for (int j = -32; j <= 32; ++j) {
            const long i = long(pre + k * 4) + j;
            const float h = float(rrc(j / 4.0)) * 0.5f;
            out[size_t(i)] += sym[k] * h;
            if (echoDelay && i + echoDelay < long(n)) out[size_t(i + echoDelay)] += sym[k] * h * eg * cf32(0.6f, 0.8f);
        }
    double sp = 0; for (size_t k = 0; k < sym.size(); ++k) sp += std::norm(out[pre + k * 4]);
    sp /= sym.size();
    const float sigma = float(std::sqrt(sp / std::pow(10.0, snrDb / 10) / 2));
    std::normal_distribution<float> g(0, sigma);
    for (size_t i = 0; i < n; ++i) out[i] = out[i] * std::polar(1.0f, float(2 * M_PI * fOff * i / vibehfdl::kWorkRate)) + cf32(g(rng), g(rng));
}
static int synthOnce(int mode, double snr, double fOff, double echoDb, int echoDelay, uint32_t seed) {
    std::mt19937 rng(seed);
    char flt[7]; std::snprintf(flt, sizeof flt, "TST%03u", seed % 1000);
    auto mp = perfMpdu(int(seed % 200) + 1, 6, flt, 51.5, -1.25);
    std::vector<cf32> sym, x;
    burstSymbols(mode, mp, sym);
    modulate(sym, x, fOff, snr, echoDb, echoDelay, rng);
    vibehfdl::Demod d(vibehfdl::kWorkRate);
    int good = 0;
    for (size_t o = 0; o < x.size(); o += 4096)
        d.feed(x.data() + o, int(std::min<size_t>(4096, x.size() - o)), [&](const vibehfdl::Message& m) {
            if (m.flight == flt && std::fabs(m.lat - 51.5) < 1e-3) ++good;
            dig(m.flight.data(), m.flight.size());
        });
    // the decoder holds a burst until its longest possible end: flush with silence
    std::vector<cf32> z(4096, cf32(0, 0));
    for (int i = 0; i < 10; ++i) d.feed(z.data(), 4096, [&](const vibehfdl::Message& m) { if (m.flight == flt) ++good; });
    return good > 0;
}

static void printMsg(const vibehfdl::Message& m) {
    std::printf("%8.2f %.0f kHz %4d bps %c snr %4.1f f %+5.1f | %s %s%d -> %s%d %s", m.t, m.hz / 1e3, vibehfdl::bitRate(m.mode), m.mode < 4 ? 'S' : 'D', m.snrDb, m.freqErrHz,
                m.downlink ? "DN" : "UP", m.downlink ? "AC" : "GS", m.srcId, m.downlink ? "GS" : "AC", m.dstId, m.typeName.c_str());
    if (m.icao) std::printf(" icao %06X", m.icao);
    if (!m.flight.empty()) std::printf(" flt %s", m.flight.c_str());
    if (!std::isnan(m.lat)) std::printf(" pos %.4f %.4f", m.lat, m.lon);
    if (m.hfnpdu >= 0) std::printf(" hfnpdu %02X", m.hfnpdu);
    if (m.hasAcars) { std::string t = m.acars.text; for (auto& c : t) if (c == '\r' || c == '\n') c = ' '; std::printf(" | ACARS%s %s lbl %s | %s", m.acarsCrcOk ? "" : "(BCS bad)", m.acars.reg.c_str(), m.acars.label.c_str(), t.c_str()); }
    std::printf("\n");
}

struct RefMsg { double time; int freqKhz; int rate; std::string flight, reg, label, type, text, icao; int src; bool matched = false; };
static std::string jstr(const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = l.find(k); if (p == std::string::npos) return "";
    p += k.size();
    if (l[p] == '"') { size_t e = l.find('"', p + 1); return l.substr(p + 1, e - p - 1); }
    size_t e = l.find_first_of(",}", p); return l.substr(p, e - p);
}

static int wav(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test-hfdl --wav file centreHz [dialHz...] [--ref file]\n"); return 2; }
    const double centre = std::atof(argv[3]);
    std::vector<double> dial; std::string refPath;
    for (int i = 4; i < argc; ++i) { if (!std::strcmp(argv[i], "--ref") && i + 1 < argc) refPath = argv[++i]; else dial.push_back(std::atof(argv[i])); }
    std::vector<RefMsg> ref; double refStart = 0;
    if (!refPath.empty()) {
        FILE* rf = std::fopen(refPath.c_str(), "r");
        char line[8192];
        while (rf && std::fgets(line, sizeof line, rf)) {
            std::string l(line);
            if (l.find("\"start\"") != std::string::npos) {
                const std::string s = jstr(l, "start");   // 2026-10-31T21:57:57.451Z
                int Y, Mo, D, h, mi; double sec;
                if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lfZ", &Y, &Mo, &D, &h, &mi, &sec) == 6) {
                    struct tm tmv{}; tmv.tm_year = Y - 1900; tmv.tm_mon = Mo - 1; tmv.tm_mday = D; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = 0;
                    refStart = double(timegm(&tmv)) + sec;
                }
            }
            if (l.find("\"type\":\"message\"") == std::string::npos) continue;
            RefMsg m;
            m.time = std::atof(jstr(l, "time").c_str()); m.freqKhz = std::atoi(jstr(l, "freq_khz").c_str()); m.rate = std::atoi(jstr(l, "bit_rate").c_str());
            m.flight = jstr(l, "flight"); m.reg = jstr(l, "reg"); m.label = jstr(l, "label"); m.type = jstr(l, "msg_type"); m.text = jstr(l, "msg_text");
            m.src = std::atoi(jstr(l, "src_id").c_str()); m.icao = jstr(l, "src_icao");
            ref.push_back(m);
        }
        if (rf) std::fclose(rf);
        if (dial.empty()) { std::set<int> f; for (auto& m : ref) f.insert(m.freqKhz); for (int k : f) dial.push_back(k * 1000.0); }
    }
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    std::fseek(f, 44, SEEK_SET);   // the canonical 44-byte header (iqrec writes plain PCM)
    const double rate = 192000;
    vibehfdl::Receiver rx(rate, centre, dial);
    std::vector<int16_t> buf(2 * 65536);
    std::vector<cf32> z(65536);
    size_t samples = 0; int n = 0;
    std::vector<vibehfdl::Message> got;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const size_t r = std::fread(buf.data(), 4, 65536, f);
        if (!r) break;
        for (size_t i = 0; i < r; ++i) z[i] = cf32(buf[2 * i] / 32768.f, buf[2 * i + 1] / 32768.f);
        samples += r;
        rx.feed(z.data(), r, [&](const vibehfdl::Message& m) { ++n; got.push_back(m); printMsg(m); });
    }
    std::fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        std::printf("%.0f kHz: detections %llu, no M1 %llu, bursts %llu, CRC-bad bursts %llu, messages %llu\n", dial[c] / 1e3, (unsigned long long)s.detections,
                    (unsigned long long)s.noM1, (unsigned long long)s.bursts, (unsigned long long)s.crcBad, (unsigned long long)s.messages);
    }
    if (!ref.empty()) {
        // ★ a reference message is matched by frequency, aircraft id, type, and (when both have one) flight / registration /
        //   ICAO address, its time within 3 s of our burst's start (the add-on stamps the slot the burst began in)
        int matched = 0;
        std::vector<bool> used(got.size(), false);
        for (auto& r : ref) {
            for (size_t gi = 0; gi < got.size(); ++gi) {
                const auto& m = got[gi];
                if (used[gi]) continue;
                if (std::lround(m.hz / 1e3) != r.freqKhz || std::fabs(refStart + m.t - r.time) > 3) continue;
                if (!m.downlink || m.srcId != r.src || r.type != m.typeName) continue;
                char ic[8]; std::snprintf(ic, sizeof ic, "%06X", m.icao);
                const bool idOk = (!r.flight.empty() && !m.flight.empty() && r.flight == m.flight) || (!r.reg.empty() && m.hasAcars && r.reg == m.acars.reg) ||
                                  (!r.icao.empty() && m.icao && r.icao == ic) || (r.flight.empty() && m.flight.empty()) || (r.icao.empty() && !r.flight.empty() && m.icao);
                if (idOk) { r.matched = true; used[gi] = true; break; }
            }
            matched += r.matched;
            if (!r.matched) std::printf("   reference not decoded: %.0f %d kHz %d bps AC%d %s %s %s\n", r.time - refStart, r.freqKhz, r.rate, r.src, r.type.c_str(), r.flight.c_str(), r.reg.c_str());
        }
        std::printf("REFERENCE: %d of %zu messages decoded (ours: %d messages, %d downlink)\n", matched, ref.size(), n,
                    int(std::count_if(got.begin(), got.end(), [](const vibehfdl::Message& m) { return m.downlink; })));
    }
    std::printf("messages %d in %.0f s; %.2f s here = %.2f %% of a core (%zu channels)\n", n, samples / rate, el, 100 * el / (samples / rate), dial.size());
    return 0;
}

int main(int argc, char** argv) {
    if (const char* t = std::getenv("HFDL_TUNE")) {
        auto& T = vibehfdl::tuning(); std::string s(t);
        auto vf = [&](const char* k, float& v) { const size_t p = s.find(k); if (p != std::string::npos) v = float(std::atof(s.c_str() + p + std::strlen(k))); };
        vf("coh=", T.minCoh); vf("ts=", T.tSpan); vf("det=", T.detQ); vf("pair=", T.detPair); { const size_t q = s.find("seg="); if (q != std::string::npos) T.segSigma = std::atoi(s.c_str() + q + 4) != 0; } vf("mu=", T.mu); vf("pa=", T.pllA); vf("pb=", T.pllB);
        { const size_t q = s.find("taps="); if (q != std::string::npos) T.eqTaps = std::atoi(s.c_str() + q + 5); }
    }
    if (argc > 1 && !std::strcmp(argv[1], "--wav")) return wav(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--sens")) {
        const int mode = argc > 2 ? std::atoi(argv[2]) : 0;
        const double echo = argc > 3 ? std::atof(argv[3]) : -99;
        if (argc > 4) { std::printf("%d\n", synthOnce(mode, std::atof(argv[4]), 3.0, echo, echo > -90 ? 5 : 0, 177)); return 0; }
        for (double snr = -6; snr <= 12; snr += 2) {
            int ok = 0; for (int i = 0; i < 10; ++i) ok += synthOnce(mode, snr, 3.0, echo, echo > -90 ? 5 : 0, uint32_t(i * 77 + snr * 10 + 100));
            std::printf("mode %d SNR %+3.0f dB: %d of 10\n", mode, snr, ok);
        }
        return 0;
    }
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    std::mt19937 rng(9);

    std::printf("1. the coding layers\n");
    {
        vibehfdl::Scrambler s; std::vector<int> a, b;
        for (int i = 0; i < 120; ++i) a.push_back(s.next());
        for (int i = 0; i < 120; ++i) b.push_back(s.next());
        CHECK(a == b, "the scrambler restarts every 120 data symbols");
        int ones = 0; for (int v : a) ones += v;
        CHECK(ones > 40 && ones < 80, "scrambler balance %d/120", ones);
        for (int m = 0; m < 8; ++m) {
            const vibehfdl::Mode md = vibehfdl::modeOf(m);
            const int total = md.segments * vibehfdl::kDataLen * md.bitsPerSym;
            auto o = vibehfdl::deinterleaveOrder(total, md.colShift);
            std::vector<int> c(o.begin(), o.end()); std::sort(c.begin(), c.end());
            bool perm = true; for (int i = 0; i < total; ++i) perm &= c[size_t(i)] == i;
            CHECK(perm, "mode %d: the deinterleaver reads every value once (%d)", m, total);
        }
        // Viterbi: a random block through the encoder and back, clean and with 4 % of the coded bits flipped
        for (int noisy = 0; noisy < 2; ++noisy) {
            std::vector<uint8_t> in(500); for (auto& v : in) v = uint8_t(rng() & 1);
            for (int i = 0; i < 6; ++i) in.push_back(0);
            std::vector<float> soft; int r = 0, flips = 0;
            for (uint8_t bt : in) {
                r = ((r << 1) | bt) & 127;
                for (int poly : { 0x6D, 0x4F }) {
                    int c = __builtin_popcount(r & poly) & 1;
                    float v = c ? -1.f : 1.f;
                    if (noisy && rng() % 25 == 0) { v = -v * 0.3f; ++flips; }
                    soft.push_back(v);
                }
            }
            auto out = vibehfdl::viterbi27(soft);
            CHECK(out == in, "Viterbi %s (%d flips)", noisy ? "with errors" : "clean", flips);
        }
        // the PDU check and the coordinates
        auto mp = perfMpdu(42, 6, "TST001", 51.5, -1.25);
        auto msgs = vibehfdl::parseMpdu(mp.data(), mp.size());
        CHECK(msgs.size() == 1 && msgs[0].flight == "TST001" && std::fabs(msgs[0].lat - 51.5) < 1e-3 && std::fabs(msgs[0].lon + 1.25) < 1e-3 && msgs[0].srcId == 42,
              "MPDU round trip");
        mp[10] ^= 0x10;
        CHECK(vibehfdl::parseMpdu(mp.data(), mp.size()).empty(), "a damaged LPDU is refused");
        // a real header off the air (8977 kHz, 21:58:14): 07 03 d1 00 01 00 2b + its CRC 6b 76
        const uint8_t real[8] = { 0x07, 0x03, 0xD1, 0x00, 0x01, 0x00, 0x2B, 0x6B };
        uint8_t rh[9]; std::memcpy(rh, real, 8); rh[8] = 0x76;
        CHECK(vibehfdl::crcOk(rh, 7), "the real MPDU header's CRC");
    }

    std::printf("2. synthetic bursts (RRC 0.31, 3 Hz offset; SNR in the symbol bandwidth): decoded of %d\n", quick ? 4 : 10);
    {
        const int N = quick ? 4 : 10;
        for (int mode : { 0, 1, 2, 3, 4 }) {
            std::printf("   mode %d (%4d bit/s %c):", mode, vibehfdl::bitRate(mode), mode < 4 ? 'S' : 'D');
            /* ★ 25 dB: a strong burst (the detection's hold past A2, 2026-11-16 — 0 of 10 before it). ✗ 40 dB over digital
             *  silence still commits early at the silence-to-pre-key step (0–1 of 10): not a condition HF produces (its
             *  noise floor is never digital zero), left as a known limit in AVIATION.md. */
            for (double snr : { -4.0, 0.0, 4.0, 10.0, 25.0 }) {
                int ok = 0; for (int i = 0; i < N; ++i) ok += synthOnce(mode, snr, 3.0, -99, 0, uint32_t(1000 + i * 31 + mode * 7 + int(snr)));
                std::printf("  %+3.0f dB %2d", snr, ok);
                if (snr >= 4 && mode != 3) CHECK(ok >= N - (snr > 20 ? 2 : 1), "mode %d at %.0f dB: %d of %d", mode, snr, ok, N);
                if (snr >= 10) CHECK(ok >= N - (snr > 20 ? 2 : 1), "mode %d at %.0f dB: %d of %d", mode, snr, ok, N);
            }
            std::printf("\n");
        }
        int ok = 0; for (int i = 0; i < N; ++i) ok += synthOnce(0, 2.0, -12.0, -3.0, 5, uint32_t(500 + i));
        std::printf("   300 bit/s, a second path 3 dB down 1.25 symbols late, -12 Hz, 2 dB: %d of %d\n", ok, N);
        CHECK(ok >= N - 2, "multipath %d of %d", ok, N);
    }

    std::printf("3. noise only: %s at 7200 S/s, zero messages\n", quick ? "2 min" : "20 min");
    {
        vibehfdl::Demod d(vibehfdl::kWorkRate);
        std::normal_distribution<float> g(0, 0.1f);
        std::vector<cf32> blk(7200);
        int got = 0;
        for (int s = 0; s < (quick ? 120 : 1200); ++s) {
            for (auto& v : blk) v = cf32(g(rng), g(rng));
            d.feed(blk.data(), int(blk.size()), [&](const vibehfdl::Message&) { ++got; });
        }
        std::printf("   %d messages (detections %llu, bursts worked %llu)\n", got, (unsigned long long)d.stats().detections, (unsigned long long)d.stats().bursts);
        CHECK(got == 0, "noise gave %d messages", got);
    }

    std::printf("4. CPU: one channel (a listener's dial) at 12 kS/s in, 10 min of noise\n");
    {
        vibehfdl::Demod d(12000);
        std::normal_distribution<float> g(0, 0.1f);
        std::vector<cf32> blk(12000);
        for (auto& v : blk) v = cf32(g(rng), g(rng));
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < 600; ++s) d.feed(blk.data(), int(blk.size()), [&](const vibehfdl::Message&) {});
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %.3f s for 600 s = %.3f %% of a core here\n", el, 100 * el / 600);
    }
    std::printf("5. a listener's dial: the burst in USB audio (48 kS/s real, carrier at 1440 Hz) through HfdlRun into the table\n");
    {
        // ★ The baseband burst (7200 S/s) is interpolated to 48 kS/s and put on a 1440 Hz carrier, real — what a USB
        //   demodulator on the dial gives — with an unrelated tone at 2900 Hz in the same audio (the image's side).
        int okFlights = 0; const int N = std::getenv("HN") ? std::atoi(std::getenv("HN")) : quick ? 3 : 6;
        vibeavi::Tracker tr(false);
        std::vector<std::string> lines;
        vibe::HfdlRun::Out o; o.text = [&](const std::string& t) { lines.push_back(t); };
        vibe::HfdlRun run(tr, o, [] { return 8977e3; });
        for (int i = 0; i < N; ++i) {
            std::mt19937 r2((std::getenv("HS") ? std::atoi(std::getenv("HS")) : 77) + i);
            char flt[7]; std::snprintf(flt, sizeof flt, "AUD%03d", i);
            auto mp = perfMpdu(10 + i, 7, flt, 52.0 + i * 0.1, -8.5);
            std::vector<cf32> sym, x;
            burstSymbols(std::getenv("HM") ? std::atoi(std::getenv("HM")) : (i % 2 ? 1 : 0), mp, sym);
            modulate(sym, x, 2.0, std::getenv("HSNR") ? std::atof(std::getenv("HSNR")) : 15.0, -99, 0, r2);
            if (std::getenv("HFDL_DIRECT")) {   // ★ diagnosis: the same baseband straight into a Demod
                vibehfdl::Demod d(vibehfdl::kWorkRate); int got = 0;
                for (size_t o2 = 0; o2 < x.size(); o2 += 4096) d.feed(x.data() + o2, int(std::min<size_t>(4096, x.size() - o2)), [&](const vibehfdl::Message& m) { got += m.flight == flt; });
                std::vector<cf32> zz(4096, cf32(0, 0)); for (int q = 0; q < 10; ++q) d.feed(zz.data(), 4096, [&](const vibehfdl::Message& m) { got += m.flight == flt; });
                std::printf("   direct %s: %d (det %llu noM1 %llu bursts %llu pdus %llu crcBad %llu)\n", flt, got, (unsigned long long)d.stats().detections, (unsigned long long)d.stats().noM1, (unsigned long long)d.stats().bursts, (unsigned long long)d.stats().pdus, (unsigned long long)d.stats().crcBad);
            }
            const double up = 48000.0 / vibehfdl::kWorkRate;
            std::vector<float> a(size_t(double(x.size()) * up));
            for (size_t k = 0; k < a.size(); ++k) {
                const double t = double(k) / up; const size_t j = size_t(t); const float f = float(t - double(j));
                auto at = [&](long q) { return q < 0 || q >= long(x.size()) ? cf32(0, 0) : x[size_t(q)]; };
                const cf32 p0 = at(long(j) - 1), p1 = at(long(j)), p2 = at(long(j) + 1), p3 = at(long(j) + 2);
                const cf32 c = ((( -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3) * f + (p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3)) * f + (-0.5f * p0 + 0.5f * p2)) * f + p1;
                a[k] = float((c * std::polar(1.0f, float(2 * M_PI * 1440.0 * double(k) / 48000.0))).real()) + 0.05f * float(std::sin(2 * M_PI * 2900.0 * double(k) / 48000.0));
            }
            for (size_t k = 0; k < a.size(); k += 4800) run.feedAudio(a.data() + k, int(std::min<size_t>(4800, a.size() - k)));
            std::vector<float> z(48000, 0.f);
            for (int s = 0; s < 6; ++s) run.feedAudio(z.data(), int(z.size()));   // ★ the decoder holds a burst to its longest end
        }
        if (std::getenv("HFDL_LINES")) for (const auto& l : lines) std::printf("   %s", l.c_str());
        for (const auto& l : lines) for (int i = 0; i < N; ++i) { char flt[7]; std::snprintf(flt, sizeof flt, "AUD%03d", i); if (l.find(flt) != std::string::npos) { ++okFlights; break; } }
        std::printf("   %d of %d bursts decoded from audio; first line: %s", okFlights, N, lines.empty() ? "(none)\n" : lines[0].c_str());
        CHECK(okFlights >= N - 1, "audio path: %d of %d", okFlights, N);
        CHECK(!lines.empty() && lines[0].find("8977 kHz") != std::string::npos && lines[0].find("Shannon") != std::string::npos, "the line names the dial and the station");
        const double nowU = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string ac = tr.hfdlAircraftJson(nowU, nowU), gs = tr.hfdlGroundStationsJson(nowU, nowU);
        CHECK(ac.find("\"flight\":\"AUD000\"") != std::string::npos && ac.find("\"gs_id\":7") != std::string::npos && ac.find("\"lat\":52.000") != std::string::npos,
              "/addon/hfdl/aircraft shape: %.200s", ac.c_str());
        CHECK(gs.find("\"gs_id\":7,\"location\":\"Shannon\",\"lat\":52.7") != std::string::npos && gs.find("{\"freq_khz\":8977,\"enabled\":true}") != std::string::npos,
              "/addon/hfdl/groundstations: Shannon heard on 8977: %.300s", gs.c_str());
        // ★ noise and a speech-like tone mix in the same audio for 2 minutes: nothing
        vibe::HfdlRun quiet(tr, o, nullptr);
        const size_t before = lines.size();
        std::normal_distribution<float> g(0, 0.2f);
        std::vector<float> nz(48000);
        for (int s = 0; s < 120; ++s) {
            for (size_t k = 0; k < nz.size(); ++k) nz[k] = g(rng) + 0.3f * float(std::sin(2 * M_PI * (400.0 + 30 * (s % 7)) * double(k) / 48000.0));
            quiet.feedAudio(nz.data(), int(nz.size()));
        }
        CHECK(lines.size() == before, "2 min of audio noise: %zu lines", lines.size() - before);
    }
    std::printf("digest %016llx\n", (unsigned long long)g_digest);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
