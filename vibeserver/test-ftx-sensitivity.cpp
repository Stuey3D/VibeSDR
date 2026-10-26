// test-ftx-sensitivity — the FT8 / FT4 / FT2 sensitivity work (2026-10-26): the second look (decoders/ftx_fine.*),
// OSD with pairs, a-priori decoding (FtxApBook), and the guards that keep every one of them from inventing a decode.
//
// ★★★ WHAT MUST HOLD (Stuart, 2026-10-27: "world class", CPU first; decodes only ever go UP, zero false decodes):
//   1. the kernels the old decoder still runs are untouched (BP with no stall = bp_decode; OSD with no pairs = OSD-1);
//      OSD never returns the all-zero codeword, and what it returns must read like a message people send;
//   2. the second look finds a clean signal's start to 1/32 symbol and its frequency to a fraction of a tone;
//   3. on a synthetic band with known answers it decodes well below where the old full depth gave up — FT8, FT4, FT2;
//   4. the AP book learns calls and pairs, forgets them, and its hypotheses are exactly the messages they stand for;
//      AP adds decodes of signals whose calls were heard, and NONE that were not sent;
//   5. pure noise, every candidate down to the weakest sync tried, a full book of 400 calls: no decode at all;
//   6. (VIBE_TEST_AUDIO) a real 40 m FT8 recording: far more decodes than the old full depth, at about its CPU;
//   7. (VIBE_TEST_AUDIO) spot SNRs on STUEY3D's scale: median difference within ±1.5 dB (FT8, FT4), ±3.5 (FT2, few).
#include "decoders/ft8_decoder.h"
#include "decoders/ftx_fine.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "ft8/constants.h"
#include "ft8/crc.h"
#include "ft8/encode.h"
#include "ft8/ldpc.h"
#include "ft8/message.h"
}

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static const int kFs = 12000;
static const double kSigma = 0.02;

static void gfskAdd(std::vector<float>& out, const uint8_t* tones, int nn, int nsps, double bt, double f0, double t0,
                    double amp, double phase0) {
    const double k = 5.336446;
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = i / (double)nsps - 1.5;
        pulse[(size_t)i] = (std::erf(k * bt * (t + 0.5)) - std::erf(k * bt * (t - 0.5))) / 2;
    }
    const int nw = nn * nsps;
    std::vector<double> dphi((size_t)(nw + 2 * nsps), 2 * M_PI * f0 / kFs);
    for (int i = 0; i < nn; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += 2 * M_PI / nsps * tones[i] * pulse[(size_t)j];
    for (int j = 0; j < 2 * nsps; ++j) {
        dphi[(size_t)j] += 2 * M_PI / nsps * pulse[(size_t)(j + nsps)] * tones[0];
        dphi[(size_t)(j + nw)] += 2 * M_PI / nsps * pulse[(size_t)j] * tones[nn - 1];
    }
    const long s0 = std::lround(t0 * kFs);
    double phi = phase0;
    for (int n = 0; n < nw; ++n) {
        const long i = s0 + n;
        if (i >= 0 && i < (long)out.size()) out[(size_t)i] += (float)(amp * std::sin(phi));
        phi += dphi[(size_t)(n + nsps)];
    }
}
static std::string randCall(std::mt19937& rng) {
    static const char* pre[] = { "G", "M", "DL", "F", "EA", "I", "SP", "OK", "PA", "ON", "LA", "OH", "SM", "K", "W", "VE", "JA", "UA" };
    std::string s = pre[rng() % 18]; s += (char)('0' + rng() % 10);
    for (int i = 0, n = 1 + (int)(rng() % 3); i < n; ++i) s += (char)('A' + rng() % 26);
    return s;
}
static std::string randMsg(std::mt19937& rng) {
    const std::string a = randCall(rng), b = randCall(rng);
    char rep[8]; std::snprintf(rep, sizeof rep, "%+03d", -24 + (int)(rng() % 30));
    char grid[5] = { (char)('A' + rng() % 18), (char)('A' + rng() % 18), (char)('0' + rng() % 10), (char)('0' + rng() % 10), 0 };
    switch (rng() % 6) {
    case 0: return "CQ " + a + " " + grid;
    case 1: return a + " " + b + " " + grid;
    case 2: return a + " " + b + " " + rep;
    case 3: return a + " " + b + " R" + rep;
    case 4: return a + " " + b + " RR73";
    default: return a + " " + b + " 73";
    }
}
static std::string text(const ftx_message_t& m0) {
    ftx_message_t m = m0; char t[64] = {}; ftx_message_offsets_t o;
    if (ftx_message_decode(&m, nullptr, t, &o) != FTX_MESSAGE_RC_OK) return "?";
    return t;
}

