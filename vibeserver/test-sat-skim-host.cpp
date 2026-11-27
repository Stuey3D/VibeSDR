// test-sat-skim-host.cpp — ★★★ beta97 THE SATELLITE DIGITAL-MODE SKIMMER THROUGH THE DECODER HOST (vibe_decoder_host.h
// buildSkimLocked_ — the finder "xpdr" with its skimmer) and an UNATTENDED PASS's attach (vibe_sat_engine.h passDecoderOf):
//   1. ★★★ Stuart's JO-97 FT4 clip as the slice's IQ (the pass's Doppler on it, turned out by the finder), attached exactly
//      as a listener's SATELLITE pick attaches (decoderOf's params), fed on today's slot grid through DecoderHost::feedIq:
//      both CQs decode through the SKIMMER (not the listener's own channel — there is no audio), each digital_spot carries
//      "via":"JO-97", "norad":43803 and the pass; NOTHING reaches PSK Reporter; the station store has them as ORIGIN
//      satellite; xpdr_status carries "skim" with both in its "heard" list
//   2. SKIM off ({"type":"xpdr_skim","on":false} → DecoderHost::xpdrSkim): the same audio decodes nothing new; on again
//   3. ★★ THE BACKGROUND PASS: the owner's "skim transponder digital modes" (satPasses "skim":true) round-trips through the
//      settings; a linear transponder becomes a pass the scheduler keeps (passKeepableWith); the pass's attach is the
//      finder with "skim":1; started by the server (gallery::UnattendedStart, as satPassStartLocked does) on the SHARED host
//      it skims at once although the owner's speed would leave a listener's off ("catchup"), and its spots are tagged and
//      held back; a queued slot decoded AFTER the lock ended (satVia cleared) keeps the tag it was cut under
//   4. keep the corrected IQ: a bounded WAV in the gallery, its header patched with the real sizes
// Silent. Exit 3 = the clip is missing.
#include "vibe_decoder_host.h"
#include "vibe_bgdecode_config.h"
#include "vibe_clock.h"
#include "vibe_sat_engine.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
using cd = std::complex<double>;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }

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
    int spots() { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) if (t.find("\"digital_spot\"") != std::string::npos) ++n; return n; }
    std::string lastStatus() {
        std::lock_guard<std::mutex> lk(m);
        for (auto it = txt.rbegin(); it != txt.rend(); ++it) if (it->find("\"xpdr_status\"") != std::string::npos) return *it;
        return std::string();
    }
};
static void fft(std::vector<cd>& a, bool inv) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / (double)len * (inv ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { cd w(1); for (size_t k = 0; k < len / 2; ++k) { const cd u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
    }
}
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

static const double kFs = 48000.0;
static const double kDop = 2140.0;   // ★ the pass's Doppler at the slice centre at this moment (Hz) — the finder turns it out

/** Stuart's clip → the RF slice the radio would cut (fixed on the span's centre), the downlink `kDop` above its frame. */
static std::vector<cf32> clipSlice(const std::vector<float>& a12, double centre, double dial) {
    const size_t N = 262144, M = 4 * N;
    std::vector<cd> X(N);
    for (size_t i = 0; i < N; ++i) X[i] = i < a12.size() ? cd(a12[i], 0) : cd(0);
    fft(X, false);
    std::vector<cd> Y(M, cd(0));
    for (size_t k = 1; k < N / 2; ++k) Y[k] = 8.0 * X[k];
    fft(Y, true);
    double rms = 0; for (float v : a12) rms += (double)v * v; rms = std::sqrt(rms / a12.size());
    std::mt19937 rng(3); std::normal_distribution<float> g(0.0f, 1.0f);
    const float nz = (float)(rms * std::sqrt(kFs / 3000.0 / 2.0) * 0.1);   // ★ 20 dB under the recording's own noise: the empty windows are not silent, PF7X (marginal) is not pushed under
    const double shift = dial - centre + kDop;
    std::vector<cf32> iq(a12.size() * 4);
    for (size_t i = 0; i < iq.size(); ++i) {
        const double ph = 2 * M_PI * shift * (double)i / kFs;
        const cd v = Y[i] / (double)M * cd(std::cos(ph), std::sin(ph));
        iq[i] = cf32((float)v.real() + nz * g(rng), (float)v.imag() + nz * g(rng));
    }
    return iq;
}
static void feed(DecoderHost& h, const std::vector<cf32>& iq, double centre, double t0) {
    const int blk = 960;
    for (size_t off = 0; off < iq.size(); off += blk) {
        while (h.iqQueuedSec() > 1.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int n = (int)std::min<size_t>(blk, iq.size() - off);
        h.feedIq(iq.data() + off, n, kFs, centre, nullptr, 0, 0, t0 + (double)(off + n) / kFs);
    }
}
static double num(const std::string& j, const std::string& k) { auto p = j.find("\"" + k + "\":"); return p == std::string::npos ? NAN : std::atof(j.c_str() + p + k.size() + 3); }

int main() {
    const char* clip = "vibeserver/testdata-sat/jo97-ft4-2026-11-27T205458Z-12k.wav";
    std::vector<float> a12; int rate = 0;
    if (!readWav16(clip, a12, rate) || rate != 12000) { std::printf("  not run — %s missing\n", clip); return 3; }

    // JO-97's transponder from the catalogue — the params a pick (and a pass) attaches with
    vibe::sat::SatDef jo; bool have = false;
    for (auto& d : vibe::sat::curatedCatalogue()) if (d.norad == 43803) { jo = d; have = true; }
    if (!have) { ok(false, "JO-97 in the catalogue"); return 1; }
    std::string params;
    const std::string dec = vibe::sat::decoderOf(jo.tx[0], params);
    ok(dec == "xpdr" && params.find("\"plo\":145855000") != std::string::npos && params.find("\"phi\":145875000") != std::string::npos,
       "decoderOf: JO-97's finder carries its passband for the skimmer (" + params + ")");
    const double centre = jo.tx[0].xpdrCentre(), dial = 145857000.0;
    const std::vector<cf32> iq = clipSlice(a12, centre, dial);
    const double tRec = 1795812898.0;
    auto grid = [&](double extra) { return tRec + std::ceil((vibeUtcNow() + extra - tRec) / 15.0) * 15.0; };

    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vibe-satskim-" + std::to_string(getpid());
    std::string mk = "mkdir -p '" + dir + "/gallery'"; if (std::system(mk.c_str()) != 0) return 2;
    FileSlots slots(dir, [] { return 4; });
    vibestations::Store stations(nullptr, false);
    std::mutex om; std::map<std::string, uint8_t> origins;
    stations.subscribe([&](const vibestations::Reception& r) { std::lock_guard<std::mutex> lk(om); origins[r.call] |= r.origin; });
    std::string skimMode = "auto";
    bool keepIq = false;
    std::vector<std::string> logs; std::mutex lm;
    DecoderRouter router([&](const std::string&) {
        DecoderHost::Env e; e.slots = &slots; e.stations = &stations;
        e.dialHz = [dial] { return dial; };
        e.satDopplerHz = [] { return kDop; };
        e.satSkimMode = [&] { return skimMode; };
        e.satSkimKeepIq = [&] { return keepIq; };
        e.bandBudgetUnits = [] { return 100.0; };
        e.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); };
        return e; });
    vibe::spotreport::Config pc; pc.on = true; pc.call = "M0TEST"; pc.grid = "IO92ng"; pc.pskHost = "127.0.0.1"; pc.pskPort = 9;
    vibe::spotreport::Reporter::shared().configure(pc);
    const size_t pend0 = vibe::spotreport::Reporter::shared().pendingCount();
    const std::string attach = vibe::sat::decoderAttachMsg("xpdr", params);

    if (std::getenv("SKIM_DIRECT")) {   // ★ debugging: the same slice straight through a Run + Skimmer (no host)
        std::mutex dm; std::vector<std::string> got;
        satskim::Skimmer::Config c;
        c.centreHz = centre; c.loHz = 145855000; c.hiHz = 145875000; c.workers = 4;
        c.emit = [&](const digi::Spot& s, double, const satskim::Tag&) { std::lock_guard<std::mutex> lk(dm); got.push_back(s.text); };
        satskim::Skimmer sk(c);
        vibexpdr::Run::Opts o;
        o.widthHz = 52876; o.centreHz = centre; o.spanLoHz = 145840000; o.spanHiHz = 145875000;
        o.dopplerHz = [&](double hz) { return kDop * hz / centre; };
        o.turned = [&](const cf32* x, int n, double r, double utc, bool locked) { if (locked) sk.feed(x, n, r, utc); };
        if (std::getenv("SKIM_TLM")) { o.tlmHz = 145840000; }
        vibexpdr::Run run(std::move(o));
        const double t0 = grid(0);
        for (size_t off = 0; off < iq.size(); off += 960) { const int n = (int)std::min<size_t>(960, iq.size() - off); run.feedIq(iq.data() + off, n, kFs, centre, t0 + (double)(off + n) / kFs); }
        sk.waitIdle(60);
        for (auto& g : got) std::printf("     direct: %s\n", g.c_str());
    }
    std::printf("── 1. a listener's SATELLITE pick of JO-97: the skimmer decodes the transponder ──\n");
    {
        auto peer = std::make_shared<TestPeer>();
        auto h = router.hostFor("S", true);
        h->addPeer(peer);
        h->setSatVia("JO-97", 43803, 1795812600);
        ok(h->start("xpdr", attach) == DecoderHost::Start::Ok, "the finder starts");
        ok(h->satSkimmer() && h->satSkimmer()->on() && h->satSkimmer()->dials().size() == 9, "its skimmer is on (benchmark: live) with 9 windows over 145.855–145.875");
        feed(*h, iq, centre, grid(0));
        ok(waitFor([&] { return !peer->spotOf("DL6UMR").empty() && !peer->spotOf("PF7X").empty(); }, 60), "★ both CQs decode through the skimmer: DL6UMR and PF7X");
        const std::string a = peer->spotOf("DL6UMR"), b = peer->spotOf("PF7X");
        std::printf("     %s\n     %s\n", a.c_str(), b.c_str());
        ok(a.find("\"mode\":\"FT4\"") != std::string::npos && b.find("\"msg\":\"CQ PF7X JO22\"") != std::string::npos, "as FT4, with the messages");
        ok(a.find("\"via\":\"JO-97\"") != std::string::npos && a.find("\"norad\":43803") != std::string::npos && a.find("\"pass\":1795812600") != std::string::npos
           && b.find("\"via\":\"JO-97\"") != std::string::npos, "each digital_spot carries via JO-97, its NORAD number and the pass");
        ok(std::fabs(num(a, "frequency") - (dial + 2020.8)) < 15 && std::fabs(num(b, "frequency") - (dial + 1958.3)) < 15, "at the satellite frame (the dial + audio offset)");
        ok(vibe::spotreport::Reporter::shared().pendingCount() == pend0, "★ NOTHING queued for PSK Reporter (held back)");
        { std::lock_guard<std::mutex> lk(om); ok(origins["DL6UMR"] == vibestations::kSatellite && origins["PF7X"] == vibestations::kSatellite, "the station store has them as ORIGIN satellite"); }
        ok(waitFor([&] { const std::string s = peer->lastStatus(); return s.find("\"skim\":{") != std::string::npos && s.find("\"c\":\"PF7X\"") != std::string::npos; }, 5),
           "xpdr_status carries the skim object with both in its heard list");
        std::printf("     %s\n", peer->lastStatus().substr(peer->lastStatus().find("\"skim\"")).c_str());
        if (std::getenv("SKIM_DEBUG")) {
            { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) std::printf("     log: %s\n", l.c_str()); }
            auto st = h->satSkimmer()->stats();
            std::printf("     stats: decoded %llu queued %zu mean %.1f ms last %.1f ms depth %d\n", (unsigned long long)st.tasksDecoded, st.queuedTasks, st.meanTaskMs, st.lastTaskMs, st.depth);
        }

        std::printf("── 2. SKIM off: nothing new; on again ──\n");
        ok(h->xpdrSkim(false) && !h->satSkimmer()->on(), "xpdr_skim off");
        const int before = peer->spots();
        feed(*h, iq, centre, grid(30));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        h->satSkimmer()->waitIdle(30);
        ok(peer->spots() == before, "the same audio again: no spots while off");
        ok(h->xpdrSkim(true) && h->satSkimmer()->on(), "on again");
        h->stop();
    }

    std::printf("── 3. the BACKGROUND pass: \"skim transponder digital modes\" ──\n");
    {
        bg::Config c; std::string err;
        ok(bg::parse("{\"satPasses\":{\"on\":true,\"minEl\":20,\"birds\":[{\"norad\":43803,\"label\":\"Transponder\"}],\"skim\":true,\"skimMode\":\"catchup\",\"skimIq\":true}}", c, err)
           && c.sat.skim && c.sat.skimMode == "catchup" && c.sat.skimIq, "the settings read the skim switch, its speed and keep-IQ");
        const std::string out = bg::toJson(c);
        ok(out.find("\"skim\":true") != std::string::npos && out.find("\"skimMode\":\"catchup\"") != std::string::npos && out.find("\"skimIq\":true") != std::string::npos,
           "…and write them back");
        bg::Config d; ok(bg::parse("{\"satPasses\":{\"on\":true,\"minEl\":20,\"birds\":[]}}", d, err) && !d.sat.skim && d.sat.skimMode == "auto" && !d.sat.skimIq
           && bg::toJson(d).find("skim") == std::string::npos, "an older setting: off, auto — and written back without them");
        ok(!vibe::sat::passKeepable(jo.tx[0]) && !vibe::sat::passKeepableWith(jo.tx[0], false) && vibe::sat::passKeepableWith(jo.tx[0], true),
           "a linear transponder is a pass worth taking only with the skim switch on");
        std::string pp;
        ok(vibe::sat::passDecoderOf(jo.tx[0], true, pp) == "xpdr" && pp.find("\"skim\":1") != std::string::npos, "the pass attaches the finder with \"skim\":1");
        std::string pq; vibe::sat::passDecoderOf(jo.tx[0], false, pq);
        ok(pq.find("skim") == std::string::npos, "…and without it when the switch is off");

        skimMode = "catchup";   // ★ the owner's speed would leave a LISTENER's skimmer off; the pass forces it on
        keepIq = true;
        gallery::shared().setDir(dir + "/gallery");
        gallery::shared().setLimitBytes(400ull << 20);
        auto peer = std::make_shared<TestPeer>();
        auto h = router.hostFor("", false);   // the shared host — what an unattended pass runs on
        h->addPeer(peer);
        h->setSatVia("JO-97", 43803, 1795813200);
        DecoderHost::Start r;
        { gallery::UnattendedStart u("Satellite pass"); r = h->start("xpdr", vibe::sat::decoderAttachMsg("xpdr", pp)); }
        ok(r == DecoderHost::Start::Ok && h->satSkimmer() && h->satSkimmer()->on(), "started by the server: the skimmer is on although the owner's speed is catch-up");
        ok(h->satSkimmer() && h->satSkimmer()->config().background && h->satSkimmer()->config().mode == satskim::Skimmer::Mode::CatchUp,
           "…in the background thread class, at the owner's speed (catch-up: one worker)");
        const double t0 = grid(60);
        feed(*h, iq, centre, t0);
        // ★ the lock ends at LOS before the queue is worked off: what was cut under the pass keeps its tag
        h->setSatVia("", 0);
        ok(waitFor([&] { return !peer->spotOf("DL6UMR").empty() && !peer->spotOf("PF7X").empty(); }, 90), "both CQs decode on the unattended pass");
        if (std::getenv("SKIM_DEBUG")) {
            auto st = h->satSkimmer()->stats();
            std::printf("     stats: decoded %llu queued %zu mean %.1f ms spots %llu depth %d\n", (unsigned long long)st.tasksDecoded, st.queuedTasks, st.meanTaskMs, (unsigned long long)st.spots, st.depth);
            for (auto& hd : h->satSkimmer()->heard()) std::printf("     heard %s %s %d\n", hd.mode.c_str(), hd.text.c_str(), hd.snr);
            std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) std::printf("     log: %s\n", l.c_str());
        }
        const std::string a = peer->spotOf("PF7X");
        ok(a.find("\"via\":\"JO-97\"") != std::string::npos && a.find("\"pass\":1795813200") != std::string::npos, "tagged with the bird and THIS pass, though the lock has ended");
        ok(vibe::spotreport::Reporter::shared().pendingCount() == pend0, "held back from PSK Reporter");
        h->stop();   // ★ the kept IQ closes with the finder
        std::string name;
        { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) if (l.find("corrected IQ kept") != std::string::npos) name = l; }
        ok(!name.empty(), "4. the corrected IQ was kept: " + name);
        // the WAV's header carries its true sizes
        bool hdr = false;
        {
            std::string cmd = "ls '" + dir + "/gallery' | grep '\\.wav$' | head -1";
            FILE* p = popen(cmd.c_str(), "r"); char fn[512] = {0};
            if (p) { if (!std::fgets(fn, sizeof fn, p)) fn[0] = 0; pclose(p); }
            std::string f = fn; while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back();
            FILE* w = f.empty() ? nullptr : std::fopen((dir + "/gallery/" + f).c_str(), "rb");
            if (w) {
                uint8_t hd[44]; const size_t got = std::fread(hd, 1, 44, w);
                std::fseek(w, 0, SEEK_END); const long sz = std::ftell(w); std::fclose(w);
                const uint32_t riff = hd[4] | hd[5] << 8 | hd[6] << 16 | (uint32_t)hd[7] << 24, data = hd[40] | hd[41] << 8 | hd[42] << 16 | (uint32_t)hd[43] << 24;
                const uint32_t sr = hd[24] | hd[25] << 8 | hd[26] << 16 | (uint32_t)hd[27] << 24;
                hdr = got == 44 && (long)data + 44 == sz && riff == data + 36 && sr == 48000 && hd[22] == 2 && hd[34] == 16;
                std::printf("     %s: %ld bytes, data %u, %u S/s\n", f.c_str(), sz, data, sr);
            }
        }
        ok(hdr, "a 2-channel 16-bit WAV at the slice's rate, its header patched with the real sizes");
    }
    router.shutdownAll();
    vibe::spotreport::Reporter::shared().configure(vibe::spotreport::Config());
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::printf("%d passed, %d failed\n%s\n", passes, fails, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
