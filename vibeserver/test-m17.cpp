// ★★ V12 M17 AS A MODE (vibe_m17_fec.h, vibe_m17.h, docs/v12/M17.md) — our own encoder from the spec against our own
// receiver, end to end.
//
//   1. The spec's own vectors: the CRC table, the address example (AB1CD = 0x9FDD51), the QPP table's entries and that it is
//      its own inverse, Golay's generator = the cyclic code of 0xC75 with minimum distance 8, the puncturing patterns' sizes.
//   2. The FEC alone: the Viterbi's three ACS kernels bit-exact with each other; encode → noise → decode for every frame
//      type; Golay soft decoding through 3 errors (and Chase past that).
//   3. Over the air, modulated (RRC 4-FSK, FM at ±2.4 kHz) at several C/N and frequency offsets: the LSF and its callsigns,
//      voice frames decoded, the speech bit-identical to codec2's own decode of the same bits where the channel is clean,
//      META text and GNSS, the extended callsign, a late joiner rebuilding the LSF from the LICH, packet SMS, the end flag.
//   4. Noise only (minutes of it): no frame, no sync, no sound.
//   5. The threaded listener (bounded queues, silence until primed) and the server's path through the real RxPipeline in NFM.
//   ★ Exit 3 = NOT RUN for the voice parts when libcodec2 is not installed (everything else still runs and must pass);
//     VIBE_CODEC2_DISABLE=1 must make the mode unavailable (checked in a child process).
//   Tools: `test-m17 clip OUT.cu8 [rate=288000] [sec=24] [cn=20]` writes the server bench clip (scripts/test-server-m17.mjs).
#include "vibe_m17.h"

#include <chrono>
#include <cstdarg>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace m = vibe::m17;
using cf = std::complex<float>;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("   %s %s%s%s\n", ok ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m", what,
                detail.empty() ? "" : " — ", detail.c_str());
    (ok ? g_pass : g_fail)++;
}
static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
    char b[512]; va_list a; va_start(a, f); std::vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}

// ── a voice: a pitch-gliding harmonic series through two moving formants, in syllables (8 kHz) ─────────────────────────
static std::vector<short> voice(double sec) {
    const int fs = 8000, n = int(sec * fs);
    std::vector<short> v(static_cast<size_t>(n));
    double ph = 0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / fs;
        const double f0 = 120 + 30 * std::sin(2 * M_PI * 0.7 * t);
        ph += 2 * M_PI * f0 / fs;
        const double f1 = 600 + 250 * std::sin(2 * M_PI * 1.3 * t), f2 = 1700 + 500 * std::sin(2 * M_PI * 0.9 * t + 1);
        double s = 0;
        for (int h = 1; h * f0 < 3500; ++h) {
            const double fh = h * f0;
            const double g = std::exp(-std::pow((fh - f1) / 200, 2)) + 0.6 * std::exp(-std::pow((fh - f2) / 300, 2)) + 0.05;
            s += g * std::sin(h * ph);
        }
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.5 * t));
        v[size_t(i)] = short(std::lrint(std::max(-1.0, std::min(1.0, 0.25 * s * syl)) * 12000));
    }
    return v;
}

/** Complex AWGN for a carrier-to-noise ratio `cnDb` in the 9 kHz M17 channel (the signal is unit amplitude). */
static void addNoise(std::vector<cf>& x, int fs, double cnDb, unsigned seed) {
    const double var = double(fs) / 9000.0 / std::pow(10.0, cnDb / 10.0);
    std::mt19937 rng(seed); std::normal_distribution<float> nd(0.f, float(std::sqrt(var / 2)));
    for (auto& v : x) v += cf(nd(rng), nd(rng));
}

/** The receiver, fed 48 kHz IQ in 10 ms blocks (as the pipeline hands a channel over), the speech collected. */
struct Rx {
    std::vector<short> speech;
    std::vector<std::pair<int, std::vector<uint8_t>>> payloads;   // (frame number, the 16 bytes) as decoded
    m::Receiver r{[this](const short* s, int n) { speech.insert(speech.end(), s, s + n); }};
    Rx() { r.setPayloadSink([this](int fn, const uint8_t* p) { payloads.push_back({fn & 0x7FFF, std::vector<uint8_t>(p, p + 16)}); }); }
    void feed(const std::vector<cf>& iq, int fs = 48000) {
        const int blk = fs / 100;
        for (size_t i = 0; i < iq.size(); i += size_t(blk)) {
            const int n = int(std::min<size_t>(size_t(blk), iq.size() - i));
            r.feedIq(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), n, double(fs));
        }
    }
};
static double rms(const std::vector<short>& s, size_t from = 0) {
    double e = 0; size_t n = 0;
    for (size_t i = from; i < s.size(); ++i) { e += double(s[i]) * s[i]; ++n; }
    return n ? std::sqrt(e / double(n)) : 0;
}
static std::vector<cf> withSilence(std::vector<cf> x, int fs, double before, double after) {
    std::vector<cf> o(size_t(before * fs), cf(0, 0));
    o.insert(o.end(), x.begin(), x.end());
    o.insert(o.end(), size_t(after * fs), cf(0, 0));
    return o;
}