struct Geo { FtxMode mode; int nn, nsps; double bt, dtNom, dtLo, dtHi; const char* name; };
static Geo geo(FtxMode m) {
    if (m == FtxMode::FT8) return { m, FT8_NN, 1920, 2.0, 0.5, -0.5, 1.0, "FT8" };
    if (m == FtxMode::FT4) return { m, FT4_NN, 576, 1.0, 0.5, -0.5, 1.0, "FT4" };
    return { m, FT4_NN, 288, 1.0, 0.0, -0.3, 1.2, "FT2" };
}
static Ft8SlotOptions oldFull() { Ft8SlotOptions o; o.maxPasses = 3; o.osdMaxErrors = 24; return o; }
static Ft8SlotOptions newFull(FtxApBook* ap) {
    Ft8SlotOptions o; o.maxPasses = 3; o.osdMaxErrors = Ft8Decoder::kOsdMaxErrors; o.osdPairSpan = Ft8Decoder::kOsdPairSpan;
    o.fine = 1; o.bpStall = Ft8Decoder::kBpStall; o.onlyChanged = true; o.ap = ap;
    return o;
}

/** Decode `slots` synthetic slots at `snr` (fixed seed per SNR), `sigs` signals each; counts true and FALSE decodes. */
static void band(const Geo& g, double snr, int slots, int sigs, Ft8SlotOptions o, int& hit, int& sent, int& fals,
                 FtxApBook* seedBook = nullptr, double* msOut = nullptr) {
    Ft8SlotDecoder dec(kFs, g.mode);
    const int W = dec.windowSamples();
    std::mt19937 rng(7000u + (unsigned)std::lround((snr + 100) * 10) + (unsigned)g.mode * 100000u);
    std::normal_distribution<double> N(0, kSigma);
    std::uniform_real_distribution<double> U(0, 1);
    const double n2500 = kSigma * kSigma * 2500.0 / (kFs / 2.0);
    const double width = (g.mode == FtxMode::FT8 ? 8 : 4) * (double)kFs / g.nsps;
    hit = sent = fals = 0; double ms = 0;
    for (int s = 0; s < slots; ++s) {
        std::vector<float> x((size_t)W);
        for (float& v : x) v = (float)N(rng);
        std::set<std::string> truth;
        for (int k = 0; k < sigs; ++k) {
            ftx_message_t m; ftx_message_init(&m);
            const std::string msg = randMsg(rng);
            if (ftx_message_encode(&m, nullptr, msg.c_str()) != FTX_MESSAGE_RC_OK) continue;
            uint8_t tones[FT4_NN];
            if (g.mode == FtxMode::FT8) ft8_encode(m.payload, tones); else ft4_encode(m.payload, tones);
            const double hz = 200.0 + (2800.0 - width) * (k + 0.1 + 0.8 * U(rng)) / sigs;
            const double dt = g.dtLo + (g.dtHi - g.dtLo) * U(rng);
            gfskAdd(x, tones, g.nn, g.nsps, g.bt, hz, g.dtNom + dt, std::sqrt(2 * n2500 * std::pow(10.0, snr / 10)), 2 * M_PI * U(rng));
            truth.insert(text(m)); ++sent;
            if (seedBook) {
                char c1[16] = {}, c2[16] = {}, ex[16] = {}; ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
                if (ftx_message_decode_std(&m, nullptr, c1, c2, ex, ft) == FTX_MESSAGE_RC_OK) {
                    if (!std::strcmp(c1, "CQ")) seedBook->seed(c2); else seedBook->seed(c1, c2);
                }
            }
        }
        std::vector<Ft8Result> res; Ft8SlotStats st;
        const auto t0 = std::chrono::steady_clock::now();
        dec.decode(x.data(), W, o, res, st);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::set<std::string> seen;
        for (const Ft8Result& r : res) {
            const std::string t = text(r.msg);
            if (!seen.insert(t).second) continue;
            if (truth.count(t)) ++hit;
            else { ++fals; std::printf("     FALSE %s: \"%s\" (pass %d%s%s ap %d)\n", g.name, t.c_str(), r.pass, r.fine ? " fine" : "", r.osd ? " osd" : "", r.ap); }
        }
    }
    if (msOut) *msOut = ms / slots;
}

