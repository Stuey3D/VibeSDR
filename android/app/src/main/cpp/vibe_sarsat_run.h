// vibe_sarsat_run.h — V12 the 406 MHz beacon decoder inside a decoder host (vibe_decoder_host.h, ext "sarsat"): the receiver
// (decoders/sarsat_rx.h) on the listener's CHANNEL IQ; this file decides WHAT MAY BE SHOWN, and to whom. docs/v12/SARSAT.md.
//
// ★★★ THE PRIVACY RULE (Stuart's brief; WTA 2006 s.48 — a distress beacon's message is not a broadcast) — decided HERE:
//   • EVERYONE on this decoder sees THAT a beacon was heard: the kind (EPIRB / ELT / PLB …), the country of registration, the
//     protocol, SELF-TEST or TEST where it is one, the frequency, the time.
//   • ONLY THE OWNER's signed-in sockets (DecoderPeer::privileged, the host's textFramePriv) see what identifies the beacon or
//     says where it is: the 15 Hex ID, the identity it carries (MMSI, call sign, registration, serial, aircraft address) and the
//     ENCODED POSITION. Never the gap log, never the gallery (nothing is kept), never a map, never another listener.
//   • ★ A clear note, on attach and beside every non-test beacon: real alerts are received by the COSPAS-SARSAT satellites
//     and handled by the rescue authorities; VibeSDR reports nothing to anyone.
//
// Wire (on the decoder socket):
//   0x01 | u64 BE ts | u32 BE len | UTF-8      one line per beacon burst (public / owner as above), and the note
//   0x03 | u8 state                            0 listening · 1 a burst being worked
//   {"type":"sarsat","data":{…}}               on attach and after each burst: counts and the last burst's PUBLIC fields
#pragma once
#include "decoders/sarsat_rx.h"
#include "vibe_maritime_mid.h"

#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace vibe {

class SarsatRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;
        std::function<void(const std::string&, const std::string&)> textPriv;
        std::function<void(uint8_t)> state;
        std::function<void(const std::string&)> json;
    };
    static const char* note() {
        return "Real distress alerts are received by the COSPAS-SARSAT satellites and handled by the rescue authorities "
               "\xe2\x80\x94 VibeSDR reports nothing to anyone. Most beacons heard on the ground are tests. If you believe one "
               "is real and not already being handled, tell the coastguard or the police (in the UK 999 or 112).";
    }
    SarsatRun(std::function<double()> dialHz, Out out) : dial_(std::move(dialHz)), out_(std::move(out)) {
        if (out_.text) out_.text(clock_() + "  406 MHz beacons \xc2\xb7 listening. " + note() + "\n");
        sendJson_();
    }
    void feedIq(const std::complex<float>* x, int n, double rate) {
        if (n <= 0 || !(rate > 0)) return;
        if (!rx_ || std::fabs(rate - rate_) > 0.5) {
            rate_ = rate;
            rx_.reset(new sarsat::Rx(rate));
            rx_->onBurst = [this](const sarsat::Burst& b) { onBurst_(b); };
        }
        rx_->feed(x, n);
        if ((since_ += n) >= rate) { since_ = 0; sendJson_(); }
    }
    int bursts() const { return bursts_; }

