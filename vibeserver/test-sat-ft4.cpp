// test-sat-ft4.cpp — ★★★ beta97 FT4 / FT8 THROUGH A LINEAR TRANSPONDER, decoded on the listener's own Doppler-corrected
// channel ("decode here" — the ordinary single-dial spots run) and TAGGED with the bird (vibe_decoder_host.h setSatVia).
//
// ★★★ REAL AUDIO: Stuart's recording of JO-97's transponder on the XCover (USB 145.857 MHz, Doppler lock on, 48 kHz stereo,
//     its first sample at 20:53:56.0 UTC). vibeserver/testdata-sat/jo97-ft4-2026-11-27T205458Z-12k.wav is 20 s of it from
//     20:54:58.0 UTC: both channels mixed to mono at 12 kHz (ffmpeg -ac 1 -ar 12000 -sample_fmt s16), cut at an EXACT
//     sample offset (62 s × 12000) — upsampled to the host's 48 kHz here. tools/ft8-bench/ft8_wav --ft4 --passes 3 finds two
//     FT4 CQs in slot 1795812907.5: "CQ DL6UMR JO71" (−6 dB, 2020.8 Hz, pass 1) and "CQ PF7X JO22" (−13 dB, 1958.3 Hz,
//     pass 2) — no correction beyond the lock's own, so the residual drift over the slot is within the decoder's tolerance.
//  ★★ PF7X IS ALIGNMENT-SENSITIVE (a marginal pass-2 decode): it decodes with the window landing at DL6UMR dt −0.52 / −1.03
//     and NOT at −0.02 / −0.57 / −1.08 — so the clip's start time and its sample-exact cut matter. A 16 kHz clip cut from
//     20:55:06 (1.5 s before the slot) decoded DL6UMR only; this one begins a slot earlier.
//  Checks: both decode; each digital_spot carries "via":"JO-97" and "norad":43803; the frequency is the dial + audio
//  offset (the satellite frame — the dial the lock shows); NOTHING reaches PSK Reporter (held back — Stuart decides);
//  the station store gets them as ORIGIN kSatellite; and the control: the same audio with the lock off is reported.
#include "vibe_decoder_host.h"
#include "vibe_clock.h"
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct TestPeer : DecoderPeer {
    std::mutex m;
    std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t*, size_t) override {}
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string spotOf(const std::string& call) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& t : txt) if (t.find("\"digital_spot\"") != std::string::npos && t.find("\"callsign\":\"" + call + "\"") != std::string::npos) return t;
        return std::string();
    }
};

