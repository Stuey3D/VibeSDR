// test-balloon.cpp — V12 balloons: UKHAS RTTY telemetry, Horus Binary, SSDV pictures (decoders/balloon_*.h,
// vibe_balloon_run.h, docs/v12/BALLOONS.md). EVERYTHING HERE IS SYNTHETIC: our own transmitter (balloon_tx.h) from the
// formats, into our own receiver. Silent — nothing is played.
//   1. The checks: CRC-16-CCITT and CRC-32 on their published check value ("123456789").
//   2. UKHAS sentences: parsed, fields, a wrong CRC refused, the XOR form refused, the framer on a noisy stream.
//   3. Golay(23,12): our check bits are the cyclic code of 0xC75 (an independent long division, all 4096 words), every
//      syndrome has one pattern of ≤ 3 errors, every such pattern is corrected, Chase corrects 4 when the soft values say
//      which.
//   4. Horus coding: v1 45 / v2 65 bytes on air, interleaver + scrambler undo themselves, a bit-exact round trip of both
//      versions, 3 errors in every codeword corrected, the fields.
//   5. Horus on air: our 4-FSK into the receiver — tones anywhere in the passband, 244/270/400 Hz spacing, the tones
//      reversed (LSB), a 0.3 % baud error, noise down to the threshold (reported), every packet bit-exact.
//   6. UKHAS RTTY on air: 50 / 100 / 300 / 600 baud, 7 or 8 bits, 1 / 2 stop bits, 350–850 Hz shift, reversed, off centre;
//      each pinned by hand too (the manual controls under AUTO).
//   7. SSDV: callsign, RS(255,223) corrects 16 and refuses beyond, our packets parsed back, the JPEG's coefficients equal
//      what went in (every subsampling mode, no-FEC too), lost packets leave empty blocks and nothing else changes.
//   8. SSDV over 600 Bd 8N2 RTTY into the receiver: the whole picture, exactly.
//   9. FALSE POSITIVES: five minutes of noise, three of ordinary RTTY (Baudot 45/170, and 8-bit text and random bytes at
//      300 Bd) — not one sentence, packet or picture packet.
//  10. SIMD: the front end's and the tone bank's SIMD paths equal the scalar ones; the cost in real time.
//   `test-balloon clip <out.cu8> [rate] [seconds]` writes the end-to-end tests' clip (see writeClip).
#include "decoders/balloon_rx.h"
#include "decoders/balloon_tx.h"
// (vibe_balloon_run.h: tested end to end)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibe::balloon;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static void section(const char* s) { std::printf("── %s ──\n", s); }

/** White noise σ (at 48 kHz) for a tone of amplitude `amp` at `snrDb` in 3 kHz. */
static double sigmaFor(double amp, double snrDb) { return std::sqrt(4.0 * amp * amp / std::pow(10.0, snrDb / 10)); }
static void addNoise(std::vector<float>& a, double sigma, unsigned seed) {
    std::mt19937 r(seed); std::normal_distribution<float> g(0.f, (float)sigma);
    for (float& v : a) v += g(r);
}
struct Got { std::vector<Telemetry> t; std::vector<ssdv::Packet> p; };
static Got run(const std::vector<float>& audio, const Config& c = Config(), Receiver** keep = nullptr) {
    Got g;
    auto* rx = new Receiver(c);
    rx->onTelemetry = [&](const Telemetry& t) { g.t.push_back(t); };
    rx->onImagePacket = [&](ssdv::Image&, const ssdv::Packet& p) { g.p.push_back(p); };
    for (size_t i = 0; i < audio.size(); i += 960) rx->feed48k(audio.data() + i, (int)std::min<size_t>(960, audio.size() - i));
    if (keep) *keep = rx; else delete rx;
    return g;
}

// ── 1, 2 ────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void testChecks() {
    section("1. CRC check values");
    const uint8_t s[] = "123456789";
    CHECK(crc16(s, 9) == 0x29B1, "CRC-16-CCITT('123456789') = %04X, want 29B1", crc16(s, 9));
    CHECK(ssdv::crc32(s, 9) == 0xCBF43926u, "CRC-32('123456789') = %08X, want CBF43926", ssdv::crc32(s, 9));
    section("2. UKHAS sentences");
    const std::string line = tx::sentence("VIBE1", 123, 12, 34, 56, 52.12345, -1.23456, 12345);
    Telemetry t;
    CHECK(parseSentence(line, t) == 1, "a sentence of our own parses: %s", line.c_str());
    CHECK(t.callsign == "VIBE1" && t.count == 123 && t.hh == 12 && t.mm == 34 && t.ss == 56, "callsign, count, time");
    CHECK(std::fabs(t.lat - 52.12345) < 1e-9 && std::fabs(t.lon + 1.23456) < 1e-9 && t.alt == 12345, "position %.5f %.5f %.0f", t.lat, t.lon, t.alt);
    CHECK(t.extra.size() == 2 && t.extra[0] == "9" && t.extra[1] == "-12", "the fields after the altitude kept (%zu)", t.extra.size());
    std::string bad = line; bad[8] = bad[8] == '2' ? '3' : '2';
    CHECK(parseSentence(bad, t) == -1, "one character changed: the CRC refuses it");
    CHECK(parseSentence("$$VIBE1,1,12:00:00,52.0,-1.0,100*5A\n", t) <= 0, "the two-digit XOR checksum is not accepted");
    CHECK(parseSentence("RYRYRY just text, no sentence\n", t) == 0, "plain text is not a sentence");
    // the framer on a stream with junk around and inside
    SentenceFramer f; int got = 0;
    f.onSentence = [&](const Telemetry&) { ++got; };
    const std::string stream = "RYRYRY $ $$$" + line + "garbage$$NOT,A,SENTENCE*ZZZZ\n" + tx::sentence("VIBE1", 124, 12, 35, 6, 52.1, -1.2, 12400) + line.substr(0, 20);
    for (char c : stream) f.put(c);
    CHECK(got == 2 && f.bad >= 1, "framer: 2 sentences from the stream (got %d), the junk one counted bad (%lu)", got, f.bad);
    // the 0,0 position is no fix
    CHECK(parseSentence(tx::sentence("VIBE1", 5, 1, 2, 3, 0, 0, 0), t) == 1 && std::isnan(t.lat), "0,0 is read as no fix");
}

