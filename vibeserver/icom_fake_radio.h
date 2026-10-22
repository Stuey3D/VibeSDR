// icom_fake_radio.h — a pretend IC-7300 that speaks CI-V, for the tests and tools/icom/fake-icom.
//
// ★★ IT IS ALSO A WITNESS. Every frame VibeServer sends it is checked twice: by civ::mayTransmit
//    (the same rule the server applies — so a server bug that bypassed it would show here), and by
//    a deliberately crude, separate test for the frames that transmit (1C with data, 17, 28, 18,
//    1A 05 with a value, VOX on). Either one fires = `violations` rises, the frame is answered NG,
//    and the end-to-end test fails. It never "transmits"; it counts.
//
// ★ The scope is synthetic but honest in shape: 475 points over the span it is set to, in 11 USB
//   divisions (header + 9×50 + 25), centre or fixed mode, paced by the BAUD RATE like the real wire
//   (~19 sweeps/s at 115200 with nothing else on it, plan §1.2). Carriers sit at fixed RADIO
//   frequencies, so tuning moves them across a centre-mode waterfall exactly as on the radio.
#pragma once
#include "civ_codec.h"
#include "icom_models.h"
#include "icom_transport.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace icom {

class FakeIcomRadio final : public CivPeer {
public:
    // ── Knobs a test turns ─────────────────────────────────────────────────────────────────
    uint8_t addr = 0x94;
    int baud = 115200;
    bool echo = false;               ///< CI-V USB Echo Back
    bool transceive = true;          ///< CI-V Transceive
    bool answer = true;              ///< false = a radio at the wrong baud: silence
    uint8_t usbSend = 0, keyCw = 0, keyRtty = 0, dataMod = 3;   ///< menu values (00 = OFF)
    bool vox = false;
    std::atomic<bool> tx{false};     ///< the owner presses PTT on the radio
    std::atomic<bool> unplugged{false};

    // ── What it saw ────────────────────────────────────────────────────────────────────────
    std::atomic<uint64_t> framesIn{0}, violations{0}, sweepsOut{0};
    std::string lastViolation;

    // ── Radio state ────────────────────────────────────────────────────────────────────────
    uint64_t freq = 14074000;
    uint8_t mode = 0x01, fil = 1, dataOn = 1;
    uint8_t att = 0, preamp = 1, agc = 2, nb = 0, nr = 0, anf = 0;
    int rf = 255, sql = 0;
    bool scopeOn = false, scopeData = false;
    uint8_t scopeMode = 0;           ///< 0 centre, 1 fixed
    uint32_t spanHalf = 25000;
    uint64_t fixedLo = 14000000, fixedHi = 14350000;
    uint8_t speed = 0;

    FakeIcomRadio() { const Model* m = modelByAddr(0x94); policyModel_ = m; }

    bool gone() const override { return unplugged.load(); }

    /** A person turns the radio's own knob. */
    void turnKnob(uint64_t hz) {
        std::lock_guard<std::mutex> lk(mtx_);
        freq = hz;
        if (transceive) { std::vector<uint8_t> d(5); civ::freqToBcd(hz, d.data()); reply(civ::kBroadcast, 0x00, d); }
    }

    void fromController(const uint8_t* b, int n) override {
        std::lock_guard<std::mutex> lk(mtx_);
        parser_.feed(b, (size_t)n, [&](const civ::Frame& f) { handle(f); });
    }

    int toController(uint8_t* b, int max, int timeoutMs) override {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                pace();
                if (!answer) out_.clear();
                int n = 0;
                while (n < max && !out_.empty() && credit_ >= 1.0) { b[n++] = out_.front(); out_.pop_front(); credit_ -= 1.0; }
                if (n > 0) return n;
            }
            if (std::chrono::steady_clock::now() >= until) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

private:
    std::mutex mtx_;
    civ::Parser parser_;
    std::deque<uint8_t> out_;
    double credit_ = 0, lastT_ = 0, nextSweep_ = 0;
    uint32_t seed_ = 12345;
    const Model* policyModel_;

    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    /** ★ The wire's own speed: 10 bits a byte. Bytes leave no faster than the baud allows, and
     *  the scope is produced only when the queue is nearly empty — so polls and scope share the
     *  link exactly as they do on a real CP2102. */
    void pace() {
        const double t = now();
        if (lastT_ == 0) lastT_ = t;
        credit_ += (t - lastT_) * baud / 10.0;
        if (credit_ > 2048) credit_ = 2048;
        lastT_ = t;
        if (scopeOn && scopeData && answer && !unplugged.load() && out_.size() < 64 && t >= nextSweep_) {
            emitSweep();
            nextSweep_ = t + (speed == 0 ? 1.0 / 30 : speed == 1 ? 1.0 / 15 : 1.0 / 8);
        }
    }

