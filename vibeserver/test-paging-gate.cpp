// test-paging-gate.cpp — WHERE PAGING MAY RUN (vibe_paging_gate.h, DecoderHost::pagingProcess). V12, 2026-11-16, for the R36S.
//
// ★★★ WHAT IS PINNED. Paging is local hardware only: the Android app, or the R36S handheld's radio process — the user's own
//     receiver on that device's own screen. The desktop VibeServer now COMPILES the engine (the handheld's radio process is
//     that binary), so this proves the build alone opens nothing:
//   • the rule, every combination: the engine offers it only built + allowed process + own device + not serving + a USB
//     radio (one of 32); a socket starts it only with that AND a loopback peer AND not VFO B (one of 8);
//   • a DecoderHost in a SERVER process (pagingProcess false, the default off Android) does not know "paging" at all —
//     start() says Unknown, nothing is sent, the channel IQ is not wanted;
//   • in the handheld's process it runs: our own FLEX pages (synthetic, made up here) reach the decoder socket that started
//     it — and NOTHING reaches the gallery (no tap is asked for), the host's log (no page text), or a socket that was not
//     the paging listener's (a second host's peer);
//   • the speaker: silence while a paging receiver runs, the person's own level the moment it stops — never changed;
//   • ★★★ RECORDINGS (Stuart, 2026-11-16: "nobody can record and decode again later"): what leaves the engine
//     (pagingAudioOut — every audio stream, int16 and the float VFO streams) is silence while a real receiver runs, so a
//     recording made of it is silence from its start when PAGE was already on, silence from the moment PAGE starts in the
//     middle of one, and the ordinary audio again, sample for sample, once PAGE stops; raw IQ out is refused meanwhile.
// ★ Silent; deterministic; no real paging traffic anywhere.
#include "vibe_decoder_host.h"
#include "vibe_paging_gate.h"
#include "benchsynth/flex_synth.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

struct Peer : DecoderPeer {
    std::mutex m;
    std::vector<std::vector<uint8_t>> bin;
    std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string lines() {
        std::lock_guard<std::mutex> lk(m); std::string o;
        for (auto& f : bin) if (f.size() >= 13 && f[0] == 0x01) {
            const uint32_t len = (uint32_t(f[9]) << 24) | (uint32_t(f[10]) << 16) | (uint32_t(f[11]) << 8) | f[12];
            if (13 + len == f.size()) o.append((const char*)f.data() + 13, len);
        }
        return o;
    }
    size_t frames() { std::lock_guard<std::mutex> lk(m); return bin.size() + txt.size(); }
};

static std::vector<std::complex<float>> flexIq() {
    std::vector<flexsyn::TxPage> pages(2);
    pages[0].cap = 123456; pages[0].kind = "alpha"; pages[0].text = "GATE TEST PAGE ONE";
    pages[1].cap = 234567; pages[1].kind = "alpha"; pages[1].text = "GATE TEST PAGE TWO";
    std::vector<flexsyn::Sym> sy;
    for (int f = 0; f < 4; ++f) { auto fr = flexsyn::frame(1600, 2, 1, 10 + f, f == 0 ? pages : std::vector<flexsyn::TxPage>{}); sy.insert(sy.end(), fr.begin(), fr.end()); }
    return flexsyn::fmIq(sy, 31250, 0, false, 24.0, 21, 16000);
}

/** A block of the channel's audio as the engine would send it: a 1 kHz tone, never silent. */
static std::vector<int16_t> toneBlock(int frames, int ch, int& phase) {
    std::vector<int16_t> b((size_t)frames * ch);
    for (int i = 0; i < frames; ++i, ++phase) {
        const int16_t v = (int16_t)std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * phase / 48000.0));
        for (int c = 0; c < ch; ++c) b[(size_t)i * ch + c] = v;
    }
    return b;
}
/** One block through the engine's exit rule (what local_sdr_shim.cpp's senders do), appended to `rec` — the recorder. */
static void sendBlock(std::vector<int16_t>& rec, const std::vector<int16_t>& blk) {
    std::vector<int16_t> z;
    const int16_t* out = pagingAudioOut(blk.data(), blk.size(), DecoderHost::pagingRunning().load(), z);
    rec.insert(rec.end(), out, out + blk.size());
}
static bool allZero(const std::vector<int16_t>& v, size_t a, size_t b) { for (size_t i = a; i < b; ++i) if (v[i]) return false; return true; }
static bool sameAs(const std::vector<int16_t>& v, size_t at, const std::vector<int16_t>& ref) {
    for (size_t i = 0; i < ref.size(); ++i) if (v[at + i] != ref[i]) return false;
    return true;
}

