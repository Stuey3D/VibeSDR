// test-lora.cpp — V12 LoRa in ISM mode (vibe_lora*.h, vibe_meshtastic.h, vibe_meshcore.h, vibe_lorawan.h;
// docs/v12/LORA-MESH.md).
//
//   · the primitives on their standards' own test vectors (AES-128, SHA-256, HMAC-SHA-256)
//   · the codec: Hamming at every rate (and the single errors 4/7 and 4/8 correct), interleaver, Gray, header checksum
//   · the PHY round trip: every SF 7-12 × BW 62.5/125/250/500 × CR 4/5-4/8, with carrier and chip-clock offsets
//   · the sensitivity table against Semtech's datasheet SNRs (SF7 −7.5 … SF12 −20 dB)
//   · the whole band at 2.4 MS/s through the channeliser and the engine: Meshtastic, MeshCore, LoRaWAN at once
//   · the parsers and the PRIVACY RULE: a wrong-key Meshtastic packet, a direct message, another MeshCore channel and a
//     LoRaWAN payload never put a byte of their content anywhere a client could see it
//   · pure noise decodes nothing; a CPU row (the default set, everything)
//   test-lora --write-clip <out.u8> <centre Hz> <rate>   writes a synthetic 868 MHz clip for the server test
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-lora vibeserver/test-lora.cpp -lpthread
#include "vibe_ism_service.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <random>
#include <string>

using namespace vibelora;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) ++fails; }

// ── test-packet builders (the encoding side of each spec) ─────────────────────────────────────────────────────────────
struct PbW {
    std::vector<uint8_t> b;
    void varint(uint64_t v) { while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; } b.push_back(uint8_t(v)); }
    void key(int f, int wt) { varint(uint64_t(f) << 3 | uint64_t(wt)); }
    void u(int f, uint64_t v) { key(f, 0); varint(v); }
    void f32(int f, uint32_t v) { key(f, 5); for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void fl(int f, float x) { uint32_t v; std::memcpy(&v, &x, 4); f32(f, v); }
    void bytes(int f, const std::vector<uint8_t>& d) { key(f, 2); varint(d.size()); b.insert(b.end(), d.begin(), d.end()); }
    void str(int f, const std::string& s) { bytes(f, std::vector<uint8_t>(s.begin(), s.end())); }
};
static std::vector<uint8_t> mtPacket(uint32_t to, uint32_t from, uint32_t id, int hopLimit, int hopStart, int port,
                                     const std::vector<uint8_t>& body, const uint8_t* key = nullptr, const char* chName = "LongFast") {
    PbW d; d.u(1, uint64_t(port)); d.bytes(2, body);
    const uint8_t* k = key ? key : meshtastic::defaultKey();
    std::vector<uint8_t> p(16);
    for (int i = 0; i < 4; ++i) { p[size_t(i)] = uint8_t(to >> (8 * i)); p[size_t(4 + i)] = uint8_t(from >> (8 * i)); p[size_t(8 + i)] = uint8_t(id >> (8 * i)); }
    p[12] = uint8_t((hopLimit & 7) | (hopStart & 7) << 5);
    p[13] = uint8_t(meshtastic::channelHash(chName, k, 16));
    uint8_t nonce[16] = {0};
    for (int i = 0; i < 4; ++i) { nonce[i] = uint8_t(id >> (8 * i)); nonce[8 + i] = uint8_t(from >> (8 * i)); }
    std::vector<uint8_t> ct(d.b.size());
    crypto::aesCtr(k, nonce, d.b.data(), ct.data(), ct.size());
    p.insert(p.end(), ct.begin(), ct.end());
    return p;
}
static std::vector<uint8_t> mcGroupText(const std::vector<uint8_t>& key16, const std::string& msg, int hops = 2) {
    std::vector<uint8_t> pt = {0x10, 0x32, 0x54, 0x66, 0x00};
    pt.insert(pt.end(), msg.begin(), msg.end());
    while (pt.size() % 16) pt.push_back(0);
    crypto::Aes128 a(key16.data());
    std::vector<uint8_t> ct(pt.size());
    for (size_t i = 0; i < pt.size(); i += 16) a.encrypt(pt.data() + i, ct.data() + i);
    uint8_t k32[32] = {0}; std::memcpy(k32, key16.data(), 16);
    uint8_t mac[32]; crypto::hmacSha256(k32, 32, ct.data(), ct.size(), mac);
    uint8_t h[32]; crypto::sha256(key16.data(), 16, h);
    std::vector<uint8_t> p = {uint8_t(0x05 << 2 | 0x01), uint8_t(hops)};
    for (int i = 0; i < hops; ++i) p.push_back(uint8_t(0xA0 + i));
    p.push_back(h[0]); p.push_back(mac[0]); p.push_back(mac[1]);
    p.insert(p.end(), ct.begin(), ct.end());
    return p;
}
static std::vector<uint8_t> mcAdvert(const std::string& name, double lat, double lon, int kind) {
    std::vector<uint8_t> p = {uint8_t(0x04 << 2 | 0x01), 0};
    for (int i = 0; i < 32; ++i) p.push_back(uint8_t(0x5A + i));            // public key
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(0x11 * i));              // timestamp
    for (int i = 0; i < 64; ++i) p.push_back(uint8_t(i));                    // signature (not checked)
    p.push_back(uint8_t(0x80 | 0x10 | kind));
    const int32_t la = int32_t(std::lround(lat * 1e6)), lo = int32_t(std::lround(lon * 1e6));
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(uint32_t(la) >> (8 * i)));
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(uint32_t(lo) >> (8 * i)));
    p.insert(p.end(), name.begin(), name.end());
    return p;
}
static std::vector<uint8_t> lwUplink(uint32_t devAddr, int fcnt, int port, size_t payLen, bool adr) {
    std::vector<uint8_t> p = {0x40};
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(devAddr >> (8 * i)));
    p.push_back(uint8_t(adr ? 0x80 : 0)); p.push_back(uint8_t(fcnt)); p.push_back(uint8_t(fcnt >> 8));
    p.push_back(uint8_t(port));
    for (size_t i = 0; i < payLen; ++i) p.push_back(uint8_t(0xE0 + i));      // "ciphertext" — must never be shown
    for (int i = 0; i < 4; ++i) p.push_back(0xC3);                           // MIC
    return p;
}
static std::string str(const Msg& m) { return m.proto + "/" + m.what; }