    void frame(uint8_t to, uint8_t c, const std::vector<uint8_t>& d) {
        out_.push_back(0xFE); out_.push_back(0xFE); out_.push_back(to); out_.push_back(addr); out_.push_back(c);
        for (uint8_t x : d) out_.push_back(x);
        out_.push_back(0xFD);
    }
    void reply(uint8_t to, uint8_t c, const std::vector<uint8_t>& d) { frame(to, c, d); }
    void ok() { frame(civ::kController, civ::kOk, {}); }
    void ng() { frame(civ::kController, civ::kNg, {}); }

    double carrierAmp(double f) const {
        // A few stations at fixed radio frequencies, amplitude above the floor (0-160 scale).
        struct C { double hz, w, a; };
        static const C cs[] = {{14074000, 1500, 55}, {14100000, 150, 95}, {14230000, 1200, 45},
                               {7074000, 1500, 60}, {14060000, 80, 70}, {3615000, 2500, 50}};
        double a = 0;
        for (const C& c : cs) { const double x = (f - c.hz) / c.w; a += c.a * std::exp(-x * x); }
        return a;
    }
    void emitSweep() {
        uint64_t lo, hi;
        if (scopeMode == 0) { lo = freq - spanHalf; hi = freq + spanHalf; } else { lo = fixedLo; hi = fixedHi; }
        const int n = 475;
        uint8_t amp[475];
        for (int i = 0; i < n; i++) {
            seed_ = seed_ * 1664525u + 1013904223u;
            const double f = (double)lo + (i + 0.5) * (double)(hi - lo) / n;
            int a = 28 + (int)((seed_ >> 24) % 10) + (int)carrierAmp(f);
            if (att) a -= 20;
            amp[i] = (uint8_t)(a < 0 ? 0 : a > 160 ? 160 : a);
        }
        // division 1: header
        std::vector<uint8_t> d = {0x00, 0x00, 0x01, 0x11, scopeMode};
        uint8_t a[5], b[5];
        if (scopeMode == 0) { civ::freqToBcd(freq, a); civ::freqToBcd(spanHalf, b); }
        else { civ::freqToBcd(lo, a); civ::freqToBcd(hi, b); }
        d.insert(d.end(), a, a + 5); d.insert(d.end(), b, b + 5); d.push_back(0x00);
        frame(civ::kController, 0x27, d);
        int at = 0;
        for (int div = 2; div <= 11; div++) {
            std::vector<uint8_t> q = {0x00, 0x00, civ::toBcdByte(div), 0x11};
            const int take = div == 11 ? 25 : 50;
            for (int k = 0; k < take && at < n; k++) q.push_back(amp[at++]);
            frame(civ::kController, 0x27, q);
        }
        sweepsOut.fetch_add(1);
    }

    /** ★ The crude, separate witness for the frames that would transmit. */
    static bool looksLikeTx(const civ::Frame& f) {
        if (f.cmd == 0x1C && f.len >= 2) return true;                // 1C 00 01, 1C 01 xx
        if (f.cmd == 0x17 || f.cmd == 0x28 || f.cmd == 0x18) return true;
        if (f.cmd == 0x1A && f.len >= 1 && f.data[0] == 0x05 && f.len > 3) return true;   // a menu WRITE
        if (f.cmd == 0x16 && f.len >= 2 && (f.data[0] == 0x46 || f.data[0] == 0x47)) return true;
        if (f.cmd == 0x14 && f.len >= 1 && (f.data[0] == 0x0A || f.data[0] == 0x0B)) return true;
        return false;
    }

