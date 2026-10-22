// ★★ IcomSource AGAINST A PRETEND IC-7300 — open, identify, PREFLIGHT, the scope, tuning both ways,
//    the owner transmitting, the radio going away and coming back. No hardware; real threads, real
//    timing (the fake paces its bytes at the baud rate). See icom_fake_radio.h.
//
// ★★★ Every run ends by asking the fake how many frames it was sent that it would not accept, by
//     two separate rules. The answer must be 0, whatever else happened.
#include "icom_fake_radio.h"
#include "icom_source.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace icom;

static int failures = 0, checks = 0;
static void ok(bool c, const std::string& what) {
    checks++;
    std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str());
    if (!c) failures++;
}
template <class F> static bool waitFor(F f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}

static IcomConfig cfgTone() { IcomConfig c; c.audio = "tone"; c.modelHint = "IC-7300 03001234"; return c; }

int main() {
    std::printf("── IcomSource against the fake IC-7300 ──\n");
    {
        FakeIcomRadio fake;
        fake.echo = true;    // ★ the harder case: our own frames come back and must be ignored
        IcomSource src;
        std::atomic<int> sweeps{0}; std::atomic<long long> lo{0}, hi{0};
        std::atomic<int> pcmBlocks{0}, pcmSilentBlocks{0};
        std::atomic<int> sweepsDuringTx{0}; std::atomic<bool> inTx{false};
        src.setSweepSink([&](const Sweep& s) { sweeps++; lo = s.loHz; hi = s.hiHz; if (inTx) sweepsDuringTx++; });
        src.setPcmSink([&](const int16_t* p, int n, int ch, int) {
            pcmBlocks++;
            bool silent = true; for (int i = 0; i < n * ch; i++) if (p[i]) { silent = false; break; }
            if (silent) pcmSilentBlocks++;
        });
        std::string err;
        const bool opened = src.open(cfgTone(), err, makeMemoryTransport(&fake));
        ok(opened, "opens: " + (opened ? std::string("ok") : err));
        if (!opened) { std::printf("%d checks, %d failures\n", checks, failures + 1); return 1; }
        ok(std::string(src.model().name) == "IC-7300", "identified as an IC-7300 by 19 00");
        ok(src.preflight().passed && src.preflight().items.size() == 5, "preflight passed, 4 menu items + VOX read");
        ok(!src.preflight().verified, "and says the 7300 menu numbers are not yet confirmed on a real radio");
        ok(fake.scopeOn && fake.scopeData, "scope on and data output on (27 10 01, 27 11 01)");
        ok(waitFor([&] { return sweeps.load() >= 8; }, 3000), "whole sweeps arrive (" + std::to_string(sweeps.load()) + ")");
        ok(lo.load() == 14074000 - 25000 && hi.load() == 14074000 + 25000, "sweep edges are the radio's: centre ± half-span");
        const RadioState s0 = src.state();
        ok(s0.freq == 14074000 && s0.mode == "usbd" && s0.fil == 1, "state read at open: 14.074 USB-D FIL1 (" + s0.mode + ")");
        ok(s0.preamp == 1 && s0.agc == 2 && s0.rfGain == 255, "RF state read at open");
        ok(waitFor([&] { return src.state().sweepsPerSec > 5; }, 3000), "sweep rate measured: " + std::to_string(src.state().sweepsPerSec));

        std::string why;
        ok(src.tune(7074000, why), "tune 7.074 MHz accepted");
        ok(waitFor([&] { return fake.freq == 7074000 && src.state().freq == 7074000; }, 1000), "the radio moved, and the state followed");
        ok(waitFor([&] { return lo.load() == 7074000 - 25000; }, 1500), "and the waterfall's edges moved with it");
        ok(!src.tune(144300000, why) && why.find("outside") != std::string::npos, "2 m on a 7300 refused: " + why);
        fake.turnKnob(14100000);
        ok(waitFor([&] { return src.state().freq == 14100000; }, 800), "the owner's knob (transceive 00) followed");
        fake.transceive = false;
        fake.turnKnob(14060000);
        ok(waitFor([&] { return src.state().freq == 14060000; }, 2000), "and with Transceive OFF, the 1 s poll finds it");
        ok(src.setMode("cw", 2, why), "mode CW FIL2 accepted");
        ok(waitFor([&] { return fake.mode == 0x03 && fake.fil == 2 && fake.dataOn == 0; }, 1000), "radio in CW FIL2, DATA off");
        ok(waitFor([&] { return src.state().mode == "cw"; }, 2000), "state says cw");
        ok(src.setMode("usbd", 1, why) && waitFor([&] { return fake.mode == 0x01 && fake.dataOn == 1; }, 1000), "USB-D sets mode 01 + 1A 06 01");
        ok(!src.setMode("wfm", 1, why), "WFM refused on a 7300: " + why);
        ok(src.setAtt(20, why) && waitFor([&] { return fake.att == 0x20; }, 1000), "ATT 20 dB");
        ok(!src.setAtt(10, why), "ATT 10 dB refused (the 7300 has 20 only)");
        ok(src.setPreamp(2, why) && waitFor([&] { return fake.preamp == 2; }, 1000), "P.AMP2");
        ok(src.setRfGain(128, why) && waitFor([&] { return fake.rf == 128; }, 1000), "RF gain 128");
        ok(src.setSpanTotal(100000, why) && waitFor([&] { return hi.load() - lo.load() == 100000; }, 1500), "span ±50 kHz: the sweep is 100 kHz wide");
        ok(!src.setSpanTotal(123000, why), "a span not on the ladder refused");
        ok(src.setScopeMode(1, why) && waitFor([&] { return lo.load() == 14000000 && hi.load() == 14350000; }, 1500), "fixed mode: the band edges");
        src.setScopeMode(0, why);
        // a burst of tunes (a held key) coalesces — the last one wins, quickly
        for (int i = 0; i < 40; i++) src.tune(14000000 + i * 1000, why);
        ok(waitFor([&] { return fake.freq == 14039000; }, 1500), "40 tunes in a burst: the radio ends on the LAST one");

        // ── the owner transmits ──
        const int pcmBefore = pcmSilentBlocks.load();
        fake.tx = true;
        ok(waitFor([&] { return src.state().tx; }, 1500), "the owner keys: 1C 00 reads 01, state.tx");
        inTx = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ok(pcmSilentBlocks.load() - pcmBefore >= 30, "listeners hear SILENCE while he transmits (" + std::to_string(pcmSilentBlocks.load() - pcmBefore) + " silent blocks)");
        ok(sweepsDuringTx.load() == 0, "and the waterfall is frozen");
        fake.tx = false;
        ok(waitFor([&] { return !src.state().tx; }, 1500), "unkey: back to receive");
        inTx = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const int afterSilent = pcmSilentBlocks.load(), afterAll = pcmBlocks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ok(pcmSilentBlocks.load() == afterSilent && pcmBlocks.load() > afterAll, "audio flows again after the 300 ms hold");

        // ── unplugged and back ──
        std::atomic<int> lost{0};
        src.setLostSink([&](const std::string&) { lost++; });
        fake.unplugged = true;
        ok(waitFor([&] { return !src.state().linkUp; }, 4000), "unplugged: the link is reported down");
        fake.unplugged = false;
        ok(waitFor([&] { return src.state().linkUp; }, 6000), "plugged back: reconnected (identify + preflight again)");
        const int s1 = sweeps.load();
        ok(waitFor([&] { return sweeps.load() > s1 + 3; }, 3000), "and the scope streams again");

        src.close();
        ok(!fake.scopeData, "close switches the scope's data output back off");
        ok(fake.violations.load() == 0, "★ the fake saw ZERO disallowed frames (" + std::to_string(fake.framesIn.load()) + " frames) " + fake.lastViolation);
        ok(src.refusedFrames() == 0, "★ and the server refused none of its own (no builder produced a bad frame)");
    }
    // ── preflight refusals ──
    struct Case { const char* what; void (*set)(FakeIcomRadio&); const char* expect; };
    const Case cases[] = {
        {"USB SEND = RTS", [](FakeIcomRadio& f) { f.usbSend = 2; }, "USB SEND is RTS"},
        {"USB Keying (CW) = DTR", [](FakeIcomRadio& f) { f.keyCw = 1; }, "USB Keying (CW) is DTR"},
        {"USB Keying (RTTY) = RTS", [](FakeIcomRadio& f) { f.keyRtty = 2; }, "USB Keying (RTTY) is RTS"},
        {"VOX on", [](FakeIcomRadio& f) { f.vox = true; }, "VOX is ON"},
    };
    for (const Case& c : cases) {
        FakeIcomRadio fake; c.set(fake);
        IcomSource src; std::string err;
        const bool o = src.open(cfgTone(), err, makeMemoryTransport(&fake));
        ok(!o && err.find(c.expect) != std::string::npos, std::string("REFUSES to start with ") + c.what + ": " + err.substr(0, 120));
        ok(!fake.scopeData && fake.violations.load() == 0, std::string("   … and sent it nothing but reads (") + c.what + ")");
    }
    {
        FakeIcomRadio fake; fake.answer = false;
        IcomSource src; std::string err;
        ok(!src.open(cfgTone(), err, makeMemoryTransport(&fake)) && err.find("Baud Rate") != std::string::npos, "a silent radio: says to check the CI-V baud rate");
    }
    {
        FakeIcomRadio fake; fake.addr = 0x98;   // an IC-7610 at its own address
        IcomSource src; std::string err; IcomConfig c = cfgTone(); c.civAddr = 0x98; c.modelHint = "";
        ok(!src.open(c, err, makeMemoryTransport(&fake)) && err.find("IC-7610") != std::string::npos, "an IC-7610 is named and refused: " + err.substr(0, 80));
    }
    // ── ★★ ANDROID'S TWO DOORS (plan Phase 4), as far as a host can reach them ──
    //    "push": the app's AudioRecord pushes blocks in; they must reach the engine exactly as ALSA's do — TX-muted,
    //    dropped when no Icom is served — and the app's audio state must reach hwinfo.
    //    "fd:": the transport wraps a UsbManager descriptor; only Linux/Android usbfs has one, so here we check the
    //    spec is understood and refused cleanly, and that the fresh-descriptor hand-off is inert on other transports.
    std::printf("── Android: push audio + fd transport ──\n");
    {
        int16_t blk[480];
        for (int i = 0; i < 480; i++) blk[i] = (int16_t)(1000 + i);
        pushPcm(blk, 480, 1, 48000);
        ok(!pushWanted(), "push: no Icom served — nothing wants audio, and a pushed block goes nowhere");
        FakeIcomRadio fake;
        IcomSource src;
        std::atomic<int> got{0}, silent{0}, chOk{0};
        src.setPcmSink([&](const int16_t* p, int n, int ch, int rate) {
            got++;
            if (ch == 1 && rate == 48000 && n == 480) chOk++;
            bool z = true; for (int i = 0; i < n * ch; i++) if (p[i]) { z = false; break; }
            if (z) silent++;
        });
        IcomConfig c = cfgTone(); c.audio = "push";
        std::string err;
        const bool opened = src.open(c, err, makeMemoryTransport(&fake));
        ok(opened, "opens with audio=push: " + (opened ? std::string("ok") : err));
        ok(pushWanted(), "push: the served Icom now wants the app's audio");
        for (int i = 0; i < 20; i++) pushPcm(blk, 480, 1, 48000);
        ok(got.load() == 20 && chOk.load() == 20 && silent.load() == 0, "20 pushed blocks reach the engine unchanged (48 kHz mono)");
        setPushState("USB Audio CODEC (USB)", "");
        ok(src.audioDescription() == "USB Audio CODEC (USB)" && src.audioProblem().empty(), "the app's device name is what hwinfo says");
        std::atomic<int> links{0};
        src.setStateSink([&](unsigned w) { if (w & IcomSource::kLink) links++; });
        setPushState("USB Audio CODEC (USB)", "microphone permission refused");
        ok(src.audioProblem() == "microphone permission refused", "and its problem, in its words");
        ok(waitFor([&] { return links.load() > 0; }, 1000), "a changed audio state re-sends hwinfo (kLink)");
        fake.tx = true;
        ok(waitFor([&] { return src.state().tx; }, 1500), "the owner keys");
        const int s0 = silent.load();
        for (int i = 0; i < 10; i++) pushPcm(blk, 480, 1, 48000);
        ok(silent.load() - s0 == 10, "★ pushed audio is MUTED while he transmits, exactly as ALSA's is");
        fake.tx = false;
        ok(!src.wantsFreshFd() && !src.usbGone(), "a memory transport never asks for a fresh descriptor");
        ok(!src.offerFd(-1), "and offerFd refuses a bad descriptor");
        src.close();
        ok(!pushWanted(), "closed: the push source is gone");
        const int g0 = got.load();
        pushPcm(blk, 480, 1, 48000);
        ok(got.load() == g0, "and a block pushed after close goes nowhere");
        ok(fake.violations.load() == 0, "★ zero disallowed frames at the fake");
        setPushState("Android USB audio (not started yet)", "");
    }
    {
        std::string err;
        auto t = makeTransport("fd:notanumber", 115200, false, err);
        ok(!t && !err.empty(), "fd:<junk> refused: " + err);
        err.clear();
        t = makeTransport("fd:7", 115200, false, err);
#if defined(__linux__)
        ok(t != nullptr || err.find("USB") != std::string::npos, "fd:7 builds the wrapped-fd transport on Linux");
#else
        ok(!t && err.find("Android or Linux") != std::string::npos, "fd:7 refused cleanly where usbfs does not exist: " + err);
#endif
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