/** The 16 payload bytes of each frame our transmitter sent: codec2's own encoding of the speech, two 20 ms frames each. */
static std::vector<std::vector<uint8_t>> referencePayloads(const std::vector<short>& sp, int frames) {
    const auto& v = vibe::freedv::vocoder();
    std::vector<std::vector<uint8_t>> out;
    auto* enc = v.ok ? v.create(vibe::freedv::kC2Mode3200) : nullptr;
    for (int fn = 0; fn < frames; ++fn) {
        std::vector<uint8_t> p(16, 0);
        for (int h = 0; h < 2; ++h) {
            short blk[160] = {};
            const size_t at = size_t(fn) * 320 + size_t(h) * 160;
            for (size_t i = 0; i < 160; ++i) blk[i] = at + i < sp.size() ? sp[at + i] : 0;
            if (enc) v.encode(enc, p.data() + 8 * h, blk);
        }
        out.push_back(p);
    }
    if (enc) v.destroy(enc);
    return out;
}

// ── 1 + 2 ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void specVectors() {
    std::printf("\n── 1. the spec's own vectors ──\n");
    const uint8_t A[1] = {'A'};
    const char* n9 = "123456789";
    std::vector<uint8_t> all(256); for (int i = 0; i < 256; ++i) all[size_t(i)] = uint8_t(i);
    check(m::crc16(nullptr, 0) == 0xFFFF && m::crc16(A, 1) == 0x206E && m::crc16(reinterpret_cast<const uint8_t*>(n9), 9) == 0x772B
          && m::crc16(all.data(), 256) == 0x1C31, "CRC: empty FFFF, \"A\" 206E, \"123456789\" 772B, 00..FF 1C31",
          fmt("%04X %04X %04X %04X", m::crc16(nullptr, 0), m::crc16(A, 1), m::crc16(reinterpret_cast<const uint8_t*>(n9), 9), m::crc16(all.data(), 256)));
    check(m::encodeCallsign("AB1CD") == 0x9FDD51 && m::decodeCallsign(0x9FDD51) == "AB1CD", "address: AB1CD = 0x9FDD51 and back",
          fmt("%llX", (unsigned long long)m::encodeCallsign("AB1CD")));
    check(m::encodeCallsign("ABC") == m::encodeCallsign("ABC      ") && m::decodeCallsign(m::kBroadcast) == "@ALL"
          && m::decodeCallsign(m::encodeCallsign(".........")) == "........." && m::encodeCallsign(".........") == m::kMaxStandard,
          "address: trailing spaces, BROADCAST, the top of the standard range (0xEE6B27FFFFFF)");
    {
        bool rt = true; std::mt19937 g(3);
        static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
        for (int t = 0; t < 2000; ++t) {
            std::string s; const int len = 1 + int(g() % 9);
            for (int i = 0; i < len; ++i) s += al[g() % 39];
            if (m::decodeCallsign(m::encodeCallsign(s)) != s) rt = false;
        }
        check(rt, "address: 2000 random callsigns encode and decode");
    }
    const auto& pi = m::qpp();
    bool inv = true; for (int x = 0; x < 368; ++x) if (pi[pi[size_t(x)]] != x) inv = false;
    check(pi[0] == 0 && pi[1] == 137 && pi[2] == 90 && pi[3] == 227 && pi[45] == 277 && pi[91] == 139 && pi[367] == 47 && inv,
          "QPP: 0→0, 1→137, 2→90, 3→227, 45→277, 91→139, 367→47 (the table), and its own inverse");
    {
        // the cyclic code of g(x) = 0xC75 plus even parity, for every data word; then the minimum distance
        bool same = true; int dmin = 24;
        for (uint32_t d = 0; d < 4096; ++d) {
            uint32_t r = d << 11;
            for (int b = 22; b >= 11; --b) if ((r >> b) & 1) r ^= 0xC75u << (b - 11);
            uint32_t cw = (d << 11) | r; cw = (cw << 1) | (__builtin_popcount(cw) & 1);
            if (cw != m::golayEncode(uint16_t(d))) same = false;
            if (d) dmin = std::min(dmin, __builtin_popcount(m::golayEncode(uint16_t(d))));
        }
        check(same && dmin == 8, "Golay: G = the cyclic code of 0xC75 with a parity bit, minimum distance 8", fmt("dmin %d", dmin));
    }
    const auto& p1 = m::punctPattern(m::Punct::P1);
    int ones = 0; for (auto v : p1) ones += v;
    check(p1.size() == 61 && ones == 46 && p1[0] == 1 && p1[1] == 1 && p1[2] == 0 && p1[3] == 1 && p1[60] == 1,
          "P1: 61 entries, 46 kept, [1,1,0,1,1,1,0,1…]");
    // ★ the frame sizes the spec states, from our encoder
    const m::Lsf l = m::makeLsf("@ALL", "G4ABC", m::streamType(2, 0, 0), nullptr);
    std::vector<uint8_t> t1; m::bytesToBits(l.b, 240, t1);
    const auto c2 = m::convEncode(m::withFlush(t1));
    std::vector<uint8_t> s1; { uint8_t c[18] = {}; m::bytesToBits(c, 144, s1); }
    std::vector<uint8_t> k1; { uint8_t c[26] = {}; m::bytesToBits(c, 206, k1); }
    check(c2.size() == 488 && m::puncture(c2, m::Punct::P1).size() == 368
          && m::puncture(m::convEncode(m::withFlush(s1)), m::Punct::P2).size() == 272
          && m::puncture(m::convEncode(m::withFlush(k1)), m::Punct::P3).size() == 368
          && m::lichBits(l, 0).size() == 96 && m::lsfFrame(l).size() == 192 && m::preamble().size() == 192 && m::eotMarker().size() == 192,
          "sizes: LSF 244→488→368, stream 148→296→272 + LICH 96, packet 210→420→368, every frame 192 symbols");
    check(l.crcOk() && m::crc16(l.b, 30) == 0, "LSF: the CRC over all 30 bytes is zero");
}