private:
    static std::string clock_() {
        const time_t t = std::time(nullptr); struct tm u{}; gmtime_r(&t, &u);
        char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02dz", u.tm_hour, u.tm_min, u.tm_sec);
        return b;
    }
    static std::string country_(int mid) {
        const vibe::mid::Row* r = vibe::mid::find(mid);
        return (r ? std::string(r->name) + " (" : std::string("country ")) + std::to_string(mid) + (r ? ")" : "");
    }
    static std::string fmtPos_(double lat, double lon) {
        char b[80];
        std::snprintf(b, sizeof b, "%.4f\xc2\xb0 %c %.4f\xc2\xb0 %c", std::fabs(lat), lat < 0 ? 'S' : 'N', std::fabs(lon), lon < 0 ? 'W' : 'E');
        return b;
    }
    void onBurst_(const sarsat::Burst& b) {
        ++bursts_;
        const sarsat::Message& m = b.msg;
        const bool test = m.selfTest || m.testProtocol;
        if (test) ++tests_;
        const double rf = (dial_ ? dial_() : 0.0) + b.offsetHz;
        char f[40]; std::snprintf(f, sizeof f, "%.4f MHz", rf / 1e6);
        std::string pub = clock_() + "  406 MHz beacon \xc2\xb7 " + (m.beaconType.empty() ? std::string("beacon") : m.beaconType) + " \xc2\xb7 "
                          + country_(m.country) + " \xc2\xb7 " + m.protocol + (m.longMsg ? " (long)" : " (short)");
        if (m.selfTest) pub += " \xc2\xb7 SELF-TEST";
        else if (m.testProtocol) pub += " \xc2\xb7 TEST protocol";
        pub += std::string(" \xc2\xb7 ") + f;
        std::string priv = pub + " \xc2\xb7 Hex ID " + m.hexId;
        if (!m.identity.empty()) priv += " \xc2\xb7 " + m.identity;
        if (m.hasPosition) priv += " \xc2\xb7 position " + fmtPos_(m.lat, m.lon) + " (" + m.posRes + (m.posSource.empty() ? "" : ", " + m.posSource) + ")";
        else priv += " \xc2\xb7 no position encoded";
        if (m.homing121) priv += " \xc2\xb7 121.5 MHz homing";
        if (!m.extra.empty()) priv += " \xc2\xb7 " + m.extra;
        char q[120]; std::snprintf(q, sizeof q, " \xc2\xb7 BCH %d/%d corrected \xc2\xb7 %.0f dB-Hz", m.bch1Errors, m.bch2Errors < 0 ? 0 : m.bch2Errors, b.cn0);
        priv += q;
        priv += " (shown to you only)";
        if (!test) { pub += std::string(" \xc2\xb7 ") + note(); priv += std::string(" \xc2\xb7 ") + note(); }
        pub += "\n"; priv += "\n";
        last_ = b; lastTest_ = test; lastRf_ = rf;
        if (out_.textPriv) out_.textPriv(pub, priv); else if (out_.text) out_.text(pub);
        sendJson_();
    }
    static std::string esc_(const std::string& s) {
        std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
    }
    void sendJson_() {
        if (!out_.json) return;
        char b[200];
        std::snprintf(b, sizeof b, "{\"type\":\"sarsat\",\"data\":{\"bursts\":%d,\"tests\":%d,\"rate\":%.0f,\"lines\":%d,\"refused\":%d", bursts_, tests_, rate_,
                      rx_ ? rx_->lines : 0, rx_ ? rx_->refused : 0);
        std::string j = b;
        if (bursts_) {
            const sarsat::Message& m = last_.msg;
            std::snprintf(b, sizeof b, ",\"last\":{\"rf\":%.0f,\"country\":%d,\"test\":%s,\"selfTest\":%s,\"long\":%s", lastRf_, m.country,
                          lastTest_ ? "true" : "false", m.selfTest ? "true" : "false", m.longMsg ? "true" : "false");
            j += b;
            j += ",\"type\":\"" + esc_(m.beaconType) + "\",\"protocol\":\"" + esc_(m.protocol) + "\",\"countryName\":\"" + esc_(country_(m.country)) + "\"}";
        }
        j += "}}";
        out_.json(j);
    }
    std::function<double()> dial_;
    Out out_;
    std::unique_ptr<sarsat::Rx> rx_;
    double rate_ = 0, lastRf_ = 0;
    int64_t since_ = 0;
    int bursts_ = 0, tests_ = 0;
    sarsat::Burst last_;
    bool lastTest_ = false;
};

} // namespace vibe
