// test-drm-xaac.cpp — DRM's xHE-AAC decoded IN PROCESS by libxaac (vibe_xaac.h), V12 2026-10-29. docs/v12/LIBXAAC.md.
//   1. CONFORMANCE: ISO's USAC conformance streams as FFmpeg's test suite publishes them, with their reference decodes
//      ($VIBE_TEST_AUDIO/usac/*.mp4 + *.s16 — fate-suite aac/usac/Fd_*, aac/Fd_2_c1_Ms_*). The stream-rate output, after the
//      decoder's start-up delay, must match the reference to within a couple of 16-bit LSBs (SNR ≥ 60 dB).
//   2. THE TOOLS FFmpeg 8.1 COULD NOT DO ($VIBE_TEST_AUDIO/usac/xhe — README.txt there): USAC SBR, the switched FD/LPD core,
//      ACELP/TCX alone, MPEG Surround 2-1-2, PVC, inter-TES. Each against macOS AudioToolbox's decode of the same units,
//      the independent reference: the median 2048-sample block must agree to ≥ 60 dB (≥ 35 dB for the ACELP-only stream,
//      whose recursive synthesis filters carry float rounding along). PVC / inter-TES (AudioToolbox refuses them) must
//      decode every unit to finite audio at a sane level.
//   3. THE 48 kHz PATH THE SERVER USES: every stream comes out at 48 kHz for its full duration, never NaN, never above
//      +6 dBFS; a unit that fails its CRC is concealed (one unit of fading audio, the stream length unchanged).
// NOT RUN (exit 3) without libxaac (VIBE_HAVE_LIBXAAC) or without the test audio.
#include "vibe_xaac.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

#if !defined(VIBE_HAVE_LIBXAAC)
int main() { std::printf("   NOT RUN — this build has no libxaac (VIBE_HAVE_LIBXAAC)\n"); return 3; }
#else
struct Stream { std::vector<uint8_t> asc; std::vector<std::vector<uint8_t>> aus; uint32_t rate = 0; };

static bool slurp(const std::string& p, std::vector<uint8_t>& b) {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    std::fseek(f, 0, SEEK_END); b.resize(size_t(std::ftell(f))); std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(b.data(), 1, b.size(), f) == b.size(); std::fclose(f); return ok;
}

// ── a deliberately small MP4 reader (as drm-usac-stream.cpp): the first audio track's ASC and access units ──────────
static std::vector<uint8_t> mp4;
static uint32_t be32(size_t o) { return (uint32_t(mp4[o]) << 24) | (uint32_t(mp4[o + 1]) << 16) | (uint32_t(mp4[o + 2]) << 8) | mp4[o + 3]; }
static uint64_t be64(size_t o) { return (uint64_t(be32(o)) << 32) | be32(o + 4); }
struct Track { uint32_t timescale = 0; std::vector<uint8_t> asc; std::vector<uint32_t> sizes; std::vector<uint64_t> chunks;
               std::vector<std::array<uint32_t, 2>> stsc; bool audio = false; };