// ── the PHY ──────────────────────────────────────────────────────────────────────────────────────────────────────────
static bool roundTrip(int sf, double bw, int cr, double snr, double cfo, std::mt19937& rng, int len = 24) {
    Phy p; p.sf = sf; p.cr = cr; p.bw = bw; p.ldro = ldroFor(sf, bw);
    std::vector<uint8_t> pl(static_cast<size_t>(len)); for (auto& b : pl) b = uint8_t(rng());
    const auto syms = encode(p, pl);
    std::uniform_real_distribution<double> U(0, 1);
    /* ★★ PHYSICAL, NOT IDEAL: the frame is made at 4× its chip rate, a random fraction of a sample late, and reaches the
     *  demodulator through the real channeliser — as off air. Sampled at exactly one per chip with no filter, a chirp's
     *  wrap is aliased into a perfect discrete chirp, which hides what fractional timing does to a band-limited one
     *  (vibe_lora.h alignAt). */
    const double fs = 4 * bw;
    const auto fr = modulate(p, syms, fs, cfo, -cfo / 868e6 * 1e6, U(rng) * 6.28, U(rng));
    const int N = 1 << sf;
    std::vector<cf> sig(size_t(4 * N * (1 + U(rng) * 3)), cf(0, 0));
    sig.insert(sig.end(), fr.begin(), fr.end());
    sig.resize(sig.size() + 8 * size_t(N), cf(0, 0));
    // ★ SNR in the bandwidth: noise over 4× the bandwidth is 4× the power
    if (snr < 90) { std::normal_distribution<float> g(0, float(std::sqrt(0.5 * 4 * std::pow(10.0, -snr / 10)))); for (auto& x : sig) x += cf(g(rng), g(rng)); }
    Channeliser ch(fs);
    Stream s(bw, 19);
    ch.add(0, bw, &s);
    Demod::Cfg c; c.sf = sf; c.bw = bw; c.carrierHz = 868e6; c.searchStride = sf >= 11 ? 2 : 1;   // ★ as the engine runs them
    Demod d(c);
    bool got = false;
    for (size_t i = 0; i < sig.size(); i += 16384) {
        ch.feed(sig.data() + i, std::min<size_t>(16384, sig.size() - i));
        d.run(s, [&](Frame&& f) { if (f.bytes == pl && f.cr == cr) got = true; });
    }
    // flush the channeliser's last block
    std::vector<cf> z(8192, cf(0, 0)); ch.feed(z.data(), z.size()); d.run(s, [&](Frame&& f) { if (f.bytes == pl && f.cr == cr) got = true; });
    return got;
}