// ── 3, 4 ────────────────────────────────────────────────────────────────────────────────────────────────────────────
static uint16_t polyRem(uint16_t d) {   // independent: d·x¹¹ mod g(x), g = x¹¹+x¹⁰+x⁶+x⁵+x⁴+x²+1 (0xC75)
    uint32_t r = (uint32_t)d << 11;
    for (int b = 22; b >= 11; --b) if (r & (1u << b)) r ^= 0xC75u << (b - 11);
    return (uint16_t)r;
}
static void testGolay() {
    section("3. Golay(23,12)");
    int bad = 0;
    for (int d = 0; d < 4096; ++d) if (horus::parity11((uint16_t)d) != polyRem((uint16_t)d)) ++bad;
    CHECK(bad == 0, "our check bits = the long division by 0xC75 for all 4096 words (%d differ)", bad);
    const auto& tab = horus::golay23Table();
    int unset = 0; for (uint32_t e : tab) if (e == 0xFFFFFFFFu) ++unset;
    CHECK(unset == 0, "every one of the 2048 syndromes has its ≤ 3-error pattern (a perfect code): %d without", unset);
    std::mt19937 r(5); int wrong = 0, tried = 0;
    for (int n = 0; n < 20000; ++n) {
        const uint16_t d = (uint16_t)(r() & 0xFFF);
        uint32_t cw = ((uint32_t)d << 11) | horus::parity11(d);
        const int ne = 1 + (int)(r() % 3);
        for (int k = 0; k < ne; ++k) cw ^= 1u << (r() % 23);
        ++tried; if (horus::golay23Hard(cw) != d) ++wrong;
    }
    CHECK(wrong == 0, "1–3 random bit errors corrected in %d words (%d wrong)", tried, wrong);
    // Chase: 4 errors on the 4 least sure bits
    wrong = 0;
    for (int n = 0; n < 2000; ++n) {
        const uint16_t d = (uint16_t)(r() & 0xFFF);
        const uint32_t cw = ((uint32_t)d << 11) | horus::parity11(d);
        float s[23];
        for (int i = 0; i < 23; ++i) s[i] = ((cw >> (22 - i)) & 1) ? 1.f : -1.f;
        int pos[4]; for (int k = 0; k < 4; ++k) { pos[k] = (int)(r() % 23); for (int j = 0; j < k; ++j) if (pos[j] == pos[k]) { --k; break; } }
        for (int k = 0; k < 4; ++k) s[pos[k]] = -0.1f * s[pos[k]];
        if (horus::golay23Soft(s) != d) ++wrong;
    }
    CHECK(wrong == 0, "soft (Chase): 4 errors where the soft values are least sure, corrected (%d wrong of 2000)", wrong);
}
static std::vector<float> softOf(const std::vector<uint8_t>& f) {
    std::vector<float> s;
    for (size_t i = 2; i < f.size(); ++i) for (int b = 7; b >= 0; --b) s.push_back(((f[i] >> b) & 1) ? 1.f : -1.f);
    return s;
}
static void testHorusCoding() {
    section("4. Horus Binary coding");
    CHECK(horus::txBytes(22) == 45 && horus::txBytes(32) == 65, "v1 45 / v2 65 bytes on air (%d / %d)", horus::txBytes(22), horus::txBytes(32));
    std::mt19937 r(9);
    for (int nb : { 43, 63 }) {
        std::vector<uint8_t> a((size_t)nb), b;
        for (auto& v : a) v = (uint8_t)r();
        b = a; horus::interleave(b.data(), nb, false); horus::interleave(b.data(), nb, true);
        CHECK(a == b, "%d bytes: interleave then de-interleave is nothing", nb);
        b = a; horus::scramble(b.data(), nb);
        CHECK(a != b, "%d bytes: the scrambler changes the frame", nb);
        horus::scramble(b.data(), nb);
        CHECK(a == b, "%d bytes: scrambled twice is nothing", nb);
    }
    for (int ver : { 1, 2 }) {
        const auto pkt = tx::horusPacket(ver, ver == 1 ? 7 : 256, 4321, 13, 14, 15, -34.95f, 138.52f, 31234);
        const auto f = horus::encodeFrame(pkt.data(), (int)pkt.size());
        CHECK(f[0] == 0x24 && f[1] == 0x24, "v%d: the frame starts with the UW $$", ver);
        auto s = softOf(f);
        uint8_t out[32]; int fixed = -1;
        horus::decodeSoft(s.data(), (int)pkt.size(), out, &fixed);
        CHECK(std::memcmp(out, pkt.data(), pkt.size()) == 0 && fixed == 0, "v%d: bit-exact round trip, nothing corrected (%d)", ver, fixed);
        // 3 errors in every codeword's 23 bits — placed in the data and check sections of the stream before interleaving is
        // undone is hard to aim, so: flip 3 random bits per 23 of the de-interleaved word by encoding errors into the payload
        // side directly — the decoder must undo the scrambler and interleaver to find them.
        std::vector<uint8_t> g = f;
        const int nbits = ((int)g.size() - 2) * 8;
        // errors into the frame BEFORE interleave/scramble = at the codewords: rebuild from the raw coder
        std::vector<uint8_t> raw((size_t)horus::txBytes((int)pkt.size()), 0);
        {   // the raw (un-interleaved) frame: undo our own interleave + scramble
            raw = f; horus::scramble(raw.data() + 2, (int)raw.size() - 2); horus::interleave(raw.data() + 2, (int)raw.size() - 2, true);
        }
        const int n = (int)pkt.size(), bits = n * 8, cws = (bits + 11) / 12;
        for (int c = 0; c < cws; ++c) {
            // this codeword's data bits are stream bits c·12 … and its checks start at bits + c·11 (MSB-first numbering)
            const int r12 = std::min(12, bits - c * 12);
            std::vector<int> pos;
            for (int i = 0; i < r12; ++i) pos.push_back(16 + c * 12 + i);
            for (int i = 0; i < 11; ++i) pos.push_back(16 + bits + c * 11 + i);
            std::shuffle(pos.begin(), pos.end(), r);
            for (int k = 0; k < 3; ++k) raw[(size_t)(pos[(size_t)k] >> 3)] ^= (uint8_t)(0x80 >> (pos[(size_t)k] & 7));
        }
        horus::interleave(raw.data() + 2, (int)raw.size() - 2, false); horus::scramble(raw.data() + 2, (int)raw.size() - 2);
        s = softOf(raw);
        horus::decodeSoft(s.data(), n, out, &fixed);
        CHECK(std::memcmp(out, pkt.data(), pkt.size()) == 0 && fixed == 3 * cws, "v%d: 3 errors in each of the %d codewords corrected (%d bits)", ver, cws, fixed);
        (void)nbits;
        Telemetry t;
        CHECK(horus::parsePacket(pkt.data(), (int)pkt.size(), t), "v%d: the packet parses", ver);
        CHECK(t.horusId == (ver == 1 ? 7 : 256) && t.count == 4321 && t.hh == 13 && t.mm == 14 && t.ss == 15 && std::fabs(t.lat + 34.95) < 1e-4
              && std::fabs(t.lon - 138.52) < 1e-4 && t.alt == 31234 && t.speedKmh == 42 && t.sats == 11 && t.tempC == -31
              && std::fabs(t.battV - 150 * 5.0 / 255) < 1e-9, "v%d: every field", ver);
        CHECK(ver == 1 || t.custom == "101112131415161718", "v2: the custom bytes (%s)", t.custom.c_str());
        Telemetry u; CHECK(parseSentence(t.sentence, u) == 1 && u.callsign == t.callsign, "v%d: written out as a valid UKHAS sentence: %s", ver, t.sentence.c_str());
        std::vector<uint8_t> bad = pkt; bad[5] ^= 1;
        CHECK(!horus::parsePacket(bad.data(), (int)bad.size(), t), "v%d: a changed byte fails the CRC", ver);
    }
}