static void fecAlone() {
    std::printf("\n── 2. the FEC alone ──\n");
    std::mt19937 g(5);
    // ★ the ACS kernels: the build's vector path (" ACS ") against the scalar reference, decisions and all
    {
        int diff = 0, trials = 400;
        for (int t = 0; t < trials; ++t) {
            std::vector<int8_t> s(488);
            for (auto& v : s) v = int8_t(int(g() % 255) - 127);
            const auto a = m::viterbi(s.data(), s.size(), m::Acs::Auto), b = m::viterbi(s.data(), s.size(), m::Acs::Scalar);
            if (a.bits != b.bits) ++diff;
        }
        check(diff == 0, fmt("Viterbi: the %s kernel equals the scalar one on %d random inputs", m::acsName(), trials).c_str(), fmt("%d differ", diff));
    }
    // ★ every frame type, coded, a fraction of its bits flipped, decoded
    auto trial = [&](int type, double pFlip, int n) {
        int ok = 0;
        std::bernoulli_distribution flip(pFlip);
        for (int t = 0; t < n; ++t) {
            std::vector<uint8_t> data(type == 0 ? 240 : type == 1 ? 144 : 206);
            for (auto& b : data) b = uint8_t(g() & 1);
            const m::Punct p = type == 0 ? m::Punct::P1 : type == 1 ? m::Punct::P2 : m::Punct::P3;
            const auto tx = m::puncture(m::convEncode(m::withFlush(data)), p);
            std::vector<int8_t> soft(tx.size());
            for (size_t i = 0; i < tx.size(); ++i) { int v = tx[i] ? 40 : -40; if (flip(g)) v = -v; soft[i] = int8_t(v); }
            const size_t coded = 2 * (data.size() + 4);
            const auto c = m::depuncture(soft.data(), soft.size(), coded, p);
            if (m::viterbi(c.data(), c.size()).bits == data) ++ok;
        }
        return ok;
    };
    const int a = trial(0, 0.01, 200), b = trial(1, 0.02, 200), c = trial(2, 0.015, 200);
    check(a >= 190 && b >= 197 && c >= 194, "Viterbi: LSF with 1 % of bits flipped, stream with 2 %, packet with 1.5 %", fmt("LSF %d, stream %d, packet %d of 200", a, b, c));
    // ★ Golay: up to three errors always; four with soft information (Chase) most of the time
    {
        int ok3 = 0, ok4 = 0;
        for (int t = 0; t < 2000; ++t) {
            const uint16_t d = uint16_t(g() & 0xFFF);
            const uint32_t cw = m::golayEncode(d);
            int8_t s[24];
            for (int i = 0; i < 24; ++i) s[i] = ((cw >> (23 - i)) & 1) ? 60 : -60;
            int pos[4]; for (int k = 0; k < 4; ++k) { pos[k] = int(g() % 24); for (int j = 0; j < k; ++j) if (pos[j] == pos[k]) { --k; break; } }
            int8_t s3[24]; std::memcpy(s3, s, 24);
            for (int k = 0; k < 3; ++k) s3[pos[k]] = int8_t(-s3[pos[k]] / 3);   // flipped, weakly
            if (m::golaySoft(s3) == d) ++ok3;
            int8_t s4[24]; std::memcpy(s4, s, 24);
            for (int k = 0; k < 4; ++k) s4[pos[k]] = int8_t(-s4[pos[k]] / 4);
            if (m::golaySoft(s4) == d) ++ok4;
        }
        check(ok3 == 2000 && ok4 > 1900, "Golay soft: three weak errors always, four weak errors (beyond hard decoding) nearly always",
              fmt("3: %d/2000, 4: %d/2000", ok3, ok4));
    }
    // ★ ideal symbols through softBits: LSF, LICH and stream payload come back exactly
    {
        uint8_t meta[14]; m::makeTextBlock("HELLO", 0, meta);
        const m::Lsf l = m::makeLsf("M17-M17 C", "G4ABC/P", m::streamType(2, 0, 5), meta);
        uint8_t pl[16]; for (int i = 0; i < 16; ++i) pl[i] = uint8_t(i * 17 + 3);
        const auto fr = m::streamFrame(l, 3, 0x0123, pl);
        float sym[184]; for (int i = 0; i < 184; ++i) sym[i] = fr[size_t(8 + i)];
        int8_t t3[368]; m::softBits(sym, 30.f, t3);
        uint8_t lich[6]; int cnt = -1, ge = 0;
        const bool lo = m::decodeLich(t3, lich, &cnt, &ge);
        const auto d = m::decodeStreamPayload(t3);
        const auto lf = m::lsfFrame(l);
        for (int i = 0; i < 184; ++i) sym[i] = lf[size_t(8 + i)];
        m::softBits(sym, 30.f, t3);
        const auto dl = m::decodeLsf(t3);
        m::Lsf back; std::memcpy(back.b, dl.bytes.data(), 30);
        check(lo && cnt == 3 && std::memcmp(lich, l.b + 15, 5) == 0 && d.bytes[0] == 0x01 && d.bytes[1] == 0x23
              && std::memcmp(d.bytes.data() + 2, pl, 16) == 0 && d.errors == 0 && back.crcOk()
              && m::decodeCallsign(back.src()) == "G4ABC/P" && m::decodeCallsign(back.dst()) == "M17-M17 C" && back.can() == 5,
              "symbols → bits: LICH chunk 3, FN 0x0123, the 16 payload bytes, the LSF with its callsigns and CAN 5");
    }
}

