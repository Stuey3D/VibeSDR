// test-lrpt-host.cpp — METEOR-M LRPT as the server runs it (docs/v12/LRPT.md): the DecoderHost's "lrpt" extension on
// its IQ path (the skimmers'), the SSTV-shaped preview a client draws, lrpt_status, the pass's PNGs through the gallery
// tap into a real gallery Store, noise (nothing, ever), and the reach / caps gate (137 MHz, never an Icom).
#include "vibe_decoder_host.h"
#include "vibe_bgdecode.h"
#include "vibe_decoder_reach.h"
#include "vibe_radio_caps.h"
#include "vibe_lrpt_tx.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vibe;
static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Peer : DecoderPeer {
    std::mutex m;
    int starts = 0, rows = 0, completes = 0, status = 0, badRow = 0, back = 0; long lastY = -1; uint32_t w = 0, h = 0;
    std::string lastStatus;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override {
        std::lock_guard<std::mutex> lk(m);
        auto u32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
        if (n >= 9 && d[0] == 0x07) { starts++; w = u32(1); h = u32(5); }
        else if (n >= 9 && d[0] == 0x01) {
            const uint32_t y = u32(1), lw = u32(5);
            if (lw != w || n != 9 + size_t(lw) * 3) badRow++;
            if (long(y) <= lastY) back++;            // ★ a line number going back reads as a NEW picture on both clients
            lastY = long(y); rows++;
        } else if (n >= 1 && d[0] == 0x05) completes++;
    }
    void text(const std::string& s) override {
        std::lock_guard<std::mutex> lk(m);
        if (s.find("\"lrpt_status\"") != std::string::npos) { status++; lastStatus = s; }
    }
};
struct PicTap : DecoderTap {
    std::mutex m; std::vector<std::string> names; std::vector<bool> partial; size_t bytes = 0;
    void picture(const std::string& n, const std::string& png, int, int, bool p) override {
        std::lock_guard<std::mutex> lk(m); names.push_back(n); partial.push_back(p); bytes += png.size();
    }
};