// ── 5 ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<float> horusAudio(int packets, double f0, double sp, double snrDb, unsigned seed, double baud = 100, int ver = 2) {
    tx::Keying k;
    tx::silence(k, 0.5);
    for (int i = 0; i < packets; ++i) {
        tx::horus(tx::horusPacket(ver, 256, 100 + i, 10, 0, i % 60, 51.5f + i * 0.001f, -0.12f, 10000 + 100 * i), f0, sp, k, baud);
        tx::silence(k, 0.7);
    }
    std::vector<float> a; double ph = 0.3;
    const float amp = 0.1f;
    tx::render(k, 48000, amp, a, ph);
    addNoise(a, sigmaFor(amp, snrDb), seed);
    return a;
}
static int countHorus(const Got& g, int packets, int ver = 2) {
    int ok = 0;
    for (int i = 0; i < packets; ++i)
        for (const auto& t : g.t)
            if (t.source == "horus" && t.horusVersion == ver && t.count == 100 + i && t.alt == 10000 + 100 * i && std::fabs(t.lat - (51.5 + i * 0.001)) < 1e-4) { ++ok; break; }
    return ok;
}
static void testHorusAir() {
    section("5. Horus Binary on air (our 4-FSK, our receiver)");
    struct C { double f0, sp, snr, baud; const char* what; };
    const C cs[] = { { 1100, 270, 15, 100, "270 Hz spacing at 1100 Hz, 15 dB" }, { 450, 270, 15, 100, "low in the passband (450 Hz)" },
                     { 2000, 244, 15, 100, "244 Hz spacing at 2 kHz" }, { 700, 400, 15, 100, "400 Hz spacing" },
                     { 1100 + 3 * 270, -270, 15, 100, "the tones reversed (an LSB listener)" }, { 1000, 270, 15, 100.3, "a 0.3 % fast symbol clock" } };
    for (const C& c : cs) {
        const int N = 6;
        const Got g = run(horusAudio(N, c.f0, c.sp, c.snr, 11), Config());
        const int ok = countHorus(g, N);
        int other = 0; for (const auto& t : g.t) if (t.source != "horus") ++other;
        CHECK(ok == N && (int)g.t.size() == N, "%s: %d of %d packets bit-exact, %zu reports", c.what, ok, N, g.t.size());
        (void)other;
    }
    { const Got g = run(horusAudio(4, 1200, 270, 15, 12, 100, 1)); CHECK(countHorus(g, 4, 1) == 4, "v1 packets (22 bytes): %d of 4", countHorus(g, 4, 1)); }
    // pinned by hand: spacing 270 given, baud given
    { Config c; c.horusSpacing = 270; c.horusBaud = 100; const Got g = run(horusAudio(4, 900, 270, 15, 13), c);
      CHECK(countHorus(g, 4) == 4, "manual: spacing 270 Hz, 100 Bd pinned: %d of 4", countHorus(g, 4)); }
    { Config c; c.horusSpacing = 400; const Got g = run(horusAudio(4, 900, 270, 15, 13), c);
      CHECK(countHorus(g, 4) == 0, "manual: a WRONG spacing pinned (400 Hz for a 270 Hz signal) decodes nothing (%d)", countHorus(g, 4)); }
    // threshold
    std::printf("   threshold (20 v2 packets, SNR in 3 kHz):");
    int at6 = 0;
    for (double snr : { 6.0, 2.0, 0.0, -2.0, -4.0, -6.0, -8.0 }) {
        const Got g = run(horusAudio(20, 1000, 270, snr, 100 + (unsigned)(snr + 10)));
        const int ok = countHorus(g, 20);
        std::printf("  %+.0f dB %d/20", snr, ok);
        if (snr == 6.0) at6 = ok;
        int falses = 0; for (const auto& t : g.t) if (!(t.count >= 100 && t.count < 120)) ++falses;
        CHECK(falses == 0, "no packet that was not sent (%d at %+.0f dB)", falses, snr);
    }
    std::printf("\n");
    CHECK(at6 >= 19, "at 6 dB in 3 kHz at least 19 of 20 (%d)", at6);
}