int main() {
    std::printf("── the rule (vibe_paging_gate.h) ──\n");
    {
        int yes = 0, attachYes = 0;
        for (int b = 0; b < 32; ++b) {
            const bool built = b & 1, process = b & 2, own = b & 4, serving = b & 8, usb = b & 16;
            const bool e = pagingGateEngine(built, process, own, serving, usb);
            if (e) { ++yes; ok(built && process && own && !serving && usb, "the one engine case: built, allowed process, own device, not serving, USB radio"); }
            for (int a = 0; a < 4; ++a) if (pagingGateAttach(e, a & 1, a & 2)) { ++attachYes; ok(e && (a & 1) && !(a & 2), "the one attach case: engine ok, loopback peer, not VFO B"); }
        }
        ok(yes == 1, "the engine offers paging in exactly 1 of 32 cases (" + std::to_string(yes) + ")");
        ok(attachYes == 1, "a socket may start it in exactly 1 of 128 cases (" + std::to_string(attachYes) + ")");
        ok(!pagingGateEngine(true, true, true, true, true), "serving (a LAN / tunnel / proxy client can exist): never");
        ok(!pagingGateEngine(true, false, true, false, true), "a server process (not the app, not the handheld's radio): never");
        ok(!pagingGateAttach(true, false, false), "a socket from the network: never, even where the engine may run it");
        ok(std::string(pagingRefusalText(true, false)).find("handheld") != std::string::npos, "the handheld's refusal names the handheld");
        ok(std::string(pagingRefusalText(false, false)).find("never decoded by a server") != std::string::npos, "a server's refusal says servers never decode it");
    }

    std::printf("── the speaker (pagingSpeakerLevel) ──\n");
    ok(pagingSpeakerLevel(0.8f, 1) == 0.0f, "paging running: silence, whatever the level");
    ok(pagingSpeakerLevel(0.8f, 0) == 0.8f, "paging stopped: the person's own level, unchanged");
    ok(pagingSpeakerLevel(0.0f, 0) == 0.0f, "muted before: still muted after");

    std::printf("── what leaves the engine (pagingAudioOut, pagingAllowsIqOut) ──\n");
    {
        std::vector<int16_t> z; std::vector<float> zf;
        const int16_t a[4] = {100, -200, 300, -400};
        const float f[3] = {0.5f, -0.25f, 0.125f};
        ok(pagingAudioOut(a, 4, 0, z) == a, "no paging: the block itself, untouched");
        const int16_t* o = pagingAudioOut(a, 4, 1, z);
        ok(o != a && o[0] == 0 && o[3] == 0 && z.size() == 4, "paging running: as many samples, all silence");
        const float* of = pagingAudioOut(f, 3, 2, zf);
        ok(of != f && of[0] == 0.0f && of[2] == 0.0f, "…the float streams (a VFO's own recording stream) too");
        ok(pagingAllowsIqOut(0) && !pagingAllowsIqOut(1), "raw IQ out: allowed with no receiver, refused while one runs");
        ok(std::string(pagingIqOutRefusal()).find("while PAGE runs") != std::string::npos, "…and the refusal says why");
    }

    std::printf("── a SERVER process: paging is not a decoder at all ──\n");
    ok(DecoderHost::pagingBuilt(), "this test is a VIBE_PAGING build (as the desktop VibeServer now is)");
    ok(!DecoderHost::pagingProcess().load(), "off Android the process starts NOT allowed (only setHandheld turns it on)");
    {
        auto peer = std::make_shared<Peer>();
        DecoderHost::Env env; env.dialHz = [] { return 153.0e6; };
        DecoderHost h(env, "server");
        h.addPeer(peer);
        const auto r = h.start("paging", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"paging\",\"mode\":\"auto\"}");
        ok(r == DecoderHost::Start::Unknown, "start(\"paging\") on a server: Unknown");
        ok(!h.wantsChannelIq() && DecoderHost::pagingRunning().load() == 0, "no channel IQ wanted, no receiver running");
        const auto iq = flexIq();
        h.feedChannelIq(iq.data(), (int)iq.size(), 31250);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok(peer->lines().empty(), "fed pages anyway: the socket gets no page");
        h.stop();
    }

    std::printf("── the handheld's radio process: pages reach its own socket, nothing else ──\n");
    DecoderHost::pagingProcess().store(true);   // ★ what LocalSdrShim::setHandheld(true) does
    {
        auto peer = std::make_shared<Peer>(), other = std::make_shared<Peer>();
        std::atomic<int> taps{0};
        std::mutex lm; std::string logs;
        DecoderHost::Env env; env.dialHz = [] { return 153.0e6; };
        env.tap = [&](const std::string&, const std::string&) { taps++; return DecoderTapPtr(); };
        env.log = [&](const std::string& s) { std::lock_guard<std::mutex> lk(lm); logs += s + "\n"; };
        DecoderHost h(env, "handheld"), h2(env, "someone-else");
        h.addPeer(peer); h2.addPeer(other);
        const auto r = h.start("paging", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"paging\",\"mode\":\"auto\"}");
        ok(r == DecoderHost::Start::Ok, "start(\"paging\") in the handheld's process: Ok");
        ok(h.wantsChannelIq() && DecoderHost::pagingRunning().load() == 1, "it wants the channel IQ; one receiver running (the speaker's silence)");
        const auto iq = flexIq();
        for (int rep = 0; rep < 2; ++rep)
            for (size_t i = 0; i < iq.size(); i += 625) { h.feedChannelIq(iq.data() + i, (int)std::min<size_t>(625, iq.size() - i), 31250); std::this_thread::sleep_for(std::chrono::microseconds(300)); }
        std::string got;
        for (int i = 0; i < 100 && (got = peer->lines()).find("GATE TEST PAGE TWO") == std::string::npos; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok(got.find("[FLEX 1600/2 \xc2\xb7 0123456 \xc2\xb7 A] GATE TEST PAGE ONE") != std::string::npos, "page one reached the paging socket: " + got.substr(0, got.find('\n')));
        ok(got.find("GATE TEST PAGE TWO") != std::string::npos, "page two too");
        ok(taps.load() == 0, "the gallery was never asked (no tap for paging)");
        { std::lock_guard<std::mutex> lk(lm); ok(logs.find("GATE TEST") == std::string::npos && logs.find("0123456") == std::string::npos, "the host's log has no page text or capcode"); }
        ok(other->frames() == 0, "a socket on another host got nothing");
        h.stop();
        ok(!h.wantsChannelIq() && DecoderHost::pagingRunning().load() == 0, "stopped: no receiver running — the speaker's own level is back");
        ok(pagingSpeakerLevel(0.6f, DecoderHost::pagingRunning().load()) == 0.6f, "…the level the person had");
    }
    std::printf("── a recording while PAGE runs is silence (a real receiver started and stopped) ──\n");
    {
        const char* attach = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"paging\",\"mode\":\"auto\"}";
        DecoderHost::Env env; env.dialHz = [] { return 153.0e6; };
        const int kB = 960, ch = 2;   // 20 ms blocks of stereo, as the engine sends
        // 1. PAGE already on when REC is pressed: the whole recording is silence
        {
            DecoderHost h(env, "handheld"); h.addPeer(std::make_shared<Peer>());
            ok(h.start("paging", attach) == DecoderHost::Start::Ok, "PAGE on");
            std::vector<int16_t> rec; int ph = 0;
            for (int b = 0; b < 50; ++b) sendBlock(rec, toneBlock(kB, ch, ph));
            ok(rec.size() == (size_t)50 * kB * ch && allZero(rec, 0, rec.size()), "REC started with PAGE on: 1 s recorded, every sample silent");
            h.stop();
        }
        // 2. PAGE starts in the middle of a recording, then stops: tone · silence · the tone again, exactly
        {
            DecoderHost h(env, "handheld"); h.addPeer(std::make_shared<Peer>());
            std::vector<int16_t> rec; int ph = 0;
            for (int b = 0; b < 25; ++b) sendBlock(rec, toneBlock(kB, ch, ph));
            const size_t before = rec.size();
            ok(!allZero(rec, 0, before), "before PAGE: the channel's audio is recorded");
            ok(h.start("paging", attach) == DecoderHost::Start::Ok, "PAGE started mid-recording");
            for (int b = 0; b < 25; ++b) sendBlock(rec, toneBlock(kB, ch, ph));
            const size_t during = rec.size();
            ok(allZero(rec, before, during), "while PAGE runs: the rest of the recording is silence (" + std::to_string((during - before) / ch) + " frames)");
            h.stop();
            ok(DecoderHost::pagingRunning().load() == 0, "PAGE stopped");
            std::vector<std::vector<int16_t>> after;
            for (int b = 0; b < 25; ++b) { after.push_back(toneBlock(kB, ch, ph)); sendBlock(rec, after.back()); }
            bool same = true; size_t at = during;
            for (auto& blk : after) { same = same && sameAs(rec, at, blk); at += blk.size(); }
            ok(same && rec.size() == (size_t)75 * kB * ch, "after PAGE: the ordinary audio again, sample for sample, the recording unbroken");
        }
        // 3. raw IQ out follows the same counter
        {
            DecoderHost h(env, "handheld"); h.addPeer(std::make_shared<Peer>());
            ok(pagingAllowsIqOut(DecoderHost::pagingRunning().load()), "no PAGE: raw IQ out may run");
            h.start("paging", attach);
            ok(!pagingAllowsIqOut(DecoderHost::pagingRunning().load()), "PAGE on: raw IQ out refused (and not a sample queued)");
            h.stop();
            ok(pagingAllowsIqOut(DecoderHost::pagingRunning().load()), "PAGE off: raw IQ out may run again");
        }
    }
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