// ── 3. over the air ───────────────────────────────────────────────────────────────────────────────────────────────────
struct AirResult { m::Receiver::Counts n; std::string src, dst, status; size_t payloadMatch = 0, payloads = 0; double speechRms = 0; double ber = 0; };
static AirResult air(const std::vector<int8_t>& syms, double cnDb, double offHz, unsigned seed,
                     const std::vector<std::vector<uint8_t>>* ref = nullptr, size_t skipSyms = 0) {
    std::vector<int8_t> s(syms.begin() + long(std::min(skipSyms, syms.size())), syms.end());
    std::vector<cf> iq = withSilence(m::modulate(s, 48000, offHz), 48000, 0.3, 0.3);
    addNoise(iq, 48000, cnDb, seed);
    Rx rx; rx.feed(iq);
    AirResult r;
    r.n = rx.r.countsNow(); r.src = rx.r.src(); r.dst = rx.r.dst(); r.status = rx.r.statusJson(nullptr);
    r.speechRms = rms(rx.speech); r.ber = rx.r.ber();
    // ★ every payload played must be the very bits sent, at its frame number
    r.payloads = rx.payloads.size();
    if (ref) for (const auto& p : rx.payloads) if (size_t(p.first) < ref->size() && (*ref)[size_t(p.first)] == p.second) ++r.payloadMatch;
    return r;
}