// ── 6 ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<float> rttyAudio(const tx::Rtty& r, int sentences, double snrDb, unsigned seed, const std::string& call = "VIBE1") {
    tx::Keying k;
    for (int i = 0; i < sentences; ++i) {
        const std::string s = tx::sentence(call, 200 + i, 9, 30, i, 50.1 + i * 0.01, -2.5, 20000 + i);
        tx::rtty(tx::bytesOf(s), r, k, i == 0 ? 2.0 : 0.25);
    }
    tx::rtty({}, r, k, 1.0);
    std::vector<float> a; double ph = 0;
    const float amp = 0.1f;
    tx::render(k, 48000, amp, a, ph);
    addNoise(a, sigmaFor(amp, snrDb), seed);
    return a;
}
static int countUkhas(const Got& g, int n, const std::string& call = "VIBE1") {
    int ok = 0;
    for (int i = 0; i < n; ++i)
        for (const auto& t : g.t)
            if (t.source == "ukhas" && t.callsign == call && t.count == 200 + i && t.alt == 20000 + i) { ++ok; break; }
    return ok;
}
static void testRttyAir() {
    section("6. UKHAS RTTY on air (our RTTY, the RTTY decoder's bank)");
    struct C { double baud, shift, centre, stop; int bits; bool rev; const char* what; };
    const C cs[] = { { 50, 425, 1500, 2, 7, false, "50 Bd 7N2, 425 Hz" }, { 50, 470, 1200, 1, 8, false, "50 Bd 8N1, 470 Hz" },
                     { 100, 350, 1700, 2, 7, false, "100 Bd 7N2, 350 Hz" }, { 300, 600, 1500, 2, 8, false, "300 Bd 8N2, 600 Hz" },
                     { 300, 425, 1100, 1, 7, false, "300 Bd 7N1, 425 Hz" }, { 600, 850, 1600, 2, 8, false, "600 Bd 8N2, 850 Hz" },
                     { 300, 600, 1500, 2, 8, true, "300 Bd 8N2 reversed" }, { 50, 425, 2400, 2, 7, false, "50 Bd high in the passband (2.4 kHz)" } };
    for (const C& c : cs) {
        tx::Rtty r; r.baud = c.baud; r.shift = c.shift; r.centre = c.centre; r.stop = c.stop; r.bits = c.bits; r.reverse = c.rev;
        const int N = c.baud >= 300 ? 6 : 3;
        Receiver* rx = nullptr;
        const Got g = run(rttyAudio(r, N, 18, 21), Config(), &rx);
        const int ok = countUkhas(g, N);
        CHECK(ok == N && (int)g.t.size() == N, "%s: %d of %d sentences, %zu reports, named \"%s\"", c.what, ok, N, g.t.size(), rx->rttySettings().c_str());
        CHECK(rx->rttySettings().find(std::to_string((int)c.baud) + " Bd") == 0, "%s: …and the settings named are its speed", c.what);
        delete rx;
        // ★ the manual controls: everything pinned to what it is
        Config m; m.rttyBaud = c.baud; m.rttyBits = c.bits; m.rttyReverse = c.rev ? 1 : 0; m.rttyShift = c.shift;
        const Got h = run(rttyAudio(r, N, 18, 22), m);
        CHECK(countUkhas(h, N) == N, "%s, pinned by hand: %d of %d", c.what, countUkhas(h, N), N);
    }
    {   // a wrong speed pinned decodes nothing
        tx::Rtty r; r.baud = 300; r.shift = 600;
        Config m; m.rttyBaud = 50;
        const Got h = run(rttyAudio(r, 4, 18, 23), m);
        CHECK(h.t.empty(), "a WRONG speed pinned (50 Bd for 300 Bd) decodes nothing (%zu)", h.t.size());
    }
    // a weaker 300 Bd signal: report
    std::printf("   300 Bd 8N2 600 Hz, 8 sentences, SNR in 3 kHz:");
    for (double snr : { 12.0, 9.0, 6.0, 3.0 }) {
        tx::Rtty r; r.baud = 300; r.shift = 600;
        const Got g = run(rttyAudio(r, 8, snr, 30 + (unsigned)snr));
        std::printf("  %+.0f dB %d/8", snr, countUkhas(g, 8));
        int falses = 0; for (const auto& t : g.t) if (t.count < 200 || t.count >= 208) ++falses;
        CHECK(falses == 0, "no sentence that was not sent at %+.0f dB", snr);
    }
    std::printf("\n");
}