static bool readWav(const std::string& path, std::vector<int16_t>& out, int& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4)) { std::fclose(f); return false; }
    rate = 0;
    for (;;) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(sz); if (std::fread(b.data(), 1, sz, f) != sz) break;
            rate = (int)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24);
        } else if (!std::memcmp(id, "data", 4)) {
            std::vector<int16_t> buf(1 << 16); size_t got;
            while ((got = std::fread(buf.data(), 2, buf.size(), f)) > 0) out.insert(out.end(), buf.begin(), buf.begin() + (long)got);
            std::fclose(f); return rate > 0;
        } else std::fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    std::fclose(f); return false;
}

/* ★ The spot SNR scale (Stuart: spots read on the scale other stations report). The UberSDR decoder stream recorded
 *  beside a WAV gives STUEY3D's SNR for each message; ours minus theirs, median over every message both decoded in
 *  the same slot. */
static int medianSnrDiff(const std::string& wav, double t0, const std::string& sse, const std::string& band, FtxMode md, int& n) {
    std::vector<int16_t> pcm; int rate = 0; n = 0;
    if (!readWav(wav, pcm, rate) || rate != kFs) return 999;
    const double P = md == FtxMode::FT8 ? 15.0 : md == FtxMode::FT4 ? 7.5 : kFt2SlotTime;
    const char* mname = md == FtxMode::FT8 ? "FT8" : md == FtxMode::FT4 ? "FT4" : "FT2";
    std::multimap<std::string, std::pair<double, int>> ref;     // message -> (UTC of the report, SNR)
    std::ifstream in(sse); std::string l;
    auto field = [&](const std::string& k) { const size_t a = l.find("\"" + k + "\":"); if (a == std::string::npos) return std::string();
        size_t b = a + k.size() + 3; if (l[b] == '"') { const size_t e = l.find('"', b + 1); return l.substr(b + 1, e - b - 1); }
        size_t e = l.find_first_of(",}", b); return l.substr(b, e - b); };
    while (std::getline(in, l)) {
        if (l.rfind("data: ", 0) || field("mode") != mname || field("band") != band) continue;
        std::string m = field("message"), ts = field("timestamp");
        std::vector<std::string> w; for (size_t a = 0, b; a < m.size(); a = b + 1) { b = m.find(' ', a); if (b == std::string::npos) b = m.size(); if (b > a) w.push_back(m.substr(a, b - a)); }
        while (!w.empty() && (w.back() == "?" || (w.back().size() == 2 && w.back()[0] == 'a'))) w.pop_back();
        std::string mm; for (auto& x : w) mm += (mm.empty() ? "" : " ") + x;
        struct tm g = {}; if (sscanf(ts.c_str(), "%d-%d-%dT%d:%d:%dZ", &g.tm_year, &g.tm_mon, &g.tm_mday, &g.tm_hour, &g.tm_min, &g.tm_sec) != 6) continue;
        g.tm_year -= 1900; g.tm_mon -= 1;
        ref.insert({ mm, { (double)timegm(&g), std::atoi(field("snr").c_str()) } });
    }
    Ft8SlotDecoder dec(kFs, md);
    const int W = dec.windowSamples();
    std::vector<float> w((size_t)W);
    Ft8SlotOptions o = newFull(nullptr);
    if (md == FtxMode::FT2) o.osdMaxErrors = 0;                  // ★ as live
    std::vector<int> d;
    for (double S = std::ceil(t0 / P) * P; (S - t0) * kFs + W <= pcm.size(); S += P) {
        const long i0 = std::lround((S - t0) * kFs);
        for (int i = 0; i < W; ++i) w[(size_t)i] = pcm[(size_t)(i0 + i)] / 32768.0f;
        std::vector<Ft8Result> res; Ft8SlotStats st;
        dec.decode(w.data(), W, o, res, st);
        for (const Ft8Result& r : res) {
            const std::string t = text(r.msg);
            double bestDt = 1e9; int bestSnr = 0;
            auto rg = ref.equal_range(t);
            for (auto it = rg.first; it != rg.second; ++it) {
                const double dt = it->second.first - (S + P);           // reported at the slot's end
                if (dt >= -1 && dt <= P + 2 && std::fabs(dt) < bestDt) { bestDt = std::fabs(dt); bestSnr = it->second.second; }
            }
            if (bestDt < 1e9) d.push_back(r.snr - bestSnr);
        }
    }
    n = (int)d.size();
    if (d.empty()) return 999;
    std::nth_element(d.begin(), d.begin() + (long)d.size() / 2, d.end());
    return d[d.size() / 2];
}