static size_t findTag(size_t p, size_t end, uint8_t tag, size_t& len) {
    while (p < end) {
        const uint8_t t = mp4[p++]; size_t l = 0;
        for (int i = 0; i < 4 && p < end; ++i) { const uint8_t b = mp4[p++]; l = (l << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        if (t == tag) { len = l; return p; }
        if (t == 0x03) { p += 3; continue; }
        if (t == 0x04) { p += 13; continue; }
        p += l;
    }
    return 0;
}
static void walk(size_t p, size_t end, Track& t) {
    while (p + 8 <= end) {
        uint64_t sz = be32(p); const std::string ty(reinterpret_cast<const char*>(&mp4[p + 4]), 4); size_t hdr = 8;
        if (sz == 1) { sz = be64(p + 8); hdr = 16; } else if (sz == 0) sz = end - p;
        const size_t b = p + hdr, e = p + size_t(sz);
        if (ty == "moov" || ty == "trak" || ty == "mdia" || ty == "minf" || ty == "stbl") walk(b, e, t);
        else if (ty == "mdhd") t.timescale = mp4[b] == 1 ? be32(b + 20) : be32(b + 12);
        else if (ty == "hdlr") t.audio = std::string(reinterpret_cast<const char*>(&mp4[b + 8]), 4) == "soun";
        else if (ty == "stsd" && t.audio) {
            for (size_t q = b + 8 + 36; q + 8 <= e; ) {
                const uint32_t csz = be32(q); const std::string cty(reinterpret_cast<const char*>(&mp4[q + 4]), 4);
                if (cty == "esds") { size_t l = 0; const size_t d = findTag(q + 12, q + csz, 0x05, l); if (d) t.asc.assign(&mp4[d], &mp4[d + l]); }
                if (csz < 8) break; q += csz;
            }
        }
        else if (ty == "stsz" && t.audio) { const uint32_t fixed = be32(b + 4), n = be32(b + 8);
            for (uint32_t i = 0; i < n; ++i) t.sizes.push_back(fixed ? fixed : be32(b + 12 + 4 * i)); }
        else if (ty == "stsc" && t.audio) { const uint32_t n = be32(b + 4);
            for (uint32_t i = 0; i < n; ++i) t.stsc.push_back({be32(b + 8 + 12 * i), be32(b + 12 + 12 * i)}); }
        else if (ty == "stco" && t.audio) { const uint32_t n = be32(b + 4); for (uint32_t i = 0; i < n; ++i) t.chunks.push_back(be32(b + 8 + 4 * i)); }
        else if (ty == "co64" && t.audio) { const uint32_t n = be32(b + 4); for (uint32_t i = 0; i < n; ++i) t.chunks.push_back(be64(b + 8 + 8 * i)); }
        if (ty == "trak" && t.audio && !t.sizes.empty()) return;
        p = e;
    }
}
static bool readMp4(const std::string& path, Stream& s) {
    if (!slurp(path, mp4)) return false;
    Track t; walk(0, mp4.size(), t);
    if (t.asc.empty() || t.sizes.empty() || t.chunks.empty() || t.stsc.empty()) return false;
    s.asc = t.asc; s.rate = t.timescale; size_t k = 0;
    for (size_t c = 0; c < t.chunks.size() && k < t.sizes.size(); ++c) {
        uint32_t per = t.stsc[0][1];
        for (const auto& r : t.stsc) if (c + 1 >= r[0]) per = r[1];
        uint64_t off = t.chunks[c];
        for (uint32_t i = 0; i < per && k < t.sizes.size(); ++i, ++k) { s.aus.emplace_back(&mp4[size_t(off)], &mp4[size_t(off)] + t.sizes[k]); off += t.sizes[k]; }
    }
    return true;
}
static bool readAus(const std::string& path, Stream& s) {   // "VAUS" files (usac/xhe/README.txt)
    std::vector<uint8_t> b; if (!slurp(path, b) || b.size() < 8 || std::memcmp(b.data(), "VAUS", 4)) return false;
    size_t p = 4; uint32_t l; std::memcpy(&l, &b[p], 4); p += 4; s.asc.assign(b.begin() + long(p), b.begin() + long(p + l)); p += l;
    while (p + 4 <= b.size()) { std::memcpy(&l, &b[p], 4); p += 4; if (p + l > b.size()) break; s.aus.emplace_back(b.begin() + long(p), b.begin() + long(p + l)); p += l; }
    return !s.aus.empty();
}
static std::vector<float> readF32(const std::string& p) { std::vector<uint8_t> b; std::vector<float> v; if (slurp(p, b)) { v.resize(b.size() / 4); std::memcpy(v.data(), b.data(), v.size() * 4); } return v; }

/** Decode a whole stream; native = at the stream's rate (else the server's 48 kHz path). corruptEvery: that unit's CRC "fails". */
static std::vector<float> decodeAll(const Stream& s, bool native, int corruptEvery, int& rate, int64_t& good, int64_t& bad) {
    vibe::XaacDecoder d; std::vector<float> out;
    if (!d.open(s.asc)) { rate = 0; return out; }
    for (size_t i = 0; i < s.aus.size(); ++i) {
        const bool ok = !(corruptEvery && (i + 1) % size_t(corruptEvery) == 0);
        if (native) d.decodeNative(s.aus[i].data(), s.aus[i].size(), ok, out);
        else d.decode(s.aus[i].data(), s.aus[i].size(), ok, out);
    }
    rate = native ? d.rateHz() : 48000; good = d.good(); bad = d.bad();
    return out;
}
static bool finiteAndSane(const std::vector<float>& v, float& peak) {
    peak = 0; for (float x : v) { if (!std::isfinite(x)) return false; peak = std::max(peak, std::fabs(x)); } return peak < 2.0f;
}
/** SNR (dB) of t against r, interleaved stereo, after the best lag of t in [0, maxLag]. */
static double snrLag(const std::vector<float>& t, const std::vector<float>& r, int maxLag, int& lagOut) {
    const size_t win = std::min<size_t>(r.size() / 2, 96000); double best = -1e300; int bl = 0;
    for (int lag = 0; lag <= maxLag; ++lag) {
        if (size_t(lag) * 2 + win * 2 > t.size()) break;
        double c = 0; for (size_t i = 0; i < win; ++i) c += double(t[2 * (i + size_t(lag))]) * r[2 * i];
        if (c > best) { best = c; bl = lag; }
    }
    lagOut = bl;
    const size_t n = std::min(t.size() / 2 - size_t(bl), r.size() / 2);
    double e = 0, d = 0;
    for (size_t i = 0; i < 2 * n; ++i) { const double a = t[2 * size_t(bl) + i], b = r[i]; e += b * b; d += (a - b) * (a - b); }
    return 10 * std::log10(e / std::max(d, 1e-30));
}
/** Median over 2048-frame blocks of the per-block SNR (blocks of near-silence skipped), zero lag. */
static double medianBlockSnr(const std::vector<float>& t, const std::vector<float>& r) {
    const size_t n = std::min(t.size(), r.size()) / 2, B = 2048; std::vector<double> s;
    for (size_t b = 0; b + B <= n; b += B) {
        double e = 0, d = 0;
        for (size_t i = 2 * b; i < 2 * (b + B); ++i) { e += double(r[i]) * r[i]; d += (double(t[i]) - r[i]) * (double(t[i]) - r[i]); }
        if (e > 1e-6) s.push_back(10 * std::log10(e / std::max(d, 1e-30)));
    }
    if (s.empty()) return 0;
    std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end()); return s[s.size() / 2];
}

int main() {
    const char* root = std::getenv("VIBE_TEST_AUDIO");
    const std::string base = std::string(root ? root : (std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/VibeSDR-test-audio").c_str()) + "/usac";
    std::printf("test-drm-xaac — %s, test audio %s\n", vibe::XaacDecoder::version().c_str(), base.c_str());
    int ran = 0;

    // ── 1. conformance: stream-rate output against ISO's reference decodes ─────────────────────────────────────────
    const struct { const char* n; int ch; } conf[] = { {"Fd_1_c1_0x03", 1}, {"Fd_2_c1_0x03", 2}, {"Fd_2_c1_0x05", 2},
        {"Fd_2_c1_Ms_0x01", 2}, {"Fd_2_c1_Ms_0x04", 2}, {"Fd_2_c1_Tns_0x04", 2} };
    for (const auto& c : conf) {
        Stream s; std::vector<uint8_t> ref;
        if (!readMp4(base + "/" + c.n + ".mp4", s) || !slurp(base + "/" + c.n + ".s16", ref)) { std::printf("   %s: not found, skipped\n", c.n); continue; }
        ++ran;
        std::vector<float> r(ref.size() / 2 / size_t(c.ch) * 2);
        const int16_t* p = reinterpret_cast<const int16_t*>(ref.data());
        for (size_t i = 0; i < r.size() / 2; ++i) { r[2 * i] = p[i * size_t(c.ch)] / 32768.0f; r[2 * i + 1] = p[i * size_t(c.ch) + (c.ch > 1)] / 32768.0f; }
        int rate = 0, lag = 0; int64_t g = 0, b = 0;
        const std::vector<float> t = decodeAll(s, true, 0, rate, g, b);
        const double snr = snrLag(t, r, 4096, lag);
        std::printf("   %-18s %5zu units %6d Hz: SNR %5.1f dB against the reference (delay %d)\n", c.n, s.aus.size(), rate, snr, lag);
        CHECK(rate == int(s.rate), "%s: decoded at %d Hz, the stream is %u Hz", c.n, rate, s.rate);
        CHECK(b == 0 && g == int64_t(s.aus.size()), "%s: %lld of %zu units decoded", c.n, (long long)g, s.aus.size());
        CHECK(snr >= 60.0, "%s: SNR %.1f dB < 60 dB against the conformance reference", c.n, snr);
    }

    // ── 2 + 3. the USAC tools FFmpeg could not do, against AudioToolbox; the server's 48 kHz path; concealment ──────────
    const struct { const char* n; double min; } xhe[] = {
        {"fd_st64_noesbr", 60}, {"fd_mono16_sbr", 60}, {"fd_st24_mps", 60}, {"sw_mono16_esbr", 60}, {"sw_speech12", 60},
        {"sw_st24_mps", 60}, {"td_speech12", 35}, {"fd_st32_sbr", 60}, {"sw_mono16_pvc", -1}, {"sw_st20_ites", -1} };
    for (const auto& x : xhe) {
        Stream s;
        if (!readAus(base + "/xhe/" + x.n + ".aus", s)) { std::printf("   %s: not found, skipped\n", x.n); continue; }
        ++ran;
        int rate = 0; int64_t g = 0, b = 0; float peak = 0;
        const std::vector<float> t = decodeAll(s, false, 0, rate, g, b);
        int nr = 0; int64_t ng = 0, nb = 0;
        const std::vector<float> nat = decodeAll(s, true, 0, nr, ng, nb);    // the stream's own rate: its true duration
        const double secs = double(t.size() / 2) / 48000.0, want = nr > 0 ? double(nat.size() / 2) / nr : 0.0;
        CHECK(g == int64_t(s.aus.size()) && b == 0, "%s: %lld of %zu units decoded", x.n, (long long)g, s.aus.size());
        CHECK(finiteAndSane(t, peak), "%s: output not finite or above +6 dBFS (peak %.2f)", x.n, peak);
        CHECK(std::fabs(secs - want) < 0.1, "%s: %.2f s at 48 kHz, the stream is %.2f s", x.n, secs, want);
        const std::vector<float> at = readF32(base + "/xhe/" + x.n + ".at.f32");
        double med = 0;
        if (!at.empty()) med = medianBlockSnr(t, at);
        std::printf("   %-16s %4zu units: %.2f s at 48 kHz, peak %.2f; against AudioToolbox: %s%.1f dB median block SNR%s\n", x.n, s.aus.size(),
                    secs, peak, at.empty() ? "(none) " : "", med, x.min < 0 && !at.empty() ? " — reported, not gated (docs/v12/LIBXAAC.md)" : "");
        if (x.min > 0) CHECK(!at.empty() && med >= x.min, "%s: median block SNR %.1f dB < %.0f dB against AudioToolbox", x.n, med, x.min);
        // ★ concealment: one unit in seven "fails its CRC" — the stream keeps its length, stays finite, no burst
        int r2 = 0; int64_t g2 = 0, b2 = 0;
        const std::vector<float> c = decodeAll(s, false, 7, r2, g2, b2);
        CHECK(b2 == int64_t(s.aus.size() / 7), "%s: %lld units concealed, %zu expected", x.n, (long long)b2, s.aus.size() / 7);
        CHECK(std::llabs(static_cast<long long>(c.size()) - static_cast<long long>(t.size())) <= 2 * 4096, "%s: concealed stream %zu samples, clean %zu", x.n, c.size(), t.size());
        CHECK(finiteAndSane(c, peak), "%s: concealed output not finite or too loud (peak %.2f)", x.n, peak);
    }

    if (!ran) { std::printf("   NOT RUN — no test audio under %s\n", base.c_str()); return 3; }
    std::printf("%s: %d/%d checks\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
#endif