// ── 7, 8 ────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** A test-side JPEG reader (baseline, our tables): the absolute DC and the AC terms of every block, in order. */
static bool readJpeg(const std::vector<uint8_t>& j, int yc, int mcus, std::vector<std::vector<int16_t>>& blocks) {
    size_t p = 2, sos = 0;
    while (p + 4 < j.size()) {
        if (j[p] != 0xFF) return false;
        const int mk = j[p + 1], len = j[p + 2] << 8 | j[p + 3];
        if (mk == 0xDA) { sos = p + 2 + (size_t)len; break; }
        p += 2 + (size_t)len;
    }
    if (!sos) return false;
    std::vector<uint8_t> d;
    for (size_t i = sos; i + 1 < j.size(); ++i) {
        if (j[i] == 0xFF) { if (j[i + 1] == 0x00) { d.push_back(0xFF); ++i; continue; } break; }
        d.push_back(j[i]);
    }
    ssdv::BitReader r(d.data(), d.size());
    int pred[3] = { 0, 0, 0 };
    blocks.clear();
    for (int m = 0; m < mcus; ++m)
        for (int part = 0; part < yc + 2; ++part) {
            const int comp = part < yc ? 0 : part - yc + 1;
            const auto& hd = ssdv::huffTable(comp ? 1 : 0); const auto& ha = ssdv::huffTable(comp ? 3 : 2);
            std::vector<int16_t> c(64, 0);
            int s; uint32_t v = 0;
            if (!r.sym(hd, s) || !r.get(s, v)) return false;
            pred[comp] += ssdv::extend(v, s); c[0] = (int16_t)pred[comp];
            for (int k = 1; k < 64;) {
                int rs; if (!r.sym(ha, rs)) return false;
                const int run = rs >> 4, sz = rs & 15;
                if (sz == 0 && run != 15) break;
                if (sz == 0) { k += 16; continue; }
                k += run;
                if (!r.get(sz, v) || k > 63) return false;
                c[(size_t)k++] = (int16_t)ssdv::extend(v, sz);
            }
            blocks.push_back(c);
        }
    return true;
}
static void testSsdv() {
    section("7. SSDV");
    for (const char* c : { "VIBE1", "M0ABC", "2E0XYZ", "A" }) CHECK(ssdv::decodeCall(ssdv::encodeCall(c)) == c, "callsign %s round trip", c);
    const auto& rs = ssdv::Rs::get();
    bool pal = true; for (int k = 0; k <= 32; ++k) if (rs.gen()[k] != rs.gen()[32 - k]) pal = false;
    CHECK(pal, "the generator is palindromic, as CCSDS's RS(255,223) with roots 112…143 is");
    std::mt19937 r(3);
    int ok16 = 0, refused17 = 0;
    for (int n = 0; n < 50; ++n) {
        uint8_t cw[255]; for (int i = 32; i < 255; ++i) cw[i] = (uint8_t)r(); rs.encode(cw);
        uint8_t c2[255]; std::memcpy(c2, cw, 255);
        std::vector<int> pos(255); for (int i = 0; i < 255; ++i) pos[(size_t)i] = i; std::shuffle(pos.begin(), pos.end(), r);
        for (int k = 0; k < 16; ++k) c2[pos[(size_t)k]] ^= (uint8_t)(1 + r() % 255);
        if (rs.decode(c2) == 16 && std::memcmp(c2, cw, 255) == 0) ++ok16;
        std::memcpy(c2, cw, 255);
        for (int k = 0; k < 17; ++k) c2[pos[(size_t)k]] ^= (uint8_t)(1 + r() % 255);
        const int d = rs.decode(c2);
        if (d < 0 || std::memcmp(c2, cw, 255) != 0) ++refused17;
    }
    CHECK(ok16 == 50, "RS(255,223): 16 byte errors corrected (%d of 50)", ok16);
    CHECK(refused17 == 50, "…17 are never 'corrected' to the sent word (%d of 50)", refused17);
    int hv = 0; for (int t = 0; t < 4; ++t) { int sum = 0; for (int i = 0; i < 16; ++i) sum += ssdv::huff(t).bits[i]; hv += sum == (int)ssdv::huff(t).vals.size(); }
    CHECK(hv == 4 && ssdv::huff(2).vals.size() == 162 && ssdv::huff(3).vals.size() == 162, "Annex K tables: 12, 12, 162, 162 symbols, counts agree");
    uint8_t q[64]; ssdv::quantTable(0, 4, q);
    CHECK(q[0] == 16 && q[1] == 12 && q[63] == 100, "quality 4 (IJG 50): luminance starts 16 12, ends 100 (%d %d … %d)", q[0], q[1], q[63]);
    ssdv::quantTable(1, 7, q);
    bool ones = true; for (int i = 0; i < 64; ++i) ones &= q[i] == 1;
    CHECK(ones, "quality 7: every entry 1");

    for (int mode = 0; mode < 4; ++mode) for (int fec = 1; fec >= 0; --fec) {
        if (!fec && mode != 0) continue;
        const int w16 = 6, h16 = 4;
        const auto blocks = tx::picture(w16, h16, mode, 7 + (unsigned)mode);
        const auto pkts = ssdv::encodeImage("VIBE1", 3, w16, h16, mode, 4, blocks, fec != 0);
        ssdv::Collector col; ssdv::Image* im = nullptr; int parsed = 0;
        for (const auto& b : pkts) { ssdv::Packet p; if (ssdv::parsePacket(b.data(), p)) { ++parsed; im = col.add(p, 0); } }
        CHECK(parsed == (int)pkts.size() && im && im->complete(), "mode %d%s: %zu packets parsed, the picture complete", mode, fec ? "" : " no-FEC", pkts.size());
        if (!im) continue;
        const auto jpg = im->jpeg();
        std::vector<std::vector<int16_t>> back;
        const bool rd = readJpeg(jpg, im->ycparts(), im->mcuCount(), back);
        CHECK(rd && back == blocks && im->mcusDecoded == im->mcuCount(), "mode %d%s: the JPEG's coefficients are exactly what went in (%d of %d MCUs)",
              mode, fec ? "" : " no-FEC", im->mcusDecoded, im->mcuCount());
        if (mode == 0 && fec && std::getenv("VIBE_BALLOON_JPEG_OUT")) {
            FILE* f = std::fopen(std::getenv("VIBE_BALLOON_JPEG_OUT"), "wb"); if (f) { std::fwrite(jpg.data(), 1, jpg.size(), f); std::fclose(f); }
        }
        // corrupt 10 bytes of a packet: RS mends it
        if (fec) {
            std::vector<uint8_t> b = pkts[1]; for (int k = 0; k < 10; ++k) b[(size_t)(20 + 17 * k)] ^= 0x5A;
            ssdv::Packet p; CHECK(ssdv::parsePacket(b.data(), p) && p.rsFixed == 10 && p.payload == [&] { ssdv::Packet o; ssdv::parsePacket(pkts[1].data(), o); return o.payload; }(),
                                  "mode %d: 10 bytes wrong in a packet, RS mends all 10", mode);
        }
    }
    {   // lost packets: the MCUs whose start was in a lost packet (or that ran into one) are empty, the rest exact
        const int w16 = 8, h16 = 6;
        const auto blocks = tx::picture(w16, h16, 0, 21);
        const auto pkts = ssdv::encodeImage("VIBE1", 4, w16, h16, 0, 4, blocks);
        ssdv::Collector col; ssdv::Image* im = nullptr;
        std::vector<ssdv::Packet> all;
        for (const auto& b : pkts) { ssdv::Packet p; ssdv::parsePacket(b.data(), p); all.push_back(p); }
        const int lost = (int)all.size() / 2;
        for (const auto& p : all) if (p.id != lost) im = col.add(p, 0);
        CHECK(im && !im->complete() && im->expected() == (int)all.size(), "one packet of %zu lost: incomplete, %d expected", all.size(), im ? im->expected() : -1);
        std::vector<std::vector<int16_t>> back;
        const bool rd = im && readJpeg(im->jpeg(), 4, im->mcuCount(), back);
        // the MCUs that could not be decoded: from the lost packet's predecessor's last MCU to the MCU before the next packet's first
        int first = 0, last = 0;
        for (int i = lost - 1; i >= 0; --i) if (all[(size_t)i].mcuIndex != 0xFFFF) { first = all[(size_t)i].mcuIndex; break; }
        for (int i = lost + 1; i < (int)all.size(); ++i) if (all[(size_t)i].mcuIndex != 0xFFFF) { last = all[(size_t)i].mcuIndex; break; }
        // ★ every MCU is either exactly what was sent or an empty one (no AC terms); the inexact ones lie between the last
        //   MCU start before the lost packet and the first after it, and there is at least one
        int exactOutside = 0, outside = 0, notExact = 0, emptyOk = 0, inRange = 0;
        for (int m = 0; m < im->mcuCount() && rd; ++m) {
            bool same = true; for (int p = 0; p < 6; ++p) same &= back[(size_t)(m * 6 + p)] == blocks[(size_t)(m * 6 + p)];
            if (m < first || m >= last) { ++outside; exactOutside += same; }
            if (same) continue;
            ++notExact; inRange += m >= first && m < last;
            bool z = true; for (int p = 0; p < 6; ++p) for (int k = 1; k < 64; ++k) z &= back[(size_t)(m * 6 + p)][(size_t)k] == 0;
            emptyOk += z;
        }
        CHECK(rd && exactOutside == outside, "every MCU outside the loss is exact (%d of %d)", exactOutside, outside);
        // (the one MCU that ran into the lost packet keeps what it had read, then ends cleanly — so all but one are empty)
        CHECK(rd && notExact > 0 && inRange == notExact && emptyOk >= notExact - 1, "the MCUs lost with it (%d, all in %d…%d) are empty blocks but the one cut short (%d empty)", notExact, first, last - 1, emptyOk);
        std::printf("   (lost packet %d of %zu → %d MCUs of %d not drawn)\n", lost, all.size(), notExact, im->mcuCount());
    }
    {   // bounds
        ssdv::Collector col;
        for (int i = 0; i < 10; ++i) {
            const auto pk = ssdv::encodeImage("CALL" + std::to_string(i), i, 2, 2, 0, 4, tx::picture(2, 2, 0, (unsigned)i));
            ssdv::Packet p; ssdv::parsePacket(pk[0].data(), p); col.add(p, i);
        }
        CHECK((int)col.images().size() == ssdv::kMaxImages, "at most %d pictures kept in progress (%zu)", ssdv::kMaxImages, col.images().size());
    }
    section("8. SSDV over 600 Bd 8N2 RTTY");
    {
        const int w16 = 5, h16 = 4;
        const auto blocks = tx::picture(w16, h16, 0, 33);
        const auto pkts = ssdv::encodeImage("VIBE1", 9, w16, h16, 0, 4, blocks);
        tx::Rtty r; r.baud = 600; r.shift = 850; r.centre = 1500; r.stop = 2; r.bits = 8;
        tx::Keying k;
        for (size_t i = 0; i < pkts.size(); ++i) {
            if (i == 1) tx::rtty(tx::bytesOf(tx::sentence("VIBE1", 900, 1, 2, 3, 52, -1, 1000)), r, k, 0.1);
            tx::rtty(pkts[i], r, k, i == 0 ? 1.5 : 0.05);
        }
        tx::rtty({}, r, k, 1.0);
        std::vector<float> a; double ph = 0;
        tx::render(k, 48000, 0.1f, a, ph);
        addNoise(a, sigmaFor(0.1, 20), 44);
        Receiver* rx = nullptr;
        const Got g = run(a, Config(), &rx);
        ssdv::Image* im = rx->pictures().images().empty() ? nullptr : rx->pictures().images().begin()->second.get();
        CHECK(im && im->complete() && (int)g.p.size() == (int)pkts.size(), "every packet (%zu of %zu), the picture complete", g.p.size(), pkts.size());
        std::vector<std::vector<int16_t>> back;
        CHECK(im && readJpeg(im->jpeg(), 4, im->mcuCount(), back) && back == blocks, "…exactly the picture sent");
        CHECK(countUkhas(g, 0) == 0 && g.t.size() == 1 && g.t[0].count == 900, "the sentence between the packets is read too (%zu)", g.t.size());
        delete rx;
    }
}