/** FT2 has too few stream decodes in any recording here, so its reference is STUEY3D's PSKReporter reports (one
 *  report per sender, flowStartSeconds = when it was heard): ours, in the slot nearest that time, for that sender. */
static int medianSnrDiffPsk(const std::string& wav, double t0, const std::string& xml, FtxMode md, int& n) {
    std::vector<int16_t> pcm; int rate = 0; n = 0;
    if (!readWav(wav, pcm, rate) || rate != kFs) return 999;
    std::ifstream in(xml); std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    struct Rep { std::string call; double t; int snr; };
    std::vector<Rep> reps;
    const char* mname = md == FtxMode::FT2 ? "FT2" : md == FtxMode::FT4 ? "FT4" : "FT8";
    for (size_t a = 0; (a = all.find("<receptionReport ", a)) != std::string::npos; ++a) {
        const size_t e = all.find("/>", a); const std::string r = all.substr(a, e - a);
        auto attr = [&](const char* k) { const size_t p = r.find(std::string(k) + "=\""); if (p == std::string::npos) return std::string();
            const size_t b = p + std::strlen(k) + 2; return r.substr(b, r.find('"', b) - b); };
        if (attr("mode") != mname) continue;
        reps.push_back({ attr("senderCallsign"), std::atof(attr("flowStartSeconds").c_str()), std::atoi(attr("sNR").c_str()) });
    }
    const double P = kFt2SlotTime;
    Ft8SlotDecoder dec(kFs, md);
    const int W = dec.windowSamples();
    std::vector<float> w((size_t)W);
    Ft8SlotOptions o = newFull(nullptr); o.osdMaxErrors = 0;            // ★ FT2 as live
    std::map<std::string, std::pair<double, int>> best;                // sender -> (|dt| to its report, our SNR)
    for (double S = std::ceil(t0 / P) * P; (S - t0) * kFs + W <= pcm.size(); S += P) {
        const long i0 = std::lround((S - t0) * kFs);
        for (int i = 0; i < W; ++i) w[(size_t)i] = pcm[(size_t)(i0 + i)] / 32768.0f;
        std::vector<Ft8Result> res; Ft8SlotStats st;
        dec.decode(w.data(), W, o, res, st);
        for (const Ft8Result& r : res) {
            char to[16] = {}, de[16] = {}, ex[16] = {}; ftx_field_t ft[FTX_MAX_MESSAGE_FIELDS];
            if (ftx_message_decode_std(&r.msg, nullptr, to, de, ex, ft) != FTX_MESSAGE_RC_OK) continue;
            for (const Rep& p : reps)
                if (p.call == de && std::fabs(p.t - S) < 4 * P && (!best.count(de) || std::fabs(p.t - S) < best[de].first))
                    best[de] = { std::fabs(p.t - S), r.snr - p.snr };
        }
    }
    std::vector<int> d; for (auto& kv : best) d.push_back(kv.second.second);
    n = (int)d.size();
    if (d.empty()) return 999;
    std::sort(d.begin(), d.end());
    return d.size() % 2 ? d[d.size() / 2] : (int)std::lround(0.5 * (d[d.size() / 2 - 1] + d[d.size() / 2]));
}