/** The 868 band scene: every network at once, at 2.4 MS/s around 868.6 MHz. */
struct Tx { double hz, bw; int sf, cr; uint8_t sync; int pre; std::vector<uint8_t> bytes; double at; double cfo; bool inv; };
static std::vector<Tx> scene() {
    const uint32_t BC = 0xFFFFFFFF;
    std::vector<Tx> v;
    PbW user; user.str(1, "!1a2b3c4d"); user.str(2, "Northampton Base"); user.str(3, "NBAS"); user.u(5, 43); user.u(7, 2);
    v.push_back({869.525e6, 250e3, 11, 1, 0x2B, 16, mtPacket(BC, 0x1a2b3c4d, 0x1001, 3, 3, 4, user.b), 0.2, 2500, false});
    PbW pos; pos.f32(1, uint32_t(int32_t(522400000))); pos.f32(2, uint32_t(int32_t(-8950000))); pos.u(3, 68); pos.u(23, 32);
    v.push_back({869.525e6, 250e3, 11, 1, 0x2B, 16, mtPacket(BC, 0x1a2b3c4d, 0x1002, 2, 3, 3, pos.b), 1.6, 2500, false});
    const std::string hello = "Hello from the Racecourse";
    v.push_back({869.525e6, 250e3, 11, 1, 0x2B, 16, mtPacket(BC, 0x55667788, 0x1003, 1, 3, 1, std::vector<uint8_t>(hello.begin(), hello.end())), 3.0, -4100, false});
    const std::string secret = "PRIVATE-SECRET-TEXT";
    uint8_t other[16]; for (int i = 0; i < 16; ++i) other[i] = uint8_t(0x30 + i * 7);
    v.push_back({869.525e6, 250e3, 11, 1, 0x2B, 16, mtPacket(BC, 0x99aabbcc, 0x1004, 3, 3, 1, std::vector<uint8_t>(secret.begin(), secret.end()), other, "Family"), 4.4, 900, false});
    const std::string dm = "DIRECT-SECRET";
    v.push_back({869.525e6, 250e3, 11, 1, 0x2B, 16, mtPacket(0x1a2b3c4d, 0x55667788, 0x1005, 3, 3, 1, std::vector<uint8_t>(dm.begin(), dm.end())), 5.8, -4100, false});
    v.push_back({869.618e6, 62.5e3, 8, 4, 0x12, 16, mcAdvert("Moulton Rptr", 52.29, -0.84, 2), 6.6, 1200, false});
    v.push_back({869.618e6, 62.5e3, 8, 4, 0x12, 16, mcGroupText(meshcore::publicKey(), "Alice: anyone on the mesh tonight?"), 8.2, -800, false});
    v.push_back({869.618e6, 62.5e3, 8, 4, 0x12, 16, mcGroupText(crypto::fromHex("00112233445566778899aabbccddeeff"), "Bob: MESHCORE-SECRET"), 9.1, -800, false});
    v.push_back({868.1e6, 125e3, 9, 1, 0x34, 8, lwUplink(0x260B1234, 4211, 2, 12, true), 0.9, 3100, false});
    v.push_back({868.5e6, 125e3, 12, 1, 0x34, 8, lwUplink(0x01ABCDEF, 77, 10, 5, false), 1.2, -2200, false});
    v.push_back({868.3e6, 125e3, 7, 1, 0x34, 8, lwUplink(0x27000042, 9, 1, 20, false), 3.3, 500, false});
    return v;
}
static std::vector<cf> render(const std::vector<Tx>& txs, double fs, double centre, double secs, double snrDb, std::mt19937& rng) {
    std::vector<cf> x(size_t(secs * fs), cf(0, 0));
    for (const auto& t : txs) {
        Phy p; p.sf = t.sf; p.bw = t.bw; p.cr = t.cr; p.sync = t.sync; p.preamble = t.pre; p.ldro = ldroFor(t.sf, t.bw); p.invertIq = t.inv;
        const auto syms = encode(p, t.bytes);
        const double off = t.hz - centre + t.cfo;
        const auto w = modulate(p, syms, fs, off, -t.cfo / t.hz * 1e6, 0.4, 0.3);
        const double amp = std::sqrt(std::pow(10.0, snrDb / 10) * 1e-4 * t.bw / fs);   // SNR in the bandwidth: noise σ² = 1e-4 per sample over fs
        size_t s0 = size_t(t.at * fs);
        for (size_t i = 0; i < w.size() && s0 + i < x.size(); ++i) x[s0 + i] += w[i] * float(amp);
    }
    std::normal_distribution<float> g(0, float(std::sqrt(0.5e-4)));
    for (auto& v : x) v += cf(g(rng), g(rng));
    return x;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--write-clip") {
        const double centre = std::atof(argv[3]), rate = std::atof(argv[4]);
        std::mt19937 rng(11);
        auto txs = scene();
        // ★ the clip repeats the scene so a client joining late still hears it
        std::vector<Tx> all;
        for (int k = 0; k < 3; ++k) for (auto t : txs) { t.at += 7.0 * k + 1.0; all.push_back(t); }
        const auto x = render(all, rate, centre, 23.0, 10, rng);
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::vector<uint8_t> u(x.size() * 2);
        for (size_t i = 0; i < x.size(); ++i) {
            u[2 * i] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 3.5f * 128 + 127.5f))));
            u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 3.5f * 128 + 127.5f))));
        }
        std::fwrite(u.data(), 1, u.size(), f); std::fclose(f);
        std::printf("wrote %s: %.1f s at %.0f S/s around %.3f MHz\n", argv[2], double(x.size()) / rate, rate, centre / 1e6);
        return 0;
    }
    std::mt19937 rng(5);

    std::printf("── the primitives on their standards' test vectors ──\n");
    {
        const auto k = crypto::fromHex("000102030405060708090a0b0c0d0e0f"), p = crypto::fromHex("00112233445566778899aabbccddeeff");
        uint8_t c[16], back[16];
        crypto::Aes128 a(k.data()); a.encrypt(p.data(), c); a.decrypt(c, back);
        ok(crypto::toHex(c, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a", "AES-128 FIPS-197 C.1 encrypts to 69c4e0d8…");
        ok(std::memcmp(back, p.data(), 16) == 0, "…and decrypts back");
        uint8_t h[32]; crypto::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, h);
        ok(crypto::toHex(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(\"abc\") (FIPS 180-4)");
        const std::string key = "Jefe", msg = "what do ya want for nothing?";
        crypto::hmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), h);
        ok(crypto::toHex(h, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA-256 RFC 4231 case 2");
    }

    std::printf("── the codec ──\n");
    {
        bool allOk = true, fixOk = true;
        for (int cr = 1; cr <= 4; ++cr) for (int n = 0; n < 16; ++n) {
            const int cw = hammingEncode(n, cr);
            if (hammingDecode(cw, cr) != n) allOk = false;
            if (cr >= 3) for (int b = 0; b < 4 + cr; ++b) if (hammingDecode(cw ^ (1 << b), cr) != n) fixOk = false;
        }
        ok(allOk, "Hamming 4/5 … 4/8: every nibble decodes");
        ok(fixOk, "4/7 and 4/8 correct every single-bit error");
        bool il = true;
        for (int sfApp = 5; sfApp <= 12; ++sfApp) for (int cwLen = 5; cwLen <= 8; ++cwLen) {
            std::vector<int> cws(static_cast<size_t>(sfApp)), syms, back;
            for (auto& c : cws) c = int(rng() & ((1u << cwLen) - 1));
            interleave(cws, sfApp, cwLen, syms); deinterleave(syms.data(), sfApp, cwLen, back);
            if (back != cws) il = false;
        }
        ok(il, "the diagonal interleaver inverts at every SF and rate");
        bool gr = true;
        for (int sf = 7; sf <= 12; ++sf) for (int v = 0; v < (1 << sf); ++v) {
            if (symbolToValue(valueToSymbol(v, sf, false), sf, false) != v) gr = false;
            if (v < (1 << (sf - 2))) {
                const int s = valueToSymbol(v, sf, true), N = 1 << sf;
                if (symbolToValue(s, sf, true) != v || symbolToValue((s + 1) % N, sf, true) != v || symbolToValue((s + N - 1) % N, sf, true) != v) gr = false;
            }
        }
        ok(gr, "Gray + offset round-trips; a reduced-rate symbol survives ±1 bin");
        ok(whiten(0) == 0xFF && whiten(1) == 0xFE && whiten(5) == 0xE1 && whiten(8) == 0x0B, "whitening starts FF FE FC F8 F0 E1 C2 85 0B");
        int good = 0;
        for (int len = 0; len < 256; ++len) for (int crc = 0; crc < 2; ++crc) for (int cr = 1; cr <= 4; ++cr) {
            const int n2 = cr << 1 | crc, c = headerChecksum(len >> 4, len & 15, n2);
            good += c >= 0 && c < 32 && headerChecksum((len ^ 1) >> 4, (len ^ 1) & 15, n2) != c;
        }
        ok(good == 2048, "the header checksum changes with every length's lowest bit");
    }

    std::printf("── the PHY round trip: SF 7-12 × BW 62.5/125/250/500 × CR 4/5-4/8, carrier + chip-clock offset ──\n");
    {
        int good = 0, all = 0;
        std::string miss;
        for (double bw : {62.5e3, 125e3, 250e3, 500e3}) for (int sf = 7; sf <= 12; ++sf) for (int cr = 1; cr <= 4; ++cr) {
            ++all;
            // ★ ±8 % of the bandwidth, at most 17 kHz (20 ppm at 868 MHz — a LoRa radio's crystal spec)
            const double cfo = (rng() & 1 ? 1 : -1) * std::min(0.08 * bw, 17e3);
            if (roundTrip(sf, bw, cr, 99, cfo, rng)) ++good;
            else miss += " SF" + std::to_string(sf) + "/" + std::to_string(int(bw / 1e3)) + "/CR" + std::to_string(cr);
        }
        ok(good == all, std::to_string(good) + " of " + std::to_string(all) + " frames byte-exact" + miss);
        int noisy = 0;
        for (int sf = 7; sf <= 12; ++sf) noisy += roundTrip(sf, 125e3, 1, -3.0, 5000, rng, 60);
        ok(noisy == 6, "a 60-byte frame at −3 dB SNR at every SF (" + std::to_string(noisy) + "/6)");
    }

    std::printf("── sensitivity: packet success vs SNR in the bandwidth (BW 125 kHz, CR 4/5, 20 bytes, ±10 kHz CFO) ──\n");
    {
        const double lim[13] = {0, 0, 0, 0, 0, 0, 0, -7.5, -10, -12.5, -15, -17.5, -20};
        const int trials = 40;
        for (int sf = 7; sf <= 12; ++sf) {
            std::printf("    SF%-2d Semtech %5.1f dB:", sf, lim[sf]);
            int atLim = 0, plus1 = 0;
            for (double d : {-3.0, -2.0, -1.0, 0.0, 1.0}) {
                int n = 0;
                std::uniform_real_distribution<double> U(-10e3, 10e3);
                for (int t = 0; t < trials; ++t) n += roundTrip(sf, 125e3, 1, lim[sf] + d, U(rng), rng, 20);
                std::printf("  %+.0f dB %3d %%", d, 100 * n / trials);
                if (d == 0.0) atLim = n; if (d == 1.0) plus1 = n;
            }
            std::printf("\n");
            ok(atLim >= trials * 8 / 10 && plus1 >= trials * 9 / 10, "SF" + std::to_string(sf) + ": ≥ 80 % at Semtech's SNR, ≥ 90 % 1 dB above");
        }
    }

    std::printf("── the parsers ──\n");
    {
        ok(meshtastic::channelHash("LongFast", meshtastic::defaultKey(), 16) == 8, "Meshtastic LongFast + the default key: channel hash 8");
        ok(meshtastic::slotHash("LongFast") % 104 == 19, "LongFast's slot in the US band (104 slots): 20 → 906.875 MHz");
        ok(meshtastic::slotHash("LongFast") % 4 == 3, "…and in EU_433 (4 slots): 433.875 MHz");
        ok(std::string(meshtastic::hwName(43)) == "Heltec V3" && std::string(meshtastic::hwName(71)) == "SenseCAP T1000-E"
           && std::string(meshtastic::hwName(152)) == "Heltec Mesh Pocket V2" && std::string(meshtastic::hwName(4)) == "T-Beam",
           "hardware names line up with the published enum (4, 43, 71, 152)");
        const auto txs = scene();
        std::vector<Msg> ms;
        for (const auto& t : txs) {
            Frame f; f.bytes = t.bytes; f.sync = t.sync; f.crcOn = true; f.sf = t.sf; f.bw = t.bw;
            ms.push_back(interpret(f, t.hz > 869.6e6 ? "meshcore" : t.hz > 869e6 ? "meshtastic" : "lorawan"));
        }
        ok(str(ms[0]) == "meshtastic/nodeinfo" && ms[0].longName == "Northampton Base" && ms[0].shortName == "NBAS" && ms[0].hw == "Heltec V3"
           && ms[0].role == "router" && ms[0].node == "!1a2b3c4d" && ms[0].channel == "LongFast", "Meshtastic node info: name, short name, hardware, role");
        ok(str(ms[1]) == "meshtastic/position" && std::fabs(ms[1].lat - 52.24) < 1e-6 && std::fabs(ms[1].lon + 0.895) < 1e-6 && ms[1].altM == 68
           && ms[1].hopStart - ms[1].hopLimit == 1, "Meshtastic position (52.2400, −0.8950, 68 m, 1 hop)");
        ok(str(ms[2]) == "meshtastic/text" && ms[2].text == "Hello from the Racecourse" && ms[2].broadcast, "Meshtastic public text on the default channel is shown");
        ok(ms[3].privateCh && ms[3].text.empty() && ms[3].node == "!99aabbcc", "a private channel: header only (from, channel hash), no text");
        ok(str(ms[4]) == "meshtastic/direct" && ms[4].text.empty(), "a text addressed to one node opens with the default key but is NOT shown");
        ok(str(ms[5]) == "meshcore/advert" && ms[5].longName == "Moulton Rptr" && ms[5].role == "repeater" && std::fabs(ms[5].lat - 52.29) < 1e-6,
           "MeshCore advert: name, repeater, position");
        ok(str(ms[6]) == "meshcore/text" && ms[6].from == "Alice" && ms[6].text == "anyone on the mesh tonight?" && ms[6].channel == "Public",
           "MeshCore Public channel: sender and text");
        ok(ms[7].privateCh && ms[7].text.empty() && ms[7].what == "group message", "another MeshCore channel: header only");
        ok(str(ms[8]) == "lorawan/data" && ms[8].node == "260B1234" && ms[8].net == "The Things Network" && ms[8].fcnt == 4211 && ms[8].fport == 2
           && ms[8].encLen == 12 && ms[8].adr, "LoRaWAN uplink: TTN DevAddr, FCnt, FPort, 12 bytes encrypted, ADR");
        ok(lorawan::line(ms[8]).find("12 bytes encrypted") != std::string::npos, "…its line: " + lorawan::line(ms[8]));
        Frame jf; jf.sync = 0x34; jf.crcOn = true;
        jf.bytes = {0x00, 1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0xAA, 0xBB, 9, 9, 9, 9};
        const Msg jm = interpret(jf, "lorawan");
        ok(jm.what == "join" && jm.devEui == "1817161514131211" && jm.joinEui == "0807060504030201", "LoRaWAN join request: DevEUI/JoinEUI big-endian");
        Frame junk; junk.sync = 0x34; junk.crcOn = true; junk.bytes = {0xE3, 1, 2, 3, 4, 5};
        ok(interpret(junk, "lorawan").proto == "lora", "a frame whose header cannot be LoRaWAN is listed as plain LoRa");
        // hostile: every length of random bytes through every parser — nothing may read past the end (ASan/UBSan builds)
        int n = 0;
        for (int len = 0; len < 260; ++len) for (int k = 0; k < 8; ++k) {
            Frame r; r.bytes.resize(size_t(len)); for (auto& b : r.bytes) b = uint8_t(rng());
            for (uint8_t s : {uint8_t(0x2B), uint8_t(0x12), uint8_t(0x34), uint8_t(0x55)}) { r.sync = s; n += !interpret(r, "meshcore").proto.empty(); }
        }
        ok(n == 260 * 8 * 4, "8 320 random frames through every parser: each lands somewhere, none crashes");

        std::printf("── the privacy rule, on what a client receives ──\n");
        MeshTable mt;
        for (size_t i = 0; i < ms.size(); ++i) mt.accept(ms[i], double(i));
        std::string all = mt.msgsJson(20);
        for (auto& r : mt.rowsJson(20)) all += r.second;
        ok(all.find("PRIVATE-SECRET") == std::string::npos && all.find("DIRECT-SECRET") == std::string::npos && all.find("MESHCORE-SECRET") == std::string::npos,
           "no private / direct text in the rows or the messages");
        ok(all.find("e0e1e2") == std::string::npos && all.find("E0E1E2") == std::string::npos && all.find("\\u00e0") == std::string::npos,
           "no LoRaWAN payload byte (only its length)");
        ok(all.find("Hello from the Racecourse") != std::string::npos && all.find("anyone on the mesh tonight?") != std::string::npos,
           "the public messages are there");
        ok(mt.nodesWithPosition() == 2, "two nodes on the map (a Meshtastic base, a MeshCore repeater)");
        std::string rows; for (auto& r : mt.rowsJson(20)) rows += r.second + "\n";
        std::printf("%s", rows.c_str());
    }

    std::printf("── the whole 868 band at 2.4 MS/s: every network at once, through the channeliser and the engine ──\n");
    {
        const double fs = 2.4e6, hc = 868.6e6;
        std::mt19937 r2(9);
        const auto txs = scene();
        const auto x = render(txs, fs, hc, 10.0, 3.0, r2);
        Engine e;
        e.startInline("868", hc, fs, INFINITY);
        ok(e.channelsRun() >= 8, std::to_string(e.channelsRun()) + " of " + std::to_string(e.planSize()) + " plan channels inside 868.6 ± 1.08 MHz");
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < x.size(); i += 65536) e.process(x.data() + i, std::min<size_t>(65536, x.size() - i));
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto got = e.take();
        int mt = 0, mc = 0, lw = 0;
        for (auto& h : got) { mt += h.m.proto == "meshtastic"; mc += h.m.proto == "meshcore"; lw += h.m.proto == "lorawan";
            std::printf("    %-34s SF%-2d %6.1f kHz %+6.0f Hz SNR %5.1f  %s\n", h.f.chan.c_str(), h.f.sf, h.f.bw / 1e3, h.f.cfoHz, h.f.snrDb,
                        h.m.proto == "lorawan" ? lorawan::line(h.m).c_str() : h.m.proto == "meshcore" ? meshcore::line(h.m).c_str() : meshtastic::line(h.m).c_str()); }
        if (mt != 5 || mc != 3 || lw != 3) std::printf("%s", e.stats().c_str());
        ok(mt == 5 && mc == 3 && lw == 3, "5 Meshtastic, 3 MeshCore, 3 LoRaWAN frames decoded at +3 dB (" + std::to_string(mt) + "/" + std::to_string(mc) + "/" + std::to_string(lw) + ")");
        std::printf("    CPU: %.1f %% of one core, every plan channel (%zu detectors)\n", 100 * cpu / 10.0, e.detectors());
        // ★★ THE ENGINE'S THREADS (2026-10-31, the Pi 2 split): vibe-lora the FFT, vibe-lora2 the channels, vibe-lora3 the detectors —
        //    the very frames the inline engine gives, field for field, fed in the radio's 32 ms blocks
        auto key = [](const Heard& h) {
            char b[200]; std::snprintf(b, sizeof b, "%s|%d|%.6f|%.3f|%.3f|%.3f|%zu|", h.f.chan.c_str(), h.f.sf, h.f.tSec, h.f.cfoHz, double(h.f.snrDb), h.f.freqHz, h.f.bytes.size());
            std::string k = b; for (uint8_t v : h.f.bytes) k += std::to_string(v) + ","; return k;
        };
        std::vector<std::string> want, have;
        for (auto& h : got) want.push_back(key(h));
        {
            Engine t;
            t.setLossless(true);
            t.start("868", hc, fs, INFINITY);
            const size_t blk = size_t(fs * 0.032);
            for (size_t i = 0; i < x.size(); i += blk) t.feed(x.data() + i, std::min(blk, x.size() - i));
            for (int w = 0; w < 6000 && !t.idle(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ok(t.idle(), "the three threads drained");
            ok(t.dropped() == 0, "lossless: nothing dropped (" + std::to_string(t.dropped()) + ")");
            for (auto& h : t.take()) have.push_back(key(h));
            t.stop();
        }
        std::sort(want.begin(), want.end()); std::sort(have.begin(), have.end());   // ★ lanes hand frames out in their own order
        ok(!want.empty() && have == want, "threads = inline: " + std::to_string(have.size()) + " frames against " + std::to_string(want.size()) + ", identical");
    }

    std::printf("── pure noise: nothing, and the CPU rows ──\n");
    {
        const double fs = 2.4e6, hc = 868.6e6, secs = 20;
        std::vector<cf> x(size_t(fs * secs));
        std::normal_distribution<float> g(0, 0.01f);
        for (auto& v : x) v = cf(g(rng), g(rng));
        for (int all = 0; all < 2; ++all) {
            Engine e;
            e.startInline("868", hc, fs, all ? INFINITY : 0);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < x.size(); i += 65536) e.process(x.data() + i, std::min<size_t>(65536, x.size() - i));
            const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const auto got = e.take();
            ok(got.empty(), std::string(all ? "every channel" : "the default set") + ": 20 s of noise → " + std::to_string(got.size()) + " frames");
            std::printf("    CPU %s: %.1f %% of one core at 2.4 MS/s (%zu channels, %zu detectors)\n", all ? "everything" : "default set",
                        100 * cpu / secs, e.channelsRun(), e.detectors());
            for (auto& n : e.channelNames()) std::printf("      %s\n", n.c_str());
        }
    }

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