static void overTheAir(bool haveVoice) {
    std::printf("\n── 3. over the air: our own transmitter, the RRC 4-FSK at ±2.4 kHz, AWGN in 9 kHz ──\n");
    const int frames = 75;   // 3 s
    const std::vector<short> sp = voice(3.2);
    const std::vector<int8_t> tx = m::voiceTransmission("@ALL", "G4ABC", frames, sp, 3);
    const std::vector<std::vector<uint8_t>> ref = referencePayloads(sp, frames);
    struct Case { double cn, off; } cases[] = { {30, 0}, {20, 0}, {16, 0}, {14, 0}, {12, 0}, {11, 0}, {10, 0}, {9, 0}, {8, 0}, {7, 0},
                                               {20, 1000}, {20, -1500}, {16, 700}, {12, 2000}, {12, -2500} };
    std::printf("      C/N   offset   LSF  stream  bad   voice  bit-exact  speech rms  BER%%\n");
    for (auto c : cases) {
        const AirResult r = air(tx, c.cn, c.off, 100 + unsigned(c.cn), &ref);
        std::printf("    %4.0f dB %+6.0f Hz  %3d   %4d  %4d   %4d     %4zu      %7.0f   %4.1f\n", c.cn, c.off, r.n.lsfOk, r.n.stream, r.n.streamBad,
                    r.n.voiceFrames, r.payloadMatch, r.speechRms, r.ber);
        const std::string tag = fmt("C/N %.0f dB, %+.0f Hz", c.cn, c.off);
        if (c.cn >= 12) {
            check(r.n.lsfOk == 1 && r.src == "G4ABC" && r.dst == "@ALL", (tag + ": the LSF, G4ABC → @ALL").c_str(), r.status);
            check(r.n.stream >= frames - 1 && r.n.transmissions == 1, (tag + ": every stream frame, one transmission").c_str(),
                  fmt("%d of %d, %d transmissions", r.n.stream, frames, r.n.transmissions));
            check(r.payloadMatch == r.payloads && r.payloads >= size_t(frames - 1),
                  (tag + ": every payload decoded is the very bits sent, at its frame number").c_str(),
                  fmt("%zu of %zu bit-exact", r.payloadMatch, r.payloads));
            if (haveVoice) check(r.n.voiceFrames >= frames - 1 && r.speechRms > 300, (tag + ": Codec 2 3200 speech out").c_str(),
                                 fmt("%d voice frames, rms %.0f", r.n.voiceFrames, r.speechRms));
            check(r.status.find("\"state\":\"search\"") != std::string::npos && r.status.find("\"last\":\"G4ABC\"") != std::string::npos,
                  (tag + ": after the end flag, searching again, the last station named").c_str(), r.status);
        } else if (c.cn >= 10) {
            check(r.n.lsfOk + r.n.lichLsf >= 1 && r.n.stream >= frames * 8 / 10 && r.payloadMatch == r.payloads,
                  (tag + ": still decodes most frames, and every one it plays is right").c_str(),
                  fmt("LSF %d, %d of %d frames, %zu of %zu bit-exact", r.n.lsfOk, r.n.stream, frames, r.payloadMatch, r.payloads));
        } else {
            // ★ near the FM threshold: voice frames carry no CRC (the spec gives them none), so a frame the Viterbi got
            //   slightly wrong is played as Codec 2 with a few bits wrong — what any M17 receiver does. Most must be right.
            check(r.payloads >= size_t(frames) * 9 / 10 && r.payloadMatch * 100 >= r.payloads * 85,
                  (tag + ": near the threshold — still nine frames in ten, and most bit-exact").c_str(),
                  fmt("%zu of %zu bit-exact, BER %.1f %%", r.payloadMatch, r.payloads, r.ber));
        }
    }
    // ★ the report while in sync: built from a transmission cut off half way (no end flag yet)
    {
        std::vector<int8_t> half(tx.begin(), tx.begin() + 192 * 30);
        std::vector<cf> iq = m::modulate(half, 48000, 300);
        addNoise(iq, 48000, 25, 9);
        Rx rx; rx.feed(withSilence(iq, 48000, 0.2, 0));
        const std::string st = rx.r.statusJson(nullptr);
        check(st.find("\"state\":\"sync\"") != std::string::npos && st.find("\"src\":\"G4ABC\"") != std::string::npos
              && st.find("\"dst\":\"@ALL\"") != std::string::npos && st.find("\"kind\":\"voice\"") != std::string::npos
              && st.find("\"can\":3") != std::string::npos && st.find("\"ber\":") != std::string::npos,
              "the report in sync: state, kind voice, src, dst, CAN, BER", st);
    }
    // ★ a late joiner: the preamble, the LSF and 7 frames missed — the LSF rebuilt from the LICH, the voice from chunk 2's TYPE
    {
        const AirResult r = air(tx, 20, 200, 77, &ref, 192 * 2 + 192 * 7);
        check(r.n.lsfOk == 0 && r.n.lichLsf == 1 && r.src == "G4ABC" && r.dst == "@ALL",
              "late join: no LSF heard, the LSF rebuilt from six LICH chunks (CRC good), G4ABC → @ALL", r.status);
        check(r.payloads >= size_t(frames - 7 - 2) && r.payloadMatch == r.payloads,
              "late join: the payloads from the second frame on, bit-exact", fmt("%zu of %zu", r.payloadMatch, r.payloads));
        if (haveVoice) check(r.n.voiceFrames >= frames - 7 - 3, "late join: the voice once chunk 2 gave the TYPE", fmt("%d voice frames", r.n.voiceFrames));
    }
    // ★ META: text over four superframes (two blocks), then GNSS, then the extended callsign
    {
        std::vector<std::vector<uint8_t>> blocks(2, std::vector<uint8_t>(14));
        const std::string msg = "VIBE TEST M17 META";   // 18 characters: two blocks
        m::makeTextBlock(msg, 0, blocks[0].data()); m::makeTextBlock(msg, 1, blocks[1].data());
        const auto t = m::voiceTransmission("@ALL", "M0ABC", 30, sp, 0, blocks[0].data(), 0, &blocks);
        const AirResult r = air(t, 22, 0, 5);
        check(r.status.find("\"text\":\"VIBE TEST M17 META\"") != std::string::npos, "META text: two blocks joined by the control byte", r.status);
        m::Gnss gn; gn.posValid = gn.altValid = gn.velValid = gn.radiusValid = true; gn.station = 1;
        gn.lat = 52.62345; gn.lon = -1.12345; gn.altM = 120.5; gn.speedKmh = 48.5; gn.bearing = 271; gn.radiusM = 8;
        uint8_t gm[14]; m::makeGnss(gn, gm);
        const m::Gnss back = m::parseGnss(gm);
        check(std::fabs(back.lat - gn.lat) < 2e-5 && std::fabs(back.lon - gn.lon) < 3e-5 && back.altM == 120.5 && back.speedKmh == 48.5
              && back.bearing == 271 && back.radiusM == 8 && back.station == 1, "GNSS META: the layout round trips (lat, lon, altitude, speed, bearing, radius)");
        const auto tg = m::voiceTransmission("@ALL", "M0ABC", 18, sp, 0, gm, 1);
        const AirResult rg = air(tg, 22, 0, 6);
        check(rg.status.find("\"gnss\":{\"lat\":52.6234") != std::string::npos && rg.status.find("\"lon\":-1.1234") != std::string::npos
              && rg.status.find("\"alt\":120.5") != std::string::npos && rg.status.find("\"brg\":271") != std::string::npos,
              "GNSS META over the air: the position in the report", rg.status);
        uint8_t xm[14] = {}; m::addrTo(m::encodeCallsign("G7XYZ"), xm); m::addrTo(m::encodeCallsign("M17-GBR A"), xm + 6);
        const auto tx2 = m::voiceTransmission("@ALL", "GB7AB", 12, sp, 0, xm, 2);
        const AirResult rx2 = air(tx2, 22, 0, 7);
        check(rx2.status.find("\"via\":\"G7XYZ via M17-GBR A\"") != std::string::npos && rx2.src == "GB7AB",
              "extended callsign: the originator and the reflector, through repeater GB7AB", rx2.status);
    }
    // ★ packets: an SMS, one frame and three frames long; another protocol counted, not shown as text
    {
        const auto p = m::packetTransmission("G4XYZ", "G4ABC", m::smsPacket("Hello from M17, 73"));
        const AirResult r = air(p, 18, -400, 11);
        check(r.n.packets == 1 && r.status.find("\"msg\":{\"id\":1,\"src\":\"G4ABC\",\"dst\":\"G4XYZ\",\"ptype\":5") != std::string::npos
              && r.status.find("\"text\":\"Hello from M17, 73\"") != std::string::npos, "SMS: one packet frame, the text and both callsigns", r.status);
        const std::string longMsg = "The quick brown fox jumps over the lazy dog. M17 packet mode carries SMS in up to 33 frames.";
        const auto p3 = m::packetTransmission("@ALL", "M0XYZ", m::smsPacket(longMsg));
        const AirResult r3 = air(p3, 18, 600, 12);
        check(r3.n.packets == 1 && r3.status.find(longMsg) != std::string::npos, "SMS: a 92-byte message across four packet frames", r3.status);
        std::vector<uint8_t> aprs{0x02}; for (int i = 0; i < 40; ++i) aprs.push_back(uint8_t('a' + i % 26));
        const AirResult ra = air(m::packetTransmission("APRS", "M0XYZ", aprs), 18, 0, 13);
        check(ra.n.packets == 1 && ra.status.find("\"ptype\":2,\"bytes\":41") != std::string::npos && ra.status.find("\"text\"") == std::string::npos,
              "another protocol (APRS, 0x02): counted with its size, no text shown", ra.status);
        // one bit wrong after the FEC cannot be made: damage the CRC instead (a packet whose CRC fails is not shown)
        std::vector<uint8_t> d = m::smsPacket("bad crc");
        auto frs = m::packetFrames(d);
        // re-make the last frame with its CRC byte changed
        std::vector<uint8_t> dd = d; const uint16_t c = m::crc16(dd.data(), dd.size()); dd.push_back(uint8_t(c >> 8)); dd.push_back(uint8_t(c ^ 1));
        uint8_t chunk[25] = {}; std::memcpy(chunk, dd.data(), dd.size());
        std::vector<int8_t> s = m::preamble(); const auto lf = m::lsfFrame(m::makeLsf("@ALL", "M0BAD", m::packetType(0), nullptr));
        s.insert(s.end(), lf.begin(), lf.end());
        const auto pf = m::packetFrame(chunk, true, int(dd.size()));
        s.insert(s.end(), pf.begin(), pf.end());
        const auto e = m::eotMarker(); s.insert(s.end(), e.begin(), e.end());
        const AirResult rb = air(s, 25, 0, 14);
        check(rb.n.packets == 0 && rb.status.find("\"msg\"") == std::string::npos, "a packet whose CRC fails is not shown", rb.status);
    }
    // ★ two transmissions one after the other, different stations: both named, two transmissions
    {
        std::vector<int8_t> both = m::voiceTransmission("@ALL", "G4ABC", 20, sp);
        const auto b2 = m::voiceTransmission("ECHO", "M0XYZ", 20, sp);
        both.insert(both.end(), 96, 0);   // (a gap of no deviation)
        both.insert(both.end(), b2.begin(), b2.end());
        const AirResult r = air(both, 20, 0, 15);
        check(r.n.transmissions == 2 && r.n.lsfOk == 2 && r.status.find("\"last\":\"M0XYZ\"") != std::string::npos,
              "two overs back to back: two transmissions, the second station last", r.status);
    }
}