// ── 9 ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void testFalse() {
    section("9. False positives");
    {
        std::vector<float> a((size_t)48000 * 300, 0.f);
        addNoise(a, 0.05, 77);
        const Got g = run(a);
        CHECK(g.t.empty() && g.p.empty(), "five minutes of noise: %zu reports, %zu picture packets", g.t.size(), g.p.size());
    }
    {   // ordinary RTTY: Baudot 45.45 Bd 170 Hz (ITA2 bits as 5N1.5 — the decoder bank's 7/8-bit framings meet it), text
        tx::Keying k;
        std::mt19937 r(8);
        for (int i = 0; i < 900; ++i) {
            const uint8_t c = (uint8_t)(r() & 0x1F);
            const double T = 1 / 45.45;
            auto tone = [&](bool m, double n) { k.push_back({ 1500 + (m ? 85.0 : -85.0), n * T }); };
            tone(false, 1); for (int b = 0; b < 5; ++b) tone((c >> b) & 1, 1); tone(true, 1.5);
        }
        // and 300 Bd 8-bit text with '$' and '*' in it but no valid sentence, and random bytes
        tx::Rtty rr; rr.baud = 300; rr.shift = 600; rr.bits = 8;
        std::string junk;
        for (int i = 0; i < 2500; ++i) { const char set[] = "$$$$**ABCDEF0123456789,.:-VIBE\n"; junk += set[r() % (sizeof set - 1)]; }
        tx::rtty(tx::bytesOf(junk), rr, k, 0.5);
        std::vector<uint8_t> rnd(3000); for (auto& v : rnd) v = (uint8_t)r();
        for (int i = 0; i < 8; ++i) { rnd[(size_t)(i * 300)] = 0x55; rnd[(size_t)(i * 300 + 1)] = 0x66; }   // sync bytes, no packet
        tx::rtty(rnd, rr, k, 0.5);
        std::vector<float> a; double ph = 0;
        tx::render(k, 48000, 0.1f, a, ph);
        addNoise(a, sigmaFor(0.1, 15), 78);
        Receiver* rx = nullptr;
        const Got g = run(a, Config(), &rx);
        CHECK(g.t.empty() && g.p.empty(), "%.0f s of ordinary RTTY (Baudot, 8-bit text, random bytes): %zu reports, %zu picture packets (%lu CRC failures seen)",
              a.size() / 48000.0, g.t.size(), g.p.size(), rx->crcFails());
        delete rx;
    }
}