static bool readWav16(const char* path, std::vector<float>& out, int& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    size_t p = 12; int ch = 0, bits = 0; rate = 0;
    while (p + 8 <= b.size()) {
        const uint32_t sz = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = b[p + 10] | b[p + 11] << 8; rate = (int)(b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | (uint32_t)b[p + 15] << 24); bits = b[p + 22] | b[p + 23] << 8; }
        else if (!std::memcmp(&b[p], "data", 4)) {
            if (ch != 1 || bits != 16) return false;
            const size_t end = std::min(b.size(), p + 8 + (size_t)sz);
            for (size_t i = p + 8; i + 1 < end; i += 2) out.push_back((float)(int16_t)(b[i] | b[i + 1] << 8) / 32768.0f);
            return rate > 0;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}
template <class F> static bool waitFor(F f, double secs) {
    for (int i = 0; i < (int)(secs * 50); ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}

/** Spots on for session `s`, `via` set ("" = no lock), the clip fed at its capture time; returns the peer. */
static std::shared_ptr<TestPeer> run(DecoderRouter& router, const std::string& s, const std::string& via, const std::vector<float>& a48, double t0) {
    auto peer = std::make_shared<TestPeer>();
    auto h = router.hostFor(s, true);
    h->addPeer(peer);
    h->setSatVia(via, via.empty() ? 0 : 43803);
    if (h->startSpots() != DecoderHost::Start::Ok) { ok(false, "spots start"); return peer; }
    double clock = t0;
    for (size_t o = 0; o < a48.size(); o += 960) {
        while (h->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int n = (int)std::min<size_t>(960, a48.size() - o);
        router.feedSession(s, a48.data() + o, n, 1, clock + (double)n / 48000.0);
        clock += (double)n / 48000.0;
    }
    // a little silence past the slot so it is cut and decoded
    std::vector<float> z(960, 0.0f);
    for (int i = 0; i < 50 * 4; ++i) { router.feedSession(s, z.data(), 960, 1, clock + 0.02); clock += 0.02; }
    return peer;
}

int main() {
    const char* clip = "vibeserver/testdata-sat/jo97-ft4-2026-11-27T205458Z-12k.wav";
    std::vector<float> a12; int rate = 0;
    if (!readWav16(clip, a12, rate) || rate != 12000) { std::printf("  not run — %s missing or not 12 kHz mono PCM16\n", clip); return 3; }
    // ★ 12 → 48 kHz, linear (the signals sit at ~2 kHz; the images near 10 kHz are far outside the FT4 window)
    std::vector<float> a48(a12.size() * 4);
    for (size_t i = 0; i < a12.size(); ++i) {
        const float x0 = a12[i], x1 = i + 1 < a12.size() ? a12[i + 1] : x0;
        for (int k = 0; k < 4; ++k) a48[4 * i + k] = x0 + (x1 - x0) * (float)k / 4.0f;
    }
    /* 2026-11-27 20:54:58.0 UTC is the clip's first sample. ★ Fed as if heard NOW, on the same slot grid (a whole number of
     * 15 s, so both FT8's and FT4's slots line up as they did): the decoder judges how late a slot's decode starts against
     * the wall clock (Ft8Decoder::runDecode lagMs), and a slot from an hour ago is "too late" for the second pass — which
     * is the pass PF7X decodes on. */
    const double tRec = 1795812898.0;
    const double t0 = tRec + std::ceil((vibeUtcNow() - tRec) / 15.0) * 15.0;
    const double dial = 145857000.0;  // USB dial (the satellite frame the lock shows)

    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vibe-satft4-" + std::to_string(getpid());
    std::string mk = "mkdir -p '" + dir + "'"; if (std::system(mk.c_str()) != 0) return 2;
    FileSlots slots(dir, [] { return 4; });
    vibestations::Store stations(nullptr, false);
    std::mutex om; std::map<std::string, uint8_t> origins;   // call → the store's origin bits
    stations.subscribe([&](const vibestations::Reception& r) { std::lock_guard<std::mutex> lk(om); origins[r.call] |= r.origin; });
    DecoderRouter router([&](const std::string&) {
        DecoderHost::Env e; e.slots = &slots; e.stations = &stations;
        e.dialHz = [dial] { return dial; };
        return e; });

    // PSK Reporter ON with a receiver name: anything noted would wait in its queue (nothing is sent — no start())
    vibe::spotreport::Config pc; pc.on = true; pc.call = "M0TEST"; pc.grid = "IO92ng"; pc.pskHost = "127.0.0.1"; pc.pskPort = 9;
    vibe::spotreport::Reporter::shared().configure(pc);
    const size_t pend0 = vibe::spotreport::Reporter::shared().pendingCount();

    std::printf("── JO-97's FT4, through the lock (via JO-97) ──\n");
    auto p = run(router, "S", "JO-97", a48, t0);
    ok(waitFor([&] { return !p->spotOf("DL6UMR").empty() && !p->spotOf("PF7X").empty(); }, 40), "both CQs decode through the lock: DL6UMR and PF7X");
    const std::string a = p->spotOf("DL6UMR"), b = p->spotOf("PF7X");
    std::printf("     %s\n     %s\n", a.c_str(), b.c_str());
    ok(a.find("\"mode\":\"FT4\"") != std::string::npos && b.find("\"mode\":\"FT4\"") != std::string::npos, "…as FT4");
    ok(a.find("\"via\":\"JO-97\"") != std::string::npos && a.find("\"norad\":43803") != std::string::npos
       && b.find("\"via\":\"JO-97\"") != std::string::npos, "each spot carries via JO-97 and its NORAD number");
    ok(a.find("\"msg\":\"CQ DL6UMR JO71\"") != std::string::npos && b.find("\"msg\":\"CQ PF7X JO22\"") != std::string::npos, "the messages as decoded");
    auto freqOf = [](const std::string& t) { auto at = t.find("\"frequency\":"); return at == std::string::npos ? 0.0 : std::atof(t.c_str() + at + 12); };
    ok(std::fabs(freqOf(a) - (dial + 2020.8)) < 15 && std::fabs(freqOf(b) - (dial + 1958.3)) < 15,
       "at the dial + audio offset — the satellite frame (" + std::to_string((long long)freqOf(a)) + ", " + std::to_string((long long)freqOf(b)) + ")");
    ok(vibe::spotreport::Reporter::shared().pendingCount() == pend0, "★ NOTHING queued for PSK Reporter (held back)");
    { std::lock_guard<std::mutex> lk(om);
      ok(origins["DL6UMR"] == vibestations::kSatellite && origins["PF7X"] == vibestations::kSatellite, "the station store has them as ORIGIN satellite, not a 2 m reception"); }
    router.shutdownAll();

    std::printf("── control: the same audio with no lock ──\n");
    auto q = run(router, "T", "", a48, t0 + 30.0);   // the next slots on the same grid (FT4 slots by capture time)
    ok(waitFor([&] { return !q->spotOf("DL6UMR").empty(); }, 40), "decoded again");
    ok(q->spotOf("DL6UMR").find("\"via\"") == std::string::npos, "no via without a lock");
    ok(vibe::spotreport::Reporter::shared().pendingCount() > pend0, "…and this one IS queued for PSK Reporter");
    router.shutdownAll();
    vibe::spotreport::Reporter::shared().configure(vibe::spotreport::Config());

    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