// ── 4. noise only ─────────────────────────────────────────────────────────────────────────────────────────────────────
static void noiseOnly() {
    std::printf("\n── 4. noise only: no frame, no sync, no sound ──\n");
    const double sec = 180;
    Rx rx;
    std::mt19937 g(1234); std::normal_distribution<float> nd(0.f, 0.5f);
    std::vector<cf> blk(48000);
    bool everSync = false;
    for (int s = 0; s < int(sec); ++s) {
        for (auto& v : blk) v = cf(nd(g), nd(g));
        rx.feed(blk);
        if (rx.r.inSync()) everSync = true;
    }
    const auto n = rx.r.countsNow();
    check(n.frames == 0 && n.transmissions == 0 && !everSync && rx.speech.empty(),
          fmt("%.0f s of band noise: no frame, never in sync, no speech", sec).c_str(),
          fmt("%ld sync candidates looked at, %d frames, %d transmissions, %zu speech samples", n.candidates, n.frames, n.transmissions, rx.speech.size()));
    // ★ and a carrier with no modulation, and a voice FM signal (NFM speech is not M17)
    Rx rc;
    std::vector<cf> fmv(48000 * 30);
    double ph = 0;
    const std::vector<short> sp = voice(30.0);
    for (size_t i = 0; i < fmv.size(); ++i) {
        const double a = double(sp[std::min(sp.size() - 1, i / 6)]) / 12000.0;
        ph += 2 * M_PI * (a * 2500.0) / 48000.0;
        fmv[i] = cf(float(std::cos(ph)), float(std::sin(ph)));
    }
    addNoise(fmv, 48000, 20, 3);
    rc.feed(fmv);
    const auto nc = rc.r.countsNow();
    check(nc.frames == 0 && rc.speech.empty(), "30 s of an analogue NFM voice: not taken for M17", fmt("%ld candidates, %d frames", nc.candidates, nc.frames));
}