int main() {
    std::mt19937 rng(2026);
    std::printf("1. The old kernels are untouched; OSD's guards\n");
    {
        std::normal_distribution<float> G(0, 1);
        int sameBp = 0, sameOsd = 0; const int T = 300;
        for (int t = 0; t < T; ++t) {
            float llr[FTX_LDPC_N];
            for (float& v : llr) v = 4.0f * G(rng);
            uint8_t p1[FTX_LDPC_N], p2[FTX_LDPC_N]; int e1 = 0, e2 = 0;
            bp_decode(llr, 25, p1, &e1); bp_decode_ex(llr, 25, 0, p2, &e2);
            sameBp += e1 == e2 && !std::memcmp(p1, p2, sizeof p1);
            uint8_t a1[12] = {}, a2[12] = {};
            const int r1 = ft8Osd1(llr, 24, a1), r2 = ft8Osd(llr, 24, 0, a2);
            sameOsd += r1 == r2 && (r1 < 0 || !std::memcmp(a1, a2, sizeof a1));
        }
        ok(sameBp == T, "bp_decode_ex with no stall = bp_decode, bit for bit (" + std::to_string(sameBp) + "/" + std::to_string(T) + ")");
        ok(sameOsd == T, "OSD with no pairs = OSD-1 exactly (" + std::to_string(sameOsd) + "/" + std::to_string(T) + ")");
        float zeros[FTX_LDPC_N]; for (float& v : zeros) v = -10.0f;      // every bit a confident 0: a carrier, a residue
        uint8_t a[12] = {};
        ok(ft8Osd(zeros, 24, 40, a) < 0, "OSD never returns the all-zero codeword (its CRC is 0 — it always 'passes')");
        auto osdOk = [](const char* t) { ftx_message_t m; ftx_message_init(&m); return ftx_message_encode(&m, nullptr, t) == FTX_MESSAGE_RC_OK && ft8OsdPlausible(m); };
        ok(osdOk("CQ G4ABC IO92") && osdOk("G4ABC DL1XYZ R-12") && osdOk("PJ4/K1ABC W9XYZ RR73") && osdOk("CQ DX OH3JF KP20"),
           "an OSD decode people send is kept (CQ, report, compound call, directed CQ)");
        auto nonstd = [](const char* to, const char* de) { ftx_message_t m; ftx_message_init(&m);
                                                           return ftx_message_encode_nonstd(&m, nullptr, to, de, "73") == FTX_MESSAGE_RC_OK && ft8OsdPlausible(m); };
        ok(nonstd("W9XYZ", "PJ4/K1ABC"), "...including a non-standard call (\"<...> PJ4/K1ABC 73\")");
        ok(!osdOk("TNX BOB 73 GL") && !nonstd("W9XYZ", "1/Y2ESZU4Y3"),
           "an OSD decode that is free text, or carries no plausible call (the FT2 noise run's \"1/Y2ESZU4Y3\"), is not");
    }

    std::printf("2. The second look measures a clean signal\n");
    {
        for (FtxMode md : { FtxMode::FT8, FtxMode::FT4, FtxMode::FT2 }) {
            const Geo g = geo(md);
            Ft8SlotDecoder dec(kFs, md);
            const int W = dec.windowSamples();
            std::vector<float> x((size_t)W, 0.0f);
            ftx_message_t m; ftx_message_init(&m); ftx_message_encode(&m, nullptr, "CQ G4ABC IO92");
            uint8_t tones[FT4_NN];
            if (md == FtxMode::FT8) ft8_encode(m.payload, tones); else ft4_encode(m.payload, tones);
            const double f0 = 1234.56, t0 = g.dtNom + 0.137;
            gfskAdd(x, tones, g.nn, g.nsps, g.bt, f0, t0, 0.1, 0.3);
            FtxFine fine((int)md, kFs, g.nsps, W);
            fine.ensure(x.data(), W);
            FtxFine::Meas M;
            const bool got = fine.measure(t0 * kFs + 0.4 * g.nsps, f0 - 0.3 * kFs / g.nsps, 10, M);   // coarse: 0.4 symbol, 0.3 tone off
            const double dtErr = std::fabs(M.start / kFs - t0) * kFs / g.nsps, dfErr = std::fabs(M.f0 - f0) / ((double)kFs / g.nsps);
            char b[200];
            std::snprintf(b, sizeof b, "%s: start to %.3f symbol (one baseband sample), frequency to %.3f tone, %d/%d Costas hits", g.name, dtErr, dfErr, M.syncHits, M.syncTotal);
            const double step = 1.0 / (md == FtxMode::FT8 ? 32 : 16);          // one baseband sample
            ok(got && dtErr <= step + 1e-9 && dfErr < 0.05 && M.syncHits == M.syncTotal, b);
            float llr[FTX_LDPC_N]; fine.llrGroup(2, llr);
            uint8_t sent[10];                    // the 77 bits as the code carries them (FT4/FT2: scrambled)
            for (int i = 0; i < 10; ++i) sent[i] = md == FtxMode::FT8 ? m.payload[i] : (uint8_t)(m.payload[i] ^ kFT4_XOR_sequence[i]);
            uint8_t a91[12] = {}; ftx_add_crc(sent, a91);
            int agree = 0;
            for (int i = 0; i < 77; ++i) agree += (llr[i] > 0) == (((a91[i / 8] >> (7 - i % 8)) & 1) != 0);
            ok(agree == 77, std::string(g.name) + ": every message bit's likelihood has the transmitted sign (" + std::to_string(agree) + "/77)");
        }
    }

    std::printf("3. Sensitivity on a synthetic band (known answers; true SNR in 2500 Hz)\n");
    {
        struct Case { FtxMode md; double snr; int sigs; double minNew, maxOld; } cases[] = {
            { FtxMode::FT8, -20.0, 10, 0.60, 0.25 },     // measured: old 13 %, new 78 %
            { FtxMode::FT4, -16.0, 10, 0.30, 0.10 },     // old 2.5 %, new 44 %
            { FtxMode::FT2, -13.0,  6, 0.30, 0.15 },     // old 4 %, new 48 %
        };
        for (const Case& c : cases) {
            const Geo g = geo(c.md);
            int h0, n0, f0, h1, n1, f1; double ms0 = 0, ms1 = 0;
            band(g, c.snr, 12, c.sigs, oldFull(), h0, n0, f0, nullptr, &ms0);
            band(g, c.snr, 12, c.sigs, newFull(nullptr), h1, n1, f1, nullptr, &ms1);
            char b[240];
            std::snprintf(b, sizeof b, "%s at %+.0f dB: old full depth %d/%d, new %d/%d (%.1f -> %.1f ms/slot), false %d / %d",
                          g.name, c.snr, h0, n0, h1, n1, ms0, ms1, f0, f1);
            ok(h1 >= c.minNew * n1 && h0 <= c.maxOld * n0 && f0 == 0 && f1 == 0, b);
        }
    }

    std::printf("4. The a-priori book\n");
    {
        FtxApBook book(false, 3);
        ftx_message_t m; ftx_message_init(&m);
        ftx_message_encode(&m, nullptr, "G4ABC DL1XYZ -12");
        book.beginSlot(); book.heard(m);
        ok(book.calls() == 2, "a decoded \"G4ABC DL1XYZ -12\" puts both calls in the book");
        const FtxApBook::Index& X = book.index();
        bool exact = X.ok && X.pairs.size() == 1;
        for (int t = 0; t < 3 && exact; ++t) {
            static const char* tails[] = { "RR73", "73", "RRR" };
            const int a = X.pairs[0].first, b = X.pairs[0].second;
            const FtxApBook::Hyp h = FtxApBook::make(X, 5, a, b, t);
            uint64_t want[2];
            exact = FtxApBook::bitsOf((std::string("G4ABC DL1XYZ ") + tails[t]).c_str(), false, want)
                    && h.nbits == 77 && h.val[0] == want[0] && h.val[1] == (want[1] & ((1ull << 13) - 1));
        }
        ok(exact, "...and the pair's sign-off hypotheses are exactly the bits of \"G4ABC DL1XYZ RR73 / 73 / RRR\"");
        for (int i = 0; i < 4; ++i) book.beginSlot();
        ok(book.calls() == 0 && book.index().pairs.empty(), "calls not heard again are forgotten after the book's age");
        FtxApBook b4(true, 10);
        ftx_message_t q; ftx_message_init(&q); ftx_message_encode(&q, nullptr, "CQ K1ABC FN42");
        uint64_t w[2]; FtxApBook::bitsOf("CQ K1ABC FN42", true, w);
        bool scr = true;
        for (int i = 0; i < 77; ++i) scr &= (((w[i >> 6] >> (i & 63)) & 1) != 0) == ((((q.payload[i / 8] ^ kFT4_XOR_sequence[i / 8]) >> (7 - i % 8)) & 1) != 0);
        ok(scr, "FT4/FT2 hypotheses are in the scrambled bits the code carries");

        const Geo g = geo(FtxMode::FT8);
        FtxApBook seeded(false, 1 << 30);
        for (int i = 0; i < 200; ++i) seeded.seed(randCall(rng), i % 2 ? randCall(rng) : std::string());
        int h0, n0, f0, h1, n1, f1;
        band(g, -22.0, 12, 10, newFull(nullptr), h0, n0, f0);
        band(g, -22.0, 12, 10, newFull(&seeded), h1, n1, f1, &seeded);
        char b[200];
        std::snprintf(b, sizeof b, "FT8 at -22 dB, the calls heard before (+200 others in the book): %d/%d without AP, %d/%d with, false %d / %d",
                      h0, n0, h1, n1, f0, f1);
        ok(h1 >= h0 + n1 / 8 && f0 == 0 && f1 == 0, b);
    }

    std::printf("5. Pure noise: every candidate down to the weakest sync, a 400-call book — no decode at all\n");
    {
        for (FtxMode md : { FtxMode::FT8, FtxMode::FT4, FtxMode::FT2 }) {
            const Geo g = geo(md);
            FtxApBook book(md != FtxMode::FT8, 1 << 30);
            for (int i = 0; i < 400; ++i) { const std::string a = randCall(rng); if (i % 2) book.seed(a, randCall(rng)); else book.seed(a); }
            Ft8SlotOptions o = newFull(&book);
            o.minScore = 0; o.maxCandidates = 300; o.fineMinSync = 6; o.apMaxBpErr = 200;   // ★ far harsher than live
            Ft8SlotDecoder dec(kFs, md);
            const int W = dec.windowSamples();
            std::normal_distribution<double> N(0, kSigma);
            const int slots = md == FtxMode::FT8 ? 24 : 40;
            int got = 0;
            for (int s = 0; s < slots; ++s) {
                std::vector<float> x((size_t)W);
                for (float& v : x) v = (float)N(rng);
                std::vector<Ft8Result> res; Ft8SlotStats st;
                dec.decode(x.data(), W, o, res, st);
                for (const Ft8Result& r : res) { ++got; std::printf("     FALSE %s: \"%s\"\n", g.name, text(r.msg).c_str()); }
            }
            ok(got == 0, std::string(g.name) + ": " + std::to_string(slots) + " slots of noise, " + std::to_string(got) + " decodes");
        }
    }

    std::printf("6. A real 40 m FT8 recording (Stuart's UberSDR, 2026-10-25 21:18 UTC)\n");
    {
        const char* root = std::getenv("VIBE_TEST_AUDIO");
        std::vector<int16_t> pcm; int rate = 0;
        if (!root || !readWav(std::string(root) + "/ft8/ft8_40m_7074000_20261025T2118.wav", pcm, rate) || rate != kFs) {
            std::printf("  (not run: VIBE_TEST_AUDIO/ft8/ft8_40m_7074000_20261025T2118.wav absent)\n");
        } else {
            const double t0 = 1792963117.948;
            auto run = [&](Ft8SlotOptions o, bool withAp, double& ms) {
                Ft8SlotDecoder dec(kFs, FtxMode::FT8);
                FtxApBook book(false, 40);
                if (withAp) o.ap = &book;
                const int W = dec.windowSamples();
                std::vector<float> w((size_t)W);
                int n = 0; ms = 0;
                for (int s = 0; s < 20; ++s) {
                    const double S = (std::ceil(t0 / 15.0) + s) * 15.0;
                    const long i0 = std::lround((S - t0) * kFs);
                    for (int i = 0; i < W; ++i) w[(size_t)i] = pcm[(size_t)(i0 + i)] / 32768.0f;
                    std::vector<Ft8Result> res; Ft8SlotStats st;
                    const auto a = std::chrono::steady_clock::now();
                    dec.decode(w.data(), W, o, res, st);
                    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
                    for (const Ft8Result& r : res) n += text(r.msg) != "?";
                }
                ms /= 20;
                return n;
            };
            double msOld, msNew;
            const int nOld = run(oldFull(), false, msOld), nNew = run(newFull(nullptr), true, msNew);
            char b[200];
            std::snprintf(b, sizeof b, "20 slots: old full depth %d decodes (%.1f ms/slot), new %d (%.1f ms/slot)", nOld, msOld, nNew, msNew);
            ok(nNew >= nOld * 6 / 5 && msNew <= 1.25 * msOld, b);   // ★ CPU: the same within a timer's noise
        }
    }
    std::printf("7. Spot SNRs on the scale other stations report (median of ours - STUEY3D's, same messages, same slots)\n");
    {
        const char* root = std::getenv("VIBE_TEST_AUDIO");
        struct R { const char* wav; double t0; const char* sse; const char* band; FtxMode md; int tol; int minN; } rs[] = {
            { "/ft8/ft8_40m_7074000_20261025T2118.wav", 1792963117.948, "/ft8/uber_sse_20261025T2118.txt", "40m-ft8", FtxMode::FT8, 1, 200 },
            { "/ft8/ft8_20m_14074000_20261026T1619.wav", 1793031575.364, "/ft8/uber_sse_20261026T1619.txt", "20m-ft8", FtxMode::FT8, 1, 200 },
            { "/ft8/ft4_20m_14080000_20261026T1619.wav", 1793031575.364, "/ft8/uber_sse_20261026T1619.txt", "20m-ft4", FtxMode::FT4, 1, 200 },
        };
        for (const R& r : rs) {
            if (!root) { std::printf("  (not run: VIBE_TEST_AUDIO not set)\n"); break; }
            int n = 0;
            const int med = medianSnrDiff(std::string(root) + r.wav, r.t0, std::string(root) + r.sse, r.band, r.md, n);
            if (med == 999 && n == 0) { std::printf("  (not run: %s absent)\n", r.wav); continue; }
            char b[200];
            std::snprintf(b, sizeof b, "%s %s: ours - STUEY3D's median %+d dB over %d decodes (within +-%d.5)", geo(r.md).name, r.band, med, n, r.tol);
            ok(n >= r.minN && std::abs(med) <= r.tol, b);
        }
        if (root) {
            int n = 0;
            const int med = medianSnrDiffPsk(std::string(root) + "/ft2/ft2_20m_14084000_20261026T0734.wav", 1793000045.300,
                                             std::string(root) + "/js8/pskreporter/stuey3d_20261026T0748.xml", FtxMode::FT2, n);
            if (n == 0) std::printf("  (not run: the FT2 recording or its PSKReporter reports absent)\n");
            else {
                char b[200];
                std::snprintf(b, sizeof b, "FT2 20m: ours - STUEY3D's (PSKReporter) median %+d dB over %d senders — a small sample, so within +-3.5", med, n);
                ok(std::abs(med) <= 3, b);
            }
        }
    }
    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS (0 failures)\n", fails);
    return fails ? 1 : 0;
}