    void handle(const civ::Frame& f) {
        if (f.to != addr) return;
        framesIn.fetch_add(1);
        if (echo) {   // ★ the frame exactly as it arrived, from E0 — the server must ignore it
            out_.push_back(0xFE); out_.push_back(0xFE); out_.push_back(f.to); out_.push_back(f.from); out_.push_back(f.cmd);
            for (int i = 0; i < f.len; i++) out_.push_back(f.data[i]);
            out_.push_back(0xFD);
        }
        // Rebuild the bytes and judge them exactly as the server must have.
        uint8_t raw[32]; size_t rn = 0;
        raw[rn++] = 0xFE; raw[rn++] = 0xFE; raw[rn++] = f.to; raw[rn++] = f.from; raw[rn++] = f.cmd;
        for (int i = 0; i < f.len && rn < 31; i++) raw[rn++] = f.data[i];
        raw[rn++] = 0xFD;
        civ::Policy p = makePolicy();
        const bool bad = looksLikeTx(f) || civ::mayTransmit(raw, rn, p) != civ::Verdict::Ok;
        if (bad) {
            violations.fetch_add(1);
            char h[96] = {0}; int at = 0;
            for (size_t i = 0; i < rn && at < 90; i++) at += std::snprintf(h + at, sizeof h - at, "%02X ", raw[i]);
            lastViolation = h;
            std::fprintf(stderr, "[fake-icom] VIOLATION: %s\n", h);
            ng(); return;
        }
        const uint8_t* d = f.data; const int len = f.len;
        auto u = [&](std::initializer_list<uint8_t> v) { reply(civ::kController, f.cmd, std::vector<uint8_t>(v)); };
        switch (f.cmd) {
        case 0x19: u({0x00, addr}); break;
        case 0x03: { std::vector<uint8_t> b(5); civ::freqToBcd(freq, b.data()); reply(civ::kController, 0x03, b); break; }
        case 0x04: u({mode, fil}); break;
        case 0x05: freq = (uint64_t)civ::bcdToFreq(d, 5); ok();
                   if (transceive) { std::vector<uint8_t> b(5); civ::freqToBcd(freq, b.data()); reply(civ::kBroadcast, 0x00, b); }
                   break;
        case 0x06: mode = d[0]; fil = d[1]; ok(); if (transceive) reply(civ::kBroadcast, 0x01, {mode, fil}); break;
        case 0x11: if (len == 0) u({att}); else { att = d[0]; ok(); } break;
        case 0x15: { const int s = sMeter(); uint8_t b[2]; civ::intToBcdBig(s, b, 2); u({0x02, b[0], b[1]}); break; }
        case 0x1C: u({0x00, (uint8_t)(tx.load() ? 1 : 0)}); break;
        case 0x14: {
            int& v = d[0] == 0x02 ? rf : sql;
            if (len == 1) { uint8_t b[2]; civ::intToBcdBig(v, b, 2); u({d[0], b[0], b[1]}); }
            else { v = civ::bcdBigToInt(d + 1, 2); ok(); }
            break;
        }
        case 0x16: {
            uint8_t* v = d[0] == 0x02 ? &preamp : d[0] == 0x12 ? &agc : d[0] == 0x22 ? &nb : d[0] == 0x40 ? &nr : d[0] == 0x41 ? &anf : nullptr;
            if (d[0] == 0x46) { u({0x46, (uint8_t)(vox ? 1 : 0)}); break; }
            if (!v) { ng(); break; }
            if (len == 1) u({d[0], *v}); else { *v = d[1]; ok(); }
            break;
        }
        case 0x1A:
            if (d[0] == 0x05 && len == 3) {
                const int item = civ::bcdBigToInt(d + 1, 2);
                const int v = item == 89 ? usbSend : item == 90 ? keyCw : item == 91 ? keyRtty : item == 67 ? dataMod : -1;
                if (v < 0) ng(); else u({0x05, d[1], d[2], (uint8_t)v});
            } else if (d[0] == 0x06) { if (len == 1) u({0x06, dataOn, dataOn ? fil : (uint8_t)0}); else { dataOn = d[1]; ok(); } }
            else if (d[0] == 0x03) { if (len == 1) u({0x03, 0x30}); else ok(); }
            else ng();
            break;
        case 0x27:
            switch (d[0]) {
            case 0x10: if (len == 1) u({0x10, (uint8_t)scopeOn}); else { scopeOn = d[1]; ok(); } break;
            case 0x11: if (len == 1) u({0x11, (uint8_t)scopeData}); else { scopeData = d[1]; ok(); } break;
            case 0x14: if (len == 2) u({0x14, 0x00, scopeMode}); else { scopeMode = d[2]; ok(); } break;
            case 0x15: if (len == 2) { uint8_t b[5]; civ::freqToBcd(spanHalf, b); u({0x15, 0x00, b[0], b[1], b[2], b[3], b[4]}); }
                       else { spanHalf = (uint32_t)civ::bcdToFreq(d + 2, 5); ok(); } break;
            case 0x1A: if (len == 2) u({0x1A, 0x00, speed}); else { speed = d[2]; ok(); } break;
            default: ok();
            }
            break;
        default: ng();
        }
    }
    int sMeter() const { const double a = carrierAmp((double)freq); return (int)(a * 1.5 > 241 ? 241 : a * 1.5); }
    civ::Policy makePolicy() const {
        civ::Policy p;
        const Model& m = *policyModel_;
        p.radio = addr;
        p.nRanges = 0; for (int i = 0; i < m.nRanges; i++) p.ranges[p.nRanges++] = {m.ranges[i].lo, m.ranges[i].hi};
        p.nModes = 0; for (int i = 0; i < m.nModes; i++) if (!civ::inList(p.modes, p.nModes, m.modeBytes[i])) p.modes[p.nModes++] = m.modeBytes[i];
        p.nAtt = 0; for (int i = 0; i < m.nAtt; i++) p.att[p.nAtt++] = m.att[i];
        p.preampMax = m.preampMax; p.dataModeMax = m.dataModeMax;
        p.nSpans = 0; for (int i = 0; i < m.nSpans; i++) p.spansHalfHz[p.nSpans++] = m.spansHalfHz[i];
        p.nMenuItems = 0; for (int i = 0; i < m.nMenu; i++) { p.menuItems[i][0] = m.menu[i].item[0]; p.menuItems[i][1] = m.menu[i].item[1]; p.nMenuItems++; }
        return p;
    }
};

}  // namespace icom