// ── 5. the threaded listener, and the server's path ───────────────────────────────────────────────────────────────────
static void listenerAndPipeline(bool haveVoice) {
    std::printf("\n── 5. the threaded listener, and IQ → RxPipeline NFM → the listener's channel IQ ──\n");
    const std::vector<short> sp = voice(4.2);
    const auto tx = m::voiceTransmission("@ALL", "G4ABC", 100, sp);
    std::vector<cf> iq = withSilence(m::modulate(tx, 48000, 0), 48000, 0.5, 1.0);
    addNoise(iq, 48000, 22, 21);
    {   // ★ the cost, for the docs: one receiver (no thread) on this IQ, against real time
        Rx rx;
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(iq);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   (cost: %.2f %% of one core for one listener decoding a voice over, 48 kHz channel IQ, %s ACS)\n",
                    100.0 * wall / (double(iq.size()) / 48000.0), m::acsName());
    }
    {
        m::M17Listener L;
        std::vector<float> out(480);
        double e = 0; size_t nz = 0, tot = 0, silentBefore = 0;
        for (size_t i = 0; i + 480 <= iq.size(); i += 480) {
            L.feedIq(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), 480, 48000.0);
            while (L.queuedFrames() > 9600) std::this_thread::sleep_for(std::chrono::microseconds(200));
            L.takePcm(out.data(), 480);
            for (float v : out) { e += double(v) * v; if (v != 0.f) ++nz; }
            if (i < 48000 / 4 && nz == 0) silentBefore += 480;
            tot += 480;
        }
        while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::string st = L.statusJson(nullptr);
        if (haveVoice) check(L.voiceFrames() >= 98 && std::sqrt(e / double(tot)) > 0.01 && L.droppedBlocks() == 0,
                             "listener: voice frames decoded, speech out at 48 kHz, nothing dropped",
                             fmt("%d voice frames, rms %.3f, audible %.0f %%, dropped %llu", L.voiceFrames(), std::sqrt(e / double(tot)), 100.0 * double(nz) / double(tot), (unsigned long long)L.droppedBlocks()));
        check(silentBefore >= 48000 / 4, "listener: silence before the transmission (nothing until there is speech)");
        check(st.find("\"last\":\"G4ABC\"") != std::string::npos, "listener: the report", st);
    }
    // ★ the server's path: a 240 kHz capture, the station 37 kHz off the capture's centre, through RxPipeline in NFM — on the
    //   dial, and 1.5 kHz off it (a radio's clock error at 70 cm is a few hundred Hz a ppm: the 12.5 kHz channel must hold it)
    for (const double errHz : {0.0, 1500.0, -2000.0}) {
        const int fs = 240000;
        std::vector<float> I(iq.size()), Q(iq.size());
        for (size_t i = 0; i < iq.size(); ++i) { I[i] = iq[i].real(); Q[i] = iq[i].imag(); }
        vibedsp::RationalResampler ui(48000, fs), uq(48000, fs);
        std::vector<float> Iu(size_t(ui.maxOut(int(I.size())))), Qu(Iu.size());
        const int n = ui.process(I.data(), int(I.size()), Iu.data());
        uq.process(Q.data(), int(Q.size()), Qu.data());
        std::vector<cf> wide(static_cast<size_t>(n));
        const double off = 37000;
        for (int i = 0; i < n; ++i) { const double ph = 2 * M_PI * (off + errHz) * i / fs; wide[size_t(i)] = cf(Iu[size_t(i)], Qu[size_t(i)]) * cf(float(std::cos(ph)), float(std::sin(ph))); }
        std::mt19937 rr(8); std::normal_distribution<float> nd(0.f, 0.02f);
        for (auto& v : wide) v += cf(nd(rr), nd(rr));
        m::M17Listener L;
        struct Ctx { m::M17Listener* L; double e = 0; size_t tot = 0; double rate = 0; } ctx{&L};
        vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
        cb.iq = [](void* c, const vibedsp::cf32* x, int k, double rate) { auto* p = static_cast<Ctx*>(c); p->rate = rate; p->L->feedIq(x, k, rate); };
        cb.audio = [](void* c, const float*, int frames, int, int) {
            auto* x = static_cast<Ctx*>(c);
            std::vector<float> o(static_cast<size_t>(frames));
            while (x->L->queuedFrames() > 48000) std::this_thread::sleep_for(std::chrono::microseconds(200));
            x->L->takePcm(o.data(), frames);
            for (float v : o) x->e += double(v) * v;
            x->tot += size_t(frames);
        };
        vibedsp::RxPipeline p; p.setMeasureThread(false);
        p.start(double(fs), 1024, 20.0, 48000, cb);
        p.setTune(off, vibedsp::RxPipeline::Mode::NFM, 12500);
        const int blk = fs / 50;
        for (int i = 0; i + blk <= n; i += blk) p.feed(reinterpret_cast<const vibedsp::cf32*>(wide.data() + i), blk);
        while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string st = L.statusJson(nullptr);
        check(st.find("\"last\":\"G4ABC\"") != std::string::npos && (!haveVoice || L.voiceFrames() >= 95),
              fmt("RxPipeline NFM (channel IQ at %.0f Hz), the station %+.0f Hz off the dial: decoded through the server's own chain", ctx.rate, errHz).c_str(),
              fmt("%d voice frames, out rms %.3f  %s", L.voiceFrames(), std::sqrt(ctx.e / double(std::max<size_t>(1, ctx.tot))), st.c_str()));
    }
}