// ── 10 ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void testSimdAndCost() {
    section("10. SIMD and cost");
    std::mt19937 r(1); std::uniform_real_distribution<float> u(-1, 1);
    std::vector<float> x(4096), h(48);
    for (auto& v : x) v = u(r);
    for (auto& v : h) v = u(r);
    int diff = 0;
    for (int i = 0; i + 48 <= (int)x.size(); ++i) if (Decim4::dot(&x[(size_t)i], h.data()) != Decim4::dotScalar(&x[(size_t)i], h.data())) ++diff;
    CHECK(diff == 0, "front end (%s): SIMD dot = scalar dot, bit for bit (%d differ)", simdName(), diff);
    horus::Demod::Bank a{}, b{};
    for (int k = 0; k < 4; ++k) { a.oRe[k] = b.oRe[k] = 1; a.stRe[k] = b.stRe[k] = std::cos(0.3f * (k + 1)); a.stIm[k] = b.stIm[k] = -std::sin(0.3f * (k + 1)); }
    float oR[4] = { 0.1f, -0.2f, 0.3f, 0.f }, oI[4] = { 0.f, 0.05f, -0.1f, 0.2f }, n1[4], n2[4], m1[4], m2[4], e1[4], e2[4];
    diff = 0;
    for (int i = 0; i < 5000; ++i) {
        const float xv = u(r);
        horus::Demod::bankStep(a, xv, oR, oI, n1, m1, e1);
        horus::Demod::bankStepScalar(b, xv, oR, oI, n2, m2, e2);
        for (int k = 0; k < 4; ++k) diff += (n1[k] != n2[k]) + (m1[k] != m2[k]) + (e1[k] != e2[k]) + (a.sRe[k] != b.sRe[k]);
    }
    CHECK(diff == 0, "Horus tone bank (%s): SIMD = scalar, bit for bit (%d differ)", simdName(), diff);
    // cost: a minute of RTTY + Horus, the whole bank searching
    tx::Keying k; tx::Rtty rr; rr.baud = 300; rr.shift = 600;
    for (int i = 0; i < 20; ++i) {
        tx::rtty(tx::bytesOf(tx::sentence("VIBE1", i, 1, 1, 1, 52, -1, 100)), rr, k, 0.3);
        tx::horus(tx::horusPacket(2, 256, i, 1, 1, 1, 52, -1, 100), 1000, 270, k);
    }
    std::vector<float> aud; double ph = 0; tx::render(k, 48000, 0.1f, aud, ph); addNoise(aud, 0.02, 5);
    Receiver rx; rx.onTelemetry = [](const Telemetry&) {};
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < aud.size(); i += 960) rx.feed48k(aud.data() + i, (int)std::min<size_t>(960, aud.size() - i));
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double pct = 100.0 * cpu / (aud.size() / 48000.0);
    std::printf("   cost: %.0f s of audio in %.2f s — %.2f %% of one core here (%d RTTY candidates searching)\n", aud.size() / 48000.0, cpu, pct, rx.candidates());
    CHECK(pct < 15, "under 15 %% of a core on this machine (%.2f %%)", pct);
}

// ── the end-to-end clip ─────────────────────────────────────────────────────────────────────────────────────────────
/** ★ The server/web tests' clip: unsigned 8-bit IQ at `rate`, centred 25 kHz BELOW the payload's dial (434.200 MHz — the
 *  clip centre 434.175), repeating every pass: two UKHAS sentences at 300 Bd 8N2 600 Hz (VIBE1), the SSDV packets of a
 *  96 × 64 picture at 600 Bd (VIBE1 image 7), and two Horus v2 packets (ID 256), the audio tones at 1–2 kHz in USB. */