static std::vector<std::complex<float>> lrptIq(double rate, int periods, double esn0, uint32_t seed) {
    vibelrpt::tx::TxConfig t; t.sampleRate = rate; t.esn0Db = esn0; t.carrierHz = -1300; t.dopplerRateHzS = 40; t.seed = seed;
    const auto iq = vibelrpt::tx::modulate(t, vibelrpt::tx::codeBits(t, vibelrpt::tx::buildVcdus(t, periods)));
    std::vector<std::complex<float>> c(iq.size() / 2);
    for (size_t i = 0; i < c.size(); i++) c[i] = { iq[2 * i], iq[2 * i + 1] };
    return c;
}
static void feed(DecoderHost& h, const std::vector<std::complex<float>>& x, double rate) {
    const size_t blk = size_t(rate / 20);
    for (size_t at = 0; at < x.size(); at += blk) {
        while (h.queuedIq() > 0.5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        h.feedIq(x.data() + at, int(std::min(blk, x.size() - at)), rate, 137.9e6);
    }
    for (int i = 0; i < 5000 && h.queuedIq() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
static const std::string kAttach = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"lrpt\"}";   // ★ what the clients send: the rate AUTO

static void testHost() {
    std::printf("the host's lrpt extension\n");
    auto peer = std::make_shared<Peer>();
    auto tap = std::make_shared<PicTap>();
    DecoderHost::Env env;
    env.dialHz = [] { return 137.9e6; };
    env.tap = [tap](const std::string& ext, const std::string&) -> DecoderTapPtr { return ext == "lrpt" ? tap : nullptr; };
    std::atomic<int> dopplerAsked{0};
    env.satDopplerHz = [&dopplerAsked] { dopplerAsked++; return 0.0; };
    {
        DecoderHost h(env, "s1");
        h.addPeer(peer);
        CHECK(h.start("lrpt", kAttach) == DecoderHost::Start::Ok, "start");
        CHECK(h.kind() == "lrpt", "kind %s", h.kind().c_str());
        CHECK(h.wantsIq() && std::fabs(h.iqWidthHz() - vibelrpt::Run::kSliceWidthHz) < 1, "IQ slice %.0f Hz", h.iqWidthHz());
        CHECK(h.start("lrpt", kAttach) == DecoderHost::Start::Joined, "same parameters joins");
        const double rate = 150000;
        feed(h, lrptIq(rate, 14, 12, 3), rate);
        {
            std::lock_guard<std::mutex> lk(peer->m);
            CHECK(peer->starts == 1 && peer->w == 392 && peer->h == 1600, "preview start %d (%ux%u)", peer->starts, peer->w, peer->h);
            CHECK(peer->rows >= 2 * 10, "preview rows %d", peer->rows);
            CHECK(peer->badRow == 0 && peer->back == 0, "rows malformed %d, gone back %d", peer->badRow, peer->back);
            CHECK(peer->status >= 3 && peer->lastStatus.find("\"data\":{") != std::string::npos, "lrpt_status x%d", peer->status);
            CHECK(peer->lastStatus.find("\"channels\":[\"1 visible\",\"2 near-IR\",\"5 thermal\"]") != std::string::npos, "status channels: %s", peer->lastStatus.c_str());
            std::printf("  preview %d rows, %d status frames; last: %s\n", peer->rows, peer->status, peer->lastStatus.c_str());
        }
        CHECK(dopplerAsked.load() > 10, "the Doppler feed-forward was read %d times", dopplerAsked.load());
        h.stop();
    }
    {
        std::lock_guard<std::mutex> lk(tap->m);
        CHECK(tap->names.size() == 4, "pictures to the gallery tap: %zu", tap->names.size());
        bool allPartial = true; for (bool p : tap->partial) allPartial = allPartial && p;
        CHECK(allPartial, "a stop mid-pass marks them partial");
        std::printf("  gallery tap: %zu pictures, %zu bytes:", tap->names.size(), tap->bytes);
        for (auto& n : tap->names) std::printf(" [%s]", n.c_str());
        std::printf("\n");
    }
    { std::lock_guard<std::mutex> lk(peer->m); CHECK(peer->completes == 1, "0x05 complete at the end: %d", peer->completes); }
}

static void testPassEndAndGallery() {
    std::printf("the pass ends by itself; the gallery Store keeps standard PNGs\n");
    char tmpl[] = "/tmp/vibe-lrpt-gal-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "temp dir");
    if (!dir) return;
    gallery::Store store; store.setDir(dir); store.setLimitBytes(64ull << 20);
    gallery::Meta meta; meta.kind = "lrpt"; meta.origin = "live"; meta.freqHz = 137.9e6; meta.decoder = "lrpt";
    CHECK(bg::galleryKindFor("lrpt", kAttach) == "lrpt", "gallery kind");
    DecoderHost::Env env;
    env.dialHz = [] { return 137.9e6; };
    env.tap = [&](const std::string& ext, const std::string& msg) -> DecoderTapPtr { gallery::Meta m = meta; m.kind = bg::galleryKindFor(ext, msg); return bg::makeGalleryTap(store, m); };
    {
        DecoderHost h(env, "s2");
        auto peer = std::make_shared<Peer>(); h.addPeer(peer);
        CHECK(h.start("lrpt", kAttach) == DecoderHost::Start::Ok, "start");
        const double rate = 150000;
        auto x = lrptIq(rate, 8, 14, 5);
        // then 32 s of noise: the receiver decides the pass is over and files it, not partial
        std::mt19937 g(1); std::normal_distribution<float> n(0, 0.3f);
        const size_t tail = size_t(rate * 32);
        x.reserve(x.size() + tail);
        for (size_t i = 0; i < tail; i++) x.push_back({ n(g), n(g) });
        feed(h, x, rate);
        const std::string list = store.listJson("lrpt", 50);
        int items = 0; for (size_t p = list.find("\"k\":\"lrpt\""); p != std::string::npos; p = list.find("\"k\":\"lrpt\"", p + 1)) items++;
        CHECK(items == 4, "4 items filed at the pass's end (%d): %s", items, list.substr(0, 300).c_str());
        CHECK(list.find("\"partial\":true") == std::string::npos, "a pass that ended is not partial");
        CHECK(list.find("METEOR LRPT \xc2\xb7 RGB 221 false colour") != std::string::npos, "the composite's title");
        h.stop();
        const std::string after = store.listJson("lrpt", 50);
        int items2 = 0; for (size_t p = after.find("\"k\":\"lrpt\""); p != std::string::npos; p = after.find("\"k\":\"lrpt\"", p + 1)) items2++;
        CHECK(items2 == 4, "a stop after the pass was filed adds nothing (%d)", items2);
    }
    // every file is a PNG
    for (const auto& nm : store.names("lrpt")) {
        const std::string path = std::string(dir) + "/" + nm;
        FILE* f = std::fopen(path.c_str(), "rb"); unsigned char sig[8] = {};
        const bool got = f && std::fread(sig, 1, 8, f) == 8; if (f) std::fclose(f);
        CHECK(got && !std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8), "%s is a PNG", nm.c_str());
        std::remove(path.c_str());
    }
    std::string cmd = std::string("rm -rf '") + dir + "'"; (void)std::system(cmd.c_str());
}

static void testNoise() {
    std::printf("noise through the host: no picture, no gallery item\n");
    auto peer = std::make_shared<Peer>();
    auto tap = std::make_shared<PicTap>();
    DecoderHost::Env env;
    env.dialHz = [] { return 137.1e6; };
    env.tap = [tap](const std::string&, const std::string&) -> DecoderTapPtr { return tap; };
    {
        DecoderHost h(env, "s3"); h.addPeer(peer);
        CHECK(h.start("lrpt", kAttach) == DecoderHost::Start::Ok, "start");
        std::mt19937 g(4); std::normal_distribution<float> n(0, 1);
        std::vector<std::complex<float>> x(size_t(150000 * 30));
        for (auto& v : x) v = { n(g), n(g) };
        feed(h, x, 150000);
        h.stop();
    }
    std::lock_guard<std::mutex> lk(peer->m), lk2(tap->m);
    CHECK(peer->starts == 0 && peer->rows == 0 && tap->names.empty(), "30 s of noise: %d starts, %d rows, %zu pictures", peer->starts, peer->rows, tap->names.size());
}

static void testReach() {
    std::printf("reach and caps\n");
    vibereach::Reach r;
    r.ranges = vibereach::reachRanges("airspyhf");
    CHECK(vibereach::whyNot("lrpt", r).empty(), "an HF+ reaches 137 MHz");
    r.ranges = vibereach::reachRanges("rtl", 0, 0, 0, 0);
    CHECK(vibereach::whyNot("lrpt", r).empty(), "an RTL reaches 137 MHz");
    r.ranges = { { 0.0, 30e6 } };
    const std::string why = vibereach::whyNot("lrpt", r);
    CHECK(why.find("137.100 and 137.900 MHz") != std::string::npos, "an HF-only radio is told why: %s", why.c_str());
    r.ranges = { { 88e6, 108e6 } }; r.locked = true;
    CHECK(vibereach::whyNot("lrpt", r).find("locked range") != std::string::npos, "a locked range says so");
    vibecaps::Facts f; f.driver = "icom"; f.ranges = { { 0.03e6, 74e6 }, { 108e6, 174e6 } };
    CHECK(!vibecaps::compute(f).has("lrpt"), "an Icom never offers LRPT (no IQ)");
    f.driver = "rtl"; f.ranges = { { 24e6, 1766e6 } };
    CHECK(vibecaps::compute(f).has("lrpt"), "an RTL offers LRPT");
    f.ranges = { { 400e6, 1766e6 } };
    CHECK(!vibecaps::compute(f).has("lrpt"), "a radio above 137 MHz does not");
}

int main() {
    testReach();
    testHost();
    testPassEndAndGallery();
    testNoise();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