// ── the bench clip ────────────────────────────────────────────────────────────────────────────────────────────────────
/* ★ scripts/test-server-m17.mjs: unsigned 8-bit IQ (rtl_tcp's format) at `rate`, `sec` long, centred on the fake dongle's
 *  --hf-centre. One station 25 kHz above the centre: a voice over from G4ABC to @ALL (4 s, META text "VIBE TEST"), 1.5 s
 *  quiet, an SMS from G4ABC to M0XYZ ("Hello from VibeServer"), quiet to the end — the clip loops, so it repeats. */
static int writeClip(const char* path, int rate, double sec, double cnDb) {
    if (rate % 48000) { std::printf("the rate must be a multiple of 48 kHz\n"); return 2; }
    const std::vector<short> sp = voice(4.2);
    uint8_t meta[14]; m::makeTextBlock("VIBE TEST", 0, meta);
    std::vector<int8_t> s = m::voiceTransmission("@ALL", "G4ABC", 100, sp, 0, meta, 0);
    s.insert(s.end(), size_t(1.5 * 4800), 0);
    const auto p = m::packetTransmission("M0XYZ", "G4ABC", m::smsPacket("Hello from VibeServer"));
    s.insert(s.end(), p.begin(), p.end());
    std::vector<cf> b = m::modulate(s, 48000, 0);
    // ★ an unmodulated gap is still a carrier; the station keys up and down, so the quiet parts carry no signal at all
    {
        size_t on = 0;
        const size_t voiceEnd = size_t(192 * (2 + 100 + 1)) * 10, pktStart = voiceEnd + size_t(1.5 * 48000);
        (void)on;
        for (size_t i = voiceEnd; i < pktStart && i < b.size(); ++i) b[i] = cf(0, 0);
    }
    const int n = int(sec * rate);
    std::vector<float> I(b.size()), Q(b.size());
    for (size_t i = 0; i < b.size(); ++i) { I[i] = b[i].real(); Q[i] = b[i].imag(); }
    vibedsp::RationalResampler ui(48000, rate), uq(48000, rate);
    std::vector<float> Iu(size_t(ui.maxOut(int(I.size())))), Qu(Iu.size());
    const int nu = ui.process(I.data(), int(I.size()), Iu.data());
    uq.process(Q.data(), int(Q.size()), Qu.data());
    std::vector<cf> iq(static_cast<size_t>(n));
    const double off = 25000;
    for (int i = 0; i < n && i < nu; ++i) {
        const double ph = 2 * M_PI * off * i / rate;
        iq[size_t(i)] = cf(Iu[size_t(i)], Qu[size_t(i)]) * cf(float(std::cos(ph)), float(std::sin(ph)));
    }
    // complex noise over `rate` for C/N `cnDb` in 9 kHz
    const double var = double(rate) / 9000.0 / std::pow(10.0, cnDb / 10.0);
    std::mt19937 rng(21); std::normal_distribution<float> nd(0.f, float(std::sqrt(var / 2)));
    double peak = 0;
    for (auto& x : iq) { x += cf(nd(rng), nd(rng)); peak = std::max(peak, double(std::max(std::fabs(x.real()), std::fabs(x.imag())))); }
    const double k = 100.0 / peak;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::vector<uint8_t> u(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        u[size_t(2 * i)] = uint8_t(std::lrint(127.5 + k * iq[size_t(i)].real()));
        u[size_t(2 * i + 1)] = uint8_t(std::lrint(127.5 + k * iq[size_t(i)].imag()));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    std::printf("wrote %s: %d S/s, %.0f s, M17 at +25 kHz (G4ABC voice 4 s with text, then an SMS), C/N %.0f dB in 9 kHz\n", path, rate, sec, cnDb);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--probe") { std::printf("%d\n", m::available() ? 1 : 0); return 0; }
    if (argc > 2 && std::string(argv[1]) == "clip")
        return writeClip(argv[2], argc > 3 ? std::atoi(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 24, argc > 5 ? std::atof(argv[5]) : 20);
    const bool haveVoice = m::available();
    std::printf("M17 — ACS kernel %s; libcodec2 vocoder: %s\n", m::acsName(), haveVoice ? "loaded (3200 / 1600)" : vibe::freedv::vocoder().why.c_str());
    specVectors();
    fecAlone();
    overTheAir(haveVoice);
    noiseOnly();
    listenerAndPipeline(haveVoice);
    if (haveVoice) {
        std::printf("\n── 6. without the library ──\n");
        std::string cmd = std::string("VIBE_CODEC2_DISABLE=1 '") + argv[0] + "' --probe";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[16] = {};
        if (p) { if (!std::fgets(buf, sizeof buf, p)) buf[0] = 0; pclose(p); }
        check(buf[0] == '0', "VIBE_CODEC2_DISABLE=1: the mode is not available", buf);
    }
    std::printf("\n%d passed, %d failed%s\n", g_pass, g_fail, haveVoice ? "" : " — the voice checks NOT RUN (no libcodec2)");
    if (g_fail) return 1;
    return haveVoice ? 0 : 3;
}