static int writeClip(const char* out, double rate, double seconds) {
    tx::Keying k;
    tx::Rtty r3; r3.baud = 300; r3.shift = 600; r3.centre = 1500; r3.bits = 8; r3.stop = 2;
    tx::Rtty r6 = r3; r6.baud = 600; r6.shift = 850;
    const auto pkts = ssdv::encodeImage("VIBE1", 7, 6, 4, 0, 4, tx::picture(6, 4, 0, 99));
    tx::rtty(tx::bytesOf(tx::sentence("VIBE1", 1, 10, 0, 0, 52.20512, 0.11893, 31000)), r3, k, 1.0);
    tx::rtty(tx::bytesOf(tx::sentence("VIBE1", 2, 10, 0, 5, 52.20601, 0.11950, 31050)), r3, k, 0.2);
    for (size_t i = 0; i < pkts.size(); ++i) tx::rtty(pkts[i], r6, k, i == 0 ? 0.6 : 0.03);
    tx::rtty({}, r6, k, 0.5);
    tx::silence(k, 0.5);
    tx::horus(tx::horusPacket(2, 256, 501, 10, 0, 30, 52.21f, 0.12f, 31200), 1100, 270, k);
    tx::silence(k, 0.4);
    tx::horus(tx::horusPacket(2, 256, 502, 10, 0, 35, 52.211f, 0.121f, 31250), 1100, 270, k);
    tx::silence(k, 0.8);
    double keySec = 0; for (auto& s : k) keySec += s.sec;
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    std::mt19937 rng(77); std::normal_distribution<float> g(0.0f, std::sqrt(0.5f));
    const size_t total = (size_t)(seconds * rate);
    size_t written = 0;
    std::vector<uint8_t> buf; buf.reserve(1 << 17);
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    // 20 dB C/N in 3 kHz: noise σ² = 1 a component over the whole rate; the carrier power A²
    const double A = std::sqrt(std::pow(10.0, 20.0 / 10) * 3000.0 / rate * 2.0);
    double ph = 0;
    while (written < total) {
        tx::renderIq(k, rate, 25000.0, (float)A, ph, [&](float re, float im) {
            if (written >= total) return;
            buf.push_back(q(re + g(rng))); buf.push_back(q(im + g(rng)));
            ++written;
            if (buf.size() >= (1 << 17)) { std::fwrite(buf.data(), 1, buf.size(), f); buf.clear(); }
        });
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    std::printf("clip: %s, %.0f s at %.0f S/s, one pass %.1f s (%zu SSDV packets)\n", out, seconds, rate, keySec, pkts.size());
    return 0;
}

/** ★ `test-balloon wav <file.wav | file.raw> [rate]` — a recording through the receiver (a .raw is mono s16 at `rate`,
 *  default 48000). Resampled to 48 kHz (linear), every report printed. Exit 0 when anything decoded, 1 when nothing. */
static int replay(const char* path, double rawRate) {
    FILE* f = std::fopen(path, "rb"); if (!f) { std::perror(path); return 2; }
    std::vector<uint8_t> d; { uint8_t b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n); }
    std::fclose(f);
    double rate = rawRate; int ch = 1; size_t off = 0, len = d.size();
    if (d.size() > 44 && std::memcmp(d.data(), "RIFF", 4) == 0) {
        size_t p = 12;
        while (p + 8 <= d.size()) {
            const uint32_t sz = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
            if (std::memcmp(&d[p], "fmt ", 4) == 0) { ch = d[p + 10] | d[p + 11] << 8; rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16 | (uint32_t)d[p + 15] << 24; }
            if (std::memcmp(&d[p], "data", 4) == 0) { off = p + 8; len = std::min<size_t>(sz, d.size() - off); break; }
            p += 8 + sz + (sz & 1);
        }
    }
    std::vector<float> in;
    for (size_t i = off; i + 2 * (size_t)ch <= off + len; i += 2 * (size_t)ch) in.push_back((int16_t)(d[i] | d[i + 1] << 8) / 32768.0f);
    std::vector<float> a;
    for (double t = 0; t < in.size() - 1; t += rate / 48000.0) { const size_t i = (size_t)t; const double fr = t - (double)i; a.push_back((float)(in[i] * (1 - fr) + in[i + 1] * fr)); }
    std::printf("%s: %.1f s at %.0f Hz, %d channel(s)\n", path, in.size() / rate, rate, ch);
    Receiver rx;
    int n = 0;
    rx.onTelemetry = [&](const Telemetry& t) {
        ++n;
        std::printf("  %6.1f s  %-5s %s%s\n", rx.seconds(), t.source.c_str(), t.sentence.c_str(), t.source == "horus" ? (" (Golay fixed " + std::to_string(t.fecFixed) + " bits)").c_str() : "");
    };
    if (std::getenv("VIBE_BALLOON_REJECTS"))
        rx.onRejected = [&](const std::string& l, int c) { std::printf("  %6.1f s  ✗ CRC [%s] %s\n", rx.seconds(), rx.candName(c).c_str(), l.c_str()); };
    rx.onImagePacket = [&](ssdv::Image& im, const ssdv::Packet& p) { ++n; std::printf("  %6.1f s  ssdv  %s image %d packet %d (RS %d)\n", rx.seconds(), im.call.c_str(), im.imageId, p.id, p.rsFixed); };
    for (size_t i = 0; i < a.size(); i += 960) rx.feed48k(a.data() + i, (int)std::min<size_t>(960, a.size() - i));
    double t4[4];
    std::printf("  %d decoded · RTTY %s (centre %.0f, shift %.0f) · Horus tones %s · CRC failures %lu\n", n, rx.rttySettings().c_str(), rx.rttyCentre(), rx.rttyShift(),
                rx.horusTones(t4) ? (std::to_string((int)t4[0]) + "/" + std::to_string((int)t4[1]) + "/" + std::to_string((int)t4[2]) + "/" + std::to_string((int)t4[3])).c_str() : "none", rx.crcFails());
    std::printf("  Horus: %lu packets passed (repeats included), UW %lu, tone placements %lu, v2 CRC failures %lu\n", rx.horus().packets, rx.horus().uwHits, rx.horus().toneSets, rx.horus().crcFails);
    return n > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "wav") return replay(argv[2], argc > 3 ? std::atof(argv[3]) : 48000);
    if (argc > 2 && std::string(argv[1]) == "clip") return writeClip(argv[2], argc > 3 ? std::atof(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 60);
    testChecks();
    testGolay();
    testHorusCoding();
    testHorusAir();
    testRttyAir();
    testSsdv();
    testFalse();
    testSimdAndCost();
    std::printf("\n%s test-balloon: %d passed, %d failed\n", g_fail ? "✗" : "✓", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
